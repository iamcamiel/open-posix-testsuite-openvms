#ifdef __VMS
#pragma module PT_CRASH_DUMP "X-1"
#endif
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Copyright 2026 iamcamiel
 * Copyright 2026 VMS Software, Inc.
 * X-1 CV Camiel Vanderhoeven 17-Nov-2026:
 *     Add explicitly armed pthread bugcheck and worker ACCVIO probes.
 *     Declare the diagnostic compatibility entry without private headers.
 *     These are destructive test fixtures, never normal conformance cases.
 */
#include "PTS_CHECK.H"

#ifndef __VMS
int test_main(int argc, char **argv)
{
	(void)argc; (void)argv;
	puts("Crash-dump probes require native OpenVMS; no host substitute.");
	return PTS_UNSUPPORTED;
}
#else
#include <stdint.h>

/* Implementation-specific diagnostic, not a POSIX conformance interface.
 * The exported compatibility entry is present in the inspected Alpha, IA64
 * and x86 RTL symbol vectors. Its native ABI takes a public pthread_t, an
 * int-sized item enum, a 64-bit buffer pointer, and a VMS long buffer size.
 * Declare only that ABI here: no private headers or thread layouts needed.
 * Required pointer size preserves the buffer argument in /POINTER_SIZE=32
 * builds too; pthread_t already has its public-header representation.
 */
#pragma __required_pointer_size __save
#pragma __required_pointer_size __long
extern int pthread_dbg_get(pthread_t, int, void *, long);
#pragma __required_pointer_size __restore

#define PROBE_BUGCHECK 1
#define PROBE_ACCVIO 2

/* Runtime volatile input prevents constant-folding the deliberate bad store.
 * Address zero is used only after the explicit native arming checks below.
 */
static volatile uintptr_t fault_address;

static void *crash_worker(void *argument)
{
	int mode = *(int *)argument;
	unsigned long buffer = 0;
	int status;

	/* A fresh process/run directory and this token let the supervisor reject
	 * an unrelated crash or an artifact left by an earlier invocation.
	 */
	printf("PTS_CRASH_TRIGGER|%s|%s\n",
	       mode == PROBE_BUGCHECK ? "BUGCHECK" : "ACCVIO",
	       pts_control.token);
	fflush(stdout);
	fflush(stderr);
	if (mode == PROBE_BUGCHECK) {
		/* The exported compatibility entry validates the live thread, then
		 * debGet's default switch arm calls errBugcheck. This is deliberate
		 * invalid API input; no TEB, lock, queue, or stack is overwritten.
		 * Expected pthread_dump.log reason in the inspected RTL builds:
		 *   deb_get: bad item code 2147483647
		 * If a future RTL rejects the item without bugchecking, report an
		 * unresolved probe instead of substituting a different crash.
		 */
		status = pthread_dbg_get(pthread_self(),
			0x7fffffff, &buffer, sizeof(buffer));
		fprintf(stderr, "BUGCHECK trigger unexpectedly returned %d; "
			"no pthread bugcheck was demonstrated.\n", status);
	} else {
		/* Exercise OS image-dump creation for a real worker-thread fault.
		 * An ACCVIO does not itself promise a pthread_dump.log file.
		 */
		*(volatile unsigned int *)(uintptr_t)fault_address = 0x50545331U;
		fputs("ACCVIO trigger unexpectedly returned.\n", stderr);
	}
	return NULL;
}

int test_main(int argc, char **argv)
{
	pthread_t worker;
	int mode;
	(void)argc; (void)argv;

	/* The dedicated RUN_CRASH command selects the separate crash manifest.
	 * Merely building this file or adding an ordinary case cannot arm it.
	 */
	if (!strcmp(pts_control.id, "PT_CRASH_BUGCHECK"))
		mode = PROBE_BUGCHECK;
	else if (!strcmp(pts_control.id, "PT_CRASH_ACCVIO"))
		mode = PROBE_ACCVIO;
	else {
		fputs("Crash probe not armed: use @RUN_CRASH BUGCHECK or ACCVIO.\n",
		      stderr);
		return PTS_UNRESOLVED;
	}
	if (pts_control.threads != 1 || pts_control.iterations != 1) {
		fputs("Crash probe not armed: require threads=1 and iterations=1.\n",
		      stderr);
		return PTS_UNRESOLVED;
	}
	CHECK(pthread_create(&worker, NULL, crash_worker, &mode));
	CHECK(pthread_join(worker, NULL));
	fputs("Intentional crash did not terminate the process.\n", stderr);
	return PTS_UNRESOLVED;
}
#endif
