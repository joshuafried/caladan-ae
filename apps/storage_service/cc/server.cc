extern "C" {
#include <base/log.h>
#include <net/ip.h>
#include <unistd.h>

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
constexpr uint64_t kNumSectorsPerPayload = 1;

void ServerWorker(std::unique_ptr<rt::TcpConn> c, int my_worker_num)
{
    binary_header_blk_t header;

    std::unique_ptr<char[]> buf(new (std::nothrow) char [payload_size]);
    if (buf.get() == nullptr) 
    {
        log_err("error allocating buffer");
        return;
    }

    struct iovec response[2];
    response[0].iov_base = &header;
    response[0].iov_len = sizeof(header);
    response[1].iov_base = buf.get();
    response[1].iov_len = payload_size;

    while (true)
    {
        // Receive a work request.
        ssize_t ret = c->ReadFull(&header, sizeof(header));
        if (ret != static_cast<ssize_t>(sizeof(header)))
        {
            if (ret == 0 || ret == -ECONNRESET)
                break;
            log_err("read failed, ret = %ld", ret);
            break;
        }

        BUG_ON(header.magic != sizeof(binary_header_blk_t));
        BUG_ON((header.opcode != CMD_GET) && (header.opcode != CMD_SET));

        // currently only handling workloads where lba_count = kNumSectorsPerPayload
        BUG_ON(header.lba_count != kNumSectorsPerPayload);

        if (header.opcode == CMD_GET) {
            ret = storage_read(buf.get(), header.lba, header.lba_count);
            if (ret < 0) {
                log_err("storage_read failed");
                break;
            }
            ret = c->Writev(response, 2);
            if (ret != static_cast<ssize_t>(sizeof(header) + payload_size)) {
                log_err("tcp_writev failed");
                break;
            }
        }
        else if (header.opcode == CMD_SET) {
            ret = c->ReadFull(buf.get(), payload_size);
            if (ret != payload_size) {
                log_err("tcp_read failed");
                break;
            }
            ret = storage_write(buf.get(), header.lba, header.lba_count);
            if (ret < 0) {
                log_err("storage_write failed");
                break;
            }
            ret = c->Write(&header, sizeof(header));
            if (ret != static_cast<ssize_t>(sizeof(header))) {
                log_err("tcp_write failed");
                break;
            }
        }
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
        rt::Thread([=] { ServerWorker(std::unique_ptr<rt::TcpConn>(c), my_worker_num); }).Detach();
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
