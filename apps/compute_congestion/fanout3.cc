extern "C" {
#include <base/log.h>
#include <net/ip.h>
#include <unistd.h>
#include <runtime/smalloc.h>
}

#include "net.h"
#include "runtime.h"
#include "sync.h"
#include "thread.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <functional>
#include <iostream>
#include <memory>
#include <queue>
#include <string>
#include <utility>
#include <vector>

/*
 *
 * Fanout Implementation with FanoutManager
 * for proxy-based circuit breaker
 *
 */

namespace {

using namespace std::chrono;
using sec = duration<double, std::micro>;
// The number of leaf servers
int num_leafs;
// Addresses to the leaf servers
std::vector<netaddr> laddrs;

constexpr uint64_t kSLOUS = 100000; // 100ms

// Port number of the Fanout node
constexpr uint64_t kFanoutPort = 8001;
// Port number of the leaf node server
constexpr uint64_t kLeafPort = 8001;
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

class StatMonitor {
public:
  StatMonitor(uint32_t max_nif) : running_(true), max_nif_(max_nif), nif_(0) {}

  bool RequestSend() {
    m_.Lock();
    while (running_ && nif_ >= max_nif_) {
      cv_.Wait(&m_);
    }
    nif_++;
    m_.Unlock();

    return running_;
  }

  void CancelSend() {
    m_.Lock();
    nif_--;
    m_.Unlock();
  }

  void RecvResponse() {
    m_.Lock();
    nif_--;
    cv_.Signal();
    m_.Unlock();
  }

  void Finish() {
    m_.Lock();
    running_ = false;
    cv_.SignalAll();
    m_.Unlock();
  }

private:
  // the number of in-flight requests
  bool running_;
  uint32_t max_nif_;
  uint32_t nif_;
  rt::Mutex m_;
  rt::CondVar cv_;
};

constexpr int kFanoutSize = 4;
// Upstream Payload
struct payloadu {
  uint64_t work_iterations[kFanoutSize];
  uint64_t index;
  uint64_t tsc_end;
  uint32_t cpu;
  uint64_t queueing_delay;
  uint64_t processing_time;
};

// Downstream Payload
struct payloadd {
  uint64_t work_iterations;
  uint64_t index;
  uint64_t tsc_end;
  uint32_t cpu;
  uint64_t queueing_delay;
  uint64_t processing_time;
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

class FanoutTracker {
public:
  FanoutTracker(std::shared_ptr<SharedTcpStream> c) : 
    response_waiting_(kFanoutSize), max_delay_(0),
    sum_processing_time_(0), conn_(c) {}

  int ReceiveResponse(uint64_t queueing_delay, uint64_t processing_time) {
    int rw;

    s_.Lock();
    response_waiting_--;
    if (queueing_delay > max_delay_)
      max_delay_ = queueing_delay;
    sum_processing_time_ += processing_time;
    rw = response_waiting_;
    s_.Unlock();

    // send response back to the upstream
    if (rw == 0) {
      p.queueing_delay = hton64(rt::RuntimeQueueingDelayUS() + max_delay_);
      p.processing_time = hton64(static_cast<uint64_t>(sum_processing_time_ / kFanoutSize));

      ssize_t ret = conn_->WriteFull(&p, sizeof(p));
      if (ret != static_cast<ssize_t>(sizeof(p))) {
        if (ret != -EPIPE && ret != -ECONNRESET) log_err("upstream tcp_write failed");
      }
    }

    return rw;
  }

  // Upstream response payload
  payloadu p;

  time_point<steady_clock> start_time;
  time_point<steady_clock> timings[kFanoutSize];

private:
  // Spin lock for response_waiting_
  rt::Spin s_;
  // The number of downstream responses waiting for
  int response_waiting_;
  // Maximum queueing delay of the responses
  uint64_t max_delay_;
  // sum of processing time
  uint64_t sum_processing_time_;
  // upstream connection
  std::shared_ptr<SharedTcpStream> conn_;
};

class ChildQueue {
public:
  ChildQueue(): next_index_(0) {
    tracker_by_id = new HashMap<uint64_t, FanoutTracker*>();
  }

  void EnqueueRequest(uint64_t work_iterations, FanoutTracker* ft) {
    uint64_t idx;

    m_.Lock();
    idx = next_index_++;

    payloadd pd;
    pd.work_iterations = hton64(work_iterations);
    pd.index = hton64(idx);
    q_.push(pd);
    cv_.Signal();
    m_.Unlock();

    tracker_by_id->insert(idx, ft);
  }

  payloadd DequeRequest() {
    m_.Lock();
    payloadd ret;
    while (q_.empty()){
      cv_.Wait(&m_);
    }

    ret = q_.front();
    q_.pop();
    m_.Unlock();

    return ret;
  }

  FanoutTracker* GetTrackerByIdx(uint64_t index) {
    FanoutTracker* ret;
    if (tracker_by_id->find(index, ret)) {
      return ret;
    }

    return nullptr;
  }

  void EraseTrackerByIdx(uint64_t index) {
    tracker_by_id->erase(index);
  }

private:
  HashMap<uint64_t, FanoutTracker *> *tracker_by_id;
  std::queue<payloadd> q_;
  uint64_t next_index_;
  rt::CondVar cv_;
  rt::Mutex m_;
};

class FanoutManager {
public:
  FanoutManager() {
    child_qs_.reserve(kFanoutSize);
  }

  void AddFanoutNode(std::shared_ptr<ChildQueue> cq) {
    child_qs_.push_back(cq);
  }

  std::shared_ptr<ChildQueue> GetChildQueue(unsigned int idx) {
    return child_qs_[idx];
  }

  void FanoutAll(uint64_t *work_iterations, FanoutTracker* ft) {
    for (int i = 0; i < kFanoutSize; ++i) {
      child_qs_[i]->EnqueueRequest(ntoh64(work_iterations[i]), ft);
    }
  }

private:
  std::vector<std::shared_ptr<ChildQueue>> child_qs_;
};

void DownstreamWorker(rt::TcpConn *c, rt::WaitGroup *starter, std::shared_ptr<ChildQueue> cq, int worker_id) {
  StatMonitor *monitor = new StatMonitor(20);

  uint64_t ewma_exe_time = 0;

  // Start receiver thread
  auto th = rt::Thread([&] {
    // downstrean response payload
    payloadd drp;

    while (true) {
      // Read paylod from downstream
      ssize_t ret = c->ReadFull(&drp, sizeof(drp));
      if (ret != static_cast<ssize_t>(sizeof(drp))) {
        if (ret == 0 || ret < 0) break;
        panic("read failed, ret = %ld", ret);
      }

      uint64_t index = ntoh64(drp.index);
      uint64_t queueing_delay = ntoh64(drp.queueing_delay);
      uint64_t processing_time = ntoh64(drp.processing_time);
      
      // find tracker from tracker_by_id
      FanoutTracker* ft = cq->GetTrackerByIdx(index);

      if (ft == nullptr)
        continue;
      cq->EraseTrackerByIdx(index);

      barrier();
      auto now = steady_clock::now();
      barrier();
      double latency_us = duration_cast<sec>(now - ft->timings[worker_id]).count();

      ewma_exe_time = static_cast<uint64_t>(0.8*ewma_exe_time + 0.2*latency_us);

      // tracker->ReceiveResponse
      if (ft->ReceiveResponse(queueing_delay, processing_time) == 0)
        delete ft;

      monitor->RecvResponse();
    }
  });

  starter->Done();
  starter->Wait();

  payloadd pd;
  // Sender Loop
  while (true) {
    // If there is enqueued request, send to c
    pd = cq->DequeRequest();

    uint64_t idx = ntoh64(pd.index);
    FanoutTracker *ft = cq->GetTrackerByIdx(idx);
    barrier();
    auto now = steady_clock::now();
    barrier();

    if (duration_cast<sec>(now - ft->start_time).count() > (kSLOUS - ewma_exe_time - 1000))
      continue;

    monitor->RequestSend();

    barrier();
    now = steady_clock::now();
    barrier();

    if (duration_cast<sec>(now - ft->start_time).count() > (kSLOUS - ewma_exe_time - 1000)) {
      monitor->CancelSend();
      continue;
    }

    ft->timings[worker_id] = now;

    // send to downstream
    ssize_t sret = c->WriteFull(&pd, sizeof(pd));
    if (sret != static_cast<ssize_t>(sizeof(pd))) {
      if (sret == -EPIPE || sret == -ECONNRESET) break;
      log_err("write failed, ret = %ld", sret);
      break;
    }
  }

  c->Shutdown(SHUT_RDWR);
  th.Join();
}

void UpstreamWorker(std::shared_ptr<rt::TcpConn> c, std::shared_ptr<FanoutManager> fm) {
  // upstream connection
  auto uc = std::make_shared<SharedTcpStream>(c);

  // allocate fanout tracker
  auto ft = new FanoutTracker(uc);

  while (true) {
    payloadu *p = &ft->p;

    // Receive upstream request
    ssize_t ret = c->ReadFull(p, sizeof(*p));
    if (ret != static_cast<ssize_t>(sizeof(*p))) {
      if (ret !=0 && ret != -ECONNRESET)
        log_err("read failed, ret = %ld\n", ret);
      delete ft;
      break;
    }
    ft->start_time = steady_clock::now();
    fm->FanoutAll(p->work_iterations, ft);
    
    ft = new FanoutTracker(uc);
  }

}

void UpstreamHandler(std::shared_ptr<FanoutManager> fm) {
  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen({0, kFanoutPort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { UpstreamWorker(std::shared_ptr<rt::TcpConn>(c), fm); }).Detach();
  }
}

void FanoutHandler(void *arg) {
  auto fm = std::make_shared<FanoutManager>();

  for (int i = 0; i < num_leafs; ++i) {
    fm->AddFanoutNode(std::make_shared<ChildQueue>());
  }

  std::vector<std::unique_ptr<rt::TcpConn>> child_conns;
  child_conns.reserve(num_leafs);
  for (int i = 0; i < num_leafs; ++i) {
    std::unique_ptr<rt::TcpConn> outc(rt::TcpConn::Dial({0, 0}, laddrs[i]));
    if (unlikely(outc == nullptr)) panic("couldn't connect to child.");
    child_conns.emplace_back(std::move(outc));
  }

  rt::WaitGroup starter(kFanoutSize + 1);
  std::vector<rt::Thread> th;

  for (int i = 0; i < kFanoutSize; ++i) {
    th.emplace_back(rt::Thread([&, i] {
      DownstreamWorker(child_conns[i].get(), &starter, fm->GetChildQueue(i), i);
    }));
  }

  starter.Done();
  starter.Wait();

  UpstreamHandler(fm);

  for (auto &t : th) t.Join();
  for (auto &c : child_conns) c->Abort();
}

int StringToAddr(const char *str, uint32_t *addr) {
  uint8_t a, b, c, d;

  if (sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) return -EINVAL;

  *addr = MAKE_IP_ADDR(a, b, c, d);
  return 0;
}

} // anonymous namespace

int main(int argc, char *argv[]) {
  int i, ret;

  if (argc < 4) {
    std::cerr << "usage: fanout [cfg_file] [# leaf server] [leaf server IP #1] ..." << std::endl;
    return -EINVAL;
  }

  num_leafs = std::stoi(argv[2], nullptr, 0);
  laddrs.reserve(num_leafs);
  for (i = 0; i < num_leafs; ++i) {
    netaddr laddr;
    ret = StringToAddr(argv[3+i], &laddr.ip);
    if (ret) return -EINVAL;
    laddr.port = kLeafPort;
    laddrs.push_back(laddr);
  }

  ret = runtime_init(argv[1], FanoutHandler, NULL);
  if (ret) {
    std::cerr << "Failed to start runtime" << std::endl;
    return ret;
  }
  return 0;
}
