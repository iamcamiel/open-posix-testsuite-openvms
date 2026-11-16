#ifdef __VMS
#pragma module PT_COND_BROADCAST "X-2"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * X-2 CV Camiel Vanderhoeven 16-Nov-2026: Match the runner worker limit.
 * X-1 CV Camiel Vanderhoeven 15-Nov-2026: Initial supplemental test.
 */
#include "PTS_CHECK.H"
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ready_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t go_cv = PTHREAD_COND_INITIALIZER;
static unsigned long ready, completed;
static int go;
static void *waiter(void *unused)
{
	(void)unused;
	CHECK(pthread_mutex_lock(&mutex));
	ready++;
	CHECK(pthread_cond_signal(&ready_cv));
	while (!go)
		CHECK(pthread_cond_wait(&go_cv, &mutex));
	completed++;
	CHECK(pthread_mutex_unlock(&mutex));
	return NULL;
}
int test_main(int argc, char **argv)
{
	pthread_t threads[PTS_MAX_THREADS];
	unsigned long i;
	(void)argc; (void)argv;
	CHECK(pthread_mutex_lock(&mutex));
	for (i = 0; i < pts_control.threads; i++)
		CHECK(pthread_create(&threads[i], NULL, waiter, NULL));
	while (ready != pts_control.threads)
		CHECK(pthread_cond_wait(&ready_cv, &mutex));
	/* Acquiring this mutex after all ready reports proves the waiters
	 * have reached their waits; timing and volatile polls are unnecessary.
	 */
	go = 1;
	CHECK(pthread_cond_broadcast(&go_cv));
	CHECK(pthread_mutex_unlock(&mutex));
	for (i = 0; i < pts_control.threads; i++)
		CHECK(pthread_join(threads[i], NULL));
	REQUIRE(completed == pts_control.threads);
	CHECK(pthread_cond_destroy(&ready_cv));
	CHECK(pthread_cond_destroy(&go_cv));
	CHECK(pthread_mutex_destroy(&mutex));
	return PTS_PASS;
}
