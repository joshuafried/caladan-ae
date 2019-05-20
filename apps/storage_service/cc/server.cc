extern "C" {
#include <base/log.h>
#include <net/ip.h>
#include <unistd.h>
#include <runtime/smalloc.h>
#include <runtime/storage.h>
}
#undef min
#undef max

#include "net.h"
#include "sync.h"
#include "thread.h"
#include "timer.h"

#include <iostream>
#include <memory>
#include <new>

#include "reflex.h"

static int sector_size;
static int payload_size;
constexpr uint64_t kStorageServicePort = 5000;
constexpr uint64_t kNumSectorsPerPayload = 8;

class SharedTcp;
class RequestContext {
public:
    RequestContext(std::shared_ptr<SharedTcp> conn) : conn_(conn) {}
    binary_header_blk_t header_;
    std::shared_ptr<SharedTcp> conn_;
    char buf_[4096];
    void * operator new(size_t size) {
        return smalloc(size);
    }
    void operator delete(void * p) {
        sfree(p);
    }
};

class SharedTcp {
public:
    SharedTcp(std::shared_ptr<rt::TcpConn> c) : c_(c) {}
    ssize_t Write(const void *buf, size_t len) {
         rt::ScopedLock<rt::Mutex> lock(&sendMutex_);
         return c_->WriteFull(buf, len);
    }
    ssize_t Writev(const iovec *iov, int iovcnt) {
         rt::ScopedLock<rt::Mutex> lock(&sendMutex_);
         return c_->Writev(iov, iovcnt);
    }
private:
    std::shared_ptr<rt::TcpConn> c_;
    rt::Mutex sendMutex_;
};

void HandleRequest(RequestContext *ctx)
{
    ssize_t ret;
    if (ctx->header_.opcode == CMD_GET) {
        ret = storage_read(ctx->buf_, ctx->header_.lba, ctx->header_.lba_count);
        if (ret < 0) {
            log_err("storage_read failed");
            return;
        }
        struct iovec response[2];
        response[0].iov_base = &ctx->header_;
        response[0].iov_len = sizeof(ctx->header_);
        response[1].iov_base = ctx->buf_;
        response[1].iov_len = payload_size;
        
        
        ret = ctx->conn_->Writev(response, 2);
        if (ret != static_cast<ssize_t>(sizeof(ctx->header_) + payload_size)) {
            BUG();
            log_err("tcp_writev failed");
            return;
        }
    }

    else {
        
        ret = storage_write(ctx->buf_, ctx->header_.lba, ctx->header_.lba_count);
        if (ret < 0) {
            log_err("storage_write failed");
            return;
        }
        
        ret = ctx->conn_->Write(&ctx->header_, sizeof(ctx->header_));
        if (ret != static_cast<ssize_t>(sizeof(ctx->header_))) {
            if (ret == -EPIPE || ret == -ECONNRESET) return;
            BUG();
            log_err("tcp_write failed");
            return;
        }       
    }
}


void ServerWorker(std::shared_ptr<rt::TcpConn> c, int my_worker_num)
{

    auto resp = std::make_shared<SharedTcp>(c);

    while (true)
    {
        /* allocate context */
        auto ctx = new RequestContext(resp);

        // Receive a work request.
        ssize_t ret = c->ReadFull(&ctx->header_, sizeof(ctx->header_));
        if (ret != static_cast<ssize_t>(sizeof(ctx->header_)))
        {
            if (ret == 0 || ret == -ECONNRESET)
                break;
            log_err("read failed, ret = %ld", ret);
            break;
        }

        BUG_ON(ctx->header_.magic != sizeof(binary_header_blk_t));
        BUG_ON((ctx->header_.opcode != CMD_GET) && (ctx->header_.opcode != CMD_SET));

        // currently only handling workloads where lba_count = kNumSectorsPerPayload
        BUG_ON(ctx->header_.lba_count != kNumSectorsPerPayload);

        if (ctx->header_.opcode == CMD_SET) {
            ret = c->ReadFull(ctx->buf_, payload_size);
            if (ret != payload_size) {
                log_err("tcp_read failed, ret = %ld", ret);
                break;
            }
        }

        rt::Thread([=] {HandleRequest(ctx); delete ctx; }).Detach(); // /*HandleRequest(ctx); }).Detach();

    }
}

void MainHandler(void *arg) {
    std::unique_ptr<rt::TcpQueue> q(
        rt::TcpQueue::Listen({0, kStorageServicePort}, 4096));
    if (q == nullptr)
        panic("couldn't listen for connections");

    sector_size = storage_block_size();
    payload_size = kNumSectorsPerPayload * sector_size;

    int worker_num = 0;
    while (true)
    {
        rt::TcpConn *c = q->Accept();
        if (c == nullptr)
            panic("couldn't accept a connection");
        int my_worker_num = worker_num++;
        rt::Thread([=] { ServerWorker(std::shared_ptr<rt::TcpConn>(c), my_worker_num); }).Detach();
    }
}

int main(int argc, char *argv[]) {
    int ret;

    if (argc < 2) {
        std::cerr << "usage: [cfg_file]" << std::endl;
        return -EINVAL;
    }

    ret = runtime_init(argv[1], MainHandler, NULL);
    if (ret) {
        printf("failed to start runtime\n");
        return ret;
    }

    return 0;
}
