/*
 * preempt.c - support for kthread preemption
 */

#include <signal.h>
#include <string.h>
#include <sys/mman.h>

#include "base/log.h"
#include "runtime/thread.h"
#include "runtime/preempt.h"
#include "runtime/storage.h"
#include "runtime/sync.h"

#include "defs.h"

/* the current preemption count */
volatile __thread unsigned int preempt_cnt = PREEMPT_NOT_PENDING;
volatile __thread bool preempt_cede;

/* set a flag to indicate a preemption request is pending */
static void set_preempt_needed(void)
{
	preempt_cnt &= ~PREEMPT_NOT_PENDING;
}

/* handles preemption cede signals from the iokernel */
static void handle_sigusr1(int s, siginfo_t *si, void *c)
{
	STAT(PREEMPTIONS)++;
	set_preempt_needed();

	/* resume execution if preemption is disabled */
	if (!preempt_enabled()) {
		preempt_cede = true;
		return;
	}

	thread_cede();
}

/* handles preemption yield signals from the iokernel */
static void handle_sigusr2(int s, siginfo_t *si, void *c)
{
	STAT(PREEMPTIONS)++;
	set_preempt_needed();

	/* resume execution if preemption is disabled */
	if (!preempt_enabled())
		return;

	thread_yield();
}

#define MAX_MAPPED_PAGES 100
static void *TEMP_MAP_ADDR = (void *)0x500000000000;
static volatile uintptr_t pages[MAX_MAPPED_PAGES];
static volatile int pg_head = -1;
static volatile int pg_tail = 0;
static mutex_t page_mux;

/* handles page fault */
static void handle_sigsegv(int s, siginfo_t *si, void *c)
{
	void *map_addr, *fault_addr, *old_page;
	uint64_t num_blocks;
	int res, idx;

	fault_addr = (void *)((uint64_t)si->si_addr / PGSIZE_2MB * PGSIZE_2MB);

	mutex_lock(&page_mux);
	for (idx = 0; idx < MAX_MAPPED_PAGES; idx++) {
		if (pages[idx] == (uintptr_t) fault_addr) {
			mutex_unlock(&page_mux);
			return;
		}
	}

	map_addr = mmap(TEMP_MAP_ADDR, PGSIZE_2MB, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (map_addr == MAP_FAILED) {
		log_err("mmap() failed: %d", errno);
		mutex_unlock(&page_mux);
		return;
	}

	num_blocks = PGSIZE_2MB / storage_block_size();
	if (pg_head == pg_tail) {
		old_page = (void *)pages[pg_head];
		res = storage_write(old_page, (uintptr_t)old_page / PGSIZE_2MB * num_blocks, num_blocks);
		if (res) {
			log_err("storage_write for flushing page failed with code %d", res);
			mutex_unlock(&page_mux);
			return;
		}
		res = munmap(old_page, PGSIZE_2MB);
		if (res != 0) {
			log_err("munmap failed with err %d", res);
			mutex_unlock(&page_mux);
			return;
		}
		pg_head = (pg_head + 1) % MAX_MAPPED_PAGES;
	}
	pages[pg_tail] = (uintptr_t) fault_addr;
	if (pg_head == -1) {
		pg_head = pg_tail;
	}
	pg_tail = (pg_tail + 1) % MAX_MAPPED_PAGES;


	res = storage_read(map_addr, (uintptr_t)fault_addr / PGSIZE_2MB * num_blocks, num_blocks);
	if (res) {
		log_err("storage_read failed in sigsegv handler");
		mutex_unlock(&page_mux);
		return;
	}
	map_addr = mremap(map_addr, PGSIZE_2MB, PGSIZE_2MB, MREMAP_MAYMOVE | MREMAP_FIXED, fault_addr);
	if (map_addr == MAP_FAILED) {
		log_err("mremap() failed: %d", errno);
		mutex_unlock(&page_mux);
		return;
	}


	mutex_unlock(&page_mux);
}

/**
 * preempt - entry point for preemption
 */
void preempt(void)
{
	assert(preempt_needed());
	if (preempt_cede) {
		preempt_cede = false;
		thread_cede();
	} else {
		thread_yield();
	}
}

/**
 * preempt_init - global initializer for preemption support
 *
 * Returns 0 if successful. otherwise fail.
 */
int preempt_init(void)
{
	struct sigaction act;

	mutex_init(&page_mux);

	act.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER | SA_RESTART;

	if (sigemptyset(&act.sa_mask) != 0) {
		log_err("couldn't empty the signal handler mask");
		return -errno;
	}

	act.sa_sigaction = handle_sigusr1;
	if (sigaction(SIGUSR1, &act, NULL) == -1) {
		log_err("couldn't register signal handler");
		return -errno;
	}

	act.sa_sigaction = handle_sigusr2;
	if (sigaction(SIGUSR2, &act, NULL) == -1) {
		log_err("couldn't register signal handler");
		return -errno;
	}

	act.sa_sigaction = handle_sigsegv;
	if (sigaction(SIGSEGV, &act, NULL) == -1) {
		log_err("couldn't register signal handler");
		return -errno;
	}

	return 0;
}
