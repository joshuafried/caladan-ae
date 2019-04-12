/*
 * control.c - the control-plane for the I/O kernel
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <base/stddef.h>
#include <base/mem.h>
#include <base/log.h>
#include <base/thread.h>
#include <iokernel/control.h>

#include "defs.h"

static int controlfd;
static int clientfds[IOKERNEL_MAX_PROC];
static struct proc *clients[IOKERNEL_MAX_PROC];
static int nr_clients;
struct lrpc_params lrpc_control_to_data_params;
struct lrpc_params lrpc_data_to_control_params;
static struct lrpc_chan_out lrpc_control_to_data;
static struct lrpc_chan_in lrpc_data_to_control;
static int nr_guaranteed;

static struct proc *control_create_proc(mem_key_t key, size_t len, pid_t pid)
{
	struct control_hdr hdr;
	struct shm_region reg;
	size_t nr_pages;
	struct proc *p;
	struct thread_spec *threads, *threads_shm;
	struct bundle_spec *bundles, *bundles_shm;
	struct hardware_queue_spec *hwq_spec, *hwq_spec_shm;
	struct timer_spec *timer_spec, *timer_spec_shm;
	void *shbuf;
	int i, j, ret;

	/* attach the shared memory region */
	if (len < sizeof(hdr))
		goto fail;
	shbuf = mem_map_shm(key, NULL, len, PGSIZE_2MB, false);
	if (shbuf == MAP_FAILED)
		goto fail;

	/* parse the control header */
	memcpy(&hdr, (struct control_hdr *)shbuf, sizeof(hdr)); /* TOCTOU */
	if (hdr.magic != CONTROL_HDR_MAGIC)
		goto fail_unmap;
	if (hdr.thread_count > NCPU || hdr.thread_count == 0 ||
			hdr.bundle_count > NCPU)
		goto fail_unmap;

	if (hdr.sched_cfg.guaranteed_cores + nr_guaranteed > get_total_cores()) {
		log_err("guaranteed cores exceeds total core count");
		goto fail_unmap;
	}

	nr_guaranteed += hdr.sched_cfg.guaranteed_cores;

	/* create the process */
	nr_pages = div_up(len, PGSIZE_2MB);
	p = malloc(sizeof(*p) + nr_pages * sizeof(physaddr_t));
	if (!p)
		goto fail_unmap;

	threads = malloc(sizeof(*threads) * hdr.thread_count);
	if (!threads)
		goto fail_free_just_proc;

	p->pid = pid;
	ref_init(&p->ref);
	reg.base = shbuf;
	reg.len = len;
	p->region = reg;
	p->removed = false;
	p->sched_cfg = hdr.sched_cfg;
	BUG_ON(p->sched_cfg.guaranteed_cores < 1);
	p->thread_count = hdr.thread_count;
	p->uniqid = rdtsc();

	threads_shm = shmptr_to_ptr(&reg, hdr.thread_specs, sizeof(*threads) * hdr.thread_count);
	if (!threads_shm)
		goto fail_free_proc;

	memcpy(threads, threads_shm, sizeof(*threads) * hdr.thread_count);

	/* initialize the threads */
	for (i = 0; i < hdr.thread_count; i++) {
		struct thread *th = &p->threads[i];
		struct thread_spec *s = &threads[i];

		/* attach the RX queue */
		ret = shm_init_lrpc_out(&reg, &s->rxcmdq, &th->rxcmdq);
		if (ret)
			goto fail_free_proc;

		/* attach the TX command queue */
		ret = shm_init_lrpc_in(&reg, &s->txcmdq, &th->txcmdq);
		if (ret)
			goto fail_free_proc;

		th->tid = s->tid;
		th->p = p;
		th->parked = true;
		th->waking = false;
		th->at_idx = -1;
		th->ts_idx = -1;
		th->kthread_idx = i;

		/* initialize pointer to queue pointers in shared memory */
		th->q_ptrs = (struct q_ptrs *) shmptr_to_ptr(&reg, s->q_ptrs,
				sizeof(struct q_ptrs));
		if (!th->q_ptrs)
			goto fail_free_proc;
	}

	bundles_shm = shmptr_to_ptr(&reg, hdr.bundle_specs, sizeof(*bundles) * hdr.bundle_count);
	if (!bundles_shm)
		goto fail_free_proc;

	bundles = malloc(sizeof(*bundles) * hdr.bundle_count);
	if (!bundles)
		goto fail_free_proc;

	memcpy(bundles, bundles_shm, sizeof(*bundles) * hdr.bundle_count);

	for (i = 0; i < hdr.bundle_count; i++) {
		struct bundle *b = &p->bundles[i];
		b->hwq_count = bundles[i].hwq_count;
		b->timer_count = bundles[i].timer_count;

		if (b->hwq_count > MAX_HWQ || b->timer_count > MAX_TIMER)
			goto fail_free_bundle;

		hwq_spec_shm = shmptr_to_ptr(&reg, bundles[i].hwq_specs, sizeof(*hwq_spec) * b->hwq_count);
		if (!hwq_spec_shm)
			goto fail_free_bundle;

		hwq_spec = malloc(sizeof(*hwq_spec) * b->hwq_count);
		if (!hwq_spec)
			goto fail_free_bundle;

		memcpy(hwq_spec, hwq_spec_shm, sizeof(*hwq_spec) * b->hwq_count);
		for (j = 0; j < b->hwq_count; j++) {
			b->qs[j].descriptor_table = shmptr_to_ptr(&reg,
				  hwq_spec[j].descriptor_table,
				  hwq_spec[j].descriptor_size * hwq_spec[j].nr_descriptors);
			b->qs[j].consumer_idx = shmptr_to_ptr(&reg, hwq_spec[j].consumer_idx, sizeof(*b->qs[j].consumer_idx));
			b->qs[j].descriptor_size = hwq_spec[j].descriptor_size;
			b->qs[j].nr_descriptors = hwq_spec[j].nr_descriptors;
			b->qs[j].parity_byte_offset = hwq_spec[j].parity_byte_offset;
			b->qs[j].parity_bit_mask = hwq_spec[j].parity_bit_mask;
			b->qs[j].hwq_type = hwq_spec[j].hwq_type;

			if (!b->qs[j].descriptor_table || !b->qs[j].consumer_idx ||
				  b->qs[j].parity_byte_offset > b->qs[j].descriptor_size ||
				  b->qs[j].nr_descriptors & (b->qs[j].nr_descriptors - 1) ||
				  b->qs[j].hwq_type >= NR_HWQ) {
				free(hwq_spec);
				goto fail_free_bundle;
			}

			b->qs[j].cq_idx = 0;
			b->qs[j].cq_pending = false;
		}
		free(hwq_spec);

		timer_spec_shm = shmptr_to_ptr(&reg, bundles[i].timer_specs, sizeof(*timer_spec) * b->timer_count);
		if (!timer_spec_shm)
			goto fail_free_bundle;

		timer_spec = malloc(sizeof(*timer_spec) * b->timer_count);
		if (!timer_spec)
			goto fail_free_bundle;

		memcpy(timer_spec, timer_spec_shm, sizeof(*timer_spec) * b->timer_count);
		for (j = 0; j < b->timer_count; j++) {
			b->timers[j].timern = shmptr_to_ptr(&reg, timer_spec[j].timern, sizeof(*b->timers[j].timern));
			b->timers[j].next_deadline_tsc = shmptr_to_ptr(&reg, timer_spec[j].next_deadline_tsc, sizeof(*b->timers[j].next_deadline_tsc));

			if (!b->timers[j].timern || !b->timers[j].next_deadline_tsc) {
				free(timer_spec);
				goto fail_free_bundle;
			}

		}
		free(timer_spec);
	}

	p->bundle_count = hdr.bundle_count;


	/* initialize the table of physical page addresses */
	ret = mem_lookup_page_phys_addrs(p->region.base, p->region.len, PGSIZE_2MB,
			p->page_paddrs);
	if (ret)
		goto fail_free_bundle;

	free(bundles);
	free(threads);

	return p;

fail_free_bundle:
	free(bundles);
fail_free_proc:
	free(threads);
fail_free_just_proc:
	free(p);
fail_unmap:
	mem_unmap_shm(shbuf);
fail:
	log_err("control: couldn't attach pid %d", pid);
	return NULL;
}

static void control_destroy_proc(struct proc *p)
{
	nr_guaranteed -= p->sched_cfg.guaranteed_cores;
	mem_unmap_shm(p->region.base);
	free(p);
}

static void control_add_client(void)
{
	struct proc *p;
	struct ucred ucred;
	socklen_t len;
	mem_key_t shm_key;
	size_t shm_len;
	ssize_t ret;
	int fd;

	fd = accept(controlfd, NULL, NULL);
	if (fd == -1) {
		log_err("control: accept() failed [%s]", strerror(errno));
		return;
	}

	if (nr_clients >= IOKERNEL_MAX_PROC) {
		log_err("control: hit client process limit");
		goto fail;
	}

	len = sizeof(struct ucred);
	if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &ucred, &len) == -1) {
		log_err("control: getsockopt() failed [%s]", strerror(errno));
		goto fail;
	}

	ret = read(fd, &shm_key, sizeof(shm_key));
	if (ret != sizeof(shm_key)) {
		log_err("control: read() failed, len=%ld [%s]",
			ret, strerror(errno));
		goto fail;
	}

	ret = read(fd, &shm_len, sizeof(shm_len));
	if (ret != sizeof(shm_len)) {
		log_err("control: read() failed, len=%ld [%s]",
			ret, strerror(errno));
		goto fail;
	}

	p = control_create_proc(shm_key, shm_len, ucred.pid);
	if (!p) {
		log_err("control: failed to create process '%d'", ucred.pid);
		goto fail;
	}

	// TODO: this prevents a race with ksched where a kthread parks
	// for the first time AFTER the the iokernel sends its wakeup.
	sleep(1);

	if (!lrpc_send(&lrpc_control_to_data, DATAPLANE_ADD_CLIENT,
			(unsigned long) p)) {
		log_err("control: failed to inform dataplane of new client '%d'",
				ucred.pid);
		goto fail_destroy_proc;
	}

	clients[nr_clients] = p;
	clientfds[nr_clients++] = fd;
	return;

fail_destroy_proc:
	control_destroy_proc(p);
fail:
	close(fd);
}

static void control_instruct_dataplane_to_remove_client(int fd)
{
	int i;

	for (i = 0; i < nr_clients; i++) {
		if (clientfds[i] == fd)
			break;
	}

	if (i == nr_clients) {
		WARN();
		return;
	}

	clients[i]->removed = true;
	if (!lrpc_send(&lrpc_control_to_data, DATAPLANE_REMOVE_CLIENT,
			(unsigned long) clients[i])) {
		log_err("control: failed to inform dataplane of removed client");
	}
}

static void control_remove_client(struct proc *p)
{
	int i;

	for (i = 0; i < nr_clients; i++) {
		if (clients[i] == p)
			break;
	}

	if (i == nr_clients) {
		WARN();
		return;
	}

	control_destroy_proc(p);
	clients[i] = clients[nr_clients - 1];

	close(clientfds[i]);
	clientfds[i] = clientfds[nr_clients - 1];
	nr_clients--;
}

static void control_loop(void)
{
	fd_set readset;
	int maxfd, i, nrdy;
	uint64_t cmd;
	unsigned long payload;
	struct proc *p;

	while (1) {
		maxfd = controlfd;
		FD_ZERO(&readset);
		FD_SET(controlfd, &readset);

		for (i = 0; i < nr_clients; i++) {
			if (clients[i]->removed)
				continue;

			FD_SET(clientfds[i], &readset);
			maxfd = (clientfds[i] > maxfd) ? clientfds[i] : maxfd;
		}

		nrdy = select(maxfd + 1, &readset, NULL, NULL, NULL);
		if (nrdy == -1) {
			log_err("control: select() failed [%s]",
				strerror(errno));
			BUG();
		}

		for (i = 0; i <= maxfd && nrdy > 0; i++) {
			if (!FD_ISSET(i, &readset))
				continue;

			if (i == controlfd) {
				/* accept a new connection */
				control_add_client();
			} else {
				/* close an existing connection */
				control_instruct_dataplane_to_remove_client(i);
			}

			nrdy--;
		}

		while (lrpc_recv(&lrpc_data_to_control, &cmd, &payload)) {
			p = (struct proc *) payload;
			assert(cmd == CONTROL_PLANE_REMOVE_CLIENT);

			/* it is now safe to remove data structures for this client */
			control_remove_client(p);
		}
	}
}

static void *control_thread(void *data)
{
	int ret;

	/* pin to our assigned core */
	ret = cores_pin_thread(gettid(), core_assign.ctrl_core);
	if (ret < 0) {
		log_err("control: failed to pin control thread to core %d",
				core_assign.ctrl_core);
		/* continue running but performance is unpredictable */
	}

	control_loop();
	return NULL;
}

/*
 * Initialize channels for communicating with the I/O kernel dataplane.
 */
static int control_init_dataplane_comm(void)
{
	int ret;
	struct lrpc_msg *buffer_out, *buffer_in;
	uint32_t *wb_out, *wb_in;

	buffer_out = malloc(sizeof(struct lrpc_msg) *
			CONTROL_DATAPLANE_QUEUE_SIZE);
	if (!buffer_out)
		goto fail;
	wb_out = malloc(CACHE_LINE_SIZE);
	if (!wb_out)
		goto fail_free_buffer_out;

	lrpc_control_to_data_params.buffer = buffer_out;
	lrpc_control_to_data_params.wb = wb_out;

	ret = lrpc_init_out(&lrpc_control_to_data,
			lrpc_control_to_data_params.buffer, CONTROL_DATAPLANE_QUEUE_SIZE,
			lrpc_control_to_data_params.wb);
	if (ret < 0) {
		log_err("control: initializing LRPC to dataplane failed");
		goto fail_free_wb_out;
	}

	buffer_in = malloc(sizeof(struct lrpc_msg) * CONTROL_DATAPLANE_QUEUE_SIZE);
	if (!buffer_in)
		goto fail_free_wb_out;
	wb_in = malloc(CACHE_LINE_SIZE);
	if (!wb_in)
		goto fail_free_buffer_in;

	lrpc_data_to_control_params.buffer = buffer_in;
	lrpc_data_to_control_params.wb = wb_in;

	ret = lrpc_init_in(&lrpc_data_to_control,
			lrpc_data_to_control_params.buffer, CONTROL_DATAPLANE_QUEUE_SIZE,
			lrpc_data_to_control_params.wb);
	if (ret < 0) {
		log_err("control: initializing LRPC from dataplane failed");
		goto fail_free_wb_in;
	}

	return 0;

fail_free_wb_in:
	free(wb_in);
fail_free_buffer_in:
	free(buffer_in);
fail_free_wb_out:
	free(wb_out);
fail_free_buffer_out:
	free(buffer_out);
fail:
	return -1;
}

int control_init(void)
{
	struct sockaddr_un addr;
	pthread_t tid;
	int sfd, ret;

	BUILD_ASSERT(strlen(CONTROL_SOCK_PATH) <= sizeof(addr.sun_path) - 1);

	memset(&addr, 0x0, sizeof(struct sockaddr_un));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, CONTROL_SOCK_PATH, sizeof(addr.sun_path) - 1);
 
	sfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (sfd == -1) {
		log_err("control: socket() failed [%s]", strerror(errno));
		return -errno;
	}
 
	if (bind(sfd, (struct sockaddr *)&addr,
		 sizeof(struct sockaddr_un)) == -1) {
		log_err("control: bind() failed [%s]", strerror(errno));
		close(sfd);
		return -errno;
	}

	if (listen(sfd, 100) == -1) {
		log_err("control: listen() failed[%s]", strerror(errno));
		close(sfd);
		return -errno;
	}

	ret = control_init_dataplane_comm();
	if (ret < 0) {
		log_err("control: cannot initialize communication with dataplane");
		return ret;
	}

	log_info("control: spawning control thread");
	controlfd = sfd;
	if (pthread_create(&tid, NULL, control_thread, NULL) == -1) {
		log_err("control: pthread_create() failed [%s]",
			strerror(errno));
		close(sfd);
		return -errno;
	}

	return 0;	
}
