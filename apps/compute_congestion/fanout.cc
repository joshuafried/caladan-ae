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
#include <string>
#include <utility>
#include <vector>

namespace {
// The number of leaf servers
int num_leafs;
// Addresses to the leaf servers
std::vector<netaddr> laddrs;

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

constexpr int kFanoutSize = 4;
// Upstream Payload
struct payloadu {
  uint64_t work_iterations[kFanoutSize];
  uint64_t index;
  uint64_t tsc_end;
  uint32_t cpu;
  uint64_t queueing_delay;
};

// Downstream Payload
struct payloadd {
  uint64_t work_iterations;
  uint64_t index;
  uint64_t tsc_end;
  uint32_t cpu;
  uint64_t queueing_delay;
};

class FanoutTracker {
public:
  FanoutTracker(int fanout_size, payloadu p) : 
    response_waiting_(fanout_size), p_(p), max_delay_(0) {}

  int ReceiveResponse(uint64_t queueing_delay) {
    rt::ScopedLock<rt::Spin> l(&s_);
    response_waiting_--;
    if (queueing_delay > max_delay_)
      max_delay_ = queueing_delay;
    return response_waiting_;
  }

  payloadu GetPayload() {
    p_.queueing_delay = hton64(rt::RuntimeQueueingDelayUS() + max_delay_);
    return p_;
  }

private:
  // Spin lock for response_waiting_
  rt::Spin s_;
  // The number of downstream responses waiting for
  int response_waiting_;
  // Upstream response payload
  payloadu p_;
  // Maximum queueing delay of the responses
  uint64_t max_delay_;
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

void FanoutWorker(std::shared_ptr<rt::TcpConn> c) {
  // tracker_by_id
  auto tracker_by_id = std::make_shared<HashMap<uint64_t, FanoutTracker*>>();

  // downstream index
  uint64_t next_index = 0;

  // Connection to the parent node
  auto parent = std::make_shared<SharedTcpStream>(c);

  // Connection to the leaf nodes
  std::vector<std::unique_ptr<rt::TcpConn>> children;
  children.reserve(num_leafs);

  // Receiver threads
  std::vector<rt::Thread> th; 

  // Dial to the leaf nodes
  for (int i = 0; i < num_leafs; ++i) {
    std::unique_ptr<rt::TcpConn> leafc(rt::TcpConn::Dial({0, 0}, laddrs[i]));
    if (unlikely(leafc == nullptr)) panic("couldn't connect to leaf node.");
    children.emplace_back(std::move(leafc));
  }

  // Start the receiver thread for leaf nodes
  for (int i = 0; i < num_leafs; ++i) {
    th.emplace_back(rt::Thread([&, i] {
      payloadd rp;

      while (true) {
        ssize_t ret = children[i]->ReadFull(&rp, sizeof(rp));
        if (ret != static_cast<ssize_t>(sizeof(rp))) {
          if (ret == 0 || ret < 0) break;
          panic("read failed, ret = %ld", ret);
        }

        uint64_t index = ntoh64(rp.index);
        uint64_t queueing_delay = ntoh64(rp.queueing_delay);
        int remaining_response;
        FanoutTracker* ft = nullptr;

        tracker_by_id->find(index, ft);
        assert(ft);
        remaining_response = ft->ReceiveResponse(queueing_delay);

        if (remaining_response == 0) {
          payloadu up = ft->GetPayload();
          tracker_by_id->erase(index);
          delete ft;

          ssize_t sret = parent->WriteFull(&up, sizeof(up));
          if (sret != static_cast<ssize_t>(sizeof(up))) {
            if (sret == -EPIPE || sret == -ECONNRESET) break;
            log_err("write failed, ret = %ld", sret);
            break;
          }
        }
      }
    }));
  }

  payloadu p;
  
  while (true) {
    ssize_t ret = c->ReadFull(&p, sizeof(p));
    if (ret != static_cast<ssize_t>(sizeof(p))) {
      if (ret == 0 || ret < 0) break;
      panic("read failed, ret = %ld", ret);
    }

    // Create tracker and assign tracker_by_id;
    FanoutTracker *ft = new FanoutTracker(kFanoutSize, p);

    // Fanout to the leafnodes
    for (int i = 0; i < kFanoutSize; ++i) {
      tracker_by_id->insert(next_index, ft);
      payloadd pd;
      pd.work_iterations = p.work_iterations[i];
      pd.index = hton64(next_index);

      // Send downstream request
      ssize_t sret = children[i]->WriteFull(&pd, sizeof(pd));
      if (sret != static_cast<ssize_t>(sizeof(pd))) {
        if (sret == -EPIPE || sret == -ECONNRESET) break;
        log_err("write failed, ret = %ld", sret);
        break;
      }

      next_index++;
    }
  }

  for (int i = 0; i < num_leafs; ++i)
    children[i]->Shutdown(SHUT_RDWR);
  for (auto &t : th) t.Join();

  // clear tracker_by_id
  tracker_by_id->clear();
}

void FanoutHandler(void *arg) {
  std::unique_ptr<rt::TcpQueue> q(
      rt::TcpQueue::Listen({0, kFanoutPort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { FanoutWorker(std::shared_ptr<rt::TcpConn>(c)); }).Detach();
  }
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
