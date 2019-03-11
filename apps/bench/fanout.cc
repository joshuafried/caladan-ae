
extern "C" {
  #include <base/bitmap.h>
  #include <base/random.h>
#include <base/log.h>
#include <net/ip.h>
}
#undef min
#undef max

#include "thread.h"
#include "sync.h"
#include "timer.h"
#include "net.h"
#include "fake_worker.h"

#include <iostream>
#include <iomanip>
#include <utility>
#include <memory>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <random>


#define WGMAGIC 0x12345678

extern __thread unsigned int thread_id;

struct payload {
  uint64_t iters;
  uint64_t index;
};
BUILD_ASSERT(sizeof(struct payload) == 16);


netaddr listen_addr;
int connsPerLeaf = 2;
int fanOutRounds = 1;
int affinity = 0;
double meaniters = 0.0;

std::vector<netaddr> leafs;
static std::vector<std::vector<std::vector<rt::TcpConn *>>> all_conns;

static int StringToAddr(const char *str, netaddr *addr) {
  uint8_t a, b, c, d;
  uint16_t p;

  if(sscanf(str, "%hhu.%hhu.%hhu.%hhu:%hu", &a, &b, &c, &d, &p) != 5)
    return -EINVAL;

  addr->ip = MAKE_IP_ADDR(a, b, c, d);
  addr->port = p;
  return 0;
}

rt::TcpConn *dial_affinity(uint32_t match, netaddr remote)
{
  uint16_t base_port, start_port;
  base_port = start_port = rand() ^ rdtsc();
  uint32_t hash;

  while (true) {
    do {
      hash = compute_rss_hash(++base_port, remote) % get_maxks();
      BUG_ON(start_port == base_port);
    } while (hash != match);
    auto t = rt::TcpConn::Dial({0, base_port}, remote);
    if (t != nullptr)
      return t;
  }
  BUG();
  return nullptr;
}



template <class Service>
void ServerWorkerN(std::unique_ptr<rt::TcpConn> c, Service s)
{
  payload p_in, p_out, p_leaf;
  uint32_t connection_hash = compute_rss_hash(listen_addr.port, c->RemoteAddr()) % get_maxks();

  std::random_device rd;
  std::mt19937 g(rd());

  std::vector<std::unique_ptr<rt::TcpConn>> conns;
  for (int i = 0; i < connsPerLeaf; i++) {
    for (auto &w: leafs) {
      if (affinity)
        conns.emplace_back(dial_affinity(connection_hash, w));
      else
        conns.emplace_back(rt::TcpConn::Dial({0, 0}, w));
    }
  }

  while (true) {
    /* read in a request */
    ssize_t ret = c->ReadFull(&p_in, sizeof(p_in));
    if (ret != sizeof(p_in)) {
      if (ret == 0 || ret == -ECONNRESET) return;
      panic("ret1");
    }

    for (int i = 0; i < fanOutRounds; i++) {
      for (auto &w: conns) {
        p_out.iters = hton64(s(g));
        ssize_t sret = w->WriteFull(&p_out, sizeof(p_out));
          if (sret != sizeof(p_out))  {
            if (sret == -EPIPE || sret == -ECONNRESET) return;
            panic("sret");
          }
      }
      for (auto &w: conns) {
        ssize_t rret = w->ReadFull(&p_leaf, sizeof(p_leaf));
        if (rret != sizeof(p_leaf))  {
          if (rret == 0 || rret == -ECONNRESET) return;
          panic("rret");
        }
      }
    }

    ssize_t s2ret = c->WriteFull(&p_in, sizeof(p_in));
    if (s2ret != sizeof(p_in))  {
      if (s2ret == -EPIPE || s2ret == -ECONNRESET) return;
      panic("s2ret");
    }
  }
}

#if 0
class WG {
public:
  uint32_t wg_magic;
  rt::WaitGroup wg;
  WG(int cnt) : wg_magic(WGMAGIC), wg(cnt) {};
};

static void leaf_response_handler(rt::TcpConn *c)
{
  struct payload P;
  while (true) {
    ssize_t ret = c->ReadFull(&P, sizeof(P));
    BUG_ON(ret != sizeof(P));

    WG *w = (WG *)P.index;
    BUG_ON(w->wg_magic != WGMAGIC);
    w->wg.Done();
  }
}


template <class Service>
void ServerWorkerPC(std::unique_ptr<rt::TcpConn> c, Service s)
{

  ssize_t fanout_size = leafs.size() * connsPerLeaf;

  WG fanout_complete(fanout_size);

  uint32_t hash = compute_rss_hash(listen_addr.port, c->RemoteAddr()) % get_maxks();

  while (true) {
    /* read in a request */
    char req[16];

    ssize_t ret = c->ReadFull(req, 16);
    if (ret != 16) {
      if (ret == 0 || ret == -ECONNRESET) break;
      panic("ret1");
    }

    for (int i = 0; i < fanOutRounds; i++) {
      for (unsigned int i = 0, ii = leafs.size() * connsPerLeaf; i < ii; i++) {
          struct payload P;
          P.index = (uint64_t)&fanout_complete;
          P.iters = 0;
          ssize_t sret = all_conns[thread_id][hash][i]->WriteFull(&P, sizeof(P));
          if (sret != 16)  {
            if (sret == -EPIPE || sret == -ECONNRESET) return;
            panic("sret");
          }
      }
      fanout_complete.wg.Wait();
      fanout_complete.wg.Add(fanout_size);
    }
    ssize_t s2ret = c->WriteFull(req, 16);
    if (s2ret != 16)  {
      if (s2ret == -EPIPE || s2ret == -ECONNRESET) break;
      panic("s2ret");
    }
  }
}

void other() {

  for (int i = 0; i < get_maxks(); i++) {
    all_conns.emplace_back();
    for (int j = 0; j < get_maxks(); j++) {
      all_conns[i].emplace_back();
    }
  }

  for (auto &w: leafs) {
    for (int whatever = 0; whatever < connsPerLeaf; whatever++) {
      for (int j = 0; j < get_maxks(); j++) {
        for (int i = 0; i < get_maxks(); i++) {
          rt::TcpConn *outc = dial_affinity(i, w);
          BUG_ON(!outc);
          all_conns[j][i].push_back(outc);
          rt::Thread([=] { leaf_response_handler(outc); }).Detach();
        }
      }
    }
  }
}

void ServerHandlerPC(void *arg) {
  // for (int i = 0, j = get_maxks(); i < j; i++) {
  //   std::vector<rt::TcpConn *> vec;
  //   for (int j = 0; j < connsPerLeaf; j++) {
  //     for (auto &w : leafs) {
  //       rt::TcpConn *outc = rt::TcpConn::Dial({0, 0}, w);
  //       vec.push_back(outc);
  //       rt::Thread([=] { leaf_response_handler(outc); }).Detach();
  //     }
  //   }
  //   all_conns.push_back(vec);
  // }

  other();

  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen(listen_addr,
                   4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");

    if (meaniters > 0.0) {
      rt::Thread([=]{ServerWorkerPC(std::unique_ptr<rt::TcpConn>(c), std::exponential_distribution<double>(meaniters)); }).Detach();
    } else {
      rt::Thread([=]{ServerWorkerPC(std::unique_ptr<rt::TcpConn>(c), std::uniform_int_distribution<int>(0, 0)); }).Detach();
    }
  }
}
#endif

void ServerHandlerN(void *arg) {
  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen(listen_addr,
						       4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");

    if (meaniters > 0.0) {
      rt::Thread([=]{ServerWorkerN(std::unique_ptr<rt::TcpConn>(c), std::exponential_distribution<double>(meaniters)); }).Detach();
    } else {
      rt::Thread([=]{ServerWorkerN(std::unique_ptr<rt::TcpConn>(c), std::uniform_int_distribution<int>(0, 0)); }).Detach();
    }
  }
}

int main(int argc, char *argv[]) {
  if (argc < 7) {
    fprintf(stderr, "usage: ./fanout [cfg] [listen_addr:listen_port] [mode (N|N+)] [connections per leaf] [fanout rounds] [service iters] [leaf addresses...]\n");
    return -EINVAL;
  }

  if (StringToAddr(argv[2], &listen_addr)) {
      printf("failed to parse addr %s\n", argv[2]);
      return -EINVAL;
  }

  std::string type(argv[3]);
  connsPerLeaf = atoi(argv[4]);
  fanOutRounds = atoi(argv[5]);
  meaniters = atoi(argv[6]);

  for (int i = 7; i < argc; i++) {
    netaddr r;
    if (StringToAddr(argv[i], &r)) {
      printf("failed to parse addr %s\n", argv[i]);
      return -EINVAL;
    }
    leafs.push_back(r);
  }

  int ret = 1;
  if (type == "N+") {
    affinity = 1;
    ret = runtime_init(argv[1], ServerHandlerN, NULL);
  }
  else if (type == "N") {
    ret = runtime_init(argv[1], ServerHandlerN, NULL);
  }
#if 0
   else {
    ret = runtime_init(argv[1], ServerHandlerPC, NULL);
  }
#endif
  if (ret)
    printf("failed to start runtime\n");

  return ret;

}
