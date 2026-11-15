/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel; X-1 CV Camiel Vanderhoeven 15-Nov-2026. */
#ifndef PTS_CHECK_H
#define PTS_CHECK_H
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/time.h>
#include "POSIXTEST.H"
void pts_check(int rc, const char *call, int line);
void pts_require(int truth, const char *assertion, int line);
void pts_yield(unsigned long *random_state);
#define CHECK(call) pts_check((call), #call, __LINE__)
#define REQUIRE(truth) pts_require(!!(truth), #truth, __LINE__)
#endif
