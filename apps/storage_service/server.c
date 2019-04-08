#include <string.h>
#include <stdio.h>

#include "base/log.h"
#include "runtime/storage.h"
#include "runtime/tcp.h"
#include "runtime/thread.h"

#include "storage_service.h"

static void server_worker(void *arg)
{
    tcpconn_t *c = (tcpconn_t *)arg;
    struct storage_cmd cmd;
    ssize_t ret;

    while (true) {
        ret = tcp_read(c, &cmd, sizeof(cmd));
        log_info("received: %ld bytes", ret);
        if (ret <= 0 || ret < sizeof(cmd)) {
            break;
        }

        log_info("received command");
        BUG_ON((cmd.cmd != CMD_READ) && (cmd.cmd != CMD_WRITE));
        char *buf = storage_zmalloc(BUF_SIZE);
        if (cmd.cmd == CMD_READ) {
            log_info("read");
            storage_read(buf, cmd.lba, cmd.lba_count);
            tcp_write(c, buf, BUF_SIZE);
        }
        if (cmd.cmd == CMD_WRITE) {
            log_info("write: %s", cmd.data);
            memcpy(buf, cmd.data, BUF_SIZE);
            storage_write(buf, cmd.lba, cmd.lba_count);
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
        log_info("accepted connection");
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
