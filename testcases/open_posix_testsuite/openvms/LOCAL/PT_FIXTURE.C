/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel; X-1 CV Camiel Vanderhoeven 15-Nov-2026. */
/* Runner self-tests only. Never include these in CASES.DAT. */
#include <unistd.h>
#include <stdlib.h>
#include "POSIXTEST.H"
#ifndef FIXTURE_KIND
#define FIXTURE_KIND 0
#endif
int test_main(int argc, char **argv)
{
	FILE *out;
	(void)argc; (void)argv;
	switch (FIXTURE_KIND) {
	case 0: return PTS_PASS;
	case 1: exit(PTS_FAIL); return PTS_FAIL;
	case 2: return PTS_UNRESOLVED;
	case 4: return PTS_UNSUPPORTED;
	case 5: return PTS_UNTESTED;
	case 6: abort();
	case 7: for (;;) sleep(1);
	case 8: _exit(0); /* Clean process status without a result is not PASS. */
	case 9: /* An already-written PASS must not hide abnormal termination. */
		out = fopen(pts_control.result, "w");
		if (!out) abort();
		fprintf(out, "PTS1|%s|%s|0\n", pts_control.id, pts_control.token);
		fclose(out);
		abort();
	case 10:
		out = fopen(pts_control.result, "w");
		if (!out) abort();
		fprintf(out, "PTS1|%s|WRONG_TOKEN|0\n", pts_control.id);
		fclose(out);
		_exit(0);
	case 11:
		out = fopen(pts_control.result, "w");
		if (!out) abort();
		fprintf(out, "PTS1|%s|%s|0\nPTS1|%s|%s|0\n",
			pts_control.id, pts_control.token, pts_control.id, pts_control.token);
		fclose(out);
		_exit(0);
	default: return 99; /* Invalid result protocol value. */
	}
}
