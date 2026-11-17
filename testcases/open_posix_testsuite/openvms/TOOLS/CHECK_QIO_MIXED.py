#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 iamcamiel
# Copyright 2026 VMS Software, Inc.
# X-2 CV Camiel Vanderhoeven 17-Nov-2026: Check stop-before-start and partial-frame draining.
# X-1 CV Camiel Vanderhoeven 17-Nov-2026: Mixed communication service simulation.
"""Run the native mixed test with real host loopback TCP and simulated services.

The existing QIO simulation supplies disk/AST support. This extension checks
message framing, native queue call use, wake-before-hibernate behavior, and
retirement/error paths. It does not validate the OpenVMS ABI, kernel upcalls,
actual AST scheduling, or compiler lowering of the VMS queue builtins.
"""
import argparse
from pathlib import Path
import shlex
import subprocess
import tempfile
import CHECK_QIO as disk


def replace_once(text, old, new):
    assert text.count(old) == 1, old[:100]
    return text.replace(old, new)


EXTRA_HEADER = r'''
#include <sys/socket.h>
int decc$get_sdc(int);
unsigned int sys$hiber(void);
unsigned int sys$wake(void *, void *);
int __PAL_INSQTIQ(void *, void *);
int __PAL_REMQHIQ(void *, void **);
int sim_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
ssize_t sim_send(int, const void *, size_t, int);
'''


EXTRA_STUBS = r'''
static pthread_mutex_t sim_native_queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t sim_hiber_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sim_hiber_cv = PTHREAD_COND_INITIALIZER;
static int sim_wake_pending;
static unsigned char sim_net_pending;
static unsigned long sim_net_qios, sim_net_signals;
static _Atomic int sim_tcp_started, sim_tcp_finished, sim_partial_seen;
static int sim_first_send;

struct sim_start { void *(*entry)(void *); void *argument; };
static void *sim_start_worker(void *argument)
{
    struct sim_start start = *(struct sim_start *)argument;
    void *result;
    free(argument);
    if (start.entry == submitter || start.entry == waiter) {
        if (sim_mode >= 20 && sim_mode <= 26)
            while (!sim_tcp_started) sched_yield();
        if (sim_mode == 31)
            while (!sim_partial_seen) sched_yield();
        if (sim_mode == 32)
            while (!sim_tcp_finished) sched_yield();
    }
    if (start.entry == mix_net_sender && sim_mode == 30) {
        for (;;) {
            int stopped;
            assert(!pthread_mutex_lock(&mix_net_mutex));
            stopped = mix_net_stop;
            assert(!pthread_mutex_unlock(&mix_net_mutex));
            if (stopped) break;
            sched_yield();
        }
    }
    result = start.entry(start.argument);
    if (start.entry == mix_net_sender) sim_tcp_finished = 1;
    return result;
}
int sim_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                       void *(*entry)(void *), void *argument)
{
    struct sim_start *start = malloc(sizeof(*start));
    int result;
    assert(start);
    start->entry = entry;
    start->argument = argument;
    result = pthread_create(thread, attr, sim_start_worker, start);
    if (result) free(start);
    return result;
}
ssize_t sim_send(int fd, const void *buffer, size_t length, int flags)
{
    ssize_t result = send(fd, buffer, length, flags);
    if (sim_mode == 31 && !sim_first_send++) {
        /* Hold the second piece until disk completion requests stop. The
         * dispatcher must process partial reads and all disk jobs meanwhile.
         */
        assert(result > 0 && length < MIX_MESSAGE_BYTES);
        for (;;) {
            int stopped;
            assert(!pthread_mutex_lock(&mix_net_mutex));
            stopped = mix_net_stop;
            assert(!pthread_mutex_unlock(&mix_net_mutex));
            if (stopped) break;
            sched_yield();
        }
        assert(sim_partial_seen);
        fprintf(stderr, "SIM disk stopped during partial TCP frame\n");
    }
    return result;
}

int __PAL_INSQTIQ(void *q, void *e)
{
    struct mix_node *head = q, *node = e;
    int empty;
    assert(((uintptr_t)head & 15) == 0 && ((uintptr_t)node & 15) == 0);
    assert(!pthread_mutex_lock(&sim_native_queue_lock));
    empty = !head->forward;
    node->forward = 0;
    node->backward = head->backward;
    if (head->backward) ((struct mix_node *)(uintptr_t)head->backward)->forward = (uintptr_t)node;
    else head->forward = (uintptr_t)node;
    head->backward = (uintptr_t)node;
    assert(!pthread_mutex_unlock(&sim_native_queue_lock));
    return empty;
}
int __PAL_REMQHIQ(void *q, void **out)
{
    struct mix_node *head = q, *node;
    int result;
    assert(!pthread_mutex_lock(&sim_native_queue_lock));
    node = (struct mix_node *)(uintptr_t)head->forward;
    if (node) {
        head->forward = node->forward;
        if (node->forward) ((struct mix_node *)(uintptr_t)node->forward)->backward = 0;
        else head->backward = 0;
        node->forward = node->backward = 0;
    }
    result = !node ? 0 : head->forward ? 1 : 2;
    *out = node;
    assert(!pthread_mutex_unlock(&sim_native_queue_lock));
    return result;
}
unsigned int sys$wake(void *pid, void *name)
{
    assert(!pid && !name);
    assert(!pthread_mutex_lock(&sim_hiber_lock));
    sim_wake_pending = 1;
    assert(!pthread_cond_signal(&sim_hiber_cv));
    assert(!pthread_mutex_unlock(&sim_hiber_lock));
    return SS$_NORMAL;
}
unsigned int sys$hiber(void)
{
    assert(!pthread_mutex_lock(&sim_hiber_lock));
    while (!sim_wake_pending) assert(!pthread_cond_wait(&sim_hiber_cv, &sim_hiber_lock));
    sim_wake_pending = 0;
    assert(!pthread_mutex_unlock(&sim_hiber_lock));
    return SS$_NORMAL;
}
int decc$get_sdc(int fd)
{
    assert(fd >= 0 && fd < 65536 - 256);
    return fd + 256;
}
'''


def source():
    prefix = disk.PREFIX.replace('"PT_QIO_AST.C"', '"PT_QIO_MIXED.C"')
    prefix = prefix.replace('#include "PT_QIO_MIXED.C"',
        '#define pthread_create sim_pthread_create\n#define send sim_send\n'
        '#include "PT_QIO_MIXED.C"\n#undef pthread_create\n#undef send')
    text = disk.STUBS
    text = replace_once(text, 'static void sim_inject(void)', EXTRA_STUBS + '\nstatic void sim_inject(void)')
    text = replace_once(text, 'int sim_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mutex)\n{\n    int index = slot_for_cv(cv), result;\n', r'''
int sim_cond_wait(pthread_cond_t *cv, pthread_mutex_t *mutex)
{
    int index = slot_for_cv(cv), result;
    if (cv == &mix_net_cv) {
        assert(mutex == &mix_net_mutex);
        if (sim_net_pending) { sim_net_pending = 0; return 0; }
        result = pthread_cond_wait(cv, mutex);
        if (!result) sim_net_pending = 0;
        return result;
    }
''')
    text = replace_once(text, '    assert(index >= 0);\n', r'''
    if (cv == &mix_net_cv) {
        assert(pthread_equal(pthread_self(), sim_thread));
        assert(!pthread_mutex_lock(&mix_net_mutex));
        if (sim_mode == 24 && !sim_injected) {
            sim_inject();
            assert(!pthread_mutex_unlock(&mix_net_mutex));
            return EINVAL;
        }
        ++sim_signals;
        ++sim_net_signals;
        sim_net_pending = 1;
        result = pthread_cond_signal(cv);
        assert(!pthread_mutex_unlock(&mix_net_mutex));
        return result;
    }
    assert(index >= 0);
''')
    text = replace_once(text, '    assert(event == EFN$C_ENF && channel == 29 && sim_fd >= 0);', r'''
    if (channel >= 256) {
        sim_tcp_started = 1;
        assert(event == EFN$C_ENF && function == IO$_READVBLK);
        assert(token < MIX_CONNECTIONS && !vbn && !a && !b && !c);
        assert(length > 0 && length <= MIX_MESSAGE_BYTES);
        assert(!pthread_mutex_lock(&sim_lock));
        if ((sim_mode == 25 || (sim_mode == 26 && length < MIX_MESSAGE_BYTES)) && !sim_injected) {
            sim_inject();
            assert(!pthread_mutex_unlock(&sim_lock));
            return SS$_EXQUOTA;
        }
        ++sim_qios;
        ++sim_net_qios;
        assert(!pthread_mutex_unlock(&sim_lock));
        job.function |= 256;
        job.vbn = channel - 256;
        sim_enqueue(&job);
        return SS$_NORMAL;
    }
    assert(event == EFN$C_ENF && channel == 29 && sim_fd >= 0);
''')
    text = replace_once(text, '        if (job.function) {', r'''
        if (job.function & 256) {
            struct io_status *iosb = job.iosb;
            ssize_t done = recv(job.vbn, job.buffer, job.length > 7 ? 7 : job.length, MSG_DONTWAIT);
            if (done < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                sim_enqueue(&job);
                sched_yield();
                continue;
            }
            assert(done > 0);
            if ((unsigned int)done < job.length) sim_partial_seen = 1;
            iosb->status = SS$_NORMAL;
            iosb->count = (uint16_t)done;
            if (!sim_injected && sim_mode >= 20 && sim_mode <= 23) {
                sim_inject();
                if (sim_mode == 20) iosb->status = SS$_ABORT;
                if (sim_mode == 21) iosb->count = 0;
                if (sim_mode == 22) ((unsigned char *)job.buffer)[0] ^= 0x80;
                if (sim_mode == 23) continue;
            }
        } else if (job.function) {
''')
    text = replace_once(text, '        assert(sim_signals == sim_callbacks);', r'''
        assert(sim_signals == sim_callbacks - sim_net_qios + sim_net_signals);
        assert(sim_net_signals == MIX_CONNECTIONS * mix_net_rounds);
        assert(mix_net_rounds == 0 || mix_net_partial > 0);
        assert(mix_net_sender_done && mix_net_rounds <= pts_control.iterations);
        if (sim_mode == 30) assert(mix_net_rounds == 0 && sim_net_qios == 0);
        if (sim_mode == 31) assert(mix_net_rounds == 1 && sim_partial_seen);
        if (sim_mode == 32) assert(mix_net_rounds == pts_control.iterations);
        assert(mix_local_completed == MIX_LOCAL_SLOTS * pts_control.iterations);
        assert(!sim_wake_pending || mix_local_completed);
''')
    text = replace_once(text, '        assert(sim_mode == 0);',
                        '        assert(sim_mode == 0 || (sim_mode >= 30 && sim_mode <= 32));')
    text = replace_once(text, '        assert(sim_callbacks == (pts_control.threads / 2) * pts_control.iterations);',
        '        assert(sim_callbacks - sim_net_qios == (pts_control.threads / 2) * pts_control.iterations);')
    text = replace_once(text, '        if (sim_callbacks >= 16) assert(sim_dclasts != 0);',
        '        if (sim_callbacks - sim_net_qios >= 16) assert(sim_dclasts != 0);')
    return prefix + text


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', default='cc')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='pts-qio-mixed-') as temp:
        root = Path(temp)
        (root / 'QIO_STUB.H').write_text(disk.HEADER.replace('#endif', EXTRA_HEADER + '\n#endif'))
        for name in ('builtins.h', 'rms.h', 'ssdef.h', 'starlet.h', 'efndef.h', 'iodef.h', 'jpidef.h'):
            (root / name).write_text('#include "QIO_STUB.H"\n')
        for name, target in {'types.h':'sys/types.h', 'socket.h':'sys/socket.h',
                             'in.h':'netinet/in.h', 'inet.h':'arpa/inet.h',
                             'tcp.h':'netinet/tcp.h'}.items():
            (root / name).write_text('#include <' + target + '>\n')
        (root / 'PTS_ORIGINAL.H').write_bytes((disk.SUITE / 'include/posixtest.h').read_bytes())
        (root / 'sim.c').write_text(source())
        binaries = {}
        for pointers in (32, 64):
            binary = root / ('sim' + str(pointers))
            command = shlex.split(args.cc) + [
                '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-Wno-unknown-pragmas',
                '-pthread', '-D__VMS', '-DPTS_NATIVE_UPCALLS=1', '-D__INITIAL_POINTER_SIZE=' + str(pointers),
                '-I' + str(root), '-I' + str(disk.PORT), '-I' + str(disk.PORT / 'LOCAL'),
                str(root / 'sim.c'), '-o', str(binary)]
            subprocess.run(command, check=True, timeout=60)
            binaries[pointers] = binary
        guard_binary = root / 'sim_no_upcalls'
        subprocess.run(command[:-2] + ['-UPTS_NATIVE_UPCALLS', '-DPTS_NATIVE_UPCALLS=0',
                       '-o', str(guard_binary)], check=True, timeout=60)
        guard_dir = root / 'no_upcalls'
        guard_dir.mkdir()
        guard = subprocess.run([str(guard_binary), '0', '8', '20', '12345'],
                               cwd=guard_dir, capture_output=True, text=True, timeout=5)
        assert guard.returncode == 4, guard.stdout + guard.stderr
        assert 'requires @BUILD with BOTH or UPCALLS' in guard.stdout
        assert not (guard_dir / 'PTS_MIXED.DAT;1').exists()
        print('PASS: no-upcalls profile reports UNSUPPORTED before creating resources')
        # Pointer modes select allocation paths, not an emulated pointer ABI.
        cases = [(32, 0, 8, 20), (64, 0, 8, 40), (64, 0, 3, 5)]
        cases += [(pointers, mode, 8, 20) for pointers in (32, 64) for mode in (30, 31, 32)]
        cases += [(64, mode, 8, 80 if mode == 4 else 20)
                  for mode in (1, 3, 4, 5, 8, 20, 21, 22, 23, 24, 25, 26)]
        for index, (pointers, mode, workers, iterations) in enumerate(cases):
            directory = root / ('case' + str(index))
            directory.mkdir()
            timed_out = False
            try:
                result = subprocess.run([str(binaries[pointers]), str(mode), str(workers),
                    str(iterations), '12345'], cwd=directory, capture_output=True, text=True,
                    timeout=5 if mode in (23, 24) else 40)
            except subprocess.TimeoutExpired as error:
                timed_out = True
                assert mode in (23, 24), error
                assert ('SIM injected mode=' + str(mode)).encode() in (error.stderr or b'')
            if mode == 0 or mode in (30, 31, 32):
                assert result.returncode == 0, result.stdout + result.stderr
                assert 'Mixed communication completed:' in result.stdout
                assert 'Mixed disk completed=' in result.stdout
                assert 'SIM callbacks=' in result.stdout
                assert not (directory / 'PTS_MIXED.DAT;1').exists()
                if mode == 30:
                    assert 'TCP_rounds=0 TCP_stop=disk-completion' in result.stdout
                if mode == 31:
                    assert 'TCP_rounds=1 TCP_stop=disk-completion' in result.stdout
                    assert 'SIM disk stopped during partial TCP frame' in result.stderr
                if mode == 32:
                    assert 'TCP_rounds=20 TCP_stop=iteration-limit' in result.stdout
            elif mode == 23:
                assert timed_out, 'Dropped TCP completion unexpectedly finished'
            elif not timed_out:
                assert result.returncode in (1, 2), result.stdout + result.stderr
                assert 'SIM injected mode=' + str(mode) in result.stderr, result.stderr
            print(f'PASS: mixed simulation allocation={pointers} mode={mode} workers={workers}')
    print('PASS: real host loopback TCP + simulated VMS services; native validation still required')


if __name__ == '__main__':
    main()
