#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/socket.h>
#include <unistd.h>

#include "storage_service.h"

#define MAKE_IP_ADDR(a, b, c, d)			\
	(((uint32_t) a << 24) | ((uint32_t) b << 16) |	\
	 ((uint32_t) c << 8) | (uint32_t) d)

static int str_to_ip(const char *str, uint32_t *addr)
{
	uint8_t a, b, c, d;
	if(sscanf(str, "%hhu.%hhu.%hhu.%hhu", &a, &b, &c, &d) != 4) {
		return -EINVAL;
	}

	*addr = MAKE_IP_ADDR(a, b, c, d);
	return 0;
}

#define SERVER_IP_ADDR "192.168.1.4"

int main(int argc, char *argv[])
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) {
        printf("error opening socket\n");
        return 1;
    }
    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = STORAGE_SERVICE_PORT;
    str_to_ip(SERVER_IP_ADDR, &(addr.sin_addr.s_addr));
	printf("%u", addr.sin_addr.s_addr);
	fflush(stdout);

    connect(s, &addr, sizeof(struct sockaddr_in));
	printf("lol\n");
	fflush(stdout);

    struct storage_cmd cmd;
    cmd.cmd = CMD_WRITE;
    cmd.lba = 0;
    cmd.lba_count = 1;
    strcpy(cmd.data, "Hello Shenango");
	printf("lol\n");
	fflush(stdout);
    write(s, (void *)&cmd, sizeof(cmd));

	printf("lol\n");
	fflush(stdout);
    cmd.cmd = CMD_READ;
    memset(cmd.data, 0, sizeof(cmd));
    read(s, &cmd, sizeof(cmd));

	printf("lol\n");
	fflush(stdout);
    if (strcmp(cmd.data, "Hello Shenango") == 0) {
        printf("CORRECT!\n");
    }
    else {
        printf("INCORRECT!\n");
    }

    return 0;
}
