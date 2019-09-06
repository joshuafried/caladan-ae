extern "C" {
#include <base/log.h>
#include <net/ip.h>
#include <unistd.h>
#include <runtime/smalloc.h>
}

#include "net.h"
#include "runtime.h"
#include "sync.h"
#include "synthetic_worker.h"
#include "thread.h"

#include <mlpack/core.hpp>
#include <mlpack/methods/cf/cf.hpp>
#include <mlpack/methods/cf/decomposition_policies/randomized_svd_method.hpp>
#include <mlpack/methods/cf/decomposition_policies/regularized_svd_method.hpp>
#include <mlpack/methods/cf/neighbor_search_policies/lmetric_search.hpp>
#include <mlpack/methods/cf/neighbor_search_policies/cosine_search.hpp>
#include <mlpack/methods/cf/neighbor_search_policies/pearson_search.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

using namespace std::chrono;
using namespace mlpack;
using namespace mlpack::cf;

using sec = duration<double, std::micro>;

// A prime number as hash size gives a better distribution of values in buckets
constexpr uint64_t HASH_SIZE_DEFAULT = 10009;

// Class representing a templatized hash node
template <typename K, typename V>
class HashNode
{
  public:
    HashNode() : next(nullptr) {}
    HashNode(K key_, V value_) : next(nullptr), key(key_), value(value_) {}
    ~HashNode() {
        next = nullptr;
    }

    const K& getKey() const {return key;}
    void setValue(V value_) {value = value_;}
    const V& getValue() const {return value;}

    // Pointer to the next node in the same bucket
    HashNode *next;
  private:
    K key;
    V value;
};

// Class representing a hash bucket. The bucket is implemented as a singly linked list.
// A bucket is always constructed with a dummy head node
template <typename K, typename V>
class HashBucket {
public:
  HashBucket() : head(nullptr) {}

  ~HashBucket() {
    // delete the bucket
    clear();
  }

  // Find an entry in the bucket matching the key
  // If key is found, the corresponding value is copied into the parameter "value" and function returns true.
  // If key is not found, function returns false
  bool find(const K &key, V &value) {
    // A shared mutex is used to enable mutiple concurrent reads
    {
      rt::ScopedLock<rt::Mutex> lk(&bucket_lock_);
      HashNode<K, V> * node = head;

      while (node != nullptr) {
        if (node->getKey() == key) {
          value = node->getValue();
          return true;
        }
        node = node->next;
      }
      return false;
    }
  }

  // Insert into the bucket
  // If key already exists, update the value, else insert a new node in the bucket with the <key, value> pair
  void insert(const K &key, const V &value) {
    // Exclusive lock to enable single write in the bucket
    {
      rt::ScopedLock<rt::Mutex> lk(&bucket_lock_);
      HashNode<K, V> * prev = nullptr;
      HashNode<K, V> * node = head;

      while (node != nullptr && node->getKey() != key) {
        prev = node;
        node = node->next;
      }

      if (nullptr == node) {
        // New entry, create a node and add to bucket

        if(nullptr == head) {
          head = new HashNode<K, V>(key, value);
        } else {
          prev->next = new HashNode<K, V>(key, value);                 
        }
      } else {
        // Key found in bucket, update the value
        node->setValue(value); 
      }
    }
  }

  // Remove an entry from the bucket, if found
  void erase(const K &key) {
    // Exclusive lock to enable single write in the bucket
    {
      rt::ScopedLock<rt::Mutex> lk(&bucket_lock_);
      HashNode<K, V> *prev  = nullptr;
      HashNode<K, V> * node = head;

      while (node != nullptr && node->getKey() != key) {
        prev = node;
        node = node->next;
      }

      if (nullptr == node) {
        // Key not found, nothing to be done
          return;
      } else {
        // Remove the node from the bucket
        if(head == node) {
          head = node->next;
        } else {
          prev->next = node->next; 
        }
        delete node;
      }
    }
  }

  // Clear the bucket
  void clear() {
    {
      rt::ScopedLock<rt::Mutex> lk(&bucket_lock_);
      HashNode<K, V> * prev = nullptr;
      HashNode<K, V> * node = head;
      while(node != nullptr) {
        prev = node;
        node = node->next;
        delete prev;
      }
      head = nullptr;
    }
  }

private:
  // The head node of the bucket
  HashNode<K, V> * head;
  // Per-bucket lock
  rt::Mutex bucket_lock_;
};

// Per-bucket Lock HashMap
template <typename K, typename V, typename F = std::hash<K> >
class HashMap {
public:
  HashMap(size_t hashSize_ = HASH_SIZE_DEFAULT) : hashSize(hashSize_) {
    // Create the hash table as an array of hash buckets
    hashTable = new HashBucket<K, V>[hashSize];
  }

  ~HashMap() {
    delete [] hashTable;
  }
  // Copy and Move of the HashMap are not supported at this moment
  HashMap(const HashMap&) = delete;
  HashMap(HashMap&&) = delete;
  HashMap& operator=(const HashMap&) = delete;  
  HashMap& operator=(HashMap&&) = delete;

  // Find an entry in the hash map matching the key.
  // If key is found, the corresponding value is copied into the parameter "value" and function returns true.
  // If key is not found, function returns false.
  bool find(const K &key, V &value) const  {
    size_t hashValue = hashFn(key) % hashSize ;
    return hashTable[hashValue].find(key, value);
  }

  // Insert into the hash map.
  // If key already exists, update the value, else insert a new node in the bucket with the <key, value> pair.
  void insert(const K &key, const V &value) {
    size_t hashValue = hashFn(key) % hashSize ;
    hashTable[hashValue].insert(key, value);
  }

  // Remove an entry from the bucket, if found
  void erase(const K &key) {
    size_t hashValue = hashFn(key) % hashSize ;
    hashTable[hashValue].erase(key);
  }   

  // Clean up the hash map
  void clear() {
    for(size_t i = 0; i < hashSize; i++) {
      (hashTable[i]).clear();
    }
  }

private:
  HashBucket<K, V> * hashTable;
  F hashFn;
  const size_t hashSize;
};


static float htonf(float value) {
  union v {
    float f;
    unsigned int i;
  };

  union v val;
  unsigned int temp;

  val.f = value;
  temp = hton32(val.i);

  return *(float*)&temp;
}

static float ntohf(float value) {
  union v {
    float f;
    unsigned int i;
  };

  union v val;
  unsigned int temp;

  val.f = value;
  temp = ntoh32(val.i);

  return *(float*)&temp;
}

constexpr uint64_t kTBMaxToken = 32;
class TokenBucket {
public:
  // rate : requests / sec
  TokenBucket(uint64_t rate)
    : refresh_interval_(1000000000 / rate),
      token_(kTBMaxToken), clock_(steady_clock::now()) {}

  void Update() {
    barrier();
    auto now = steady_clock::now();
    barrier();

    uint64_t elapsed_time_ns = duration_cast<nanoseconds>(now - clock_).count();

    if (elapsed_time_ns >= refresh_interval_) {
      int new_token = elapsed_time_ns / refresh_interval_;
      assert(new_token > 0);
      token_ += new_token;
      token_ = std::min<uint64_t>(token_, kTBMaxToken);
      clock_ += nanoseconds(new_token * refresh_interval_);
    }
  }

  bool GetToken() {
    if (token_ > 0) {
      token_--;
      return true;
    }
    return false;
  }

  void SetRate(uint64_t rate) {
    refresh_interval_ = 1000000000 / rate;
  }

  uint64_t GetRate() {
    return (1000000000 / refresh_interval_);
  }

private:
  // token refill time (ns / req)
  uint64_t refresh_interval_;
  // the number of remaining token (reqs)
  uint64_t token_;
  // internal timer
  time_point<steady_clock> clock_;
};

constexpr uint32_t RCTarget = 10000;
constexpr uint32_t RCInit = 10000;
constexpr uint32_t RCVSize = 100;
class RateController {
public:
  RateController(std::shared_ptr<TokenBucket> tb) : 
    tb_(tb), i_(0), ninety_percentile_(RCTarget), cur_rate_(RCInit) {
      resp_times_.reserve(RCVSize);
      tb_->SetRate(RCInit); // initial rate setting: 1k req / s
    }

  void SampleResponseTime(uint64_t response_time_us) {
    m_.Lock();
    resp_times_[i_++] = response_time_us;
    if (i_ == 100) {
      std::sort(resp_times_.begin(), resp_times_.begin()+RCVSize);
      ninety_percentile_ = static_cast<uint32_t>(0.7 * ninety_percentile_ + 0.3* resp_times_[89]);

      double err = ((double)ninety_percentile_ - (double)RCTarget) / (double)RCTarget;
      uint32_t new_rate = cur_rate_;
      if (err > 0) {
        new_rate = static_cast<uint32_t>(cur_rate_ / 1.2);
      } else if (err < - 0.5) {
        new_rate = static_cast<uint32_t>(cur_rate_ - (err + 0.1) * 200.0);
      }
      new_rate = std::max<uint32_t>(new_rate, 1);
      new_rate = std::min<uint32_t>(new_rate, 1000000);
      cur_rate_ = new_rate;
      tb_->SetRate(cur_rate_);
      i_ = 0;
    }
    m_.Unlock();
  }

  bool RequestSend() {
    bool ret;
    m_.Lock();
    tb_->Update();
    ret = tb_->GetToken();
    m_.Unlock();

    return ret;
  }

private:
  std::shared_ptr<TokenBucket> tb_;
  std::atomic<uint32_t> i_;
  uint32_t ninety_percentile_;
  uint32_t cur_rate_;
  std::vector<uint32_t> resp_times_;
  rt::Mutex m_;
};

constexpr uint64_t kUptimePort = 8002;
constexpr uint64_t kUptimeMagic = 0xDEADBEEF;
struct uptime {
  uint64_t idle;
  uint64_t busy;
//  uint32_t load;
};

void UptimeWorker(std::unique_ptr<rt::TcpConn> c) {
  while (true) {
    // Receive an uptime request.
    uint64_t magic;
    ssize_t ret = c->ReadFull(&magic, sizeof(magic));
    if (ret != static_cast<ssize_t>(sizeof(magic))) {
      if (ret == 0 || ret == -ECONNRESET) break;
      log_err("read failed, ret = %ld", ret);
      break;
    }

    // Check for the right magic value.
    if (ntoh64(magic) != kUptimeMagic) break;

    // Calculate the current uptime.
    std::ifstream file("/proc/stat");
    std::string line;
    std::getline(file, line);
    std::istringstream ss(line);
    std::string tmp;
    uint64_t user, nice, system, idle, iowait, irq, softirq, steal, guest,
        guest_nice;
    ss >> tmp >> user >> nice >> system >> idle >> iowait >> irq >> softirq >>
        steal >> guest >> guest_nice;
    uptime u = {hton64(idle + iowait),
                hton64(user + nice + system + irq + softirq + steal)};
//                hton32(static_cast<uint32_t>(rt::RuntimeLoad() * 1000000.0))};

    // Send an uptime response.
    ssize_t sret = c->WriteFull(&u, sizeof(u));
    if (sret != sizeof(u)) {
      if (sret == -EPIPE || sret == -ECONNRESET) break;
      log_err("write failed, ret = %ld", sret);
      break;
    }
  }
}

void UptimeServer() {
  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen({0, kUptimePort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { UptimeWorker(std::unique_ptr<rt::TcpConn>(c)); }).Detach();
  }
}

constexpr uint64_t kServerPort = 8001;
struct payload {
  uint64_t user_id;
  uint64_t movie_id;
  uint64_t request_index;
  uint64_t queueing_delay;
  uint64_t processing_time;
  float rating;
};

class SharedTcpStream {
public:
  SharedTcpStream(std::shared_ptr<rt::TcpConn> c) : c_(c) {}
  ssize_t WriteFull(const void *buf, size_t len) {
    rt::ScopedLock<rt::Mutex> lock(&sendMutex_);
    return c_->WriteFull(buf, len);
  }

private:
  std::shared_ptr<rt::TcpConn> c_;
  rt::Mutex sendMutex_;
};

class SharedWorkerPool {
public:
  SharedWorkerPool(std::string s) {
    unsigned int max_cores = rt::RuntimeMaxCores();
    idle_workers_.reserve(max_cores);
    for(unsigned int i = 0 ; i < max_cores ; ++i) {
      SyntheticWorker *w = SyntheticWorkerFactory(s);
      std::shared_ptr<SyntheticWorker> shared_w(w);
      idle_workers_[i] = shared_w;
    }
  }

  std::shared_ptr<SyntheticWorker> GetWorker(unsigned int kthread_idx) {
    return idle_workers_[kthread_idx];
  }

private:
  std::vector<std::shared_ptr<SyntheticWorker>> idle_workers_;
};

class RequestContext {
public:
  RequestContext(std::shared_ptr<SharedTcpStream> c) : timed_out(false), conn(c) {}
  payload p;
  bool timed_out;
  std::shared_ptr<SharedTcpStream> conn;
  void *operator new(size_t size) {
    void *p = smalloc(size);
    if (unlikely(p == nullptr)) throw std::bad_alloc();
    return p;
  }
  void operator delete(void *p) { sfree(p); }
};

void HandleRequest(RequestContext *ctx,
                   std::shared_ptr<SharedWorkerPool> wpool,
                   CFType<> *cf) {
  auto w = wpool->GetWorker(rt::RuntimeKthreadIdx());
  payload *p = &ctx->p;
  uint64_t processing_time = 0;
  double rating = 0;

  if (!ctx->timed_out) {
    // perform fake work
    auto start = steady_clock::now();
    uint64_t uid = ntoh64(p->user_id);
    uint64_t mid = ntoh64(p->movie_id);
    rating = cf->Predict(uid,mid);
    barrier();
    auto finish = steady_clock::now();
    barrier();
  
    processing_time = duration_cast<microseconds>(finish - start).count();
  }

  p->queueing_delay = hton64(rt::RuntimeQueueingDelayUS());
  p->processing_time = hton64(processing_time);
  p->rating = htonf(rating);

  ssize_t ret = ctx->conn->WriteFull(&ctx->p, sizeof(ctx->p));
  if (ret != static_cast<ssize_t>(sizeof(ctx->p))) {
    if (ret != -EPIPE && ret != -ECONNRESET) log_err("tcp_write failed");
  }
}

void ServerWorker(std::shared_ptr<rt::TcpConn> c, std::shared_ptr<SharedWorkerPool> wpool,
                  std::shared_ptr<RateController> rc, CFType<> *cf,
                  std::shared_ptr<HashMap<uint64_t, RequestContext *>> context_by_id) {
  auto resp = std::make_shared<SharedTcpStream>(c);

  /* allocate context */
  auto ctx = new RequestContext(resp);
  while (true) {
    payload *p = &ctx->p;

    /* Receive a work request */
    ssize_t ret = c->ReadFull(p, sizeof(*p));
    if (ret != static_cast<ssize_t>(sizeof(*p))) {
      if (ret != 0 && ret != -ECONNRESET)
        log_err("read failed, ret = %ld", ret);
      delete ctx;
      return;
    }
/*
    if (rt::RuntimeLoad() > 0.9999){
      delete ctx;
      ctx = new RequestContext(resp);
      continue;
    }
    */
/*
    if (rt::RuntimeQueueingDelayUS() > 100) {
      delete ctx;
      ctx = new RequestContext(resp);
      continue;
    }*/
/*
    if (!rc->RequestSend()) {
      delete ctx;
      ctx = new RequestContext(resp);
      continue;
    }

    barrier();
    auto now = steady_clock::now();
    barrier();
*/
    uint64_t user_id = ntoh64(p->user_id);
    uint64_t movie_id = ntoh64(p->movie_id);
    uint64_t index = ntoh64(p->request_index);

    if (user_id == 0 && movie_id == 0) {
      // mark context as timeout
      RequestContext *to_ctx;
      if (context_by_id->find(index, to_ctx)) {
        to_ctx->timed_out = true;
      }
    } else {
      context_by_id->insert(index, ctx);
    }

    rt::Thread([=] {
      HandleRequest(ctx, wpool, cf);
      context_by_id->erase(index);
      delete ctx;
//      auto ts = steady_clock::now();
//      rc->SampleResponseTime(duration_cast<microseconds>(ts - now).count());
    }).Detach();
    ctx = new RequestContext(resp);
  }
}

void ServerHandler(void *arg) {
  auto wpool = std::make_shared<SharedWorkerPool>("stridedmem:3200:64");
  TokenBucket *tb = new TokenBucket(1000);
  auto rc = std::make_shared<RateController>(std::shared_ptr<TokenBucket>(tb));
  auto context_by_id = std::make_shared<HashMap<uint64_t, RequestContext*>>();

  // Load sharded data
  std::cout << "Loading Data..." << std::endl;
  arma::mat shard_data;
  data::Load("rating.csv", shard_data);

  auto start = steady_clock::now();
  CFType<> *cf = new CFType<>(shard_data);
  auto finish = steady_clock::now();
  std::cout << "Data Loaded. Training time = "
    << duration_cast<duration<double>>(finish - start).count() << " s" << std::endl;
  std::cout << "Starting server..." << std::endl;

  rt::Thread([] { UptimeServer(); }).Detach();

  std::unique_ptr<rt::TcpQueue> q(
      rt::TcpQueue::Listen({0, kServerPort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { ServerWorker(std::shared_ptr<rt::TcpConn>(c), wpool, rc, cf, context_by_id); }).Detach();
  }
}

int main(int argc, char *argv[]) {
  int ret;

  if (argc < 2) {
    std::cerr << "usage: [cfg_file]" << std::endl;
    return -EINVAL;
  }

  ret = runtime_init(argv[1], ServerHandler, NULL);
  if (ret) {
    std::cerr << "Failed to start runtime" << std::endl;
    return ret;
  }

  return 0;
}
