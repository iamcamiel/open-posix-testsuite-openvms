/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel; X-1 CV Camiel Vanderhoeven 15-Nov-2026. */
#include "PTS_CHECK.H"
static pthread_mutex_t mutex;
static void *nonowner(void *unused)
{
	(void)unused;
	REQUIRE(pthread_mutex_unlock(&mutex) == EPERM);
	REQUIRE(pthread_mutex_trylock(&mutex) == EBUSY);
	return NULL;
}
int test_main(int argc, char **argv)
{
	pthread_mutexattr_t attr;
	pthread_t thread;
	(void)argc; (void)argv;
	CHECK(pthread_mutexattr_init(&attr));
	CHECK(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE));
	CHECK(pthread_mutex_init(&mutex, &attr));
	CHECK(pthread_mutex_lock(&mutex));
	CHECK(pthread_mutex_lock(&mutex));
	CHECK(pthread_mutex_trylock(&mutex));
	CHECK(pthread_mutex_unlock(&mutex));
	CHECK(pthread_mutex_unlock(&mutex));
	CHECK(pthread_mutex_unlock(&mutex));
	CHECK(pthread_mutex_destroy(&mutex));
	CHECK(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK));
	CHECK(pthread_mutex_init(&mutex, &attr));
	CHECK(pthread_mutexattr_destroy(&attr));
	CHECK(pthread_mutex_lock(&mutex));
	REQUIRE(pthread_mutex_lock(&mutex) == EDEADLK);
	CHECK(pthread_create(&thread, NULL, nonowner, NULL));
	CHECK(pthread_join(thread, NULL));
	CHECK(pthread_mutex_unlock(&mutex));
	REQUIRE(pthread_mutex_unlock(&mutex) == EPERM);
	CHECK(pthread_mutex_destroy(&mutex));
	return PTS_PASS;
}
