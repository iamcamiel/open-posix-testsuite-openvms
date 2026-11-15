/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel; X-1 CV Camiel Vanderhoeven 15-Nov-2026. */
#include "PTS_CHECK.H"
static pthread_mutex_t mutex;
static pthread_cond_t ready_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t wait_cv = PTHREAD_COND_INITIALIZER;
static int ready, cleaned;
static void cleanup(void *unused)
{
	(void)unused;
	/* POSIX cancellation of cond_wait must reacquire the mutex first. */
	REQUIRE(pthread_mutex_lock(&mutex) == EDEADLK);
	cleaned++;
	CHECK(pthread_mutex_unlock(&mutex));
}
static void *worker(void *unused)
{
	(void)unused;
	CHECK(pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL));
	CHECK(pthread_setcanceltype(PTHREAD_CANCEL_DEFERRED, NULL));
	CHECK(pthread_mutex_lock(&mutex));
	pthread_cleanup_push(cleanup, NULL);
	ready = 1;
	CHECK(pthread_cond_signal(&ready_cv));
	for (;;)
		CHECK(pthread_cond_wait(&wait_cv, &mutex));
	pthread_cleanup_pop(1);
	return NULL;
}
int test_main(int argc, char **argv)
{
	pthread_mutexattr_t attr;
	pthread_t thread;
	void *value = NULL;
	(void)argc; (void)argv;
	CHECK(pthread_mutexattr_init(&attr));
	CHECK(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK));
	CHECK(pthread_mutex_init(&mutex, &attr));
	CHECK(pthread_mutexattr_destroy(&attr));
	CHECK(pthread_mutex_lock(&mutex));
	CHECK(pthread_create(&thread, NULL, worker, NULL));
	while (!ready)
		CHECK(pthread_cond_wait(&ready_cv, &mutex));
	CHECK(pthread_cancel(thread));
	CHECK(pthread_mutex_unlock(&mutex));
	CHECK(pthread_join(thread, &value));
	REQUIRE(value == PTHREAD_CANCELED);
	REQUIRE(cleaned == 1);
	CHECK(pthread_mutex_trylock(&mutex));
	CHECK(pthread_mutex_unlock(&mutex));
	CHECK(pthread_cond_destroy(&ready_cv));
	CHECK(pthread_cond_destroy(&wait_cv));
	CHECK(pthread_mutex_destroy(&mutex));
	return PTS_PASS;
}
