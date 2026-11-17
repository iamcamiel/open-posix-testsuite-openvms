#ifdef __VMS
#pragma module PT_AST_TIMER_CRTL "X-1"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 17-Nov-2026:
 *     Add serialized ordinary-context time conversion, formatting and
 *     output to the disabled timer-AST test. Workers do not enable ASTs.
 */
#define PTS_AST_TIMER_ACTIVITY 2
#define PTS_AST_TIMER_WORKER_ENABLE 0
#include "PT_AST_TIMER_COMMON.H"
