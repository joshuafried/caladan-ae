/*
 * random.c - utilities for generating random data
 */

#include <fcntl.h>
#include <unistd.h>

#include <base/random.h>

int fill_random_bytes(void *addr, size_t len)
{
	int fd;
	ssize_t ret;

	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0)
		return -errno;

	ret = read(fd, addr, len);
	close(fd);

	if (ret != len)
		return -errno;

	return 0;
}
