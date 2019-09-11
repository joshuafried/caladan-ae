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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>

/**
 *
 *
 * Collaborative Filltering server with health check and AQM
 *
 *
 */

namespace {

using namespace std::chrono;
using sec = duration<double, std::micro>;

static std::vector<uint32_t> service_time_dist;
static uint64_t MAX_SERVICE_TIME_IDX;

// A prime number as hash size gives a better distribution of values in buckets
constexpr uint64_t HASH_SIZE_DEFAULT = 10009;
constexpr uint64_t kIterationsPerUS = 88;
constexpr uint64_t kAQMThresh = 20000; // 10ms: according to WeChat

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

class AdmissionController {
public:
  AdmissionController(): sum_qdel_(0), sample_qdel_(0), num_pending_reqs_(0), window_(10.0) {
    last_update_ = steady_clock::now();
  }

  void UpdateWindow(time_point<steady_clock> now) {
    double avg_qdel = static_cast<double>(sum_qdel_) / static_cast<double>(sample_qdel_);
    double new_window = window_;

    // If average queueing delay is more than 20ms
    if (avg_qdel >= 20000.0) {
      // congested! decrease the window by 5%
      new_window = 0.95 * window_;
    } else {
      // not congested! increase the window by 1%
      new_window = 1.01 * window_;
    }

    if (new_window < 10.0)
      new_window = 10.0;
    
    s_.Lock();
    sum_qdel_ = 0;
    sample_qdel_ = 0;
    last_update_ = now;
    window_ = new_window;
    s_.Unlock();
  }

  void ReportQueueingDelay(uint64_t delay) {
    uint64_t num_sample;
    s_.Lock();
    sum_qdel_ += delay;
    num_sample = ++sample_qdel_;
    s_.Unlock();

    barrier();
    auto now = steady_clock::now();
    barrier();

    uint64_t elapsed_time_us = duration_cast<microseconds>(now - last_update_).count();
    if (num_sample >= 2000 || elapsed_time_us >= 1000000) {
      UpdateWindow(now);
    }
  }

  void SentResponse() {
    s_.Lock();
    num_pending_reqs_--;
    s_.Unlock();
  }

  bool GetAdmission() {
    bool admission = (static_cast<double>(num_pending_reqs_ + 1) <= window_);
    if (admission) {
      s_.Lock();
      num_pending_reqs_++;
      s_.Unlock();
    }
    
    return admission;
  }

private:
  uint64_t sum_qdel_;
  uint64_t sample_qdel_;
  uint64_t num_pending_reqs_;
  double window_;
  rt::Spin s_;
  time_point<steady_clock> last_update_;
};

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
  time_point<steady_clock> start_time;
  
  void *operator new(size_t size) {
    void *p = smalloc(size);
    if (unlikely(p == nullptr)) throw std::bad_alloc();
    return p;
  }
  void operator delete(void *p) { sfree(p); }
};

void HandleRequest(RequestContext *ctx,
                   std::shared_ptr<SharedWorkerPool> wpool,
                   std::shared_ptr<AdmissionController> monitor) {
  auto w = wpool->GetWorker(rt::RuntimeKthreadIdx());
  payload *p = &ctx->p;

  // If the request is canceled, just ignore the request
  if (ctx->timed_out)
    return;

  auto now = steady_clock::now();

  uint64_t qdel = duration_cast<microseconds>(now - ctx->start_time).count();

  monitor->ReportQueueingDelay(qdel);

  // perform fake work
  auto start = steady_clock::now();
  uint64_t uid = ntoh64(p->user_id);
  uint64_t mid = ntoh64(p->movie_id);
  // Do fake work
  w->Work(service_time_dist[rand()%MAX_SERVICE_TIME_IDX] * kIterationsPerUS);
  double rating = 1.0;
  barrier();
  auto finish = steady_clock::now();
  barrier();
  
  uint64_t processing_time = std::max<uint64_t>(duration_cast<microseconds>(finish - start).count(), 1);

  p->queueing_delay = hton64(rt::RuntimeQueueingDelayUS());
  p->processing_time = hton64(processing_time);
  p->rating = htonf(rating);

  ssize_t ret = ctx->conn->WriteFull(&ctx->p, sizeof(ctx->p));
  if (ret != static_cast<ssize_t>(sizeof(ctx->p))) {
    if (ret != -EPIPE && ret != -ECONNRESET) log_err("tcp_write failed");
  }
}

void ServerWorker(std::shared_ptr<rt::TcpConn> c, std::shared_ptr<SharedWorkerPool> wpool,
                  std::shared_ptr<RateController> rc,
                  std::shared_ptr<HashMap<uint64_t, RequestContext *>> context_by_id,
                  std::shared_ptr<AdmissionController> monitor) {
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

    uint64_t movie_id = ntoh64(p->movie_id);
    uint64_t user_id = ntoh64(p->user_id);
    uint64_t index = ntoh64(p->request_index);

    if (user_id == 0 && movie_id == 0) {
      // mark context as timeout
      RequestContext *to_ctx;
      if (context_by_id->find(index, to_ctx)) {
        to_ctx->timed_out = true;
      }
      continue;
    }

    // AQM Logic
    if (!monitor->GetAdmission()) {
      continue;
    }
/*
    // AQM Logic
    if (monitor->GetQueueingDelay() > kAQMThresh && monitor->GetQLen() > 0 && movie_id < 2) {
      p->processing_time = hton64(0);
      p->movie_id = hton64(ntoh64(p->movie_id) + 1);

      // return the request.
      ssize_t ret = ctx->conn->WriteFull(p, sizeof(*p));
      if (ret != static_cast<ssize_t>(sizeof(*p))) {
        if (ret != -EPIPE && ret != -ECONNRESET) log_err("tcp_write failed");
      }
      
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

    context_by_id->insert(index, ctx);

    ctx->start_time = steady_clock::now();

    rt::Thread([=] {
      HandleRequest(ctx, wpool, monitor);
      context_by_id->erase(index);
      monitor->SentResponse();
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

  auto monitor = std::make_shared<AdmissionController>();

  std::ifstream stdfile("search.dist");
  uint32_t stu;
  while (stdfile >> stu)
    service_time_dist.push_back(stu);

  MAX_SERVICE_TIME_IDX = service_time_dist.size();
  srand(time(NULL));
  std::cout << "Starting server..." << std::endl;

//  rt::Thread([=] { HealthCheckServer(qdm); }).Detach();

  std::unique_ptr<rt::TcpQueue> q(
      rt::TcpQueue::Listen({0, kServerPort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { ServerWorker(std::shared_ptr<rt::TcpConn>(c), wpool, rc, context_by_id, monitor); }).Detach();
  }
}
} // anonymous namespace

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
