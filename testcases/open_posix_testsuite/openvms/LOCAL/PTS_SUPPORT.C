/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel; X-1 CV Camiel Vanderhoeven 15-Nov-2026. */
#include <sched.h>
#include "PTS_CHECK.H"
void pts_check(int rc, const char *call, int line)
{
	if (rc) {
		fprintf(stderr, "line %d: %s: %s (%d)\n", line, call, strerror(rc), rc);
		exit(PTS_UNRESOLVED);
	}
}
void pts_require(int truth, const char *assertion, int line)
{
	if (!truth) {
		fprintf(stderr, "line %d: assertion failed: %s\n", line, assertion);
		exit(PTS_FAIL);
	}
}
void pts_yield(unsigned long *state)
{
	*state = (*state * 1664525UL + 1013904223UL) & 0xffffffffUL;
	if ((*state & 31) == 0) {
#ifdef __VMS
		CHECK(pthread_yield_np());
#else
		CHECK(sched_yield());
#endif
	}
}
