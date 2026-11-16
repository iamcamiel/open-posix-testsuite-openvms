#ifdef __VMS
#pragma module PT_MUTEX_STRESS "X-2"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * X-2 CV Camiel Vanderhoeven 16-Nov-2026: Match the runner worker limit.
 * X-1 CV Camiel Vanderhoeven 15-Nov-2026: Initial supplemental test.
 */
#include "PTS_CHECK.H"
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned long count, mirror = ~0UL;
static void *worker(void *arg)
{
	unsigned long i, random = *(unsigned long *)arg + pts_control.seed;
	for (i = 0; i < pts_control.iterations; i++) {
		CHECK(pthread_mutex_lock(&mutex));
		REQUIRE(mirror == ~count);
		count++;
		pts_yield(&random);
		mirror = ~count;
		CHECK(pthread_mutex_unlock(&mutex));
		pts_yield(&random);
	}
	return NULL;
}
int test_main(int argc, char **argv)
{
	pthread_t threads[PTS_MAX_THREADS];
	unsigned long indices[PTS_MAX_THREADS], i;
	(void)argc; (void)argv;
	for (i = 0; i < pts_control.threads; i++) {
		indices[i] = i;
		CHECK(pthread_create(&threads[i], NULL, worker, &indices[i]));
	}
	for (i = 0; i < pts_control.threads; i++)
		CHECK(pthread_join(threads[i], NULL));
	REQUIRE(count == pts_control.threads * pts_control.iterations);
	REQUIRE(mirror == ~count);
	CHECK(pthread_mutex_destroy(&mutex));
	return PTS_PASS;
}
