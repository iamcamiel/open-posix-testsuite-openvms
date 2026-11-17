#ifdef __VMS
#pragma module PT_AST_WORKER "X-1"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 17-Nov-2026:
 *     Test process AST exclusion with two overlapping worker disablers.
 */
#define PTS_AST_SCENARIO 2
#include "PT_AST_COMMON.H"
