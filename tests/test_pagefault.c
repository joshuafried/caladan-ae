/*
 * test_storage.c - writes and reads to the storage device
 */

#include <stdio.h>
#include <time.h>
#include <string.h>

#include <base/log.h>
#include <runtime/storage.h>
#include <runtime/thread.h>
#include <runtime/timer.h>
#include <runtime/sync.h>

#include <sys/mman.h>

#define N 100
#define N_PAGES 1000

static waitgroup_t wg;
uint64_t PGSIZE_2MB = 0x200000;

static void work_handler(void *thread_no)
{
	int i;
	char *data_addr;
	char buf[50];

	for (i = 1; i < N_PAGES; i++) {
		sprintf(buf, "page number: %d", i);
		data_addr = (char *)(i * PGSIZE_2MB);
		if (strcmp(buf, data_addr) != 0) {
			log_err("wrong data at %p: %s", data_addr, data_addr);
			return;
		}
	}

	waitgroup_done(&wg);
}

static void main_handler(void *arg)
{
	uint64_t i, ret;
	char *data_addr;

	for (i = 1; i < N_PAGES; i++) {
		data_addr = (char *)(i * PGSIZE_2MB);
		sprintf(data_addr, "page number: %lu", i);
	}

	waitgroup_init(&wg);
	waitgroup_add(&wg, N);
	for (i = 0; i < N; i++) {
		ret = thread_spawn(work_handler, (void *) i);
		BUG_ON(ret);
		thread_yield();
	}

	waitgroup_wait(&wg);
}

int main(int argc, char *argv[])
{
	int ret;

	ret = runtime_init(argv[1], main_handler, NULL);
	if (ret) {
		log_err("failed to start runtime");
		return ret;
	}
	return 0;
}
