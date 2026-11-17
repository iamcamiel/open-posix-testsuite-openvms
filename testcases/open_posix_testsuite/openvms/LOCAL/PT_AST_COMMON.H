/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 17-Nov-2026:
 *     Native disabled-AST regressions with ordinary pthread handoffs that
 *     remain independent of AST completion. No private RTL interfaces.
 */
#ifndef PTS_AST_COMMON_H
#define PTS_AST_COMMON_H
#include "PTS_CHECK.H"

#ifndef __VMS
int test_main(int argc, char **argv)
{
	(void)argc; (void)argv;
	puts("Disabled-AST tests require native OpenVMS with upcalls.");
	return PTS_UNSUPPORTED;
}
#else
#include <builtins.h>
#include <ssdef.h>
#include <starlet.h>

#define AST_ITEMS 4
#define AST_HELPERS 2
#define COMMAND_NONE 0
#define COMMAND_PING 1
#define COMMAND_DISABLE 2
#define COMMAND_ENABLE 3
#define COMMAND_QUIT 4

/* Process ASTs are serialized by the upcall-enabled Threads Library. The
 * callback is the only writer; main reads/resets these fields only while
 * SYS$SETAST excludes AST execution. Volatile is for AST visibility, not a
 * substitute for the SETAST exclusion or the mutex protecting commands.
 */
static volatile unsigned int ast_seen[AST_ITEMS];
static volatile unsigned int ast_total, ast_bad_token;
static volatile unsigned int callback_queue_status, callback_disable_status;
static volatile unsigned int ast_generation;
static unsigned long iteration;

struct ast_snapshot {
	unsigned int seen[AST_ITEMS], total, bad_token;
	unsigned int queue_status, disable_status, previous_state;
};

struct ast_helper {
	int command, reply;
};
static struct ast_helper helpers[AST_HELPERS];
static pthread_mutex_t command_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t command_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t reply_cv = PTHREAD_COND_INITIALIZER;

static void native_failure(const char *operation, unsigned int status)
{
	fprintf(stderr, "iteration %lu: %s returned OpenVMS status %08x\n",
		iteration, operation, status);
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
		fprintf(stderr, "iteration %lu: SYS$SETAST(%d) previous state "
			"%08x, expected %08x\n", iteration, enabled, status,
			previous);
		exit(PTS_FAIL);
	}
}

/* No allocation, formatting, mutexes, or ordinary condition signals here.
 * The callback variant declares its follower before explicitly disabling:
 * the follower cannot execute at the same AST level before this AST returns.
 */
static void ast_callback(unsigned __int64 token)
{
	unsigned int item = (unsigned int)token;
	unsigned int generation = (unsigned int)(token >> 32);
	if (item >= AST_ITEMS || generation != ast_generation) {
		ast_bad_token = 1;
		return;
	}
#if PTS_AST_SCENARIO == 3
	if (item == 0) {
		callback_queue_status = sys$dclast(ast_callback,
				((unsigned __int64)generation << 32) | 1, 0);
		callback_disable_status = sys$setast(0);
	}
#endif
	ast_seen[item]++;
	ast_total++;
	__MB();
}

static void declare_ast(unsigned int item)
{
	unsigned int status = sys$dclast(ast_callback,
			((unsigned __int64)iteration << 32) | item, 0);
	if (!(status & 1))
		native_failure("SYS$DCLAST", status);
}

static struct ast_snapshot snapshot(void)
{
	struct ast_snapshot value;
	unsigned int i;
	value.previous_state = set_ast(0);
	__MB();
	for (i = 0; i < AST_ITEMS; ++i)
		value.seen[i] = ast_seen[i];
	value.total = ast_total;
	value.bad_token = ast_bad_token;
	value.queue_status = callback_queue_status;
	value.disable_status = callback_disable_status;
	__MB();
	/* Preserve a callback's disable and the caller's explicit disable.
	 * A snapshot must never accidentally release pending callbacks.
	 */
	if (value.previous_state == SS$_WASSET)
		expect_ast_state(1, SS$_WASCLR);
	return value;
}

static void require_counts(struct ast_snapshot value, unsigned int mask)
{
	unsigned int i, expected_total = 0;
	int valid = value.bad_token == 0;
	for (i = 0; i < AST_ITEMS; ++i) {
		unsigned int expected = (mask >> i) & 1;
		expected_total += expected;
		if (value.seen[i] != expected)
			valid = 0;
	}
	if (value.total != expected_total)
		valid = 0;
	if (!valid) {
		fprintf(stderr, "iteration %lu: AST counts [%u,%u,%u,%u], "
			"total %u, bad token %u; expected mask %x\n", iteration,
			value.seen[0], value.seen[1], value.seen[2], value.seen[3],
			value.total, value.bad_token, mask);
		exit(PTS_FAIL);
	}
}

static void reset_counts(void)
{
	unsigned int i;
	expect_ast_state(0, SS$_WASSET);
	__MB();
	/* Recheck the previous round under exclusion before clearing it. A late
	 * callback before this point is detected here; after this point its
	 * generation will no longer match. Never recycle a token silently.
	 */
	if (iteration > 1) {
#if PTS_AST_SCENARIO == 3
		require_counts(snapshot(), 3);
#else
		require_counts(snapshot(), (1U << AST_ITEMS) - 1);
#endif
	}
	for (i = 0; i < AST_ITEMS; ++i)
		ast_seen[i] = 0;
	ast_total = ast_bad_token = 0;
	callback_queue_status = callback_disable_status = 0;
	ast_generation = (unsigned int)iteration;
	__MB();
	expect_ast_state(1, SS$_WASCLR);
}

/* Loss of an AST or an RTL livelock is bounded by the runner's independent
 * process watchdog. No timer AST is needed to escape a disabled window.
 * Snapshots preserve AST state; yielding gives pending delivery a chance.
 */
static struct ast_snapshot await_total(unsigned int total, unsigned long *random)
{
	struct ast_snapshot value;
	for (;;) {
		value = snapshot();
		if (value.total >= total || value.bad_token)
			return value;
		pts_yield(random);
		CHECK(pthread_yield_np());
	}
}

static void *helper_main(void *argument)
{
	struct ast_helper *helper = argument;
	int command;
	for (;;) {
		CHECK(pthread_mutex_lock(&command_mutex));
		while (helper->command == COMMAND_NONE)
			CHECK(pthread_cond_wait(&command_cv, &command_mutex));
		command = helper->command;
		helper->command = COMMAND_NONE;
		CHECK(pthread_mutex_unlock(&command_mutex));
		if (command == COMMAND_DISABLE) {
			expect_ast_state(0, SS$_WASSET);
			expect_ast_state(0, SS$_WASCLR);
		} else if (command == COMMAND_ENABLE)
			expect_ast_state(1, SS$_WASCLR);
		else if (command == COMMAND_PING)
			CHECK(pthread_yield_np());
		CHECK(pthread_mutex_lock(&command_mutex));
		helper->reply = command;
		CHECK(pthread_cond_signal(&reply_cv));
		CHECK(pthread_mutex_unlock(&command_mutex));
		if (command == COMMAND_QUIT)
			return NULL;
	}
}

static void command_helper(unsigned int index, int command)
{
	struct ast_helper *helper = &helpers[index];
	CHECK(pthread_mutex_lock(&command_mutex));
	helper->reply = COMMAND_NONE;
	helper->command = command;
	/* Both helpers share this CV; signal could wake the wrong one. */
	CHECK(pthread_cond_broadcast(&command_cv));
	while (helper->reply != command)
		CHECK(pthread_cond_wait(&reply_cv, &command_mutex));
	CHECK(pthread_mutex_unlock(&command_mutex));
}

static void handoffs(unsigned long *random)
{
	unsigned int i;
	for (i = 0; i < 4; ++i) {
		command_helper(i % AST_HELPERS, COMMAND_PING);
		pts_yield(random);
		CHECK(pthread_yield_np());
	}
}

int test_main(int argc, char **argv)
{
	pthread_t threads[AST_HELPERS];
	unsigned int i;
	unsigned long random = pts_control.seed;
	struct ast_snapshot value;
	(void)argc; (void)argv;
#if !defined(PTS_NATIVE_UPCALLS) || !PTS_NATIVE_UPCALLS
	puts("Disabled-AST tests require @BUILD with BOTH or UPCALLS.");
	return PTS_UNSUPPORTED;
#endif
	printf("AST scenario %d: %lu iterations, two helper threads; "
	       "runner watchdog bounds lost delivery or stalled handoffs.\n",
	       PTS_AST_SCENARIO, pts_control.iterations);
	fflush(stdout);
	(void)set_ast(1);
	for (i = 0; i < AST_HELPERS; ++i)
		CHECK(pthread_create(&threads[i], NULL, helper_main, &helpers[i]));
	for (iteration = 1; iteration <= pts_control.iterations; ++iteration) {
		reset_counts();
#if PTS_AST_SCENARIO == 1
		expect_ast_state(0, SS$_WASSET);
		expect_ast_state(0, SS$_WASCLR);
		for (i = 0; i < AST_ITEMS; ++i)
			declare_ast(i);
		handoffs(&random);
		value = snapshot();
		REQUIRE(value.previous_state == SS$_WASCLR);
		require_counts(value, 0);
		/* Repeated disabling is not a nesting count for this thread. */
		expect_ast_state(1, SS$_WASCLR);
		require_counts(await_total(AST_ITEMS, &random), (1U << AST_ITEMS) - 1);
#elif PTS_AST_SCENARIO == 2
		command_helper(0, COMMAND_DISABLE);
		command_helper(1, COMMAND_DISABLE);
		for (i = 0; i < AST_ITEMS; ++i)
			declare_ast(i);
		handoffs(&random);
		value = snapshot();
		REQUIRE(value.previous_state == SS$_WASSET);
		require_counts(value, 0);
		command_helper(0, COMMAND_ENABLE);
		handoffs(&random);
		value = snapshot();
		REQUIRE(value.previous_state == SS$_WASSET);
		require_counts(value, 0);
		command_helper(1, COMMAND_ENABLE);
		require_counts(await_total(AST_ITEMS, &random), (1U << AST_ITEMS) - 1);
#elif PTS_AST_SCENARIO == 3
		declare_ast(0);
		value = await_total(1, &random);
		require_counts(value, 1);
		if (!(value.queue_status & 1))
			native_failure("SYS$DCLAST from AST", value.queue_status);
		REQUIRE(value.disable_status == SS$_WASSET);
		REQUIRE(value.previous_state == SS$_WASCLR);
		handoffs(&random);
		value = snapshot();
		REQUIRE(value.previous_state == SS$_WASCLR);
		require_counts(value, 1);
		expect_ast_state(1, SS$_WASCLR);
		require_counts(await_total(2, &random), 3);
#else
#error Unknown disabled-AST scenario
#endif
		/* Give a late or duplicate delivery a chance to become observable
		 * before any next-round reset or normal image rundown.
		 */
		handoffs(&random);
		value = snapshot();
		REQUIRE(value.previous_state == SS$_WASSET);
#if PTS_AST_SCENARIO == 3
		require_counts(value, 3);
#else
		require_counts(value, (1U << AST_ITEMS) - 1);
#endif
	}
	for (i = 0; i < AST_HELPERS; ++i) {
		command_helper(i, COMMAND_QUIT);
		CHECK(pthread_join(threads[i], NULL));
	}
	CHECK(pthread_cond_destroy(&command_cv));
	CHECK(pthread_cond_destroy(&reply_cv));
	CHECK(pthread_mutex_destroy(&command_mutex));
	return PTS_PASS;
}
#endif /* __VMS */
#endif /* PTS_AST_COMMON_H */
