#ifdef __VMS
#pragma module PT_RMS_AST_SPIN "X-3"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 VMS Software, Inc.
 * X-3 CV Camiel Vanderhoeven 23-Nov-2026: Block the default thread during reads.
 * X-2 CV Camiel Vanderhoeven 23-Nov-2026: Fix RMS pointers and exercise ASTs.
 * X-1 CV Camiel Vanderhoeven 23-Nov-2026: Poll ordered RMS completion ASTs.
 */
#define PTS_RMS_AST_YIELD 0
#include "PT_RMS_AST_COMMON.H"
