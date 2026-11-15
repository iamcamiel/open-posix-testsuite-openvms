/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel; X-1 CV Camiel Vanderhoeven 15-Nov-2026. */
#include "PTS_CHECK.H"
int test_main(int argc, char **argv)
{
	pthread_mutex_t mutex;
	pthread_mutexattr_t attr;
	pthread_cond_t cv;
	struct timeval current;
	struct timespec deadline;
	int rc;
	(void)argc; (void)argv;
	CHECK(pthread_mutexattr_init(&attr));
	CHECK(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK));
	CHECK(pthread_mutex_init(&mutex, &attr));
	CHECK(pthread_mutexattr_destroy(&attr));
	CHECK(pthread_cond_init(&cv, NULL));
	CHECK(pthread_mutex_lock(&mutex));
	REQUIRE(gettimeofday(&current, NULL) == 0);
	deadline.tv_sec = current.tv_sec;
	deadline.tv_nsec = current.tv_usec * 1000L + 25000000L;
	if (deadline.tv_nsec >= 1000000000L) {
		deadline.tv_nsec -= 1000000000L;
		deadline.tv_sec++;
	}
	/* Spurious wakes are permitted; repeat against the same deadline. */
	do {
		rc = pthread_cond_timedwait(&cv, &mutex, &deadline);
		REQUIRE(rc == 0 || rc == ETIMEDOUT);
	} while (rc == 0);
	/* ERRORCHECK makes this an ownership test without undefined behavior. */
	REQUIRE(pthread_mutex_lock(&mutex) == EDEADLK);
	CHECK(pthread_mutex_unlock(&mutex));
	CHECK(pthread_cond_destroy(&cv));
	CHECK(pthread_mutex_destroy(&mutex));
	return PTS_PASS;
}
