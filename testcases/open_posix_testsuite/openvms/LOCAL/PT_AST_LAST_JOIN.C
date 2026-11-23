#ifdef __VMS
#pragma module PT_AST_LAST_JOIN "X-1"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 23-Nov-2026
 *     Exercise a final process AST with the initial thread joining its worker.
 */
#define PTS_LAST_JOIN 1
#include "PT_AST_LAST_COMMON.H"
