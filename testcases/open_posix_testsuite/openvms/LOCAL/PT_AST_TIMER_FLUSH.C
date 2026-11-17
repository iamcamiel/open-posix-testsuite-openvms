#ifdef __VMS
#pragma module PT_AST_TIMER_FLUSH "X-1"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 17-Nov-2026:
 *     Add explicit fflush after ordinary-context printf in the disabled
 *     timer-AST sequence, without a test mutex. Workers do not enable ASTs.
 */
#define PTS_AST_TIMER_ACTIVITY 3
#define PTS_AST_TIMER_WORKER_ENABLE 0
#include "PT_AST_TIMER_COMMON.H"
