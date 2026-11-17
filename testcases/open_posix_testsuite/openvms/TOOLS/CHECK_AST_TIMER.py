#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 iamcamiel
# Copyright 2026 VMS Software, Inc.
# X-1 CV Camiel Vanderhoeven 17-Nov-2026: Check timer AST regression tests.
"""Validate native timer-AST tests with real host waits and simulated services.

The actual test sources run with host pthreads and timed condition waits.
SETIMR queues simulated timer ASTs, delivered serially on the initial thread
at service/yield/wait-return boundaries. This tests the assertions and error
paths; it does not validate native OpenVMS timers, AST scheduling or ABI.
"""

import argparse
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile

import CHECK_AST as base


PORT = Path(__file__).resolve().parent.parent
CASES = ("PT_AST_TIMER", "PT_AST_TIMER_PRINTF", "PT_AST_TIMER_FLUSH", "PT_AST_TIMER_CRTL",
         "PT_AST_TIMER_ENABLE")


def replace_once(text, old, new):
    assert text.count(old) == 1, old
    return text.replace(old, new)


HEADER = base.HEADER.replace("#endif", r'''
#define EFN$C_ENF 128
typedef long long GENERIC_64;
unsigned int sys$setimr(unsigned int, const GENERIC_64 *,
                       void (*)(unsigned long long), unsigned long long,
                       unsigned int);
unsigned int sys$gettim(GENERIC_64 *);
int sim_timedwait(pthread_cond_t *, pthread_mutex_t *, const struct timespec *);
int sim_join(pthread_t, void **);
int sim_printf(const char *, ...);
int sim_fflush(FILE *);
#endif
''')


TIMERS = r'''
static int sim_pre_enable_print;

/* Relative SETIMR intervals use 100-ns units. Timer eligibility uses the host
 * monotonic clock; GETTIM has the OpenVMS epoch so interval comparisons are
 * meaningful without teaching the test about the host's time representation.
 */
unsigned int sys$setimr(unsigned int efn, const GENERIC_64 *interval,
                       void (*callback)(unsigned long long),
                       unsigned long long token, unsigned int flags)
{
    struct timespec now;
    long long delta;
    assert(efn == EFN$C_ENF && flags == 0 && callback != NULL);
    memcpy(&delta, interval, sizeof(delta));
    assert(delta < 0);
    assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
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
    sim_queue[sim_count].token = token;
    sim_queue[sim_count++].due_ns = (long long)now.tv_sec * 1000000000LL +
                                  now.tv_nsec - delta * 100;
    assert(pthread_mutex_unlock(&sim_lock) == 0);
    sim_dispatch();
    return SS$_NORMAL;
}

unsigned int sys$gettim(GENERIC_64 *value)
{
    struct timespec now;
    long long ticks;
    assert(pthread_mutex_lock(&sim_lock) == 0);
    ++sim_services;
    if (sim_mode == 8 && !sim_injected) {
        sim_injected = 1;
        assert(pthread_mutex_unlock(&sim_lock) == 0);
        return SS$_ABORT;
    }
    assert(pthread_mutex_unlock(&sim_lock) == 0);
    assert(clock_gettime(CLOCK_REALTIME, &now) == 0);
    ticks = ((long long)now.tv_sec + 3506716800LL) * 10000000LL +
            now.tv_nsec / 100;
    memcpy(value, &ticks, sizeof(ticks));
    return SS$_NORMAL;
}

int sim_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex,
                  const struct timespec *deadline)
{
    static _Thread_local unsigned int spurious;
    static _Thread_local struct timespec first_deadline;
    int status;
    if (sim_mode == 9)
        return ETIMEDOUT;
    if (sim_mode == 10) {
        if (spurious == 0)
            first_deadline = *deadline;
        else if (spurious <= 2)
            assert(deadline->tv_sec == first_deadline.tv_sec &&
                   deadline->tv_nsec == first_deadline.tv_nsec);
    }
    if (sim_mode == 10 && spurious++ < 2) {
        assert(pthread_mutex_unlock(mutex) == 0);
        (void)sched_yield();
        assert(pthread_mutex_lock(mutex) == 0);
        return 0;
    }
    status = pthread_cond_timedwait(condition, mutex, deadline);
    sim_dispatch();
    return status;
}

int sim_join(pthread_t thread, void **value)
{
    int status = pthread_join(thread, value);
    /* Simulate delivery escaping the main thread's disable when a worker
     * exits. The test must detect it before the explicit final enable. */
    if (sim_mode == 12) {
        assert(pthread_mutex_lock(&sim_lock) == 0);
        sim_bad_first_enable = 1;
        sim_injected = 1;
        assert(pthread_mutex_unlock(&sim_lock) == 0);
    }
    sim_dispatch();
    return status;
}

int sim_printf(const char *format, ...)
{
    va_list arguments, copy;
    char text[512];
    int status, inject = 0;
    va_start(arguments, format);
    va_copy(copy, arguments);
    (void)vsnprintf(text, sizeof(text), format, copy);
    va_end(copy);
    assert(pthread_mutex_lock(&sim_lock) == 0);
    if (sim_disablers) ++sim_disabled_prints;
    if (pthread_equal(pthread_self(), sim_main)) {
        /* No test printf is allowed in simulated AST context. */
        assert(!sim_active);
        if (strstr(text, "main re-enables") && sim_disabled) {
            sim_pre_enable_print = 1;
            if (sim_mode == 13) {
                assert(sim_count != 0);
                sim_bad_first_enable = 1;
                sim_injected = 1;
                inject = 1;
            }
        }
    }
    assert(pthread_mutex_unlock(&sim_lock) == 0);
    if (inject) sim_dispatch();
    status = vprintf(format, arguments);
    va_end(arguments);
    return status;
}

int sim_fflush(FILE *stream)
{
    int inject = 0;
    assert(pthread_mutex_lock(&sim_lock) == 0);
    if (sim_disablers) ++sim_disabled_flushes;
    if (pthread_equal(pthread_self(), sim_main)) {
        assert(!sim_active);
        if (sim_mode == 14 && sim_pre_enable_print && sim_disabled) {
            assert(sim_count != 0);
            sim_bad_first_enable = 1;
            sim_injected = 1;
            inject = 1;
        }
    }
    assert(pthread_mutex_unlock(&sim_lock) == 0);
    if (inject) sim_dispatch();
    return fflush(stream);
}
'''


def program(case):
    prefix = base.PREFIX.replace('#include "AST_CASE.C"',
        '#include <time.h>\n#include <errno.h>\n#include <stdarg.h>\n'
        '#define pthread_cond_timedwait sim_timedwait\n'
        '#define pthread_join sim_join\n'
        '#define printf sim_printf\n'
        '#define fflush sim_fflush\n'
        f'#include "{case}.C"\n'
        '#undef pthread_cond_timedwait\n#undef pthread_join\n#undef printf\n#undef fflush')
    stubs = replace_once(base.STUBS, "    unsigned long long token;",
                         "    unsigned long long token;\n    long long due_ns;")
    stubs = replace_once(stubs,
        "static int sim_mode, sim_injected, sim_bad_first_enable;",
        "static int sim_mode, sim_injected, sim_bad_first_enable;\n"
        "static unsigned long sim_disabled_prints, sim_disabled_flushes;")
    stubs = replace_once(stubs, "    int duplicate;", "    int duplicate;\n"
        "    unsigned int ready;\n    struct timespec now;\n    long long now_ns;")
    stubs = replace_once(stubs,
        "        sim_active = 1;\n        job = sim_queue[0];\n"
        "        --sim_count;\n"
        "        memmove(sim_queue, sim_queue + 1, sim_count * sizeof(sim_queue[0]));",
        "        assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);\n"
        "        now_ns = (long long)now.tv_sec * 1000000000LL + now.tv_nsec;\n"
        "        for (ready = 0; ready < sim_count; ++ready)\n"
        "            if (sim_queue[ready].due_ns <= now_ns) break;\n"
        "        if (ready == sim_count) {\n"
        "            assert(pthread_mutex_unlock(&sim_lock) == 0);\n"
        "            return;\n        }\n"
        "        sim_active = 1;\n        job = sim_queue[ready];\n"
        "        --sim_count;\n"
        "        memmove(sim_queue + ready, sim_queue + ready + 1,\n"
        "                (sim_count - ready) * sizeof(sim_queue[0]));")
    # A worker's already-enabled SETAST(1) must not release the main thread's
    # disable. Deliberately violate that contract in mode 7.
    stubs = replace_once(stubs,
        "    assert(pthread_mutex_unlock(&sim_lock) == 0);\n"
        "    if (enable) sim_dispatch();",
        "    if (sim_mode == 7 && enable &&\n"
        "        !pthread_equal(pthread_self(), sim_main)) {\n"
        "        sim_bad_first_enable = 1;\n        sim_injected = 1;\n    }\n"
        "    assert(pthread_mutex_unlock(&sim_lock) == 0);\n"
        "    if (enable) sim_dispatch();")
    stubs = replace_once(stubs,
        "!(sim_bad_first_enable && !sim_disabled)",
        "!sim_bad_first_enable")
    stubs = replace_once(stubs, "assert(sim_mode == 0);",
        "assert(sim_mode == 0 || sim_mode == 10 ||\n"
        "               (sim_mode == 7 && !sim_injected));\n"
        "        if (PTS_AST_TIMER_ACTIVITY == 0) {\n"
        "            assert(sim_disabled_prints == 0 && sim_disabled_flushes == 0);\n"
        "        } else if (PTS_AST_TIMER_ACTIVITY == 1) {\n"
        "            assert(sim_disabled_prints != 0 && sim_disabled_flushes == 0);\n"
        "        } else {\n"
        "            assert(sim_disabled_prints != 0 && sim_disabled_flushes != 0);\n"
        "        }")
    return prefix + stubs + TIMERS


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    checks = 0
    with tempfile.TemporaryDirectory(prefix="pts-ast-timer-check-") as directory:
        temp = Path(directory)
        (temp / "AST_STUB.H").write_text(HEADER)
        for header in ("builtins.h", "efndef.h", "gen64def.h", "ssdef.h", "starlet.h"):
            (temp / header).write_text('#include "AST_STUB.H"\n')
        for name in (*[case + ".C" for case in CASES], "PT_AST_TIMER_COMMON.H", "PTS_CHECK.H"):
            shutil.copyfile(PORT / "LOCAL" / name, temp / name)
        for name in ("POSIXTEST.H", "PTS_PORT.H"):
            shutil.copyfile(PORT / name, temp / name)
        shutil.copyfile(PORT.parent / "include/posixtest.h", temp / "PTS_ORIGINAL.H")
        for case in CASES:
            source = temp / (case + "_check.c")
            source.write_text(program(case))
            flags = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                     "-Wno-unknown-pragmas", "-pthread", "-I" + str(temp)]
            executable = temp / case
            subprocess.run(shlex.split(args.cc) + flags +
                           ["-D__VMS", "-DPTS_NATIVE_UPCALLS=1", str(source),
                            "-o", str(executable)], check=True, timeout=60)
            scenarios = [("normal", 0, 0), ("premature AST delivery", 1, 1),
                         ("duplicate callback", 3, 1),
                         ("lost callback", 4, "timeout"),
                         ("SETIMR failure", 5, 2), ("SETAST failure", 6, 2),
                         ("worker enable isolation", 7,
                          1 if case == "PT_AST_TIMER_ENABLE" else 0),
                         ("GETTIM failure", 8, 2), ("premature timeout", 9, 1),
                         ("spurious condition wakeups", 10, 0),
                         ("delivery during worker exit", 12, 1)]
            if case in ("PT_AST_TIMER_PRINTF", "PT_AST_TIMER_FLUSH", "PT_AST_TIMER_CRTL"):
                scenarios.append(("delivery during final pre-enable printf", 13, 1))
            if case in ("PT_AST_TIMER_FLUSH", "PT_AST_TIMER_CRTL"):
                scenarios.append(("delivery during final pre-enable fflush", 14, 1))
            for label, mode, expected in scenarios:
                try:
                    result = subprocess.run([str(executable), str(mode), "1", "12345"],
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
            for native, label in ((True, "no upcalls"), (False, "non-OpenVMS")):
                config = (["-D__VMS"] if native else []) + ["-DPTS_NATIVE_UPCALLS=0"]
                subprocess.run(shlex.split(args.cc) + flags + config +
                               [str(source), "-o", str(executable)],
                               check=True, timeout=60)
                result = subprocess.run([str(executable), "0", "1", "1"],
                                        text=True, capture_output=True, timeout=3)
                assert result.returncode == 4, (case, label, result)
                checks += 1
        print(f"Timer AST host checks: PASS ({checks} scenarios; simulated services)")


if __name__ == "__main__":
    main()
