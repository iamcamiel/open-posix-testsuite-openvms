/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel; X-1 CV Camiel Vanderhoeven 15-Nov-2026. */
#include "PTS_CHECK.H"
struct pair {
	pthread_mutex_t mutex;
	pthread_cond_t cv;
	int ready, go;
};
static void *waiter(void *arg)
{
	struct pair *p = arg;
	CHECK(pthread_mutex_lock(&p->mutex));
	p->ready = 1;
	CHECK(pthread_cond_signal(&p->cv));
	while (!p->go)
		CHECK(pthread_cond_wait(&p->cv, &p->mutex));
	CHECK(pthread_mutex_unlock(&p->mutex));
	return p;
}
static void *churn(void *arg)
{
	unsigned long i, random = pts_control.seed + *(unsigned long *)arg;
	for (i = 0; i < pts_control.iterations; i++) {
		struct pair p;
		pthread_t thread;
		void *value = NULL;
		memset(&p, 0, sizeof(p));
		CHECK(pthread_mutex_init(&p.mutex, NULL));
		CHECK(pthread_cond_init(&p.cv, NULL));
		CHECK(pthread_mutex_lock(&p.mutex));
		CHECK(pthread_create(&thread, NULL, waiter, &p));
		while (!p.ready)
			CHECK(pthread_cond_wait(&p.cv, &p.mutex));
		p.go = 1;
		CHECK(pthread_cond_signal(&p.cv));
		CHECK(pthread_mutex_unlock(&p.mutex));
		CHECK(pthread_join(thread, &value));
		REQUIRE(value == &p);
		/* No waiter or user remains: destruction is always legal. */
		CHECK(pthread_cond_destroy(&p.cv));
		CHECK(pthread_mutex_destroy(&p.mutex));
		pts_yield(&random);
	}
	return NULL;
}
int test_main(int argc, char **argv)
{
	pthread_t threads[256];
	unsigned long indices[256], i;
	(void)argc; (void)argv;
	for (i = 0; i < pts_control.threads; i++) {
		indices[i] = i;
		CHECK(pthread_create(&threads[i], NULL, churn, &indices[i]));
	}
	for (i = 0; i < pts_control.threads; i++)
		CHECK(pthread_join(threads[i], NULL));
	return PTS_PASS;
}
