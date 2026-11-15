/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * X-1  CV  Camiel Vanderhoeven  15-Nov-2026: Native result adapter.
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include "PTS_PORT.H"

struct pts_control pts_control;
extern int test_main(int argc, char **argv);

void pts_exit(int code)
{
	FILE *out;
	int fd, ok;

	/* Exclusive creation prevents competing reports or stale files passing.
	 * The native runner supplies an explicit ;1 version on OpenVMS.
	 */
	fd = open(pts_control.result, O_WRONLY | O_CREAT | O_EXCL, 0600);
	if (fd < 0) {
		perror("creating PTS result");
		exit(EXIT_FAILURE);
	}
	out = fdopen(fd, "w");
	if (!out) {
		close(fd);
		exit(EXIT_FAILURE);
	}
	ok = fprintf(out, "PTS1|%s|%s|%d\n", pts_control.id,
		     pts_control.token, code) > 0;
	if (fclose(out) != 0)
		ok = 0;
	fflush(stdout);
	fflush(stderr);
	/* The record carries the POSIX result; process status proves a clean
	 * shutdown. A crash after writing PASS must not become a pass.
	 */
	exit(ok ? EXIT_SUCCESS : EXIT_FAILURE);
}

int main(int argc, char **argv)
{
	if (pts_read_control(stdin, &pts_control) != 0) {
		fprintf(stderr, "Invalid or missing runner control input\n");
		return EXIT_FAILURE;
	}
	if (chdir(pts_control.directory) != 0) {
		perror("entering test directory");
		return EXIT_FAILURE;
	}
	printf("PTS case=%s seed=%lu threads=%lu iterations=%lu pointer_bits=%u\n",
	       pts_control.id, pts_control.seed, pts_control.threads,
	       pts_control.iterations, (unsigned int)(sizeof(void *) * 8));
	fflush(stdout);
	pts_exit(test_main(argc, argv));
	return EXIT_FAILURE;
}
