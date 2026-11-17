#ifdef __VMS
#pragma module PTS_DUMP_CHECK "X-1"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 17-Nov-2026:
 *     Verify artifacts from an explicitly armed, isolated crash probe.
 *     Artifact presence is not a claim that SDA can analyze the dump.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <dirent.h>
#include "PTS_PORT.H"

static const char summary_header[] =
    "case,repeat,result,pts_code,native_status,seed,threads,iterations,seconds,directory";

static int read_line(FILE *in, char *buffer, size_t size)
{
	size_t n;
	if (!fgets(buffer, (int)size, in))
		return -1;
	n = strlen(buffer);
	if (!n || buffer[n - 1] != '\n')
		return -1;
	buffer[--n] = 0;
	if (n && buffer[n - 1] == '\r')
		buffer[--n] = 0;
	return 0;
}

/* Decode the runner's CSV, including commas and doubled quotes in paths. */
static int csv_fields(char *line, char **fields, unsigned int count)
{
	char *src = line, *dst = line;
	unsigned int i;
	int quoted;
	for (i = 0; i < count; ++i) {
		fields[i] = dst;
		quoted = *src == '"';
		if (quoted) {
			++src;
			for (;;) {
				if (!*src)
					return -1;
				if (*src == '"') {
					if (src[1] != '"') {
						++src;
						break;
					}
					++src;
				}
				*dst++ = *src++;
			}
		} else {
			while (*src && *src != ',') {
				if (*src == '"')
					return -1;
				*dst++ = *src++;
			}
		}
		if (i + 1 < count) {
			if (*src++ != ',')
				return -1;
		} else if (*src) {
			return -1;
		}
		*dst++ = 0;
	}
	return 0;
}

/* Case-insensitive names, with optional numeric OpenVMS file versions. */
static int file_name(const char *name, const char *wanted, int suffix)
{
	const char *version = strchr(name, ';');
	size_t length = version ? (size_t)(version - name) : strlen(name);
	size_t n = strlen(wanted), i;
	if (version) {
		++version;
		if (!*version)
			return 0;
		for (; *version; ++version)
			if (!isdigit((unsigned char)*version))
				return 0;
	}
	if (length < n || (!suffix && length != n))
		return 0;
	name += length - n;
	for (i = 0; i < n; ++i)
		if (toupper((unsigned char)name[i]) !=
		    toupper((unsigned char)wanted[i]))
			return 0;
	return 1;
}

/* Return 1 for a match, 0 for absence, -1 for an unreadable/truncated log.
 * Refuse overlong records rather than accidentally accepting a fragment.
 */
static int log_contains(const char *path, const char *needle, int exact)
{
	FILE *in = fopen(path, "r");
	char line[PTS_LINE_MAX];
	size_t n;
	int found = 0, error = 0;
	if (!in)
		return -1;
	while (fgets(line, sizeof(line), in)) {
		n = strlen(line);
		if (n && line[n - 1] == '\n') {
			line[--n] = 0;
			if (n && line[n - 1] == '\r')
				line[--n] = 0;
		} else if (!feof(in)) {
			error = 1;
			break;
		}
		if (exact ? !strcmp(line, needle) : strstr(line, needle) != NULL)
			found = 1;
	}
	if (ferror(in))
		error = 1;
	if (fclose(in))
		error = 1;
	return error ? -1 : found;
}

static int nonempty_file(const char *path)
{
	FILE *in = fopen(path, "rb");
	int nonempty;
	if (!in)
		return 0;
	nonempty = fgetc(in) != EOF && !ferror(in);
	if (fclose(in))
		nonempty = 0;
	return nonempty;
}

static int fail(const char *reason)
{
	fprintf(stderr, "FAIL: crash dump verification: %s\n", reason);
	return EXIT_FAILURE;
}

int main(int argc, char **argv)
{
	FILE *in;
	DIR *directory;
	struct dirent *entry;
	struct pts_control control;
	char line[PTS_LINE_MAX], *field[10], path[PTS_PATH_MAX];
	char marker[PTS_TOKEN_MAX + 40];
	const char *mode;
	unsigned long seed, threads, iterations;
	int error, dump_found = 0, log_found = 0, enumeration_error = 0;

	if (argc != 3 || (strcmp(argv[2], "PT_CRASH_BUGCHECK") &&
			  strcmp(argv[2], "PT_CRASH_ACCVIO"))) {
		fputs("Usage: PTS_DUMP_CHECK summary.csv "
		      "PT_CRASH_BUGCHECK|PT_CRASH_ACCVIO\n", stderr);
		return EXIT_FAILURE;
	}
	mode = !strcmp(argv[2], "PT_CRASH_BUGCHECK") ? "BUGCHECK" : "ACCVIO";
	in = fopen(argv[1], "r");
	if (!in)
		return fail("cannot open the fresh supervisor summary");
	error = read_line(in, line, sizeof(line)) || strcmp(line, summary_header);
	if (!error)
		error = read_line(in, line, sizeof(line)) ||
			csv_fields(line, field, 10);
	if (!error)
		error = fgetc(in) != EOF || ferror(in);
	if (fclose(in))
		error = 1;
	if (error)
		return fail("expected the summary header and exactly one complete case row");
	if (strcmp(field[0], argv[2]) || strcmp(field[1], "1") ||
	    strcmp(field[2], "CRASH") || strcmp(field[3], "-1"))
		return fail("the selected probe was not recorded as one abnormal crash");
	if (pts_number(field[5], 1, 2147483647UL, &seed) ||
	    pts_number(field[6], 1, 1, &threads) ||
	    pts_number(field[7], 1, 1, &iterations) || !field[9][0])
		return fail("summary controls do not describe an armed crash probe");
	if (pts_join(path, sizeof(path), field[9], "CONTROL.DAT"))
		return fail("control path is too long");
	in = fopen(path, "r");
	if (!in)
		return fail("cannot read the recorded case control file");
	error = pts_read_control(in, &control) || ferror(in);
	if (fclose(in))
		error = 1;
	if (error || strcmp(control.id, argv[2]) ||
	    strcmp(control.directory, field[9]) || control.seed != seed ||
	    control.threads != threads || control.iterations != iterations)
		return fail("case control does not match the supervisor summary");
	if (snprintf(marker, sizeof(marker), "PTS_CRASH_TRIGGER|%s|%s",
		     mode, control.token) >= (int)sizeof(marker) ||
	    pts_join(path, sizeof(path), control.directory, "STDOUT.LOG") ||
	    log_contains(path, marker, 1) != 1)
		return fail("missing exact trigger marker for this case and token");
	if (pts_join(path, sizeof(path), control.directory, "STDERR.LOG") ||
	    log_contains(path, "IMGDMP aborting", 0) != 0 ||
	    log_contains(path, "Improperly handled condition", 0) != 0)
		return fail("stderr is unreadable or reports an image-dump failure");

	directory = opendir(control.directory);
	if (!directory)
		return fail("cannot enumerate the crash case directory");
	for (;;) {
		errno = 0;
		entry = readdir(directory);
		if (!entry) {
			if (errno)
				enumeration_error = 1;
			break;
		}
		if (pts_join(path, sizeof(path), control.directory, entry->d_name)) {
			enumeration_error = 1;
			break;
		}
		if (file_name(entry->d_name, ".DMP", 1) && nonempty_file(path)) {
			dump_found = 1;
			printf("Image dump file written (nonempty): %s\n", path);
		}
		if (file_name(entry->d_name, "PTHREAD_DUMP.LOG", 0) &&
		    log_contains(path, "deb_get: bad item code 2147483647", 0) == 1) {
			log_found = 1;
			printf("Pthread bugcheck log with expected reason: %s\n", path);
		}
	}
	if (closedir(directory))
		enumeration_error = 1;
	if (enumeration_error)
		return fail("error enumerating crash artifacts");
	if (!dump_found)
		return fail("no nonempty .DMP in the fresh case directory; "
			    "check dump settings, redirection, quotas, and stderr");
	if (!strcmp(mode, "BUGCHECK") && !log_found)
		return fail("missing PTHREAD_DUMP.LOG with the expected bugcheck reason");
	printf("PASS: %s crash artifacts found; SDA validation of the dump "
	       "is still required.\n", mode);
	return EXIT_SUCCESS;
}
