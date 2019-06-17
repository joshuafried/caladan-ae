
extern "C" {
#include <base/hash.h>
#include <base/log.h>
#include <net/ip.h>
#include <runtime/smalloc.h>
#include <runtime/storage.h>
// #include "../storage_service/reflex.h"
}
#undef min
#undef max

#include "fake_worker.h"
#include "net.h"
#include "sync.h"
#include "thread.h"

#include <memory>
#include <vector>

#include "RpcManager.h"

netaddr listen_addr;
netaddr flash_server_addr;

FakeWorker *worker;

struct Payload {
  uint64_t work_iterations;
  uint64_t index;
};

struct SharedTcpStream {
  SharedTcpStream(std::shared_ptr<rt::TcpConn> c) : conn(c) {}
  std::shared_ptr<rt::TcpConn> conn;
  rt::Mutex lock;
};

class RequestContext {
public:
  RequestContext(std::shared_ptr<SharedTcpStream> _s) : stream(_s){};
  std::shared_ptr<SharedTcpStream> stream;
  ReflexHdr storage_hdr;
  Payload req_hdr;
  void *operator new(size_t size) {
    void *p = smalloc(size);
    if (unlikely(p == nullptr))
      throw std::bad_alloc();
    return p;
  }
  void operator delete(void *p) { sfree(p); }
};

#define REMOTE
#ifdef REMOTE

RpcEndpoint<ReflexHdr> *flash_server;
void setup() {
  flash_server = RpcEndpoint<ReflexHdr>::Create(flash_server_addr);
}
#else
void setup() {}
#endif

static int StringToAddr(const char *str, netaddr *addr) {
  uint8_t a, b, c, d;
  uint16_t p;

  if (sscanf(str, "%hhu.%hhu.%hhu.%hhu:%hu", &a, &b, &c, &d, &p) != 5)
    return -EINVAL;

  addr->ip = MAKE_IP_ADDR(a, b, c, d);
  addr->port = p;
  return 0;
}

void HandleSingleRequest(RequestContext *ctx) {
  unsigned char buf[512];
  unsigned long lba = (unsigned long)rdtsc() % 524280UL;

#ifdef REMOTE
  Rpc<ReflexHdr> r;
  r.req.magic = 24;
  r.req.opcode = 0;
  r.req.lba = lba;
  r.req.lba_count = 1;
  r.req_body = nullptr;
  r.req_body_len = 0;

  r.rsp_body = buf;
  r.rsp_body_len = 512;

  int ret = flash_server->SubmitRequestBlocking(&r);
#else
  int ret = storage_read(buf, lba, 1);
#endif
  BUG_ON(ret);

  uint32_t h = jenkins_hash(buf, 512);
  std::ignore = h;

  uint64_t iterations = ntoh64(ctx->req_hdr.work_iterations);
  worker->Work(iterations);

  rt::ScopedLock<rt::Mutex> lck(&ctx->stream->lock);

  ssize_t sret =
      ctx->stream->conn->WriteFull(&ctx->req_hdr, sizeof(ctx->req_hdr));
  if (sret != sizeof(ctx->req_hdr)) {
    if (sret == -EPIPE || sret == -ECONNRESET)
      return;
    BUG();
  }
}

void ClientHandler(std::shared_ptr<rt::TcpConn> c) {

  auto stream = std::shared_ptr<SharedTcpStream>(new SharedTcpStream(c));

  while (true) {

    auto ctx = new RequestContext(stream);

    ssize_t rret = c->ReadFull(&ctx->req_hdr, sizeof(ctx->req_hdr));
    if (rret != static_cast<ssize_t>(sizeof(ctx->req_hdr))) {
      if (rret == 0 || rret == -ECONNRESET)
        return;
      BUG();
    }

    rt::Thread([=] {
      HandleSingleRequest(ctx);
      delete ctx;
    })
        .Detach();
  }
}

void ServerHandler(void *arg) {
  setup();

  std::unique_ptr<rt::TcpQueue> q(rt::TcpQueue::Listen(listen_addr, 4096));
  if (q == nullptr)
    panic("couldn't listen for connections");

  while (true) {
    rt::TcpConn *c = q->Accept();
    if (c == nullptr)
      panic("couldn't accept a connection");
    rt::Thread([=] { ClientHandler(std::shared_ptr<rt::TcpConn>(c)); })
        .Detach();
  }
}

int main(int argc, char *argv[]) {
#ifdef REMOTE
  if (argc < 4) {
#else
  if (argc < 3) {
#endif
    fprintf(stderr, "usage: ./flash_client [cfg] [listen_addr:listen_port] "
                    "<storageserveraddr>\n");
    return -EINVAL;
  }

  if (StringToAddr(argv[2], &listen_addr)) {
    printf("failed to parse addr %s\n", argv[2]);
    return -EINVAL;
  }

#ifdef REMOTE
  if (StringToAddr(argv[3], &flash_server_addr)) {
    printf("failed to parse addr %s\n", argv[3]);
    return -EINVAL;
  }
#endif

  worker = FakeWorkerFactory("stridedmem:1024:7");
  if (!worker)
    return -EINVAL;

  int ret = runtime_init(argv[1], ServerHandler, NULL);
  if (ret)
    printf("failed to start runtime\n");

  return ret;
}
