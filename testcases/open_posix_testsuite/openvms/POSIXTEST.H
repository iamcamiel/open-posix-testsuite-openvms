/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * X-1  CV  Camiel Vanderhoeven  15-Nov-2026: Initial OpenVMS adapter.
 * PTS_ORIGINAL.H is a byte-for-byte copy of upstream include/posixtest.h.
 */
#ifndef PTS_VMS_POSIXTEST_H
#define PTS_VMS_POSIXTEST_H

/* Load the real declaration before replacing calls in test code. */
#include <stdlib.h>
#include "PTS_ORIGINAL.H"
#include "PTS_PORT.H"

/* Worker-thread exit(PTS_*) must produce a result too. */
#ifdef exit
#undef exit
#endif
#define exit(code) pts_exit(code)

#endif
