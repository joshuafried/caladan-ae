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

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <utility>
#include <chrono>

using namespace std::chrono;

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

constexpr uint64_t kUptimePort = 8002;
constexpr uint64_t kUptimeMagic = 0xDEADBEEF;
struct uptime {
  uint64_t idle;
  uint64_t busy;
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
  uint64_t work_iterations;
  uint64_t index;
  uint64_t tsc_end;
  uint32_t cpu;
  uint64_t queueing_delay;
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
  RequestContext(std::shared_ptr<SharedTcpStream> c) : conn(c) {}
  payload p;
  std::shared_ptr<SharedTcpStream> conn;
  void *operator new(size_t size) {
    void *p = smalloc(size);
    if (unlikely(p == nullptr)) throw std::bad_alloc();
    return p;
  }
  void operator delete(void *p) { sfree(p); }
};

void HandleRequest(RequestContext *ctx,
                   std::shared_ptr<SharedWorkerPool> wpool) {
  auto w = wpool->GetWorker(rt::RuntimeKthreadIdx());
  payload *p = &ctx->p;

  // perform fake work
  uint64_t workn = ntoh64(p->work_iterations);
  if (workn != 0) w->Work(workn);
  p->tsc_end = hton64(rdtscp(&p->cpu));
  p->cpu = hton32(p->cpu);
  p->queueing_delay = hton64(rt::RuntimeQueueingDelayUS());

  ssize_t ret = ctx->conn->WriteFull(&ctx->p, sizeof(ctx->p));
  if (ret != static_cast<ssize_t>(sizeof(ctx->p))) {
    if (ret != -EPIPE && ret != -ECONNRESET) log_err("tcp_write failed");
  }
}

void ServerWorker(std::shared_ptr<rt::TcpConn> c, std::shared_ptr<SharedWorkerPool> wpool) {
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

    rt::Thread([=] {
      HandleRequest(ctx, wpool);
      delete ctx;
    }).Detach();
    ctx = new RequestContext(resp);
  }
}

void ServerHandler(void *arg) {
  rt::Thread([] { UptimeServer(); }).Detach();

  std::unique_ptr<rt::TcpQueue> q(
      rt::TcpQueue::Listen({0, kServerPort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  auto wpool = std::make_shared<SharedWorkerPool>("stridedmem:3200:64");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { ServerWorker(std::shared_ptr<rt::TcpConn>(c), wpool); }).Detach();
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
