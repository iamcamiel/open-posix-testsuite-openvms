/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * X-1  CV  Camiel Vanderhoeven  15-Nov-2026: Initial OpenVMS port.
 */
#ifndef PTS_PORT_H
#define PTS_PORT_H

#include <stddef.h>
#include <stdio.h>

#define PTS_PATH_MAX 1024
#define PTS_ID_MAX 64
#define PTS_TOKEN_MAX 64
#define PTS_LINE_MAX 4096

/* The runner deliberately has no dependency on pthreads. */
struct pts_control {
	char id[PTS_ID_MAX];
	char token[PTS_TOKEN_MAX];
	char result[PTS_PATH_MAX];
	char directory[PTS_PATH_MAX];
	unsigned long seed;
	unsigned long threads;
	unsigned long iterations;
};

extern struct pts_control pts_control;
void pts_exit(int code);
int pts_read_control(FILE *in, struct pts_control *ctl);
int pts_write_control(FILE *out, const struct pts_control *ctl);
int pts_read_result(const char *path, const char *id, const char *token,
		    int *code);
int pts_valid_code(int code);
const char *pts_code_name(int code);
int pts_number(const char *s, unsigned long min, unsigned long max,
	       unsigned long *value);
int pts_safe_id(const char *s);
int pts_join(char *out, size_t size, const char *directory, const char *file);
int pts_subdirectory(char *out, size_t size, const char *directory,
		     const char *leaf);

#endif
