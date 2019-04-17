#include <string.h>
#include <stdio.h>

#include "base/log.h"
#include "runtime/storage.h"
#include "runtime/tcp.h"
#include "runtime/thread.h"

// #include "storage_service.h"
#include "reflex.h"

#define STORAGE_SERVICE_PORT 5000
#define SECTOR_SIZE 512 

static void server_worker(void *arg)
{
    tcpconn_t *c = (tcpconn_t *)arg;
    binary_header_blk_t header;
    ssize_t ret;

    while (true) {
        ret = tcp_read(c, &header, sizeof(header));
        // log_info("received: %ld bytes", ret);
        if (ret <= 0 || ret < sizeof(header)) {
            break;
        }

        // log_info("received command");
        // log_info("magic: %u, lba: %lu, lba_count: %u", header.magic, header.lba, header.lba_count);

        // BUG_ON(header.magic != sizeof(binary_header_blk_t));
        // BUG_ON((header.opcode != CMD_GET) && (header.opcode != CMD_SET));
        //
        if(header.magic != sizeof(binary_header_blk_t)) {
            // printf("skipped\n");
            // printf("%s\n", (char*)&header);
            continue;
        }
        uint32_t len = header.lba_count * SECTOR_SIZE;
        char *buf = storage_zmalloc(len);
        if (header.opcode == CMD_GET) {
            // log_info("get");
            storage_read(buf, header.lba, header.lba_count);
            tcp_write(c, &header, sizeof(header));
            tcp_write(c, buf, len);
        }
        if (header.opcode == CMD_SET) {
            // log_info("set");
            uint32_t read = 0;
            // log_info("set len: %u", len);
            do {
                ret = tcp_read(c, buf+read, len-read);
                read += ret;
                // log_info("ret: %lu", ret);
            } while(ret > 0 && len > read);
            // log_info("set data: %s", buf);
            storage_write(buf, header.lba, header.lba_count);
            tcp_write(c, &header, sizeof(header));
        }
        storage_free(buf);
    }
}

static void main_handler(void *arg)
{
    struct netaddr addr;
    tcpqueue_t *q;
    int ret;

    addr.ip = 0;
    addr.port = STORAGE_SERVICE_PORT;

    ret = tcp_listen(addr, 4096, &q);
    log_info("listening");
    BUG_ON(ret);

    while (true) {
        tcpconn_t *c;

        ret = tcp_accept(q, &c);
        // log_info("accepted connection");
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
