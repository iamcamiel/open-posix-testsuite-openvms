#ifdef __VMS
#pragma module PT_AST_CALLBACK "X-1"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 17-Nov-2026:
 *     Test an AST disabling delivery while another AST remains queued.
 */
#define PTS_AST_SCENARIO 3
#include "PT_AST_COMMON.H"
