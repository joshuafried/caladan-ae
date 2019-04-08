#include <string.h>
#include <stdio.h>

#include "base/log.h"
#include "net/ip.h"
#include "runtime/storage.h"
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
    tcpconn_t *c;
    struct netaddr laddr;
    struct storage_cmd *cmd;
    ssize_t ret;

    laddr.ip = 0;
    laddr.port = 0;

    ret = tcp_dial(laddr, raddr, &c);
	if (ret) {
		log_err("tcp_dial() failed, ret = %ld", ret);
		goto done;
	}

    cmd->cmd = CMD_WRITE;
    cmd->lba = 0;
    cmd->lba_count = 1;
    strcpy(cmd->data, "Hello Shenango");
	printf("lol\n");
	fflush(stdout);
    ret = tcp_write(c, (void *)cmd, sizeof(struct storage_cmd));
    if (ret != sizeof(struct storage_cmd)) {
        log_err("tcp_write() failed, ret = %ld", ret);
        goto done;
    }

	printf("lol\n");
	fflush(stdout);
    cmd->cmd = CMD_READ;
    memset(cmd->data, 0, sizeof(struct storage_cmd));
    ret = tcp_read(c, (void *)cmd, sizeof(struct storage_cmd));
    if (ret != sizeof(struct storage_cmd)) {
        log_err("tcp_read() failed, ret = %ld", ret);
        goto done;
    }

    if (strcmp(cmd->data, "Hello Shenango") == 0) {
        printf("CORRECT!\n");
    }
    else {
        printf("INCORRECT!\n");
    }

done: 
	log_info("close port %hu", tcp_local_addr(c).port);
	tcp_abort(c);
	tcp_close(c);
}

static void main_handler(void *arg)
{
    int ret;
    ret = thread_spawn(client_worker, NULL);
    BUG_ON(ret);
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
