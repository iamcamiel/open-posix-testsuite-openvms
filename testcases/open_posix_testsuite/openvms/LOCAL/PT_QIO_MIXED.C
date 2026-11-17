#ifdef __VMS
#pragma module PT_QIO_MIXED "X-1"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 17-Nov-2026:
 *     Add a separate disk-QIO, loopback TCP-QIO and local queue stress test.
 *     Keep the original QIO-only test unchanged as a comparison workload.
 */
#include "PTS_CHECK.H"

#ifndef __VMS
int test_main(int argc, char **argv)
{
	(void)argc; (void)argv;
	puts("Mixed QIO/communication stress requires native OpenVMS.");
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
#include <types.h>
#include <socket.h>
#include <in.h>
#include <inet.h>
#include <tcp.h>

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
static char scratch_name[] = "PTS_MIXED.DAT;1";
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

/* The communication side deliberately uses real loopback TCP channels.
 * There are no external peers or privileged listeners. Local clients are
 * threads sharing this process's memory, not separate global-section users.
 * Five additional threads run beside the requested disk worker population:
 * local sender, hibernating dispatcher, local worker, TCP sender, TCP worker.
 */
#define MIX_CONNECTIONS 16
#define MIX_LOCAL_SLOTS 16
#define MIX_MESSAGE_BYTES 64
#define MIX_HELPERS 5

/* Quadword queue builtins always return a 64-bit pointer, even with /POINTER=32.
 * Nodes have a 32-byte stride and are placed on 16-byte boundaries. The first
 * two quadwords are the VMS interlocked queue links. Persistent storage also
 * prevents a deferred interrupt-safe signal from referring to freed memory.
 */
#pragma __pointer_size __save
#pragma __pointer_size 64
typedef void *mix_pointer64;
#pragma __pointer_size __restore
struct mix_node {
	unsigned __int64 forward, backward;
	unsigned int index, padding[3];
};
typedef char mix_node_stride[(sizeof(struct mix_node) == 32) ? 1 : -1];
struct mix_local {
	pthread_mutex_t mutex;
	pthread_cond_t cv;
	unsigned long acknowledged;
	unsigned long sequence;
	unsigned char message[MIX_MESSAGE_BYTES];
};
struct mix_connection {
	int client, server;
	unsigned short channel;
	pthread_mutex_t mutex;
	pthread_cond_t cv;
	unsigned long acknowledged, sequence;
	struct io_status iosb;
	volatile unsigned int retired;
	unsigned int received, submit_status;
	int signal_status;
	unsigned char sent[MIX_MESSAGE_BYTES], received_message[MIX_MESSAGE_BYTES];
};
static struct mix_local mix_local[MIX_LOCAL_SLOTS];
static struct mix_connection mix_connections[MIX_CONNECTIONS];
static struct mix_node *mix_incoming, *mix_local_work, *mix_net_work;
static struct mix_node *mix_local_nodes, *mix_net_nodes;
static void *mix_queue_storage;
static pthread_mutex_t mix_local_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mix_local_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t mix_net_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t mix_net_cv = PTHREAD_COND_INITIALIZER;
static unsigned long long mix_local_completed, mix_net_completed;
static unsigned long long mix_hibernations, mix_wakes, mix_net_asts;
static unsigned long long mix_net_partial, mix_net_waits;

static void mix_insert(struct mix_node *head, struct mix_node *node)
{
	__MB();
	while (__PAL_INSQTIQ(head, node) == -1)
		; /* The interlock is held only during the native queue operation. */
}

static struct mix_node *mix_remove(struct mix_node *head)
{
	mix_pointer64 removed = NULL;
	int status;
	do {
		status = __PAL_REMQHIQ(head, &removed);
	} while (status == -1);
	__MB();
	return status == 0 ? NULL : (struct mix_node *)removed;
}

static unsigned char mix_byte(unsigned int index, unsigned long sequence,
			     unsigned int byte)
{
	return (unsigned char)(pts_control.seed + index * 23UL + sequence * 37UL +
			       byte * 13UL + (sequence >> 8));
}

static void *mix_local_sender(void *unused)
{
	unsigned long sequence;
	unsigned int index, byte, status;
	(void)unused;
	start_wait();
	for (sequence = 1; sequence <= pts_control.iterations; ++sequence) {
		/* Fill a batch before waking the dispatcher. Nodes are reusable only
		 * after the worker has acknowledged the preceding sequence.
		 */
		for (index = 0; index < MIX_LOCAL_SLOTS; ++index) {
			struct mix_local *item = &mix_local[index];
			CHECK(pthread_mutex_lock(&item->mutex));
			REQUIRE(item->acknowledged == sequence - 1);
			item->sequence = sequence;
			for (byte = 0; byte < MIX_MESSAGE_BYTES; ++byte)
				item->message[byte] = mix_byte(index, sequence, byte);
			CHECK(pthread_mutex_unlock(&item->mutex));
			mix_insert(mix_incoming, &mix_local_nodes[index]);
		}
		status = sys$wake(0, 0);
		if (!(status & 1)) native_failure("SYS$WAKE", status);
		mix_wakes++;
		for (index = 0; index < MIX_LOCAL_SLOTS; ++index) {
			struct mix_local *item = &mix_local[index];
			CHECK(pthread_mutex_lock(&item->mutex));
			while (item->acknowledged != sequence)
				CHECK(pthread_cond_wait(&item->cv, &item->mutex));
			CHECK(pthread_mutex_unlock(&item->mutex));
		}
	}
	return NULL;
}

static void *mix_local_dispatcher(void *unused)
{
	unsigned long long dispatched = 0;
	unsigned long long total = (unsigned long long)MIX_LOCAL_SLOTS * pts_control.iterations;
	(void)unused;
	start_wait();
	while (dispatched < total) {
		struct mix_node *node = mix_remove(mix_incoming);
		if (!node) {
			unsigned int status;
			mix_hibernations++;
			/* WAKE is sticky if the sender wins this race. Spurious wakes
			 * simply cause another queue check; no polling timer is used.
			 */
			status = sys$hiber();
			if (!(status & 1)) native_failure("SYS$HIBER", status);
			continue;
		}
		CHECK(pthread_mutex_lock(&mix_local_mutex));
		mix_insert(mix_local_work, node);
		CHECK(pthread_cond_signal(&mix_local_cv));
		CHECK(pthread_mutex_unlock(&mix_local_mutex));
		dispatched++;
	}
	return NULL;
}

static void *mix_local_worker(void *unused)
{
	unsigned long long total = (unsigned long long)MIX_LOCAL_SLOTS * pts_control.iterations;
	(void)unused;
	start_wait();
	while (mix_local_completed < total) {
		struct mix_node *node;
		struct mix_local *item;
		unsigned int byte;
		CHECK(pthread_mutex_lock(&mix_local_mutex));
		while ((node = mix_remove(mix_local_work)) == NULL)
			CHECK(pthread_cond_wait(&mix_local_cv, &mix_local_mutex));
		CHECK(pthread_mutex_unlock(&mix_local_mutex));
		REQUIRE(node->index < MIX_LOCAL_SLOTS);
		item = &mix_local[node->index];
		CHECK(pthread_mutex_lock(&item->mutex));
		REQUIRE(item->sequence == item->acknowledged + 1);
		for (byte = 0; byte < MIX_MESSAGE_BYTES; ++byte)
			REQUIRE(item->message[byte] == mix_byte(node->index, item->sequence, byte));
		item->acknowledged = item->sequence;
		CHECK(pthread_cond_signal(&item->cv));
		CHECK(pthread_mutex_unlock(&item->mutex));
		mix_local_completed++;
	}
	return NULL;
}

static void mix_net_ast(unsigned __int64 token);

static unsigned int mix_net_read(unsigned int index)
{
	struct mix_connection *connection = &mix_connections[index];
	memset(&connection->iosb, 0, sizeof(connection->iosb));
	return sys$qio(EFN$C_ENF, connection->channel, IO$_READVBLK,
		(void *)&connection->iosb, mix_net_ast, (unsigned __int64)index,
		connection->received_message + connection->received,
		MIX_MESSAGE_BYTES - connection->received, 0, 0, 0, 0);
}

/* No allocation, stdio, mutex operation or ordinary CV signal in this AST.
 * A TCP read can complete with only part of the frame. Reissue the remaining
 * read and publish one queued message only when the entire frame is present,
 * or when an error needs to be reported by the worker outside AST context.
 */
static void mix_net_ast(unsigned __int64 token)
{
	unsigned int index = (unsigned int)token;
	struct mix_connection *connection = &mix_connections[index];
	mix_net_asts++;
	if ((connection->iosb.status & 1) && connection->iosb.count != 0 &&
	    connection->iosb.count <= MIX_MESSAGE_BYTES - connection->received) {
		connection->received += connection->iosb.count;
		if (connection->received < MIX_MESSAGE_BYTES) {
			unsigned int status;
			mix_net_partial++;
			status = mix_net_read(index);
			if (status & 1)
				return;
			connection->submit_status = status;
		}
	}
	mix_insert(mix_net_work, &mix_net_nodes[index]);
	connection->signal_status = pthread_cond_signal_int_np(&mix_net_cv);
	__MB();
	/* The worker must observe the completed signal before acknowledging a
	 * message. The sender cannot overwrite this slot before that handshake.
	 */
	connection->retired = 1;
}

static void mix_send_bytes(int socket_fd, const unsigned char *buffer, unsigned int length)
{
	while (length != 0) {
		int done = send(socket_fd, (const char *)buffer, length, 0);
		if (done < 0 && errno == EINTR) continue;
		if (done <= 0) {
			perror("sending loopback TCP message");
			exit(PTS_UNRESOLVED);
		}
		buffer += done;
		length -= (unsigned int)done;
	}
}

static void *mix_net_sender(void *unused)
{
	unsigned long sequence, random = pts_control.seed;
	unsigned int index, byte;
	(void)unused;
	start_wait();
	for (sequence = 1; sequence <= pts_control.iterations; ++sequence) {
		for (index = 0; index < MIX_CONNECTIONS; ++index) {
			struct mix_connection *connection = &mix_connections[index];
			unsigned int status;
			CHECK(pthread_mutex_lock(&connection->mutex));
			REQUIRE(connection->acknowledged == sequence - 1);
			connection->sequence = sequence;
			connection->received = 0;
			connection->retired = 0;
			connection->submit_status = SS$_NORMAL;
			connection->signal_status = 0;
			for (byte = 0; byte < MIX_MESSAGE_BYTES; ++byte)
				connection->sent[byte] = mix_byte(index, sequence, byte);
			memset(connection->received_message, 0xa5, MIX_MESSAGE_BYTES);
			CHECK(pthread_mutex_unlock(&connection->mutex));
			__MB();
			status = mix_net_read(index);
			if (!(status & 1)) native_failure("TCP SYS$QIO submission", status);
		}
		/* Two sends exercise stream framing; packet coalescing is allowed.
		 * Partial read completions are handled, never assumed to occur.
		 */
		for (index = 0; index < MIX_CONNECTIONS; ++index) {
			struct mix_connection *connection = &mix_connections[index];
			unsigned int first = 1 + (unsigned int)((sequence + index) % (MIX_MESSAGE_BYTES - 1));
			mix_send_bytes(connection->client, connection->sent, first);
			pts_yield(&random);
			mix_send_bytes(connection->client, connection->sent + first, MIX_MESSAGE_BYTES - first);
		}
		for (index = 0; index < MIX_CONNECTIONS; ++index) {
			struct mix_connection *connection = &mix_connections[index];
			CHECK(pthread_mutex_lock(&connection->mutex));
			while (connection->acknowledged != sequence)
				CHECK(pthread_cond_wait(&connection->cv, &connection->mutex));
			CHECK(pthread_mutex_unlock(&connection->mutex));
		}
	}
	return NULL;
}

static void *mix_net_worker(void *unused)
{
	unsigned long long total = (unsigned long long)MIX_CONNECTIONS * pts_control.iterations;
	(void)unused;
	start_wait();
	while (mix_net_completed < total) {
		struct mix_node *node;
		struct mix_connection *connection;
		CHECK(pthread_mutex_lock(&mix_net_mutex));
		while ((node = mix_remove(mix_net_work)) == NULL) {
			mix_net_waits++;
			CHECK(pthread_cond_wait(&mix_net_cv, &mix_net_mutex));
		}
		CHECK(pthread_mutex_unlock(&mix_net_mutex));
		REQUIRE(node->index < MIX_CONNECTIONS);
		connection = &mix_connections[node->index];
		while (!connection->retired) CHECK(pthread_yield_np());
		__MB();
		CHECK(connection->signal_status);
		if (!(connection->submit_status & 1))
			native_failure("TCP SYS$QIO reissue", connection->submit_status);
		if (!(connection->iosb.status & 1))
			native_failure("TCP QIO completion IOSB", connection->iosb.status);
		REQUIRE(connection->received == MIX_MESSAGE_BYTES);
		REQUIRE(memcmp(connection->received_message, connection->sent, MIX_MESSAGE_BYTES) == 0);
		CHECK(pthread_mutex_lock(&connection->mutex));
		REQUIRE(connection->sequence == connection->acknowledged + 1);
		connection->acknowledged = connection->sequence;
		CHECK(pthread_cond_signal(&connection->cv));
		CHECK(pthread_mutex_unlock(&connection->mutex));
		mix_net_completed++;
	}
	return NULL;
}

static void mix_socket_failure(const char *operation)
{
	perror(operation);
	exit(PTS_UNRESOLVED);
}

static void mix_setup(void)
{
	struct sockaddr_in address;
	socklen_t address_length = sizeof(address);
	unsigned int index;
	int listener, nodelay = 1;
	struct mix_node *nodes;
	size_t bytes = (3 + MIX_LOCAL_SLOTS + MIX_CONNECTIONS) * sizeof(*nodes);
	mix_queue_storage = calloc(1, bytes + 15);
	if (!mix_queue_storage) {
		fputs("Cannot allocate mixed communication queue nodes\n", stderr);
		exit(PTS_UNRESOLVED);
	}
	nodes = (struct mix_node *)(((uintptr_t)mix_queue_storage + 15) & ~(uintptr_t)15);
	mix_incoming = &nodes[0];
	mix_local_work = &nodes[1];
	mix_net_work = &nodes[2];
	mix_local_nodes = &nodes[3];
	mix_net_nodes = &nodes[3 + MIX_LOCAL_SLOTS];
	for (index = 0; index < MIX_LOCAL_SLOTS; ++index) {
		mix_local_nodes[index].index = index;
		CHECK(pthread_mutex_init(&mix_local[index].mutex, NULL));
		CHECK(pthread_cond_init(&mix_local[index].cv, NULL));
	}
	listener = socket(AF_INET, SOCK_STREAM, 0);
	if (listener < 0) mix_socket_failure("creating loopback TCP listener");
	memset(&address, 0, sizeof(address));
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = 0;
	if (bind(listener, (struct sockaddr *)&address, sizeof(address)) < 0)
		mix_socket_failure("binding loopback TCP listener");
	if (getsockname(listener, (struct sockaddr *)&address, &address_length) < 0)
		mix_socket_failure("querying loopback TCP listener");
	if (listen(listener, 1) < 0) mix_socket_failure("listening on loopback TCP");
	for (index = 0; index < MIX_CONNECTIONS; ++index) {
		struct mix_connection *connection = &mix_connections[index];
		int channel_number;
		mix_net_nodes[index].index = index;
		CHECK(pthread_mutex_init(&connection->mutex, NULL));
		CHECK(pthread_cond_init(&connection->cv, NULL));
		connection->client = socket(AF_INET, SOCK_STREAM, 0);
		if (connection->client < 0) mix_socket_failure("creating loopback TCP client");
		if (setsockopt(connection->client, IPPROTO_TCP, TCP_NODELAY,
			      (const char *)&nodelay, sizeof(nodelay)) < 0)
			mix_socket_failure("setting TCP_NODELAY");
		if (connect(connection->client, (struct sockaddr *)&address, sizeof(address)) < 0)
			mix_socket_failure("connecting loopback TCP client");
		connection->server = accept(listener, NULL, NULL);
		if (connection->server < 0) mix_socket_failure("accepting loopback TCP client");
		channel_number = decc$get_sdc(connection->server);
		if (channel_number < 0) mix_socket_failure("obtaining TCP socket QIO channel");
		connection->channel = (unsigned short)channel_number;
	}
	if (close(listener) < 0) mix_socket_failure("closing loopback TCP listener");
	printf("Mixed communication: TCP_connections=%u local_slots=%u helpers=%u "
	       "messages_per_path=%llu frame_bytes=%u\n", MIX_CONNECTIONS, MIX_LOCAL_SLOTS,
	       MIX_HELPERS, (unsigned long long)MIX_CONNECTIONS * pts_control.iterations,
	       MIX_MESSAGE_BYTES);
}

static void mix_finish(void)
{
	unsigned int index;
	REQUIRE(mix_local_completed == (unsigned long long)MIX_LOCAL_SLOTS * pts_control.iterations);
	REQUIRE(mix_net_completed == (unsigned long long)MIX_CONNECTIONS * pts_control.iterations);
	REQUIRE(mix_remove(mix_incoming) == NULL);
	REQUIRE(mix_remove(mix_local_work) == NULL);
	REQUIRE(mix_remove(mix_net_work) == NULL);
	for (index = 0; index < MIX_CONNECTIONS; ++index) {
		struct mix_connection *connection = &mix_connections[index];
		REQUIRE(connection->retired && connection->acknowledged == pts_control.iterations);
		/* Channels belong to the C RTL socket descriptors: close them once,
		 * after all QIOs and ASTs have retired; do not SYS$DASSGN them too.
		 */
		if (close(connection->server) < 0) mix_socket_failure("closing TCP server socket");
		if (close(connection->client) < 0) mix_socket_failure("closing TCP client socket");
	}
	printf("Mixed communication completed: local=%llu TCP=%llu "
	       "wake_calls=%llu hibernate_calls=%llu TCP_ASTs=%llu "
	       "partial_reads=%llu TCP_waits=%llu\n", mix_local_completed,
	       mix_net_completed, mix_wakes, mix_hibernations, mix_net_asts,
	       mix_net_partial, mix_net_waits);
	/* Keep static CVs and queue backing storage until image rundown. */
}

int test_main(int argc, char **argv)
{
	pthread_t *threads, helpers[MIX_HELPERS];
	struct worker_context *contexts;
	pthread_attr_t attr;
	unsigned long i, producers, consumers, used_waiters = 0;
	unsigned long long total, submitted = 0, completed = 0, reads = 0;
	unsigned long long writes = 0, synthetic = 0, early = 0, waited = 0;
	unsigned long long wait_calls = 0, retire_yields = 0;
	unsigned int status;
	char name[64];
	(void)argc; (void)argv;
#if !defined(PTS_NATIVE_UPCALLS) || !PTS_NATIVE_UPCALLS
	puts("PT_QIO_MIXED requires @BUILD with BOTH or UPCALLS for SYS$HIBER coordination.");
	return PTS_UNSUPPORTED;
#endif
	if (pts_control.threads < 2) {
		fputs("PT_QIO_MIXED needs at least two disk workers (submitter and waiter).\n",
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
	mix_setup();
	CHECK(pthread_attr_init(&attr));
	CHECK(pthread_attr_setstacksize(&attr, WORKER_STACK));
	printf("Mixed disk workload: submitters=%lu waiters=%lu slots=%u bytes=%u "
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
	CHECK(pthread_create(&helpers[0], &attr, mix_local_sender, NULL));
	CHECK(pthread_create(&helpers[1], &attr, mix_local_dispatcher, NULL));
	CHECK(pthread_create(&helpers[2], &attr, mix_local_worker, NULL));
	CHECK(pthread_create(&helpers[3], &attr, mix_net_sender, NULL));
	CHECK(pthread_create(&helpers[4], &attr, mix_net_worker, NULL));
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
	for (i = 0; i < MIX_HELPERS; ++i)
		CHECK(pthread_join(helpers[i], NULL));
	mix_finish();
	REQUIRE(issued == total && submitted == total && completed == total);
	REQUIRE(reads + writes + synthetic == total);
	REQUIRE(early + waited == total);
	REQUIRE(wait_calls > 0);
	REQUIRE(free_count == IO_SLOTS && ready_count == 0 && producers_left == 0);
	printf("Mixed disk completed=%llu writes=%llu reads=%llu declared_errors=%llu "
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
