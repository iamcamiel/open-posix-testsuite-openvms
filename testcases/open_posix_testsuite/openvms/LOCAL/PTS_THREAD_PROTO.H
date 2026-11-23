/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 23-Nov-2026: Prototype legacy start routines.
 */

/* The four selected cases define a_thread_func() and pass its bare name to
 * pthread_create. Supply the missing parameter only at the definition.
 */
#define a_thread_func() a_thread_func(void *arg PTS_ATTRIBUTE_UNUSED)
