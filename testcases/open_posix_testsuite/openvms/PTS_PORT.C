/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * X-1  CV  Camiel Vanderhoeven  15-Nov-2026: Strict control/result protocol.
 * X-2  CV  Camiel Vanderhoeven  15-Nov-2026: Retry existing run names.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/stat.h>
#include "PTS_PORT.H"

static int line(FILE *in, char *out, size_t size)
{
	size_t n;
	if (!fgets(out, (int)size, in))
		return -1;
	n = strlen(out);
	if (!n || out[n - 1] != '\n')
		return -1;
	out[--n] = 0;
	if (n && out[n - 1] == '\r')
		out[--n] = 0;
	return 0;
}

int pts_number(const char *s, unsigned long min, unsigned long max,
	       unsigned long *value)
{
	char *end;
	unsigned long n;
	const unsigned char *p = (const unsigned char *)s;
	if (!*p)
		return -1;
	for (; *p; p++)
		if (!isdigit(*p))
			return -1;
	errno = 0;
	n = strtoul(s, &end, 10);
	if (errno || *end || n < min || n > max)
		return -1;
	*value = n;
	return 0;
}

int pts_safe_id(const char *s)
{
	const unsigned char *p = (const unsigned char *)s;
	if (!*p || strlen(s) >= PTS_ID_MAX)
		return 0;
	for (; *p; p++)
		if (!isalnum(*p) && *p != '_')
			return 0;
	return 1;
}

int pts_valid_code(int code)
{
	return code == 0 || code == 1 || code == 2 || code == 4 || code == 5;
}

const char *pts_code_name(int code)
{
	switch (code) {
	case 0: return "PASS";
	case 1: return "FAIL";
	case 2: return "UNRESOLVED";
	case 4: return "UNSUPPORTED";
	case 5: return "UNTESTED";
	default: return "HARNESS_ERROR";
	}
}

int pts_read_control(FILE *in, struct pts_control *ctl)
{
	char buf[128];
	memset(ctl, 0, sizeof(*ctl));
	if (line(in, buf, sizeof(buf)) || strcmp(buf, "PTS1") ||
	    line(in, ctl->id, sizeof(ctl->id)) || !pts_safe_id(ctl->id) ||
	    line(in, ctl->token, sizeof(ctl->token)) ||
	    !pts_safe_id(ctl->token) ||
	    line(in, ctl->result, sizeof(ctl->result)) || !ctl->result[0] ||
	    line(in, ctl->directory, sizeof(ctl->directory)) ||
	    !ctl->directory[0] ||
	    line(in, buf, sizeof(buf)) ||
	    pts_number(buf, 1, 2147483647UL, &ctl->seed) ||
	    line(in, buf, sizeof(buf)) ||
	    pts_number(buf, 1, 256, &ctl->threads) ||
	    line(in, buf, sizeof(buf)) ||
	    pts_number(buf, 1, 10000000UL, &ctl->iterations) ||
	    fgetc(in) != EOF)
		return -1;
	return 0;
}

int pts_write_control(FILE *out, const struct pts_control *ctl)
{
	return fprintf(out, "PTS1\n%s\n%s\n%s\n%s\n%lu\n%lu\n%lu\n",
		       ctl->id, ctl->token, ctl->result, ctl->directory,
		       ctl->seed, ctl->threads, ctl->iterations) < 0 ? -1 : 0;
}

int pts_read_result(const char *path, const char *id, const char *token,
		    int *code)
{
	FILE *in;
	char buf[256], expected[256], *last;
	unsigned long n;
	int ok;
	in = fopen(path, "r");
	if (!in)
		return -1;
	ok = !line(in, buf, sizeof(buf)) && fgetc(in) == EOF;
	if (ferror(in))
		ok = 0;
	if (fclose(in) != 0)
		ok = 0;
	if (!ok)
		return -1;
	last = strrchr(buf, '|');
	if (!last || pts_number(last + 1, 0, 255, &n) ||
	    !pts_valid_code((int)n))
		return -1;
	*last = 0;
	if (snprintf(expected, sizeof(expected), "PTS1|%s|%s", id, token)
	    >= (int)sizeof(expected) || strcmp(buf, expected))
		return -1;
	*code = (int)n;
	return 0;
}

int pts_join(char *out, size_t size, const char *directory, const char *file)
{
	int n;
#ifdef __VMS
	n = snprintf(out, size, "%s%s", directory, file);
#else
	n = snprintf(out, size, "%s/%s", directory, file);
#endif
	return n < 0 || (size_t)n >= size ? -1 : 0;
}

int pts_subdirectory(char *out, size_t size, const char *directory,
		     const char *leaf)
{
	int n;
#ifdef __VMS
	size_t len = strlen(directory);
	if (!len || directory[len - 1] != ']')
		return -1;
	n = snprintf(out, size, "%.*s.%s]", (int)(len - 1), directory, leaf);
#else
	n = snprintf(out, size, "%s/%s", directory, leaf);
#endif
	return n < 0 || (size_t)n >= size ? -1 : 0;
}

int pts_create_run_directory(char *out, size_t size, const char *directory,
			     unsigned long pid, unsigned long timestamp)
{
	char leaf[PTS_ID_MAX];
	unsigned long serial;
	int n;

	/* Successive images run in the same DCL process. PID plus seconds is
	 * therefore not unique. Reserve a fresh directory atomically; never
	 * reuse a previous run, even if the timestamp or PID is repeated.
	 */
	for (serial = 0; serial < 100000; serial++) {
		n = snprintf(leaf, sizeof(leaf), "R_%08lx_%08lx_%05lu",
			     pid, timestamp, serial);
		if (n < 0 || (size_t)n >= sizeof(leaf) ||
		    pts_subdirectory(out, size, directory, leaf)) {
			errno = ENAMETOOLONG;
			return -1;
		}
		if (mkdir(out, 0700) == 0)
			return 0;
		if (errno != EEXIST)
			return -1;
	}
	errno = EEXIST;
	return -1;
}
