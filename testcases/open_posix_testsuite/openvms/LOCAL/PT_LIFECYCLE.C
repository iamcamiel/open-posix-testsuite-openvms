/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel; X-1 CV Camiel Vanderhoeven 15-Nov-2026. */
#include "PTS_CHECK.H"
struct state {
	pthread_mutex_t mutex;
	pthread_cond_t condition;
	int ready, go, done;
	pthread_t identity;
	char payload;
};
static struct state s = {
	.mutex = PTHREAD_MUTEX_INITIALIZER,
	.condition = PTHREAD_COND_INITIALIZER
};
static void *joinable(void *arg)
{
	pthread_t *identity = arg;
	CHECK(pthread_mutex_lock(&s.mutex));
	*identity = pthread_self();
	s.ready = 1;
	CHECK(pthread_cond_signal(&s.condition));
	while (!s.go)
		CHECK(pthread_cond_wait(&s.condition, &s.mutex));
	CHECK(pthread_mutex_unlock(&s.mutex));
	pthread_exit(&s.payload);
	return NULL;
}
static void *detached(void *arg)
{
	struct state *st = arg;
	CHECK(pthread_mutex_lock(&st->mutex));
	st->ready = 1;
	CHECK(pthread_cond_signal(&st->condition));
	while (!st->go)
		CHECK(pthread_cond_wait(&st->condition, &st->mutex));
	st->done = 1;
	CHECK(pthread_cond_signal(&st->condition));
	CHECK(pthread_mutex_unlock(&st->mutex));
	return NULL;
}
int test_main(int argc, char **argv)
{
	pthread_t thread;
	void *value;
	pthread_attr_t attr;
	int mode;
	(void)argc; (void)argv;
	CHECK(pthread_attr_init(&attr));
	CHECK(pthread_attr_getdetachstate(&attr, &mode));
	REQUIRE(mode == PTHREAD_CREATE_JOINABLE);
	CHECK(pthread_mutex_lock(&s.mutex));
	CHECK(pthread_create(&thread, &attr, joinable, &s.identity));
	while (!s.ready)
		CHECK(pthread_cond_wait(&s.condition, &s.mutex));
	REQUIRE(pthread_equal(thread, s.identity));
	REQUIRE(!pthread_equal(pthread_self(), s.identity));
	s.go = 1;
	CHECK(pthread_cond_signal(&s.condition));
	CHECK(pthread_mutex_unlock(&s.mutex));
	CHECK(pthread_join(thread, &value));
	/* The joined handle is expired. Do not call pthread_equal on it. */
	REQUIRE(value == &s.payload);
	CHECK(pthread_attr_destroy(&attr));

	CHECK(pthread_mutex_lock(&s.mutex));
	s.ready = s.go = 0;
	CHECK(pthread_create(&thread, NULL, detached, &s));
	while (!s.ready)
		CHECK(pthread_cond_wait(&s.condition, &s.mutex));
	/* The worker is alive and blocked, so detaching this handle is legal. */
	CHECK(pthread_detach(thread));
	s.go = 1;
	CHECK(pthread_cond_signal(&s.condition));
	while (!s.done)
		CHECK(pthread_cond_wait(&s.condition, &s.mutex));
	CHECK(pthread_mutex_unlock(&s.mutex));
	/* Worker has released the mutex and makes no further accesses to s. */
	CHECK(pthread_cond_destroy(&s.condition));
	CHECK(pthread_mutex_destroy(&s.mutex));
	return PTS_PASS;
}
