#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 iamcamiel
# X-2 CV Camiel Vanderhoeven 16-Nov-2026: Check native quota reporting failures.
# X-1 CV Camiel Vanderhoeven 16-Nov-2026: QIO/AST service simulation.
"""Exercise the actual native QIO stress source with controlled host services.

This is a control-flow and lifetime test, not validation of the OpenVMS ABI,
AST delivery, interrupt-safe pthread implementation, or disk driver. A host
dispatcher serializes simulated ASTs. Its signal shim locks the associated
mutex to emulate a sticky interrupt wake; production AST code does not do so.
AST-shared volatile scalars are C11 atomics only in this host simulation,
where the dispatcher is a real POSIX thread rather than an OpenVMS AST.
"""

import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile


PORT = Path(__file__).resolve().parent.parent
SUITE = PORT.parent


HEADER = r'''
#ifndef QIO_STUB_H
#define QIO_STUB_H
#include <stdint.h>
#include <stddef.h>
#define __int64 long long
#define __MB() __sync_synchronize()
#define EFN$C_ENF 128
#define SS$_NORMAL 1
#define SS$_IVCHAN 12
#define SS$_ABORT 44
#define SS$_EXQUOTA 28
#define SS$_INSFMEM 52
#define SS$_ENDOFFILE 56
#define SS$_WASSET 9
#define SS$_WASCLR 17
#define JPI$_ASTLM 101
#define JPI$_ASTCNT 102
#define JPI$_DIOLM 103
#define JPI$_DIOCNT 104
#define IO$_READVBLK 33
#define IO$_WRITEVBLK 34
#define FAB$M_UFO 0x100
#define FAB$M_GET 1
#define FAB$M_PUT 2
#define FAB$M_BIO 4
#define FAB$M_CBT 8
#define FAB$M_CTG 16
#define FAB$C_FIX 1
struct FAB {
    unsigned char fab$b_fac, fab$b_shr, fab$b_fns, fab$b_rfm;
    unsigned short fab$w_mrs;
    unsigned int fab$l_fop, fab$l_stv, fab$l_alq;
    char *fab$l_fna;
};
#define cc$rms_fab ((struct FAB){0})
unsigned int sys$open(struct FAB *);
unsigned int sys$create(struct FAB *);
unsigned int sys$dassgn(unsigned short);
unsigned int sys$getjpiw(int, void *, void *, void *, void *, void *, void *);
unsigned int sys$qio(unsigned int, unsigned short, unsigned int, void *,
    void (*)(unsigned long long), unsigned long long, void *, unsigned int,
    unsigned int, unsigned int, unsigned int, unsigned int);
unsigned int sys$dclast(void (*)(unsigned long long), unsigned long long,
    unsigned int);
int pthread_cond_signal_int_np(pthread_cond_t *);
int pthread_cond_setname_np(pthread_cond_t *, const char *, void *);
int pthread_mutex_setname_np(pthread_mutex_t *, const char *, void *);
int pthread_yield_np(void);
int sim_cond_wait(pthread_cond_t *, pthread_mutex_t *);
void *_malloc64(size_t);
#endif
'''


PREFIX = r'''
#define _POSIX_C_SOURCE 200809L
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sched.h>
#include <time.h>
#include <errno.h>
#include "QIO_STUB.H"
/* The host file has byte-stream semantics; RMS record attributes are not modeled. */
#define open(name, flags, mode, ...) open(name, flags, mode)
#define volatile _Atomic
#define pthread_cond_wait sim_cond_wait
#include "PT_QIO_AST.C"
#undef pthread_cond_wait
#undef volatile
#undef open
#undef exit
'''


STUBS = r'''
/* All system-service behavior below is deliberately a host simulation. */
struct sim_job {
    void (*callback)(unsigned long long);
    unsigned long long token;
    unsigned int function, length, vbn;
    void *iosb, *buffer;
};
static struct sim_job sim_queue[4096];
static unsigned sim_count, sim_stopping, sim_tick;
static pthread_mutex_t sim_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sim_ready = PTHREAD_COND_INITIALIZER;
static pthread_t sim_thread;
static int sim_fd = -1, sim_mode, sim_injected;
static unsigned long sim_qios, sim_dclasts, sim_callbacks, sim_signals;
static unsigned long sim_waits, sim_pending_consumed;
static unsigned char sim_pending[128];
static const char *sim_filename;

static void sim_inject(void)
{
    sim_injected = 1;
    /* Stub-only marker proves the requested fault was actually exercised. */
    fprintf(stderr, "SIM injected mode=%d\n", sim_mode);
    fflush(stderr);
}

struct pts_control pts_control;
void pts_exit(int result)
{
    fflush(stdout);
    fflush(stderr);
    _Exit(result);
}
void pts_check(int rc, const char *call, int line)
{
    if (rc) {
        fprintf(stderr, "line %d: %s: %s (%d)\n", line, call, strerror(rc), rc);
        pts_exit(PTS_UNRESOLVED);
    }
}
void pts_require(int truth, const char *assertion, int line)
{
    if (!truth) {
        fprintf(stderr, "line %d: assertion failed: %s\n", line, assertion);
        pts_exit(PTS_FAIL);
    }
}
void pts_yield(unsigned long *state)
{
    *state = (*state * 1664525UL + 1013904223UL) & 0xffffffffUL;
    if ((*state & 31) == 0) sched_yield();
}
int pthread_yield_np(void) { return sched_yield(); }
void *_malloc64(size_t size) { return malloc(size); }
int pthread_cond_setname_np(pthread_cond_t *cv, const char *name, void *arg)
{ (void)cv; assert(name && !arg); return 0; }
int pthread_mutex_setname_np(pthread_mutex_t *mutex, const char *name, void *arg)
{ (void)mutex; assert(name && !arg); return 0; }

unsigned int sys$getjpiw(int event, void *pid, void *name, void *list,
                         void *iosb, void *callback, void *argument)
{
    struct item { unsigned short length, code; void *buffer; unsigned short *returned; };
    struct item *items = list;
    unsigned int *status = iosb;
    const unsigned short codes[] = {JPI$_ASTLM, JPI$_ASTCNT, JPI$_DIOLM, JPI$_DIOCNT};
    const unsigned int values[] = {4096, 4088, 1024, 1020};
    unsigned i;
    assert(event == EFN$C_ENF && !pid && !name && !callback && !argument);
    if (sim_mode == 8) { sim_inject(); return SS$_ABORT; }
    for (i = 0; i < 4; ++i) {
        assert(items[i].length == 4 && items[i].code == codes[i]);
        assert(items[i].buffer && items[i].returned);
        memcpy(items[i].buffer, &values[i], 4);
        *items[i].returned = 4;
    }
    assert(!items[4].length && !items[4].code && !items[4].buffer && !items[4].returned);
    status[0] = SS$_NORMAL;
    if (sim_mode == 9) { sim_inject(); status[0] = 0x12340000; }
    if (sim_mode == 10) { sim_inject(); *items[0].returned = 2; }
    if (sim_mode == 11) { sim_inject(); *items[3].returned = 2; }
    return SS$_NORMAL;
}

static int slot_for_cv(pthread_cond_t *cv)
{
    unsigned i;
    for (i = 0; i < 128; ++i)
        if (cv == &slots[i].cv) return (int)i;
    return -1;
}
int sim_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mutex)
{
    int index = slot_for_cv(cv), result;
    if (index < 0) return pthread_cond_wait(cv, mutex);
    assert(mutex == &slots[index].mutex);
    __sync_fetch_and_add(&sim_waits, 1);
    if (sim_pending[index]) {
        sim_pending[index] = 0;
        __sync_fetch_and_add(&sim_pending_consumed, 1);
        return 0;
    }
    result = pthread_cond_wait(cv, mutex);
    if (!result) sim_pending[index] = 0;
    return result;
}
int pthread_cond_signal_int_np(pthread_cond_t *cv)
{
    int index = slot_for_cv(cv), result;
    assert(index >= 0);
    assert(pthread_equal(pthread_self(), sim_thread));
    assert(!pthread_mutex_lock(&slots[index].mutex));
    ++sim_signals;
    if (sim_mode == 7 && !sim_injected) {
        sim_inject();
        assert(!pthread_mutex_unlock(&slots[index].mutex));
        return EINVAL;
    }
    sim_pending[index] = 1;
    result = pthread_cond_signal(cv);
    assert(!pthread_mutex_unlock(&slots[index].mutex));
    return result;
}

static unsigned int sim_open(struct FAB *fab, int create)
{
    char filename[512];
    assert(fab->fab$l_fop & FAB$M_UFO);
    memcpy(filename, fab->fab$l_fna, fab->fab$b_fns);
    filename[fab->fab$b_fns] = 0;
    assert(sim_fd == -1);
    sim_fd = open(filename, O_RDWR | (create ? O_CREAT | O_EXCL : 0), 0600);
    if (sim_fd < 0) return SS$_ABORT;
    if (create && ftruncate(sim_fd, (off_t)fab->fab$l_alq * 512)) return SS$_ABORT;
    fab->fab$l_stv = 29;
    free((void *)sim_filename);
    sim_filename = strdup(filename);
    assert(sim_filename);
    return SS$_NORMAL;
}
unsigned int sys$open(struct FAB *fab) { return sim_open(fab, 0); }
unsigned int sys$create(struct FAB *fab) { return sim_open(fab, 1); }
unsigned int sys$dassgn(unsigned short channel)
{
    assert(channel == 29 && sim_fd >= 0);
    assert(!pthread_mutex_lock(&sim_lock));
    assert(sim_count == 0);
    assert(!pthread_mutex_unlock(&sim_lock));
    assert(!close(sim_fd)); sim_fd = -1;
    return SS$_NORMAL;
}
static void sim_enqueue(const struct sim_job *job)
{
    assert(!pthread_mutex_lock(&sim_lock));
    assert(sim_count < sizeof(sim_queue) / sizeof(sim_queue[0]));
    sim_queue[sim_count++] = *job;
    assert(!pthread_cond_signal(&sim_ready));
    assert(!pthread_mutex_unlock(&sim_lock));
}
unsigned int sys$qio(unsigned int event, unsigned short channel,
    unsigned int function, void *iosb, void (*callback)(unsigned long long),
    unsigned long long token, void *buffer, unsigned int length,
    unsigned int vbn, unsigned int a, unsigned int b, unsigned int c)
{
    struct sim_job job = { callback, token, function, length, vbn, iosb, buffer };
    int reject = 0;
    assert(event == EFN$C_ENF && channel == 29 && sim_fd >= 0);
    assert(a == 0 && b == 0 && c == 0 && token < 128);
    assert(function == IO$_READVBLK || function == IO$_WRITEVBLK);
    assert(length == 4096 && vbn == 1 + token * 8);
    assert(!pthread_mutex_lock(&sim_lock));
    ++sim_qios;
    if (sim_mode == 1 && !sim_injected) {
        sim_inject(); reject = 1;
    }
    assert(!pthread_mutex_unlock(&sim_lock));
    if (reject) return SS$_EXQUOTA;
    sim_enqueue(&job);
    return SS$_NORMAL;
}
unsigned int sys$dclast(void (*callback)(unsigned long long),
    unsigned long long token, unsigned int mode)
{
    struct sim_job job = { callback, token, 0, 0, 0, 0, 0 };
    int reject = 0;
    assert(mode == 0 && token < 128);
    assert(!pthread_mutex_lock(&sim_lock));
    ++sim_dclasts;
    if (sim_mode == 5 && !sim_injected) {
        sim_inject(); reject = 1;
    }
    assert(!pthread_mutex_unlock(&sim_lock));
    if (reject) return SS$_EXQUOTA;
    sim_enqueue(&job);
    return SS$_NORMAL;
}
static void *sim_dispatch(void *unused)
{
    (void)unused;
    for (;;) {
        struct sim_job job;
        unsigned index;
        struct timespec delay = {0, 100000};
        assert(!pthread_mutex_lock(&sim_lock));
        while (!sim_count && !sim_stopping)
            assert(!pthread_cond_wait(&sim_ready, &sim_lock));
        if (!sim_count) {
            assert(!pthread_mutex_unlock(&sim_lock));
            return NULL;
        }
        /* Mix FIFO and LIFO completion order while keeping finite jobs fair. */
        index = (++sim_tick & 1) ? 0 : sim_count - 1;
        job = sim_queue[index];
        memmove(&sim_queue[index], &sim_queue[index + 1],
                (sim_count - index - 1) * sizeof(job));
        --sim_count;
        assert(!pthread_mutex_unlock(&sim_lock));
        if (sim_tick & 1) nanosleep(&delay, NULL);
        if (job.function) {
            struct io_status *iosb = job.iosb;
            ssize_t done;
            off_t offset = ((off_t)job.vbn - 1) * 512;
            if (job.function == IO$_WRITEVBLK)
                done = pwrite(sim_fd, job.buffer, job.length, offset);
            else
                done = pread(sim_fd, job.buffer, job.length, offset);
            assert(done == (ssize_t)job.length);
            iosb->status = SS$_NORMAL;
            iosb->count = (uint16_t)done;
            if (sim_mode == 2 && !sim_injected) {
                sim_inject(); iosb->status = SS$_ABORT;
            }
            if (sim_mode == 3 && !sim_injected) {
                sim_inject(); iosb->count--;
            }
            if (sim_mode == 4 && !sim_injected && job.function == IO$_READVBLK) {
                sim_inject(); ((unsigned char *)job.buffer)[0] ^= 0x40;
            }
        }
        if (sim_mode == 6) {
            if (!sim_injected) sim_inject();
            continue; /* A missing AST must leave a real wait. */
        }
        ++sim_callbacks;
        job.callback(job.token);
    }
}
int main(int argc, char **argv)
{
    int result;
    assert(argc == 5);
    sim_mode = atoi(argv[1]);
    pts_control.threads = strtoul(argv[2], 0, 10);
    pts_control.iterations = strtoul(argv[3], 0, 10);
    pts_control.seed = strtoul(argv[4], 0, 10);
    assert(!pthread_create(&sim_thread, 0, sim_dispatch, 0));
    result = test_main(0, NULL);
    assert(!pthread_mutex_lock(&sim_lock));
    sim_stopping = 1;
    assert(!pthread_cond_signal(&sim_ready));
    assert(!pthread_mutex_unlock(&sim_lock));
    assert(!pthread_join(sim_thread, 0));
    if (result == PTS_PASS) {
        assert(sim_mode == 0);
        assert(sim_fd == -1 && sim_count == 0);
        assert(sim_callbacks == sim_qios + sim_dclasts);
        assert(sim_signals == sim_callbacks);
        assert(sim_callbacks == (pts_control.threads / 2) * pts_control.iterations);
        assert(sim_waits != 0);
        if (sim_callbacks >= 16) assert(sim_dclasts != 0);
        assert(sim_filename && access(sim_filename, F_OK) != 0);
        printf("SIM callbacks=%lu qio=%lu dclast=%lu waits=%lu sticky=%lu\n",
            sim_callbacks, sim_qios, sim_dclasts, sim_waits, sim_pending_consumed);
    }
    free((void *)sim_filename);
    return result;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    parser.add_argument("--large", action="store_true",
                        help="also attempt 600 actual host workers")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pts-qio-sim-") as temp:
        root = Path(temp)
        (root / "QIO_STUB.H").write_text(HEADER)
        for name in ("builtins.h", "rms.h", "ssdef.h", "starlet.h", "efndef.h", "iodef.h", "jpidef.h"):
            (root / name).write_text('#include "QIO_STUB.H"\n')
        (root / "PTS_ORIGINAL.H").write_bytes((SUITE / "include/posixtest.h").read_bytes())
        (root / "sim.c").write_text(PREFIX + STUBS)
        command = shlex.split(args.cc) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Wno-unknown-pragmas", "-pthread", "-D__VMS", "-I" + str(root),
            "-I" + str(PORT), "-I" + str(PORT / "LOCAL"),
            str(root / "sim.c"),
        ]
        binaries = {}
        for pointer_mode in (32, 64):
            binaries[pointer_mode] = root / f"sim{pointer_mode}"
            subprocess.run(command + [f"-D__INITIAL_POINTER_SIZE={pointer_mode}",
                                      "-o", str(binaries[pointer_mode])],
                           check=True, timeout=60)
        # These select the source's allocation branches, not the host pointer ABI.
        checks = [(32, 0, 8, 80, 31), (64, 0, 8, 800, 31), (64, 0, 3, 40, 7)]
        checks.extend((64, mode, 8, 80, 31)
                      for mode in range(1, 12))
        if args.large:
            checks.append((64, 0, 600, 2, 31))
        for number, (pointer_mode, mode, threads, iterations, seed) in enumerate(checks):
            directory = root / f"case_{number}"
            directory.mkdir()
            timed_out = False
            try:
                result = subprocess.run(
                    [str(binaries[pointer_mode]), str(mode), str(threads),
                     str(iterations), str(seed)],
                    cwd=directory, capture_output=True, text=True,
                    timeout=5 if mode in (6, 7) else 30,
                )
            except subprocess.TimeoutExpired as error:
                timed_out = True
                if mode not in (6, 7):
                    raise
                assert f"SIM injected mode={mode}".encode() in (error.stderr or b"")
            if mode == 0:
                if threads == 600 and result.returncode == 2 and "pthread_create" in result.stderr:
                    print("UNRESOLVED: host resources do not permit 600 workers")
                    continue
                if result.returncode:
                    raise AssertionError(result.stdout + result.stderr)
                assert "SIM callbacks=" in result.stdout, result.stdout
                assert "QIO quotas: ASTLM=4096 AST_available=4088 DIOLM=1024 DIO_available=1020" in result.stdout
            elif mode == 6:
                assert timed_out, "Missing completion unexpectedly allowed the test to finish"
            elif not timed_out:
                assert f"SIM injected mode={mode}" in result.stderr, result.stderr
                assert result.returncode in (1, 2), result.stdout + result.stderr
                if mode >= 8:
                    assert result.returncode == 2
                    assert "SYS$GETJPIW" in result.stderr
                    if mode == 9:
                        assert "12340000" in result.stderr
                    assert "QIO workload:" not in result.stdout
                    assert not (directory / "PTS_QIO.DAT;1").exists()
            print(f"PASS: QIO simulation allocation={pointer_mode}, "
                  f"mode={mode}, workers={threads}")
    print("PASS: native test control flow; not OpenVMS ABI/AST validation")


if __name__ == "__main__":
    main()
