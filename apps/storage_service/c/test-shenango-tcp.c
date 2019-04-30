#include <string.h>
#include <stdio.h>

#include "base/log.h"
#include "net/ip.h"
#include "runtime/sync.h"
#include "runtime/tcp.h"
#include "runtime/thread.h"

#include "storage_service.h"

static struct netaddr raddr;

static int str_to_ip(const char *str, uint32_t *addr)
{
    uint8_t a, b, c, d;
    if(sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) {
        return -EINVAL;
    }

    *addr = MAKE_IP_ADDR(a, b, c, d);
    return 0;
}

static void client_worker(void *arg)
{
    waitgroup_t *wg = (waitgroup_t *) arg;
    tcpconn_t *c;
    struct netaddr laddr;
    struct storage_cmd cmd;
    ssize_t ret;

    laddr.ip = 0;
    laddr.port = 0;

    ret = tcp_dial(laddr, raddr, &c);
    if (ret) {
        log_err("tcp_dial() failed, ret = %ld", ret);
        goto done;
    }

    log_info("tcp dial");
    cmd.cmd = CMD_WRITE;
    cmd.lba = 0;
    cmd.lba_count = 1;
    strcpy(cmd.data, "Hello Shenango");
    ret = tcp_write(c, (void *)&cmd, sizeof(struct storage_cmd));
    if (ret != sizeof(struct storage_cmd)) {
        log_err("tcp_write() failed, ret = %ld", ret);
        goto done;
    }

    log_info("tcp_write");
    cmd.cmd = CMD_READ;
    char response[BUF_SIZE];
    memset(response, 0, BUF_SIZE);

    ret = tcp_write(c, (void *)&cmd, sizeof(struct storage_cmd));
    if (ret != sizeof(struct storage_cmd)) {
        log_err("tcp_write() failed, ret = %ld", ret);
        goto done;
    }
    ret = tcp_read(c, response, BUF_SIZE);
    log_info("tcp_read %s", response);

    if (strcmp(response, "Hello Shenango") == 0) {
        log_info("CORRECT!");
    }
    else {
        log_info("INCORRECT!");
    }

done: 
    log_info("close port %hu", tcp_local_addr(c).port);
    tcp_close(c);
    waitgroup_done(wg);
}

static void main_handler(void *arg)
{
    waitgroup_t wg;
    int ret;

    waitgroup_init(&wg);
    waitgroup_add(&wg, 1);
    ret = thread_spawn(client_worker, &wg);
    BUG_ON(ret);
    waitgroup_wait(&wg);
}

int main(int argc, char *argv[])
{
    int ret;
    uint32_t addr;

    ret = str_to_ip(argv[2], &addr);
    if (ret) {
        printf("couldn't parse [ip] '%s'\n", argv[2]);
        return -EINVAL;
    }
    raddr.ip = addr;
    raddr.port = STORAGE_SERVICE_PORT;

    ret = runtime_init(argv[1], main_handler, NULL);
    if (ret) {
        printf("failed to start runtime\n");
        return ret;
    }

    return 0;
}
