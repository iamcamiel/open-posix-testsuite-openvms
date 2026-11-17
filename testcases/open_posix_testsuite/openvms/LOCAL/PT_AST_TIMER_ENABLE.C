#ifdef __VMS
#pragma module PT_AST_TIMER_ENABLE "X-1"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 17-Nov-2026:
 *     Test that a worker enabling ASTs without first disabling them cannot
 *     release timer ASTs while main retains its own disable request.
 */
#define PTS_AST_TIMER_ACTIVITY 0
#define PTS_AST_TIMER_WORKER_ENABLE 1
#include "PT_AST_TIMER_COMMON.H"
