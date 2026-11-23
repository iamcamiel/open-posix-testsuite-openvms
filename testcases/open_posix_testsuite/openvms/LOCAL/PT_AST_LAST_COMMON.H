/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 23-Nov-2026
 *     Test delivery of the last queued process AST after a remote enable.
 */
#include "PTS_CHECK.H"

#if !defined(__VMS) || !defined(PTS_NATIVE_UPCALLS) || !PTS_NATIVE_UPCALLS
int test_main(int argc, char **argv)
{
	(void)argc;
	(void)argv;
	puts("Final-AST tests require OpenVMS with thread upcalls.");
	return PTS_UNSUPPORTED;
}
#else
#include <builtins.h>
#include <jpidef.h>
#include <lib$routines.h>
#include <ssdef.h>
#include <starlet.h>

static int kernel_threads;
static volatile unsigned int setup_done;
static volatile unsigned int target_waiting, release_started;
static unsigned long initial_sequence;
static volatile unsigned int callbacks, bad_token, early, wrong_thread;
#define AST_TOKEN 0x41D7U

static void require_status(const char *operation, unsigned int status)
{
	if (!(status & 1)) {
		fprintf(stderr, "%s returned OpenVMS status %08x\n", operation, status);
		exit(PTS_UNRESOLVED);
	}
}

static void set_delivery(int enable, unsigned int expected)
{
	unsigned int status = sys$setast(enable);
	require_status("SYS$SETAST", status);
	REQUIRE(status == expected);
}

static void final_ast(unsigned __int64 token)
{
	/* Application ASTs are serialized. Publish the observations together
	 * before waking either polling thread with the aligned longword count. */
	if (!release_started)
		early = 1;
	if (token != AST_TOKEN)
		bad_token = 1;
	/* The OpenVMS accessor reads the current thread environment block. */
	if (pthread_getselfseq_np() != initial_sequence)
		wrong_thread = 1;
	__MB();
	++callbacks;
}

static void *post_final_ast(void *unused)
{
	int item = JPI$_KT_COUNT;
	int attempt;
	(void)unused;
	/* Both threads remain runnable during setup. A new VP may still be
	 * starting immediately after pthread_create. */
	for (attempt = 0; attempt < 32; ++attempt) {
		require_status("LIB$GETJPI(KT_COUNT)",
				lib$getjpi(&item, 0, 0, &kernel_threads));
		if (kernel_threads >= 2)
			break;
		CHECK(pthread_yield_np());
	}
	if (kernel_threads >= 2) {
		set_delivery(0, SS$_WASSET);
		require_status("SYS$DCLAST", sys$dclast(final_ast, AST_TOKEN, 0));
	}
	__MB();
	setup_done = 1;
	if (kernel_threads < 2)
		return NULL;

	/* Yield only during setup, while our process AST fence is closed. */
	while (!target_waiting)
		CHECK(pthread_yield_np());
	__MB();
#if PTS_LAST_JOIN
	/* Give T1 an opportunity to enter join before releasing the fence. */
	CHECK(pthread_yield_np());
#endif
	REQUIRE(callbacks == 0);
	release_started = 1;
	__MB();
	set_delivery(1, SS$_WASCLR);

	/* Thread exit itself is a scheduling opportunity. Hold the worker
	 * here until the AST runs, with no timer, yield or other RTL entry. */
	while (!callbacks)
		;
	return NULL;
}

int test_main(int argc, char **argv)
{
	pthread_t worker;
	(void)argc;
	(void)argv;
	puts(PTS_LAST_JOIN ? "Final AST with initial thread joining its worker"
	                   : "Final AST with initial thread polling in user code");
	fflush(stdout);
	initial_sequence = pthread_getselfseq_np();
	CHECK(pthread_create(&worker, NULL, post_final_ast, NULL));
	while (!setup_done)
		CHECK(pthread_yield_np());
	__MB();
	if (kernel_threads < 2) {
		CHECK(pthread_join(worker, NULL));
		puts("Final-AST tests need at least two kernel threads in this process.");
		return PTS_UNSUPPORTED;
	}
	__MB();
	target_waiting = 1;
#if !PTS_LAST_JOIN
	while (!callbacks)
		;
#endif
	CHECK(pthread_join(worker, NULL));
	/* This fence also waits for an executing process AST to finish. */
	set_delivery(0, SS$_WASSET);
	__MB();
	REQUIRE(callbacks == 1);
	REQUIRE(!early && !bad_token && !wrong_thread);
	set_delivery(1, SS$_WASCLR);
	REQUIRE(callbacks == 1);
	return PTS_PASS;
}
#endif
