/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 17-Nov-2026:
 *     Original timer-AST tests based on a supplied reproducer's sequence:
 *     disable before creating workers, queue timers, timed wait, join, enable.
 *     Keep CRTL calls out of AST context and assert the resulting behavior.
 *     Separate quiet, printf-only, flushed, timestamped and worker-enable variants;
 *     count early callbacks independently of the order of printed messages.
 */
#ifndef PTS_AST_TIMER_COMMON_H
#define PTS_AST_TIMER_COMMON_H
#if defined(__VMS) && !defined(__NEW_STARLET)
#define __NEW_STARLET 1
#endif
#include "PTS_CHECK.H"

#ifndef __VMS
int test_main(int argc, char **argv)
{
	(void)argc; (void)argv;
	puts("Timer-AST tests require native OpenVMS with upcalls.");
	return PTS_UNSUPPORTED;
}
#else
#include <builtins.h>
#include <efndef.h>
#include <gen64def.h>
#include <ssdef.h>
#include <starlet.h>
#include <time.h>

#if PTS_AST_TIMER_ACTIVITY < 0 || PTS_AST_TIMER_ACTIVITY > 3
#error Invalid timer test activity mode
#endif
#if PTS_AST_TIMER_WORKER_ENABLE != 0 && PTS_AST_TIMER_WORKER_ENABLE != 1
#error Invalid timer test worker-enable mode
#endif

#define TIMER_ITEMS 3
#define TIMER_TOKEN_TAG ((unsigned __int64)0x41535454 << 32)

/* Counters have one serialized AST writer. Main reads them only while
 * SYS$SETAST excludes AST execution; volatile alone is not synchronization.
 * Static storage outlives every timer and is independent of pointer size.
 */
static volatile unsigned int timer_seen[TIMER_ITEMS];
static volatile unsigned int timer_total, timer_bad_token, timer_early;
static volatile unsigned int timer_release_started;

struct timer_case {
	unsigned int slot;
	long delay_ms, wait_ms;
	__int64 expiry_upper_bound;
	unsigned int enable_status;
};
static struct timer_case timer_cases[TIMER_ITEMS] = {
	{0, 200, 500, 0, 0},
	{1, 100, 350, 0, 0},
	{2, 150, 700, 0, 0}
};

struct timer_snapshot {
	unsigned int seen[TIMER_ITEMS], total, bad_token, early, previous_state;
};

static void native_failure(const char *operation, unsigned int status)
{
	fprintf(stderr, "%s returned OpenVMS status %08x\n", operation, status);
	exit(PTS_UNRESOLVED);
}

static unsigned int set_ast(int enabled)
{
	unsigned int status = sys$setast(enabled);
	if (status != SS$_WASSET && status != SS$_WASCLR)
		native_failure("SYS$SETAST", status);
	return status;
}

static void expect_ast_state(int enabled, unsigned int previous)
{
	unsigned int status = set_ast(enabled);
	if (status != previous) {
		fprintf(stderr, "SYS$SETAST(%d) previous state %08x, expected %08x\n",
			enabled, status, previous);
		exit(PTS_FAIL);
	}
}

static __int64 vms_time(void)
{
	__int64 value;
	unsigned int status = sys$gettim((GENERIC_64 *)&value);
	if (!(status & 1))
		native_failure("SYS$GETTIM", status);
	return value;
}

/* This is the whole AST: no time conversion, output, allocation, mutex,
 * condition-variable call, or other CRTL operation can run in this context.
 */
static void timer_ast(unsigned __int64 token)
{
	unsigned int item = (unsigned int)token;
	if ((token >> 32) != (TIMER_TOKEN_TAG >> 32)
			|| item == 0 || item > TIMER_ITEMS) {
		timer_bad_token = 1;
		return;
	}
	if (!timer_release_started)
		timer_early++;
	timer_seen[item - 1]++;
	timer_total++;
	__MB();
}

#if PTS_AST_TIMER_ACTIVITY == 2
static pthread_mutex_t crtl_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static void crtl_activity(unsigned int slot, const char *phase)
{
#if PTS_AST_TIMER_ACTIVITY == 1 || PTS_AST_TIMER_ACTIVITY == 3
	/* Deliberately rely on the CRTL's own output synchronization. There is
	 * no test mutex around either call, and no time/ctime/formatting step.
	 * Printed line order is not used to decide whether an AST ran early.
	 */
	if (printf("thread %u: %s\n", slot, phase) < 0) {
		fputs("timer test output failed\n", stderr);
		exit(PTS_UNRESOLVED);
	}
#if PTS_AST_TIMER_ACTIVITY == 3
	if (fflush(stdout) != 0) {
		fputs("timer test flush failed\n", stderr);
		exit(PTS_UNRESOLVED);
	}
#endif
#elif PTS_AST_TIMER_ACTIVITY == 2
	time_t now;
	char *converted;
	char text[160];
	int length;
	/* ctime uses shared storage. Serialize conversion, copying, and output
	 * in this test; none of these calls is made from an AST routine.
	 */
	CHECK(pthread_mutex_lock(&crtl_mutex));
	now = time(NULL);
	if (now == (time_t)-1) {
		perror("time");
		exit(PTS_UNRESOLVED);
	}
	converted = ctime(&now);
	if (!converted) {
		fputs("ctime failed\n", stderr);
		exit(PTS_UNRESOLVED);
	}
	length = snprintf(text, sizeof(text), "%.24s: thread %u: %s",
			  converted, slot, phase);
	REQUIRE(length > 0 && (size_t)length < sizeof(text));
	if (printf("%s\n", text) < 0 || fflush(stdout) != 0) {
		fputs("timer test output failed\n", stderr);
		exit(PTS_UNRESOLVED);
	}
	CHECK(pthread_mutex_unlock(&crtl_mutex));
#else
	(void)slot; (void)phase;
#endif
}

static void queue_timer(struct timer_case *test)
{
	__int64 delta = -(__int64)test->delay_ms * 10000;
	unsigned int status;
	crtl_activity(test->slot, "queue timer");
	status = sys$setimr(EFN$C_ENF, (GENERIC_64 *)&delta, timer_ast,
			   TIMER_TOKEN_TAG | (test->slot + 1), 0);
	if (!(status & 1))
		native_failure("SYS$SETIMR", status);
	/* The timer was queued before this timestamp. Waiting past this upper
	 * bound proves its requested expiration time has passed before enabling.
	 */
	test->expiry_upper_bound = vms_time() - delta;
}

static void timed_nap(unsigned int slot, long milliseconds)
{
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	struct timeval now;
	struct timespec deadline;
	int status;
	CHECK(pthread_mutex_init(&mutex, NULL));
	CHECK(pthread_cond_init(&condition, NULL));
	CHECK(pthread_mutex_lock(&mutex));
	if (gettimeofday(&now, NULL) != 0) {
		perror("gettimeofday");
		exit(PTS_UNRESOLVED);
	}
	deadline.tv_sec = now.tv_sec + milliseconds / 1000;
	deadline.tv_nsec = now.tv_usec * 1000L + (milliseconds % 1000) * 1000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec++;
	}
	crtl_activity(slot, "begin condition timed wait");
	/* The condition is never signalled. Spurious success is permitted and
	 * must not shorten the wait or move its original absolute deadline.
	 */
	do {
		status = pthread_cond_timedwait(&condition, &mutex, &deadline);
		if (status != 0 && status != ETIMEDOUT)
			pts_check(status, "pthread_cond_timedwait", __LINE__);
	} while (status == 0);
	if (gettimeofday(&now, NULL) != 0) {
		perror("gettimeofday");
		exit(PTS_UNRESOLVED);
	}
	REQUIRE(now.tv_sec > deadline.tv_sec
		|| (now.tv_sec == deadline.tv_sec
		    && now.tv_usec * 1000L >= deadline.tv_nsec));
	CHECK(pthread_mutex_unlock(&mutex));
	CHECK(pthread_cond_destroy(&condition));
	CHECK(pthread_mutex_destroy(&mutex));
	crtl_activity(slot, "condition wait timed out");
}

static void *timer_worker(void *argument)
{
	struct timer_case *test = argument;
	queue_timer(test);
	timed_nap(test->slot, test->wait_ms);
#if PTS_AST_TIMER_WORKER_ENABLE
	if (test->slot == 1) {
		crtl_activity(test->slot, "enable without a preceding disable");
		test->enable_status = set_ast(1);
		/* A new worker starts enabled. It cannot undo main's disable. */
		REQUIRE(test->enable_status == SS$_WASSET);
	}
#endif
	crtl_activity(test->slot, "worker returns");
	return test;
}

static struct timer_snapshot snapshot(void)
{
	struct timer_snapshot value;
	unsigned int i;
	value.previous_state = set_ast(0);
	__MB();
	for (i = 0; i < TIMER_ITEMS; ++i)
		value.seen[i] = timer_seen[i];
	value.total = timer_total;
	value.bad_token = timer_bad_token;
	value.early = timer_early;
	__MB();
	if (value.previous_state == SS$_WASSET)
		expect_ast_state(1, SS$_WASCLR);
	return value;
}

static void require_counts(struct timer_snapshot value, unsigned int each)
{
	if (value.bad_token || value.early || value.total != TIMER_ITEMS * each
			|| value.seen[0] != each || value.seen[1] != each
			|| value.seen[2] != each) {
		fprintf(stderr, "timer AST counts [%u,%u,%u], total %u, "
			"bad token %u, early %u; expected %u each and no early AST\n", value.seen[0],
			value.seen[1], value.seen[2], value.total,
			value.bad_token, value.early, each);
		exit(PTS_FAIL);
	}
}

int test_main(int argc, char **argv)
{
	pthread_t workers[2];
	struct timer_snapshot value;
	unsigned int i;
	unsigned long random = pts_control.seed;
	void *returned;
	__int64 now;
	const char *activity = PTS_AST_TIMER_ACTIVITY == 0 ? "none"
		: PTS_AST_TIMER_ACTIVITY == 1 ? "printf without explicit flush or test mutex"
		: PTS_AST_TIMER_ACTIVITY == 2 ? "timestamp/format/output with a test mutex"
		: "printf/fflush without a test mutex";
	(void)argc; (void)argv;
#if !defined(PTS_NATIVE_UPCALLS) || !PTS_NATIVE_UPCALLS
	puts("Timer-AST tests require @BUILD with BOTH or UPCALLS.");
	return PTS_UNSUPPORTED;
#endif
	printf("Timer ASTs: one sequence per process, normal-context activity: %s; "
	       "redundant worker enable: %s; iterations=%lu is not multiplied.\n",
	       activity, PTS_AST_TIMER_WORKER_ENABLE ? "yes" : "no",
	       pts_control.iterations);
	fflush(stdout);
	expect_ast_state(0, SS$_WASSET);
	crtl_activity(0, "main disabled ASTs before creating workers");
	queue_timer(&timer_cases[0]);
	for (i = 0; i < 2; ++i)
		CHECK(pthread_create(&workers[i], NULL, timer_worker, &timer_cases[i + 1]));
	timed_nap(0, timer_cases[0].wait_ms);
	for (i = 0; i < 2; ++i) {
		crtl_activity(0, "join worker while ASTs remain disabled");
		CHECK(pthread_join(workers[i], &returned));
		REQUIRE(returned == &timer_cases[i + 1]);
		value = snapshot();
		REQUIRE(value.previous_state == SS$_WASCLR);
		require_counts(value, 0);
	}
#if PTS_AST_TIMER_WORKER_ENABLE
	REQUIRE(timer_cases[1].enable_status == SS$_WASSET);
#else
	REQUIRE(timer_cases[1].enable_status == 0);
#endif
	now = vms_time();
	for (i = 0; i < TIMER_ITEMS; ++i)
		REQUIRE(now >= timer_cases[i].expiry_upper_bound);
	crtl_activity(0, "main re-enables ASTs after all timer deadlines");
	/* Publish while ASTs are still excluded. Legitimate pending callbacks
	 * may execute inside SETAST(1), before that service returns. Any callback
	 * during earlier output or waits retains an early count even if later
	 * printf lines would make its execution appear to have happened later.
	 */
	timer_release_started = 1;
	__MB();
	expect_ast_state(1, SS$_WASCLR);
	/* Losing delivery or spinning in the RTL is bounded by the independent
	 * runner watchdog, which does not depend on this process's AST state.
	 */
	for (;;) {
		value = snapshot();
		if (value.total >= TIMER_ITEMS || value.bad_token)
			break;
		pts_yield(&random);
		CHECK(pthread_yield_np());
	}
	require_counts(value, 1);
	/* Allow an erroneous late or duplicate callback to become observable. */
	timed_nap(0, 50);
	require_counts(snapshot(), 1);
#if PTS_AST_TIMER_ACTIVITY == 2
	CHECK(pthread_mutex_destroy(&crtl_mutex));
#endif
	puts("All timer deadlines elapsed while disabled; each AST delivered once.");
	return PTS_PASS;
}
#endif /* __VMS */
#endif /* PTS_AST_TIMER_COMMON_H */
