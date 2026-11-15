/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel; X-1 CV Camiel Vanderhoeven 15-Nov-2026. */
#include "PTS_CHECK.H"
static pthread_rwlock_t rwlock;
static unsigned long value, complement = ~0UL;
struct state { unsigned long index; int writer_held; };
static void *probe(void *arg)
{
	struct state *s = arg;
	int rc = pthread_rwlock_tryrdlock(&rwlock);
	if (s->writer_held) {
		REQUIRE(rc == EBUSY);
	} else {
		CHECK(rc);
		CHECK(pthread_rwlock_unlock(&rwlock));
	}
	REQUIRE(pthread_rwlock_trywrlock(&rwlock) == EBUSY);
	return NULL;
}
static void *worker(void *arg)
{
	struct state *s = arg;
	unsigned long i, random = pts_control.seed + s->index;
	for (i = 0; i < pts_control.iterations; i++) {
		if ((s->index & 1) == 0) {
			CHECK(pthread_rwlock_wrlock(&rwlock));
			value++;
			complement = ~value;
		} else {
			CHECK(pthread_rwlock_rdlock(&rwlock));
			REQUIRE(complement == ~value);
		}
		CHECK(pthread_rwlock_unlock(&rwlock));
		pts_yield(&random);
	}
	return NULL;
}
int test_main(int argc, char **argv)
{
	pthread_rwlockattr_t attr;
	pthread_t threads[256], child;
	struct state states[256], p;
	unsigned long i;
	int scope;
	(void)argc; (void)argv;
	CHECK(pthread_rwlockattr_init(&attr));
	CHECK(pthread_rwlockattr_getpshared(&attr, &scope));
	REQUIRE(scope == PTHREAD_PROCESS_PRIVATE);
	CHECK(pthread_rwlock_init(&rwlock, &attr));
	CHECK(pthread_rwlockattr_destroy(&attr));
	CHECK(pthread_rwlock_wrlock(&rwlock));
	p.writer_held = 1;
	CHECK(pthread_create(&child, NULL, probe, &p));
	CHECK(pthread_join(child, NULL));
	CHECK(pthread_rwlock_unlock(&rwlock));
	CHECK(pthread_rwlock_rdlock(&rwlock));
	p.writer_held = 0;
	CHECK(pthread_create(&child, NULL, probe, &p));
	CHECK(pthread_join(child, NULL));
	CHECK(pthread_rwlock_unlock(&rwlock));
	for (i = 0; i < pts_control.threads; i++) {
		states[i].index = i;
		CHECK(pthread_create(&threads[i], NULL, worker, &states[i]));
	}
	for (i = 0; i < pts_control.threads; i++)
		CHECK(pthread_join(threads[i], NULL));
	REQUIRE(value == ((pts_control.threads + 1) / 2) * pts_control.iterations);
	REQUIRE(complement == ~value);
	CHECK(pthread_rwlock_destroy(&rwlock));
	return PTS_PASS;
}
