#include <string.h>
#include <stdio.h>

#include "base/log.h"
#include "runtime/storage.h"
#include "runtime/tcp.h"
#include "runtime/thread.h"

#include "reflex.h"

#define STORAGE_SERVICE_PORT 5000
#define SECTOR_SIZE 512

static void server_worker(void *arg) {
    tcpconn_t *c = (tcpconn_t *)arg;
    ssize_t ret;

    binary_header_blk_t header;
    char *buf = storage_zmalloc(SECTOR_SIZE);
    if (buf == NULL) {
        log_err("storage_zmalloc: out of memory!");
        return;
    }

    struct iovec response[2];
    response[0].iov_base = &header;
    response[0].iov_len = sizeof(header);
    response[1].iov_base = buf;
    response[1].iov_len = SECTOR_SIZE;

    while (true) {
        ret = tcp_read(c, &header, sizeof(header));
        if (ret <= 0 || ret < sizeof(header)) {
            break;
        }

        BUG_ON(header.magic != sizeof(binary_header_blk_t));
        BUG_ON((header.opcode != CMD_GET) && (header.opcode != CMD_SET));

        // currently only handling workloads where lba_count = 1
        BUG_ON(header.lba_count != 1);

        uint32_t len = header.lba_count * SECTOR_SIZE;
        if (header.opcode == CMD_GET) {
            if (storage_read(buf, header.lba, header.lba_count)) {
                log_err("storage_read failed");
                break;
            }
            ret = tcp_writev(c, response, 2);
            if (ret < 0) {
                log_err("tcp_writev failed");
                break;
            }
        }
        if (header.opcode == CMD_SET) {
            uint32_t read = 0;
            do {
                ret = tcp_read(c, buf+read, len-read);
                read += ret;
            } while(ret > 0 && len > read);
            if (ret < 0) {
                log_err("tcp_read failed");
            }

            if (storage_write(buf, header.lba, header.lba_count)) {
                log_err("storage_write failed");
                break;
            }
            ret = tcp_write(c, &header, sizeof(header));
            if (ret < 0) {
                log_err("tcp_write failed");
                break;
            }
        }
    }
    storage_free(buf);
    tcp_close(c);
}

static void main_handler(void *arg)
{
    struct netaddr addr;
    tcpqueue_t *q;
    int ret;

    addr.ip = 0;
    addr.port = STORAGE_SERVICE_PORT;

    ret = tcp_listen(addr, 4096, &q);
    BUG_ON(ret);

    while (true) {
        tcpconn_t *c;

        ret = tcp_accept(q, &c);
        BUG_ON(ret);
        ret = thread_spawn(server_worker, c);
        BUG_ON(ret);
    }
}

int main(int argc, char *argv[])
{
    int ret;
    ret = runtime_init(argv[1], main_handler, NULL);
    if (ret) {
        printf("failed to start runtime\n");
        return ret;
    }

    return 0;
}
