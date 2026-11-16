#ifdef __VMS
#pragma module PT_QIO_AST "X-2"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-2 CV Camiel Vanderhoeven 16-Nov-2026:
 *     Report effective child AST and direct-I/O quotas before the workload.
 * X-1 CV Camiel Vanderhoeven 16-Nov-2026:
 *     Exercise persistent I/O slots, cross-thread waits and QIO/DCLAST AST
 *     completion through pthread_cond_signal_int_np. Original supplemental
 *     test based on the reported workload; no application source is copied.
 */
#include "PTS_CHECK.H"

#ifndef __VMS
int test_main(int argc, char **argv)
{
	(void)argc; (void)argv;
	puts("QIO/AST completion requires native OpenVMS; no host substitute.");
	return PTS_UNSUPPORTED;
}
#else
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <builtins.h>
#include <efndef.h>
#include <iodef.h>
#include <jpidef.h>
#include <rms.h>
#include <ssdef.h>
#include <starlet.h>

#define IO_SLOTS 128
#define IO_BYTES 4096
#define IO_BLOCKS (IO_BYTES / 512)
#define WORKER_STACK (512UL * 1024UL)
#define OP_WRITE 1
#define OP_READ 2
#define OP_SYNTHETIC 3

/* Disk QIO IOSB layout, independent of the selected C pointer size. */
struct io_status {
	unsigned short status;
	unsigned short count;
	unsigned int device;
};
typedef char iosb_must_be_eight_bytes[(sizeof(struct io_status) == 8) ? 1 : -1];

struct io_slot {
	pthread_cond_t cv;
	pthread_mutex_t mutex;
	volatile unsigned int pending;
	volatile unsigned int ast_done;
	volatile unsigned int wait_started;
	volatile int signal_status;
	struct io_status iosb;
	unsigned char buffer[IO_BYTES];
	unsigned char expected[IO_BYTES];
	unsigned long generation;
	unsigned long submitter;
	unsigned int submit_status;
	int operation;
};

struct io_work {
	unsigned int index;
	unsigned long generation;
};

struct worker_context {
	unsigned long index;
	unsigned long random;
	unsigned long submitted;
	unsigned long completed;
	unsigned long reads;
	unsigned long writes;
	unsigned long synthetic;
	unsigned long early;
	unsigned long waited;
	unsigned long long wait_calls;
	unsigned long long retire_yields;
};

static struct io_slot *slots;
static struct io_work ready[IO_SLOTS];
static unsigned int free_slots[IO_SLOTS];
static unsigned int free_count, ready_head, ready_tail, ready_count, high_water;
static unsigned long producers_left;
static unsigned long long issued;
static unsigned short channel;
static pthread_mutex_t pool_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t free_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t ready_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t start_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cv = PTHREAD_COND_INITIALIZER;
static int start_go;

/* RMS FAB pointer fields are 32-bit, also in a /POINTER_SIZE=64 build. */
#pragma __pointer_size __save
#pragma __pointer_size 32
static struct FAB scratch_fab;
static char scratch_name[] = "PTS_QIO.DAT;1";
/* GETJPI uses an ILE3 item list here, with 32-bit buffer/length pointers.
 * Keep the list and its targets in static low-address storage in both builds.
 */
static unsigned int quota_values[4];
static unsigned short quota_lengths[4];
static struct {
	unsigned int status, reserved;
} quota_iosb;
static struct {
	unsigned short length, code;
	void *buffer;
	unsigned short *returned;
} quota_items[5] = {
	{sizeof(unsigned int), JPI$_ASTLM, &quota_values[0], &quota_lengths[0]},
	{sizeof(unsigned int), JPI$_ASTCNT, &quota_values[1], &quota_lengths[1]},
	{sizeof(unsigned int), JPI$_DIOLM, &quota_values[2], &quota_lengths[2]},
	{sizeof(unsigned int), JPI$_DIOCNT, &quota_values[3], &quota_lengths[3]},
	{0, 0, NULL, NULL}
};
#pragma __pointer_size __restore

static void native_failure(const char *operation, unsigned int status)
{
	fprintf(stderr, "%s failed: OpenVMS status %08x\n", operation, status);
	if (status == SS$_EXQUOTA)
		fputs("Process quota exhausted; see startup QIO quotas. "
		      "SYS$DCLAST requires an available AST quota entry.\n", stderr);
	exit(PTS_UNRESOLVED);
}

static void report_quotas(void)
{
	unsigned int status, i;
	status = sys$getjpiw(EFN$C_ENF, 0, 0, quota_items, &quota_iosb, 0, 0);
	if (!(status & 1))
		native_failure("SYS$GETJPIW quotas", status);
	if (!(quota_iosb.status & 1))
		native_failure("SYS$GETJPIW quotas IOSB", quota_iosb.status);
	for (i = 0; i < 4; ++i) {
		if (quota_lengths[i] != sizeof(quota_values[i])) {
			fputs("SYS$GETJPIW returned an incomplete quota value\n", stderr);
			exit(PTS_UNRESOLVED);
		}
	}
	printf("QIO quotas: ASTLM=%u AST_available=%u "
	       "DIOLM=%u DIO_available=%u\n", quota_values[0], quota_values[1],
	       quota_values[2], quota_values[3]);
	fflush(stdout);
}

static void start_wait(void)
{
	CHECK(pthread_mutex_lock(&start_mutex));
	while (!start_go)
		CHECK(pthread_cond_wait(&start_cv, &start_mutex));
	CHECK(pthread_mutex_unlock(&start_mutex));
}

/* This is the only completion writer for one live slot. No allocation,
 * formatting, mutex operation, or ordinary pthread_cond_signal in an AST.
 * The application predicate is published before the interrupt-safe signal.
 * ast_done is only a retirement marker for this test's post-signal status;
 * it is never used to escape an unsignalled condition-variable wait.
 */
static void completion_ast(unsigned __int64 token)
{
	struct io_slot *slot = &slots[(unsigned int)token];
	if (--slot->pending == 0)
		slot->signal_status = pthread_cond_signal_int_np(&slot->cv);
	else
		slot->signal_status = EINVAL;
	__MB();
	slot->ast_done = 1;
}

static void declare_completion(unsigned int index, unsigned int status)
{
	unsigned int queued;
	slots[index].iosb.status = (unsigned short)status;
	slots[index].iosb.count = 0;
	__MB();
	queued = sys$dclast(completion_ast, (unsigned __int64)index, 0);
	if (!(queued & 1))
		native_failure("SYS$DCLAST", queued);
}

static void issue_io(unsigned int index)
{
	struct io_slot *slot = &slots[index];
	unsigned int status;
	unsigned int function;
	if (slot->operation == OP_SYNTHETIC) {
		declare_completion(index, SS$_IVCHAN);
		return;
	}
	function = slot->operation == OP_WRITE ? IO$_WRITEVBLK : IO$_READVBLK;
	status = sys$qio(EFN$C_ENF, channel, function, (void *)&slot->iosb,
			 completion_ast, (unsigned __int64)index, slot->buffer,
			 IO_BYTES, (unsigned long)(index * IO_BLOCKS + 1), 0, 0, 0);
	if (!(status & 1)) {
		/* A rejected QIO does not queue its completion AST. Preserve the
		 * error and use the same declared-AST path as an early I/O error.
		 */
		slot->submit_status = status;
		declare_completion(index, status);
	}
	/* A queued request can already have completed and been consumed.
	 * Do not touch slot fields after a successful SYS$QIO return.
	 */
}

static void enqueue(unsigned int index)
{
	CHECK(pthread_mutex_lock(&pool_mutex));
	REQUIRE(ready_count < IO_SLOTS);
	ready[ready_tail].index = index;
	ready[ready_tail].generation = slots[index].generation;
	ready_tail = (ready_tail + 1) % IO_SLOTS;
	ready_count++;
	CHECK(pthread_cond_signal(&ready_cv));
	CHECK(pthread_mutex_unlock(&pool_mutex));
}

static void *submitter(void *argument)
{
	struct worker_context *context = argument;
	unsigned long iteration;
	start_wait();
	for (iteration = 0; iteration < pts_control.iterations; ++iteration) {
		unsigned int index, byte;
		unsigned long long serial;
		struct io_slot *slot;
		CHECK(pthread_mutex_lock(&pool_mutex));
		while (free_count == 0)
			CHECK(pthread_cond_wait(&free_cv, &pool_mutex));
		index = free_slots[--free_count];
		if (IO_SLOTS - free_count > high_water)
			high_water = IO_SLOTS - free_count;
		serial = ++issued;
		CHECK(pthread_mutex_unlock(&pool_mutex));

		slot = &slots[index];
		slot->generation++;
		slot->submitter = context->index;
		slot->operation = (serial % 16 == 0) ? OP_SYNTHETIC :
			(slot->generation & 1) ? OP_WRITE : OP_READ;
		if (slot->operation == OP_WRITE) {
			for (byte = 0; byte < IO_BYTES; ++byte)
				slot->buffer[byte] = (unsigned char)(pts_control.seed +
					index * 13UL + slot->generation * 19UL +
					byte * 17UL + (byte >> 8));
		} else {
			/* A failed/short read must not accidentally match old data. */
			memset(slot->buffer, 0xa5, sizeof(slot->buffer));
		}
		memset(&slot->iosb, 0, sizeof(slot->iosb));
		slot->submit_status = SS$_NORMAL;
		slot->signal_status = 0;
		slot->ast_done = 0;
		slot->wait_started = 0;
		slot->pending = 1;
		__MB();
		if (serial & 1) {
			/* Make wait-before-completion possible without sleeping in
			 * the AST or using a timer to substitute for a real wakeup.
			 */
			enqueue(index);
			if (serial == 1) {
				/* Ensure even a short run reaches the real CV wait.
				 * This gates submission, never completion observation.
				 */
				while (!slot->wait_started)
					CHECK(pthread_yield_np());
				__MB();
			}
			pts_yield(&context->random);
			issue_io(index);
		} else {
			issue_io(index);
			pts_yield(&context->random);
			enqueue(index);
		}
		context->submitted++;
		pts_yield(&context->random);
	}
	CHECK(pthread_mutex_lock(&pool_mutex));
	REQUIRE(producers_left > 0);
	producers_left--;
	CHECK(pthread_cond_broadcast(&ready_cv));
	CHECK(pthread_mutex_unlock(&pool_mutex));
	return NULL;
}

static void *waiter(void *argument)
{
	struct worker_context *context = argument;
	start_wait();
	for (;;) {
		struct io_work work;
		struct io_slot *slot;
		CHECK(pthread_mutex_lock(&pool_mutex));
		while (ready_count == 0 && producers_left != 0)
			CHECK(pthread_cond_wait(&ready_cv, &pool_mutex));
		if (ready_count == 0) {
			CHECK(pthread_mutex_unlock(&pool_mutex));
			break;
		}
		work = ready[ready_head];
		ready_head = (ready_head + 1) % IO_SLOTS;
		ready_count--;
		CHECK(pthread_mutex_unlock(&pool_mutex));
		slot = &slots[work.index];
		REQUIRE(slot->generation == work.generation);
		/* Worker indices remain valid after a finished submitter is joined;
		 * its pthread_t would no longer be safe to compare here. The two
		 * roles occupy disjoint index ranges and distinct created threads.
		 */
		REQUIRE(slot->submitter != context->index);
		pts_yield(&context->random);
		CHECK(pthread_mutex_lock(&slot->mutex));
		if (slot->pending)
			context->waited++;
		else
			context->early++;
		while (slot->pending) {
			context->wait_calls++;
			slot->wait_started = 1;
			__MB();
			CHECK(pthread_cond_wait(&slot->cv, &slot->mutex));
		}
		__MB();
		CHECK(pthread_mutex_unlock(&slot->mutex));
		/* Completion has already been observed through the real predicate
		 * wait. Let the AST finish its status write before recycling this
		 * slot. This cannot repair a missing CV wake.
		 */
		while (!slot->ast_done) {
			context->retire_yields++;
			CHECK(pthread_yield_np());
		}
		__MB();
		CHECK(slot->signal_status);
		if (!(slot->submit_status & 1))
			native_failure("SYS$QIO submission", slot->submit_status);
		if (slot->operation == OP_SYNTHETIC) {
			REQUIRE(slot->iosb.status == (unsigned short)SS$_IVCHAN);
			REQUIRE(slot->iosb.count == 0);
			context->synthetic++;
		} else {
			if (!(slot->iosb.status & 1))
				native_failure("QIO completion IOSB", slot->iosb.status);
			REQUIRE(slot->iosb.count == IO_BYTES);
			if (slot->operation == OP_READ) {
				REQUIRE(memcmp(slot->buffer, slot->expected, IO_BYTES) == 0);
				context->reads++;
			} else {
				memcpy(slot->expected, slot->buffer, IO_BYTES);
				context->writes++;
			}
		}
		context->completed++;
		CHECK(pthread_mutex_lock(&pool_mutex));
		REQUIRE(free_count < IO_SLOTS);
		free_slots[free_count++] = work.index;
		CHECK(pthread_cond_signal(&free_cv));
		CHECK(pthread_mutex_unlock(&pool_mutex));
	}
	return NULL;
}

static void open_scratch(void)
{
	unsigned char zero[512];
	unsigned int block, status;
	FILE *file;
	int fd;
	/* The runner gives every case a fresh private directory. Exclusive
	 * creation also protects against an accidentally repeated direct run.
	 */
	fd = open(scratch_name, O_WRONLY | O_CREAT | O_EXCL, 0600,
		  "rfm=fix", "mrs=512", "ctx=bin");
	if (fd < 0) {
		perror("creating QIO scratch file");
		exit(PTS_UNRESOLVED);
	}
	file = fdopen(fd, "wb");
	if (!file) {
		perror("opening QIO scratch stream");
		close(fd);
		exit(PTS_UNRESOLVED);
	}
	memset(zero, 0, sizeof(zero));
	for (block = 0; block < IO_SLOTS * IO_BLOCKS; ++block) {
		if (fwrite(zero, 1, sizeof(zero), file) != sizeof(zero)) {
			perror("initializing QIO scratch file");
			fclose(file);
			exit(PTS_UNRESOLVED);
		}
	}
	if (fclose(file) != 0) {
		perror("closing QIO scratch stream");
		exit(PTS_UNRESOLVED);
	}
	scratch_fab = cc$rms_fab;
	scratch_fab.fab$l_fna = scratch_name;
	scratch_fab.fab$b_fns = (unsigned char)strlen(scratch_name);
	scratch_fab.fab$b_fac = FAB$M_GET | FAB$M_PUT | FAB$M_BIO;
	scratch_fab.fab$b_shr = 0;
	scratch_fab.fab$l_fop = FAB$M_UFO;
	status = sys$open(&scratch_fab);
	if (!(status & 1))
		native_failure("RMS UFO open", status);
	channel = (unsigned short)scratch_fab.fab$l_stv;
}

int test_main(int argc, char **argv)
{
	pthread_t *threads;
	struct worker_context *contexts;
	pthread_attr_t attr;
	unsigned long i, producers, consumers, used_waiters = 0;
	unsigned long long total, submitted = 0, completed = 0, reads = 0;
	unsigned long long writes = 0, synthetic = 0, early = 0, waited = 0;
	unsigned long long wait_calls = 0, retire_yields = 0;
	unsigned int status;
	char name[64];
	(void)argc; (void)argv;
	if (pts_control.threads < 2) {
		fputs("PT_QIO_AST needs at least two workers (submitter and waiter).\n",
		      stderr);
		return PTS_UNRESOLVED;
	}
	report_quotas();
	producers = pts_control.threads / 2;
	consumers = pts_control.threads - producers;
	total = (unsigned long long)producers * pts_control.iterations;
	threads = calloc(pts_control.threads, sizeof(*threads));
	contexts = calloc(pts_control.threads, sizeof(*contexts));
#if __INITIAL_POINTER_SIZE == 64
	/* Exercise 64-bit CV, IOSB and buffer addresses like a large server.
	 * Print the actual placement; allocation location is not an assertion.
	 */
	slots = _malloc64(IO_SLOTS * sizeof(*slots));
	if (slots)
		memset(slots, 0, IO_SLOTS * sizeof(*slots));
#else
	slots = calloc(IO_SLOTS, sizeof(*slots));
#endif
	if (!threads || !contexts || !slots) {
		fputs("Cannot allocate QIO slots or worker contexts\n", stderr);
		free(threads); free(contexts);
		free(slots);
		return PTS_UNRESOLVED;
	}
	for (i = 0; i < IO_SLOTS; ++i) {
		CHECK(pthread_cond_init(&slots[i].cv, NULL));
		CHECK(pthread_mutex_init(&slots[i].mutex, NULL));
		snprintf(name, sizeof(name), "QIO cond %lu", i);
		CHECK(pthread_cond_setname_np(&slots[i].cv, name, NULL));
		snprintf(name, sizeof(name), "QIO mutex %lu", i);
		CHECK(pthread_mutex_setname_np(&slots[i].mutex, name, NULL));
		free_slots[i] = (unsigned int)i;
	}
	free_count = IO_SLOTS;
	producers_left = producers;
	open_scratch();
	CHECK(pthread_attr_init(&attr));
	CHECK(pthread_attr_setstacksize(&attr, WORKER_STACK));
	printf("QIO workload: submitters=%lu waiters=%lu slots=%u bytes=%u "
	       "operations=%llu stack_bytes=%lu pool=%016llx\n", producers, consumers,
	       IO_SLOTS, IO_BYTES, total, WORKER_STACK,
	       (unsigned long long)(uintptr_t)slots);
	fflush(stdout);
	for (i = 0; i < pts_control.threads; ++i) {
		contexts[i].index = i;
		contexts[i].random = pts_control.seed + i;
		CHECK(pthread_create(&threads[i], &attr,
				     i < producers ? submitter : waiter, &contexts[i]));
	}
	CHECK(pthread_attr_destroy(&attr));
	CHECK(pthread_mutex_lock(&start_mutex));
	start_go = 1;
	CHECK(pthread_cond_broadcast(&start_cv));
	CHECK(pthread_mutex_unlock(&start_mutex));
	/* The initial thread stays in pthread_join, allowing application ASTs
	 * to run while submitters and waiters contend on other virtual processors.
	 */
	for (i = 0; i < pts_control.threads; ++i) {
		CHECK(pthread_join(threads[i], NULL));
		submitted += contexts[i].submitted;
		completed += contexts[i].completed;
		reads += contexts[i].reads;
		writes += contexts[i].writes;
		synthetic += contexts[i].synthetic;
		early += contexts[i].early;
		waited += contexts[i].waited;
		wait_calls += contexts[i].wait_calls;
		retire_yields += contexts[i].retire_yields;
		if (contexts[i].completed != 0)
			used_waiters++;
	}
	REQUIRE(issued == total && submitted == total && completed == total);
	REQUIRE(reads + writes + synthetic == total);
	REQUIRE(early + waited == total);
	REQUIRE(wait_calls > 0);
	REQUIRE(free_count == IO_SLOTS && ready_count == 0 && producers_left == 0);
	printf("QIO completed=%llu writes=%llu reads=%llu declared_errors=%llu "
	       "cross_thread=%llu early=%llu waited=%llu wait_calls=%llu "
	       "retire_yields=%llu active_slots_high_water=%u used_waiters=%lu\n",
	       completed, writes, reads, synthetic, completed, early, waited,
	       wait_calls, retire_yields, high_water, used_waiters);
	status = sys$dassgn(channel);
	if (!(status & 1))
		native_failure("SYS$DASSGN", status);
	/* These persistent objects remain alive until process exit, as in the
	 * workload being modelled. A completed AST's signal can still be on
	 * pthreads' deferred-CV list; teardown is not part of this test.
	 */
	CHECK(pthread_cond_destroy(&free_cv));
	CHECK(pthread_cond_destroy(&ready_cv));
	CHECK(pthread_mutex_destroy(&pool_mutex));
	CHECK(pthread_cond_destroy(&start_cv));
	CHECK(pthread_mutex_destroy(&start_mutex));
	free(contexts);
	free(threads);
	if (remove(scratch_name) != 0) {
		perror("removing QIO scratch file");
		return PTS_UNRESOLVED;
	}
	return PTS_PASS;
}
#endif
