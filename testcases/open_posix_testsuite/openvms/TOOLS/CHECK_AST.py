#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 iamcamiel
# Copyright 2026 VMS Software, Inc.
# X-3 CV Camiel Vanderhoeven 23-Nov-2026: Exercise both final-AST case protocols.
# X-2 CV Camiel Vanderhoeven 17-Nov-2026: Check expanded timer-case selection.
# X-1 CV Camiel Vanderhoeven 17-Nov-2026: Validate disabled-AST assertions.
"""Compile the native AST tests against controlled host service substitutes.

Real host pthreads execute the command workers. Simulated ASTs execute serially
at service/yield boundaries. The final-AST cases also allow the worker to
simulate a callback while main spins or joins. This verifies test control
flow, disable accounting, and detection of deliberately broken delivery. It
does not validate OpenVMS system-service ABI, scheduler or actual AST delivery.
"""

import argparse
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import tempfile


PORT = Path(__file__).resolve().parent.parent
LAST_CASES = ("PT_AST_LAST_RUN", "PT_AST_LAST_JOIN")
CASES = ("PT_AST_MAIN", "PT_AST_WORKER", "PT_AST_CALLBACK") + LAST_CASES
SELECTED_CASES = CASES + ("PT_AST_TIMER", "PT_AST_TIMER_PRINTF",
                          "PT_AST_TIMER_FLUSH", "PT_AST_TIMER_CRTL",
                          "PT_AST_TIMER_ENABLE")


def check_selection():
    """Check integration contracts without claiming to interpret DCL."""
    rows = [line.split("|") for line in (PORT / "CASES.DAT").read_text().splitlines()
            if line and not line.startswith("#")]
    assert all(len(row) == 8 for row in rows)
    for case in SELECTED_CASES:
        selected = [row for row in rows if row[0] == case]
        assert len(selected) == 1, (case, selected)
        row = selected[0]
        assert row[1] == "openvms/LOCAL/" + case + ".C"
        assert (PORT.parent / row[1]).is_file()
        assert row[2:5] == ["300", "regression", ""]
        assert row[5] and row[6:] == ["local", "ready"]
    assert {"PT_QIO_AST", "PT_QIO_MIXED"} <= {row[0] for row in rows}
    script = (PORT / "RUN_AST.COM").read_text()
    selected_ids = set(re.findall(r'"(PT_AST_[A-Z_]+)"', script))
    assert selected_ids == set(SELECTED_CASES)
    assert "timeout = f$integer(p4)" in script
    assert '"|" + f$string(timeout) + "|" + f$element(3,"|",row)' in script
    assert '''$ runner 'manifest' "[.RESULTS]" 'p1' 'p2' 3 'p3' "''p5'"''' in script
    assert "if .not. found_main .or. .not. found_worker .or. .not. found_callback" in script
    assert "if .not. found_timer .or. .not. found_timer_crtl" in script
    assert "if .not. found_timer_printf .or. .not. found_timer_enable" in script
    assert "if .not. found_timer_flush then goto missing" in script
    assert "if .not. found_last_run .or. .not. found_last_join then goto missing" in script
    assert 'if p3 .eqs. "" then p3 = "200"' in script
    assert 'if p4 .eqs. "" then p4 = "300"' in script
    print("AST manifest and DCL selection contracts: PASS (static checks)", flush=True)


HEADER = r'''
#ifndef AST_STUB_H
#define AST_STUB_H
#define __int64 long long
#define __MB() __sync_synchronize()
#define SS$_NORMAL 1
#define SS$_WASSET 9
#define SS$_WASCLR 17
#define SS$_EXQUOTA 28
#define SS$_ABORT 44
#define JPI$_KT_COUNT 1
int lib$getjpi(const int *, void *, void *, int *);
unsigned long pthread_getselfseq_np(void);
unsigned int sys$dclast(void (*)(unsigned long long), unsigned long long,
                       unsigned int);
unsigned int sys$setast(unsigned int);
int pthread_yield_np(void);
#endif
'''


PREFIX = r'''
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include "AST_STUB.H"
#include "AST_CASE.C"
#undef exit
'''


STUBS = r'''
struct pts_control pts_control;

struct sim_ast {
    void (*callback)(unsigned long long);
    unsigned long long token;
};
static pthread_mutex_t sim_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_t sim_main;
static struct sim_ast sim_queue[64];
static unsigned int sim_count, sim_disablers, sim_active;
static unsigned long sim_queued, sim_delivered, sim_services;
static _Thread_local unsigned int sim_disabled;
static _Thread_local int sim_main_ast;
static int sim_mode, sim_injected, sim_bad_first_enable;

unsigned long pthread_getselfseq_np(void)
{
    return pthread_equal(pthread_self(), sim_main) || sim_main_ast ? 1UL : 2UL;
}

int lib$getjpi(const int *item, void *pid, void *name, int *value)
{
    assert(*item == JPI$_KT_COUNT && pid == NULL && name == NULL);
    *value = sim_mode == 8 ? 1 : 2;
    return SS$_NORMAL;
}

/* Fault modes: 1 early, 2 one disabler incorrectly enables everybody,
 * 3 duplicate, 4 lost, 5 DCLAST quota error, 6 SETAST service error,
 * 7 a callback from an earlier generation. */
static void sim_dispatch(void)
{
    struct sim_ast job;
    int duplicate;
#ifndef PTS_LAST_JOIN
    if (!pthread_equal(pthread_self(), sim_main))
        return;
#endif
    /* The final-AST host model executes the callback on the posting worker
     * so it can test the outcome logic while main spins or joins. Native
     * delivery to the initial thread requires an OpenVMS run. */
    for (;;) {
        assert(pthread_mutex_lock(&sim_lock) == 0);
        if (sim_active || !sim_count ||
            (sim_disablers && sim_mode != 1 &&
             !(sim_bad_first_enable && !sim_disabled))) {
            assert(pthread_mutex_unlock(&sim_lock) == 0);
            return;
        }
        sim_active = 1;
        job = sim_queue[0];
        --sim_count;
        memmove(sim_queue, sim_queue + 1, sim_count * sizeof(sim_queue[0]));
        duplicate = sim_mode == 3 && !sim_injected;
        if (duplicate) sim_injected = 1;
        assert(pthread_mutex_unlock(&sim_lock) == 0);
        sim_main_ast = sim_mode != 9;
        job.callback(job.token);
        if (duplicate) job.callback(job.token);
        sim_main_ast = 0;
        assert(pthread_mutex_lock(&sim_lock) == 0);
        sim_delivered += duplicate ? 2 : 1;
        sim_active = 0;
        assert(pthread_mutex_unlock(&sim_lock) == 0);
    }
}

unsigned int sys$setast(unsigned int enable)
{
    unsigned int previous;
    assert(enable == 0 || enable == 1);
    assert(pthread_mutex_lock(&sim_lock) == 0);
    ++sim_services;
    if (sim_mode == 6 && !sim_injected) {
        sim_injected = 1;
        assert(pthread_mutex_unlock(&sim_lock) == 0);
        return SS$_ABORT;
    }
    previous = sim_disabled ? SS$_WASCLR : SS$_WASSET;
    if (enable && sim_disabled) {
        assert(sim_disablers != 0);
        --sim_disablers;
        sim_disabled = 0;
        if (sim_mode == 2 && sim_disablers &&
            !pthread_equal(pthread_self(), sim_main)) {
            sim_bad_first_enable = 1;
            sim_injected = 1;
        }
    } else if (!enable && !sim_disabled) {
        ++sim_disablers;
        sim_disabled = 1;
    }
    assert(pthread_mutex_unlock(&sim_lock) == 0);
    if (enable) sim_dispatch();
    return previous;
}

unsigned int sys$dclast(void (*callback)(unsigned long long),
                       unsigned long long token, unsigned int access_mode)
{
    assert(access_mode == 0);
    assert(callback != NULL);
    assert(pthread_mutex_lock(&sim_lock) == 0);
    ++sim_services;
    if (sim_mode == 5 && !sim_injected) {
        sim_injected = 1;
        assert(pthread_mutex_unlock(&sim_lock) == 0);
        return SS$_EXQUOTA;
    }
    ++sim_queued;
    if (sim_mode == 4 && !sim_injected) {
        sim_injected = 1;
        assert(pthread_mutex_unlock(&sim_lock) == 0);
        return SS$_NORMAL;
    }
    assert(sim_count < sizeof(sim_queue) / sizeof(sim_queue[0]));
    sim_queue[sim_count].callback = callback;
    if (sim_mode == 7 && !sim_injected) {
        sim_injected = 1;
        token ^= 1ULL << 32;
    }
    sim_queue[sim_count++].token = token;
    assert(pthread_mutex_unlock(&sim_lock) == 0);
    sim_dispatch();
    return SS$_NORMAL;
}

int pthread_yield_np(void)
{
    sim_dispatch();
    return sched_yield();
}

void pts_check(int rc, const char *call, int line)
{
    if (rc) {
        fprintf(stderr, "line %d: %s: %s (%d)\n", line, call,
                strerror(rc), rc);
        exit(PTS_UNRESOLVED);
    }
}

void pts_require(int truth, const char *expression, int line)
{
    if (!truth) {
        fprintf(stderr, "line %d: assertion failed: %s\n", line, expression);
        exit(PTS_FAIL);
    }
}

void pts_exit(int code)
{
    exit(code);
}

void pts_yield(unsigned long *state)
{
    *state = (*state * 1664525UL + 1013904223UL) & 0xffffffffUL;
    if ((*state & 31) == 0) (void)pthread_yield_np();
}

int main(int argc, char **argv)
{
    int result;
    assert(argc == 4);
    sim_mode = atoi(argv[1]);
    pts_control.iterations = strtoul(argv[2], NULL, 10);
    pts_control.seed = strtoul(argv[3], NULL, 10);
    pts_control.threads = 3;
    sim_main = pthread_self();
    result = test_main(0, NULL);
    if (result == PTS_PASS) {
        assert(sim_mode == 0);
        assert(sim_count == 0 && sim_disablers == 0 && !sim_active);
        assert(sim_queued != 0 && sim_queued == sim_delivered);
    } else if (result == PTS_UNSUPPORTED) {
        assert(sim_services == 0);
    }
    printf("SIM result=%d queued=%lu delivered=%lu\n", result,
           sim_queued, sim_delivered);
    return result;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    check_selection()
    checks = 0
    with tempfile.TemporaryDirectory(prefix="pts-ast-check-") as directory:
        temp = Path(directory)
        (temp / "AST_STUB.H").write_text(HEADER)
        for header in ("builtins.h", "ssdef.h", "starlet.h", "jpidef.h", "lib$routines.h"):
            (temp / header).write_text('#include "AST_STUB.H"\n')
        shutil.copyfile(PORT.parent / "include/posixtest.h",
                        temp / "PTS_ORIGINAL.H")
        for case in CASES:
            source = temp / (case + "_check.c")
            source.write_text(PREFIX.replace("AST_CASE.C", case + ".C") + STUBS)
            flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unknown-pragmas", "-pthread", "-I" + str(temp),
                     "-I" + str(PORT), "-I" + str(PORT / "LOCAL")]
            executable = temp / case
            subprocess.run(shlex.split(args.cc) + flags +
                           ["-D__VMS", "-DPTS_NATIVE_UPCALLS=1", str(source),
                            "-o", str(executable)], check=True, timeout=60)
            scenarios = [("normal", 0, 0), ("early delivery", 1, 1),
                         ("duplicate callback", 3, 1),
                         ("lost callback", 4, "timeout"),
                         ("DCLAST failure", 5, 2),
                         ("SETAST failure", 6, 2),
                         ("stale callback generation", 7, 1)]
            if case == "PT_AST_WORKER":
                scenarios.append(("first disabler releases all", 2, 1))
            if case in LAST_CASES:
                scenarios.append(("single kernel thread", 8, 4))
                scenarios.append(("wrong application thread", 9, 1))
            for label, mode, expected in scenarios:
                try:
                    iterations = "50" if mode == 0 else "4"
                    result = subprocess.run([str(executable), str(mode), iterations, "12345"],
                                            text=True, capture_output=True, timeout=3)
                except subprocess.TimeoutExpired:
                    if expected != "timeout":
                        raise
                else:
                    if result.returncode != expected:
                        raise AssertionError(
                            f"{case} {label}: expected {expected}, got "
                            f"{result.returncode}\n{result.stdout}\n{result.stderr}")
                checks += 1
                print(f"{case}: {label}: expected result", flush=True)
            for native, upcalls, label in ((True, False, "no upcalls"),
                                           (False, False, "non-OpenVMS")):
                config = (["-D__VMS"] if native else []) + [
                    "-DPTS_NATIVE_UPCALLS=" + str(int(upcalls))]
                subprocess.run(shlex.split(args.cc) + flags + config +
                               [str(source), "-o", str(executable)],
                               check=True, timeout=60)
                result = subprocess.run([str(executable), "0", "1", "1"],
                                        text=True, capture_output=True, timeout=3)
                assert result.returncode == 4, (case, label, result)
                checks += 1
        print(f"AST host checks: PASS ({checks} scenarios; simulated services)")


if __name__ == "__main__":
    main()
