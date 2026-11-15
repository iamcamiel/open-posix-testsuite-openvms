/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel; X-1 CV Camiel Vanderhoeven 15-Nov-2026. */
#include "PTS_CHECK.H"
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t condition = PTHREAD_COND_INITIALIZER;
static int turn;
static unsigned long transfers;
static void *worker(void *unused)
{
	unsigned long i, random = pts_control.seed;
	(void)unused;
	for (i = 0; i < pts_control.iterations; i++) {
		CHECK(pthread_mutex_lock(&mutex));
		while (turn != 1)
			CHECK(pthread_cond_wait(&condition, &mutex));
		REQUIRE(transfers == i * 2 + 1);
		transfers++;
		turn = 0;
		CHECK(pthread_cond_signal(&condition));
		CHECK(pthread_mutex_unlock(&mutex));
		pts_yield(&random);
	}
	return NULL;
}
int test_main(int argc, char **argv)
{
	pthread_t thread;
	unsigned long i, random = pts_control.seed + 1;
	(void)argc; (void)argv;
	CHECK(pthread_cond_signal(&condition));
	CHECK(pthread_cond_broadcast(&condition));
	CHECK(pthread_create(&thread, NULL, worker, NULL));
	for (i = 0; i < pts_control.iterations; i++) {
		CHECK(pthread_mutex_lock(&mutex));
		while (turn != 0)
			CHECK(pthread_cond_wait(&condition, &mutex));
		REQUIRE(transfers == i * 2);
		transfers++;
		turn = 1;
		CHECK(pthread_cond_signal(&condition));
		CHECK(pthread_mutex_unlock(&mutex));
		pts_yield(&random);
	}
	CHECK(pthread_join(thread, NULL));
	REQUIRE(transfers == pts_control.iterations * 2);
	CHECK(pthread_cond_destroy(&condition));
	CHECK(pthread_mutex_destroy(&mutex));
	return PTS_PASS;
}
