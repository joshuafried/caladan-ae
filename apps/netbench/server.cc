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
  int32_t standing_queue_len;
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

void HandleRequest(RequestContext *ctx) {
  std::unique_ptr<SyntheticWorker> w(
      SyntheticWorkerFactory("stridedmem:3200:64"));
  if (w == nullptr) panic("couldn't create worker");

  payload *p = &ctx->p;
  
  // perform fake work
  uint64_t workn = ntoh64(p->work_iterations);
  if (workn != 0) w->Work(workn);
  p->tsc_end = hton64(rdtscp(&p->cpu));
  p->cpu = hton32(p->cpu);
  p->standing_queue_len = hton32(rt::RuntimeStandingQueueLen());

  ssize_t ret = ctx->conn->WriteFull(&ctx->p, sizeof(ctx->p));
  if (ret != static_cast<ssize_t>(sizeof(ctx->p))) {
    if (ret != -EPIPE && ret != -ECONNRESET) log_err("tcp_write failed");
  }
}

void ServerWorker(std::shared_ptr<rt::TcpConn> c) {
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
      HandleRequest(ctx);
      delete ctx;
    }).Detach();
    rt::Yield();
    ctx = new RequestContext(resp);
  }
}

void ServerHandler(void *arg) {
  rt::Thread([] { UptimeServer(); }).Detach();

  std::unique_ptr<rt::TcpQueue> q(
      rt::TcpQueue::Listen({0, kServerPort}, 4096));
  if (q == nullptr) panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr) panic("couldn't accept a connection");
    rt::Thread([=] { ServerWorker(std::shared_ptr<rt::TcpConn>(c)); }).Detach();
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
