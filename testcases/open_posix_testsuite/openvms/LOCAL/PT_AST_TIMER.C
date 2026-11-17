#ifdef __VMS
#pragma module PT_AST_TIMER "X-1"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 17-Nov-2026:
 *     Exercise timer ASTs across timed waits and joins with main alone
 *     disabling and reenabling ASTs, without output in the disabled window.
 */
#define PTS_AST_TIMER_ACTIVITY 0
#define PTS_AST_TIMER_WORKER_ENABLE 0
#include "PT_AST_TIMER_COMMON.H"
