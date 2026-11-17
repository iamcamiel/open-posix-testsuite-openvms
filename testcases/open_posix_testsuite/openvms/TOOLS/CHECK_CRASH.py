#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 iamcamiel
# Copyright 2026 VMS Software, Inc.
# X-1 CV Camiel Vanderhoeven 17-Nov-2026: Check crash-probe arming and routing.
"""Exercise the crash probe with harmless host substitutes for fatal actions.

This checks refusal to arm, worker dispatch, trigger identity and unexpected
returns. It does not validate OpenVMS ABI, bugcheck handling, or dump creation.
The ACCVIO path writes an ordinary host object; the simulated fatal debug
entry exits with a distinctive code, without creating a host core file.
"""

import argparse
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile


PORT = Path(__file__).resolve().parent.parent
SUITE = PORT.parent

HARNESS = r'''
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "PT_CRASH_DUMP.C"
#undef exit

struct pts_control pts_control;
static pthread_t main_thread;
static int debug_calls, debug_fatal;
static unsigned int safe_target;

void pts_exit(int code) { _Exit(code); }
void pts_check(int rc, const char *call, int line) {
    if (rc) { fprintf(stderr, "%s:%d: %d\n", call, line, rc); _Exit(90); }
}
#ifdef __VMS
int pthread_dbg_get(pthread_t thread, int item,
                    void *buffer, long size) {
    assert(!pthread_equal(thread, main_thread));
    assert(pthread_equal(thread, pthread_self()));
    assert((int)item == 0x7fffffff);
    assert(buffer && size == sizeof(unsigned long));
    ++debug_calls;
    if (debug_fatal) _Exit(77);
    return 22;
}
#endif
int main(int argc, char **argv) {
    int result;
    assert(argc == 5);
    assert(strlen(argv[1]) < sizeof(pts_control.id));
    strcpy(pts_control.id, argv[1]);
    strcpy(pts_control.token, "CRASH-HOST-TOKEN");
    pts_control.threads = strtoul(argv[2], NULL, 10);
    pts_control.iterations = strtoul(argv[3], NULL, 10);
    debug_fatal = atoi(argv[4]);
    main_thread = pthread_self();
#ifdef __VMS
    fault_address = (uintptr_t)&safe_target;
#endif
    result = test_main(0, NULL);
    printf("SIM result=%d debug_calls=%d target=%08x\n",
           result, debug_calls, safe_target);
    return result;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pts-crash-check-") as tmp:
        work = Path(tmp)
        for name in ("PTS_PORT.H", "POSIXTEST.H"):
            shutil.copyfile(PORT / name, work / name)
        for name in ("PTS_CHECK.H", "PT_CRASH_DUMP.C"):
            shutil.copyfile(PORT / "LOCAL" / name, work / name)
        shutil.copyfile(SUITE / "include/posixtest.h", work / "PTS_ORIGINAL.H")
        (work / "harness.c").write_text(HARNESS)
        flags = ["-std=c99", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                 "-Werror", "-Wno-unknown-pragmas", "-pthread", "-I" + str(work)]
        cases = [
            ("ordinary case refuses to arm", "PT_QIO_AST", 1, 1, 0, 2, None, 0, 0),
            ("wrong worker count refuses to arm", "PT_CRASH_BUGCHECK", 2, 1, 0,
             2, None, 0, 0),
            ("wrong iteration count refuses to arm", "PT_CRASH_ACCVIO", 1, 2, 0,
             2, None, 0, 0),
            ("debug entry returning is unresolved", "PT_CRASH_BUGCHECK", 1, 1, 0,
             2, "BUGCHECK", 1, 0),
            ("fatal debug entry identifies worker trigger", "PT_CRASH_BUGCHECK",
             1, 1, 1, 77, "BUGCHECK", None, None),
            ("ACCVIO returning is unresolved", "PT_CRASH_ACCVIO", 1, 1, 0,
             2, "ACCVIO", 0, 0x50545331),
        ]
        count = 0
        for opt in ("-O0", "-O2"):
            native = work / ("native" + opt)
            host = work / ("host" + opt)
            for exe, definitions in ((native, ["-D__VMS"]), (host, [])):
                subprocess.run([*shlex.split(args.cc), *flags, opt, *definitions,
                                str(work / "harness.c"), "-o", str(exe)],
                               check=True, timeout=60)
            for label, case, threads, iterations, fatal, expected, trigger, calls, target in cases:
                result = subprocess.run([str(native), case, str(threads),
                                         str(iterations), str(fatal)],
                                        capture_output=True, text=True, timeout=10)
                assert result.returncode == expected, (label, result)
                marker = "PTS_CRASH_TRIGGER|"
                if trigger:
                    assert result.stdout.count(marker) == 1, (label, result)
                    assert marker + trigger + "|CRASH-HOST-TOKEN\n" in result.stdout
                else:
                    assert marker not in result.stdout, (label, result)
                if calls is not None:
                    assert f"debug_calls={calls} target={target:08x}" in result.stdout
                count += 1
                print(f"{opt} PASS: {label}")
            result = subprocess.run([str(host), "PT_CRASH_BUGCHECK", "1", "1", "0"],
                                    capture_output=True, text=True, timeout=10)
            assert result.returncode == 4, result
            assert "PTS_CRASH_TRIGGER|" not in result.stdout, result
            count += 1
            print(f"{opt} PASS: host build remains UNSUPPORTED")
        print(f"PASS: {count} crash-probe routing checks; no native dumps tested")


if __name__ == "__main__":
    main()
