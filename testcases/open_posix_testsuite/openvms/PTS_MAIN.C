#ifdef __VMS
#pragma module PTS_MAIN "X-2"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-2  CV  Camiel Vanderhoeven  16-Nov-2026:
 *     Select one result reporter before file creation and process exit.
 * X-1  CV  Camiel Vanderhoeven  15-Nov-2026: Native result adapter.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#ifdef __VMS
#include <builtins.h>
#include <pthread.h>
#include <ssdef.h>
#include <starlet.h>
#endif
#include "PTS_PORT.H"

struct pts_control pts_control;
extern int test_main(int argc, char **argv);

/* 0: unclaimed, 1: PASS reporter, 2: non-PASS reporter. The result kind is
 * part of the atomic claim, so contenders need no separate publication flag.
 * No pthread lock protects reporting: the caller may already hold test locks.
 */
static volatile int result_reporter;

static int claim_result(int kind)
{
#ifdef __VMS
	/* Alpha's store-conditional may fail without another reporter winning.
	 * Retry until this claim succeeds or an actual competing claim is visible.
	 */
	while (result_reporter == 0) {
#ifdef __ia64
		if (__CMP_SWAP_LONG((volatile void *)&result_reporter, 0, kind))
#else
		if (__CMP_STORE_LONG((volatile void *)&result_reporter, 0, kind,
				     (volatile void *)&result_reporter))
#endif
			return 1;
	}
	return 0;
#else
	return __sync_bool_compare_and_swap(&result_reporter, 0, kind);
#endif
}

static void await_reporter(int code)
{
	/* An earlier failure must never be replaced by a later PASS. Conflicting
	 * failure versus an already claimed PASS is conservatively abnormal; do
	 * not let a late failure disappear behind a successful process exit.
	 */
	if (code != 0 && result_reporter == 1) {
#ifdef __VMS
		sys$exit(SS$_ABORT);
#else
		_Exit(EXIT_FAILURE);
#endif
	}
	for (;;) {
#ifdef __VMS
		/* SYS$HIBER could park the only VP while the reporting user thread
		 * is descheduled in the NONE profile. Yield through the scheduler
		 * instead. This is an ordinary test error path, not an RTL fatal
		 * handler; the external watchdog remains the fallback if it stalls.
		 */
		(void)pthread_yield_np();
#else
		(void)pause();
#endif
	}
}

void pts_exit(int code)
{
	FILE *out;
	int fd, ok;

	if (!claim_result(code == 0 ? 1 : 2))
		await_reporter(code);

	/* Only the first reporter writes or invokes normal CRTL exit processing.
	 * Exclusive creation still prevents a stale file from passing.
	 * The native runner supplies an explicit ;1 version on OpenVMS.
	 */
	fd = open(pts_control.result, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		perror("creating PTS result");
		exit(EXIT_FAILURE);
	}
	out = fdopen(fd, "w");
	if (!out) {
		close(fd);
		exit(EXIT_FAILURE);
	}
	ok = fprintf(out, "PTS1|%s|%s|%d\n", pts_control.id,
		     pts_control.token, code) > 0;
	if (fclose(out) != 0)
		ok = 0;
	fflush(stdout);
	fflush(stderr);
	/* The record carries the POSIX result; process status proves a clean
	 * shutdown. A crash after writing PASS must not become a pass.
	 */
	exit(ok ? EXIT_SUCCESS : EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	if (pts_read_control(stdin, &pts_control) != 0) {
		fprintf(stderr, "Invalid or missing runner control input\n");
		return EXIT_FAILURE;
	}
	if (chdir(pts_control.directory) != 0) {
		perror("entering test directory");
		return EXIT_FAILURE;
	}
	printf("PTS case=%s seed=%lu threads=%lu iterations=%lu pointer_bits=%u\n",
	       pts_control.id, pts_control.seed, pts_control.threads,
	       pts_control.iterations, (unsigned int)(sizeof(void *) * 8));
	fflush(stdout);
	pts_exit(test_main(argc, argv));
	return EXIT_FAILURE;
}
