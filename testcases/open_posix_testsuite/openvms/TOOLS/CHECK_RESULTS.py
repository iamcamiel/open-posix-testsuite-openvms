#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 iamcamiel
# X-1 CV Camiel Vanderhoeven 16-Nov-2026: Concurrent result-report regression.
"""Exercise the real result adapter; native branches use host service stubs."""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile

PORT = Path(__file__).resolve().parent.parent

HEADER = r'''
#ifndef RESULT_STUB_H
#define RESULT_STUB_H
#define SS$_ABORT 44
int __CMP_STORE_LONG(volatile void *, int, int, volatile void *);
int __CMP_SWAP_LONG(volatile void *, int, int);
unsigned int sys$exit(unsigned int);
int pthread_yield_np(void);
#endif
'''

TEST = r'''
#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int controlled_open(const char *, int, ...);
static int parked_pause(void);
#define open controlled_open
#define pause parked_pause
#define main pts_adapter_main
#include "PTS_MAIN.C"
#undef main
#undef pause
#undef open

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static int opened, release_writer, hold_writer;
static volatile int parked;

static void tick(void)
{
    struct timespec delay = {0, 1000000};
    nanosleep(&delay, NULL);
}

static int controlled_open(const char *name, int flags, ...)
{
    va_list args;
    int mode;
    va_start(args, flags);
    mode = va_arg(args, int);
    va_end(args);
    assert(!pthread_mutex_lock(&gate));
    ++opened;
    assert(!pthread_cond_broadcast(&changed));
    while (hold_writer && !release_writer)
        assert(!pthread_cond_wait(&changed, &gate));
    assert(!pthread_mutex_unlock(&gate));
    return open(name, flags, mode);
}

static int parked_pause(void)
{
    __sync_fetch_and_add(&parked, 1);
    for (;;) pause();
    return 0;
}

#ifdef __VMS
int __CMP_STORE_LONG(volatile void *src, int old, int new, volatile void *dst)
{
    static __thread int attempted;
    assert(src == dst);
    /* Force a failed store-conditional without a competing winning claim. */
    if (!attempted++) return 0;
    return __sync_bool_compare_and_swap((volatile int *)dst, old, new);
}
int __CMP_SWAP_LONG(volatile void *dst, int old, int new)
{ return __sync_bool_compare_and_swap((volatile int *)dst, old, new); }
unsigned int sys$exit(unsigned int status)
{ assert(status == SS$_ABORT); _Exit(17); }
int pthread_yield_np(void)
{
    /* Each simulated user thread is a host kernel thread; park it after
       recording entry to the native loser-yield path. Not an ABI check. */
    return parked_pause();
}
#endif

static void *report(void *arg)
{
    pts_exit(*(int *)arg);
    abort();
    return NULL;
}

int test_main(int argc, char **argv)
{
    pthread_t winner, losers[16];
    int codes[16], first, i, mixed;
    assert(argc == 2);
    if (!strcmp(argv[1], "stale") || !strcmp(argv[1], "unwritable"))
        return 2;
    mixed = !strcmp(argv[1], "failure_before_pass");
    first = !strcmp(argv[1], "conflicting_pass") ? 0 : 2;
    hold_writer = 1;
    assert(!pthread_create(&winner, NULL, report, &first));
    assert(!pthread_mutex_lock(&gate));
    while (!opened) assert(!pthread_cond_wait(&changed, &gate));
    assert(opened == 1);
    assert(!pthread_mutex_unlock(&gate));
    for (i = 0; i < 16; ++i) {
        codes[i] = mixed && (i & 1) ? 0 : 2;
        assert(!pthread_create(&losers[i], NULL, report, &codes[i]));
    }
    while (__sync_fetch_and_add(&parked, 0) < 16) tick();
    assert(!pthread_mutex_lock(&gate));
    /* No contender may open RESULT.PTS or call exit while the winner waits. */
    assert(opened == 1);
    release_writer = 1;
    assert(!pthread_cond_broadcast(&changed));
    assert(!pthread_mutex_unlock(&gate));
    /* Exercise the adapter's normal PASS return racing an existing failure. */
    return 0;
}

int main(int argc, char **argv) { return pts_adapter_main(argc, argv); }
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    compiler = shlex.split(args.cc)
    with tempfile.TemporaryDirectory(prefix="pts-result-check-") as temp:
        root = Path(temp)
        (root / "RESULT_STUB.H").write_text(HEADER)
        for name in ("builtins.h", "ssdef.h", "starlet.h"):
            (root / name).write_text('#include "RESULT_STUB.H"\n')
        (root / "check.c").write_text(TEST)
        flags = ["-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
                 "-Wno-unknown-pragmas", "-pthread", "-I" + str(root),
                 "-I" + str(PORT)]
        common = root / "port.o"
        subprocess.run(compiler + flags + ["-x", "c", "-c",
                       str(PORT / "PTS_PORT.C"), "-o", str(common)],
                       check=True, timeout=60)
        profiles = [("host", []), ("native_store", ["-D__VMS"]),
                    ("native_swap", ["-D__VMS", "-D__ia64"])]
        for profile, defines in profiles:
            executable = root / profile
            subprocess.run(compiler + flags + defines + [str(root / "check.c"),
                           str(common), "-o", str(executable)],
                           check=True, timeout=60)
            for scenario in ("concurrent", "failure_before_pass",
                             "conflicting_pass", "stale", "unwritable"):
                directory = root / (profile + "_" + scenario)
                directory.mkdir()
                result = directory / "RESULT.PTS"
                if scenario == "stale":
                    result.write_text("preserve previous result\n")
                elif scenario == "unwritable":
                    result = directory / "missing" / "RESULT.PTS"
                control = (f"PTS1\nCASE\nTOKEN\n{result}\n{directory}\n"
                           "1\n16\n1\n")
                run = subprocess.run([str(executable), scenario], input=control,
                                     text=True, capture_output=True, timeout=10)
                if scenario in ("concurrent", "failure_before_pass"):
                    assert run.returncode == 0, (profile, scenario, run.stderr)
                    assert result.read_text() == "PTS1|CASE|TOKEN|2\n"
                    assert "creating PTS result" not in run.stderr
                else:
                    assert run.returncode != 0, (profile, scenario)
                    if scenario == "stale":
                        assert result.read_text() == "preserve previous result\n"
                    else:
                        assert not result.exists(), (profile, scenario)
                print(f"PASS: result reporting {profile} {scenario}", flush=True)
    print("PASS: real result adapter; native services/atomics simulated, not VMS ABI")


if __name__ == "__main__":
    main()
