/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel; X-1 CV Camiel Vanderhoeven 15-Nov-2026. */
#include "PTS_CHECK.H"
static pthread_once_t once = PTHREAD_ONCE_INIT;
static pthread_key_t key;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static unsigned long ready, init_count;
static int go;
struct worker_state { unsigned long index, destructor_count; };
static void destructor(void *arg)
{
	struct worker_state *s = arg;
	s->destructor_count++;
}
static void initialize(void)
{
	CHECK(pthread_mutex_lock(&mutex));
	init_count++;
	CHECK(pthread_key_create(&key, destructor));
	CHECK(pthread_mutex_unlock(&mutex));
}
static void *worker(void *arg)
{
	struct worker_state *s = arg;
	unsigned long i, random = pts_control.seed + s->index;
	CHECK(pthread_mutex_lock(&mutex));
	ready++;
	CHECK(pthread_cond_broadcast(&cv));
	while (!go)
		CHECK(pthread_cond_wait(&cv, &mutex));
	CHECK(pthread_mutex_unlock(&mutex));
	CHECK(pthread_once(&once, initialize));
	REQUIRE(pthread_getspecific(key) == NULL);
	CHECK(pthread_setspecific(key, s));
	for (i = 0; i < pts_control.iterations; i++) {
		REQUIRE(pthread_getspecific(key) == s);
		pts_yield(&random);
	}
	return s;
}
int test_main(int argc, char **argv)
{
	struct worker_state states[256];
	pthread_t threads[256];
	unsigned long i;
	void *value;
	(void)argc; (void)argv;
	memset(states, 0, sizeof(states));
	CHECK(pthread_mutex_lock(&mutex));
	for (i = 0; i < pts_control.threads; i++) {
		states[i].index = i;
		CHECK(pthread_create(&threads[i], NULL, worker, &states[i]));
	}
	while (ready != pts_control.threads)
		CHECK(pthread_cond_wait(&cv, &mutex));
	go = 1;
	CHECK(pthread_cond_broadcast(&cv));
	CHECK(pthread_mutex_unlock(&mutex));
	for (i = 0; i < pts_control.threads; i++) {
		CHECK(pthread_join(threads[i], &value));
		REQUIRE(value == &states[i]);
		REQUIRE(states[i].destructor_count == 1);
	}
	REQUIRE(init_count == 1);
	CHECK(pthread_key_delete(key));
	CHECK(pthread_cond_destroy(&cv));
	CHECK(pthread_mutex_destroy(&mutex));
	return PTS_PASS;
}
