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
#include <string>
#include <utility>
#include <vector>

namespace {
// The number of leaf servers
int num_leafs;
// Addresses to the leaf servers
std::vector<netaddr> laddrs;

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
      rt::ScopedLock<rt::Mutex> lk(bucket_lock_);
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
      rt::ScopedLock<rt::Mutex> lk(bucket_lock_);
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
      rt::ScopedLock<rt::Mutex> lk(bucket_lock_);
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
      rt::ScopedLock<rt::Mutex> lk(bucket_lock_);
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

// Per-bucket HashMap
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

void FanoutHandler(void *arg) {

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
