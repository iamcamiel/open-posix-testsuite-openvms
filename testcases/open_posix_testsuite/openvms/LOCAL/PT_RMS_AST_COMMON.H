/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 23-Nov-2026: Exercise ordered RMS completions.
 */
#ifndef PTS_RMS_AST_COMMON_H
#define PTS_RMS_AST_COMMON_H
#include "PTS_CHECK.H"

#if !defined(__VMS) || !defined(PTS_NATIVE_UPCALLS) || !PTS_NATIVE_UPCALLS
int test_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	puts("RMS AST tests require OpenVMS with upcalls.");
	return PTS_UNSUPPORTED;
}
#else
#include <builtins.h>
#include <rms.h>
#include <rmsdef.h>
#include <starlet.h>

#define MAX_STREAMS 64
#define MAX_RECORDS 200000UL
#define RECORD_BYTES 512
#define WORKER_STACK (512 * 1024)

struct rms_stream {
	struct RAB rab;
	struct FAB fab;
	unsigned char record[RECORD_BYTES];
	pthread_cond_t retired_cv;
	unsigned long retired;
	unsigned int status;
	volatile unsigned int returned, completed;
};

/* RMS control blocks and record buffers must be addressable by 32-bit RMS. */
#pragma __pointer_size __save
#pragma __pointer_size 32
static char file_name[] = "PTS_RMS_AST.DAT;1";
static struct FAB fixture_fab;
static struct RAB fixture_rab;
static unsigned char fixture_record[RECORD_BYTES];
static struct rms_stream streams[MAX_STREAMS];
#pragma __pointer_size __restore

static pthread_mutex_t queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queued_cv = PTHREAD_COND_INITIALIZER;
static unsigned int pending[MAX_STREAMS], head, count;
static const char *phase;

static void rms_check(const char *operation, unsigned int status)
{
	if (!(status & 1)) {
		fprintf(stderr, "%s: %s returned RMS status %08x\n", phase, operation, status);
		exit(PTS_UNRESOLVED);
	}
}

static void set_file_name(struct FAB *fab)
{
	*fab = cc$rms_fab;
	fab->fab$l_fna = file_name;
	fab->fab$b_fns = sizeof(file_name) - 1;
}

static void fill_record(unsigned char *record, unsigned long sequence)
{
	unsigned int pattern = (unsigned int)pts_control.seed ^ (unsigned int)sequence;
	unsigned int byte;
	for (byte = 0; byte < RECORD_BYTES; ++byte) {
		pattern = pattern * 1664525U + 1013904223U;
		record[byte] = (unsigned char)(pattern >> 24);
	}
}

static void create_fixture(void)
{
	unsigned long sequence;
	set_file_name(&fixture_fab);
	fixture_fab.fab$b_fac = FAB$M_PUT;
	fixture_fab.fab$b_org = FAB$C_SEQ;
	fixture_fab.fab$b_rfm = FAB$C_FIX;
	fixture_fab.fab$w_mrs = RECORD_BYTES;
	rms_check("SYS$CREATE", sys$create(&fixture_fab, 0, 0));
	fixture_rab = cc$rms_rab;
	fixture_rab.rab$l_fab = &fixture_fab;
	fixture_rab.rab$b_rac = RAB$C_SEQ;
	fixture_rab.rab$l_rbf = fixture_record;
	fixture_rab.rab$w_rsz = RECORD_BYTES;
	fixture_rab.rab$b_mbc = 32;
	fixture_rab.rab$l_rop = RAB$M_WBH;
	rms_check("SYS$CONNECT", sys$connect(&fixture_rab, 0, 0));
	for (sequence = 0; sequence < pts_control.iterations; ++sequence) {
		fill_record(fixture_record, sequence);
		rms_check("SYS$PUT", sys$put(&fixture_rab, 0, 0));
	}
	rms_check("SYS$DISCONNECT", sys$disconnect(&fixture_rab, 0, 0));
	rms_check("SYS$CLOSE", sys$close(&fixture_fab, 0, 0));
}

static void open_stream(struct rms_stream *stream, int async)
{
	set_file_name(&stream->fab);
	stream->fab.fab$b_fac = FAB$M_GET;
	stream->fab.fab$b_shr = FAB$M_SHRGET;
	rms_check("SYS$OPEN", sys$open(&stream->fab, 0, 0));
	stream->rab = cc$rms_rab;
	stream->rab.rab$l_fab = &stream->fab;
	stream->rab.rab$b_rac = RAB$C_SEQ;
	stream->rab.rab$l_ubf = stream->record;
	stream->rab.rab$w_usz = RECORD_BYTES;
	stream->rab.rab$b_mbc = async ? 1 : 2;
	stream->rab.rab$b_mbf = 2;
	stream->rab.rab$l_rop = async ? 0 : RAB$M_RAH;
	rms_check("SYS$CONNECT", sys$connect(&stream->rab, 0, 0));
	if (async)
		stream->rab.rab$l_rop = RAB$M_ASY | RAB$M_SYNCSTS;
	stream->retired = 0;
	CHECK(pthread_cond_init(&stream->retired_cv, NULL));
}

static void close_stream(struct rms_stream *stream)
{
	CHECK(pthread_cond_destroy(&stream->retired_cv));
	stream->rab.rab$l_rop = 0;
	rms_check("SYS$DISCONNECT", sys$disconnect(&stream->rab, 0, 0));
	rms_check("SYS$CLOSE", sys$close(&stream->fab, 0, 0));
}

/* The RAB is the first member of the stream. */
static void read_complete(struct RAB *rab)
{
	struct rms_stream *stream = (struct rms_stream *)rab;
	__MB();
	stream->completed = 1;
}

static void *submit_reads(void *argument)
{
	struct rms_stream *stream = (struct rms_stream *)argument;
	unsigned int index = (unsigned int)(stream - streams);
	unsigned long sequence;
	for (sequence = 0; sequence < pts_control.iterations; ++sequence) {
		CHECK(pthread_mutex_lock(&queue_mutex));
		stream->returned = 0;
		stream->completed = 0;
		pending[(head + count) % MAX_STREAMS] = index;
		++count;
		CHECK(pthread_cond_signal(&queued_cv));
		CHECK(pthread_mutex_unlock(&queue_mutex));
		stream->status = sys$get(&stream->rab, read_complete, read_complete);
		__MB();
		stream->returned = 1;
		CHECK(pthread_mutex_lock(&queue_mutex));
		while (stream->retired == sequence)
			CHECK(pthread_cond_wait(&stream->retired_cv, &queue_mutex));
		CHECK(pthread_mutex_unlock(&queue_mutex));
	}
	return NULL;
}

static int read_phase(int async, const pthread_attr_t *attr)
{
	pthread_t workers[MAX_STREAMS];
	unsigned char expected[RECORD_BYTES];
	unsigned long long total, retired, callbacks = 0, inline_gets = 0;
	unsigned long long pending_gets = 0, waited = 0;
	unsigned int i, index;
	struct rms_stream *stream;

	phase = async ? "asynchronous RMS" : "synchronous RMS with read-ahead";
	printf("%s: %s, %lu streams, %lu records per stream\n", phase,
	       PTS_RMS_AST_YIELD ? "yield" : "spin", pts_control.threads,
	       pts_control.iterations);
	fflush(stdout);
	for (i = 0; i < pts_control.threads; ++i)
		open_stream(&streams[i], async);
	for (i = 0; i < pts_control.threads; ++i)
		CHECK(pthread_create(&workers[i], attr, submit_reads, &streams[i]));

	total = (unsigned long long)pts_control.threads * pts_control.iterations;
	for (retired = 0; retired < total; ++retired) {
		CHECK(pthread_mutex_lock(&queue_mutex));
		while (!count)
			CHECK(pthread_cond_wait(&queued_cv, &queue_mutex));
		index = pending[head];
		CHECK(pthread_mutex_unlock(&queue_mutex));
		stream = &streams[index];
		/* Allow submission to finish, including on a single VP. */
		while (!stream->returned)
			CHECK(pthread_yield_np());
		__MB();
		rms_check("SYS$GET", stream->status);
		if (stream->status == RMS$_SYNCH) {
			++inline_gets;
		} else {
			if (stream->status == RMS$_PENDING)
				++pending_gets;
			if (!stream->completed)
				++waited;
			while (!stream->completed) {
#if PTS_RMS_AST_YIELD
				CHECK(pthread_yield_np());
#endif
			}
			__MB();
			++callbacks;
		}
		rms_check("SYS$GET completion", stream->rab.rab$l_sts);
		fill_record(expected, stream->retired);
		if (stream->rab.rab$w_rsz != RECORD_BYTES ||
		    memcmp(expected, stream->record, RECORD_BYTES) != 0) {
			fprintf(stderr, "%s: wrong data in stream %u, record %lu, length %u\n",
				phase, index, stream->retired, (unsigned int)stream->rab.rab$w_rsz);
			exit(PTS_FAIL);
		}
		CHECK(pthread_mutex_lock(&queue_mutex));
		head = (head + 1) % MAX_STREAMS;
		--count;
		++stream->retired;
		CHECK(pthread_cond_signal(&stream->retired_cv));
		CHECK(pthread_mutex_unlock(&queue_mutex));
	}
	for (i = 0; i < pts_control.threads; ++i) {
		CHECK(pthread_join(workers[i], NULL));
		close_stream(&streams[i]);
	}
	printf("%s: %llu reads, %llu callbacks, %llu inline, %llu pending, %llu waits for AST\n",
	       phase, total, callbacks, inline_gets, pending_gets, waited);
	if (async && !pending_gets) {
		fprintf(stderr, "%s: no SYS$GET returned RMS$_PENDING\n", phase);
		return PTS_UNRESOLVED;
	}
	return PTS_PASS;
}

int test_main(int argc, char **argv)
{
	pthread_attr_t attr;
	int result;
	(void)argc;
	(void)argv;
	if (!pts_control.threads || pts_control.threads > MAX_STREAMS ||
	    !pts_control.iterations || pts_control.iterations > MAX_RECORDS) {
		fprintf(stderr, "RMS AST tests require 1..%u streams and 1..%lu records per stream\n",
			(unsigned int)MAX_STREAMS, MAX_RECORDS);
		return PTS_UNRESOLVED;
	}
	phase = "create fixture";
	create_fixture();
	CHECK(pthread_attr_init(&attr));
	CHECK(pthread_attr_setstacksize(&attr, WORKER_STACK));
	read_phase(0, &attr);
	result = read_phase(1, &attr);
	CHECK(pthread_attr_destroy(&attr));
	phase = "remove fixture";
	set_file_name(&fixture_fab);
	rms_check("SYS$ERASE", sys$erase(&fixture_fab, 0, 0));
	return result;
}
#endif
#endif
