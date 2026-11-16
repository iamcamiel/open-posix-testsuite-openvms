.. SPDX-License-Identifier: GPL-2.0-or-later

OpenVMS pthread test port
=========================

This is the initial OpenVMS port of a selected Open POSIX Test Suite
subset, targeting VSI C. It is not a complete
conformance suite or a certification claim.

Get the sources
---------------

Repository: https://github.com/iamcamiel/open-posix-testsuite-openvms

On an OpenVMS node with Git::

    $ GIT CLONE https://github.com/iamcamiel/open-posix-testsuite-openvms.git POSIX_TESTS
    $ SET DEFAULT [.POSIX_TESTS.TESTCASES.OPEN_POSIX_TESTSUITE.OPENVMS]

Alternatively, transfer a checkout from another machine. Preserve the directory
hierarchy. An ODS-5 volume is recommended for the original upstream filenames.
GNV, Bash, make, autoconf, Python, and the main Linux Test Project build are
not needed for the native build or execution.

Build on OpenVMS
----------------

Use VSI C, the normal OpenVMS development headers, and the native linker.
The build uses the installed pthread header and library; it does not require
private operating-system headers.

From the ``OPENVMS`` directory::

    $ @BUILD 64 BOTH

The complete interface is::

    @BUILD [32|64] [BOTH|UPCALLS|KERNEL|NONE] [pthread_shareable_image]

Defaults are 64-bit test pointers, ``BOTH``, and
``SYS$SHARE:PTHREAD$RTL.EXE``.

``BOTH`` enables multiple kernel threads and upcalls. ``UPCALLS`` and
``KERNEL`` select the individual /THREADS_ENABLE keywords. ``NONE`` uses
/NOTHREADS_ENABLE. Additional examples::

    $ @BUILD 32 BOTH
    $ @BUILD 64 UPCALLS
    $ @BUILD 64 NONE

Each build replaces the executable set in ``[.BUILD]``. Run a profile before
building the next profile. Completed runs retain their build information.

The supervisor is always compiled with 32-bit pointers for its ILE3 system
service item lists, and linked without an explicit PTHREAD$RTL dependency.
The test programs, main adapter, and test support use the selected pointer
mode and explicitly link PTHREAD$RTL.

BUILD.COM copies selected upstream files into ``[.BUILD]`` under unique
native-friendly names. It does not modify their contents. POSIXTEST.H
preserves upstream result constants and intercepts explicit exit(PTS_*)
calls, including calls made by worker threads.

Successful output includes::

    [.BUILD]PTS_RUNNER.EXE
    [.BUILD]PT_*.EXE
    [.BUILD]PT_*.LIS
    [.BUILD]PT_*.MAP
    [.BUILD]BUILD_INFO.TXT
    [.BUILD]COMPILER_VERSION.TXT
    [.BUILD]BUILD_STATE.DAT

The readiness marker becomes READY only after a successful build. A failed
build leaves BUILDING; the runner refuses to use that executable set.
Both _POSIX_EXIT and /MAIN=POSIX_EXIT are enabled so DCL receives meaningful
supervisor exit status. Correct build failures before running tests.

Run on OpenVMS
--------------

After building successfully, from the same directory::

    $ @RUN

Before the first target run, the optional native harness check is::

    $ @SELFTEST

It deliberately runs failing, crashing, and hanging fixtures outside CASES.DAT.
It checks result classification, a crash after writing PASS, token validation,
and the external watchdog. Crash fixtures may generate dumps according to the
node's dump settings. Their output stays under the normal results tree.

The complete interface is::

    @RUN [repeats [seed [threads [iterations [CONTINUE|STOP_ON_CRASH]]]]]

Defaults are one repetition, seed 1, eight workers, and 2000 iterations.
For the first target run, use a short workload::

    $ @RUN 1 1 4 200

For repeated stress testing::

    $ @RUN 100 12345 32 10000

To stop the entire run at the first CRASH and print that case's stderr::

    $ @RUN 100 12345 32 10000 STOP_ON_CRASH

The option is case-insensitive. Omitting it, or specifying CONTINUE, keeps
the existing behaviour. A CRASH is an abnormal child-process exit; FAIL,
UNRESOLVED, and TIMEOUT do not trigger this option. The runner first records
the crash in SUMMARY.CSV, then prints the case/repetition, native status,
STDERR.LOG path and contents, and totals for the completed cases. It starts
no further cases or repetitions, and returns a failure status. The original
logs and any dumps remain in the case directory. Empty or unavailable stderr
is identified explicitly. Output is read after child termination, with no
extra logging during the test workload.

After updating the sources, rebuild using ``@BUILD`` with your selected
profile so that PTS_RUNNER.EXE includes this option.

Repetitions apply to the entire selected manifest. The seed advances between
repetitions. Worker and iteration settings apply to supplemental tests;
upstream tests retain their original fixed workloads. A seed controls yield
injection, not the operating system scheduler, and does not guarantee identical
interleavings.

Choose settings appropriate for the node's resources. Condition-variable churn
can have approximately two pthreads per worker active, plus the initial thread.
Setup/resource errors are UNRESOLVED rather than silently reducing the workload.
The runner accepts 1 through 1024 workers. The QIO case requires at least two.

Each case has a timeout in CASES.DAT. Increase it deliberately for substantially
larger workloads. A timeout is a diagnostic failure, not proof of a library bug.

To invoke the supervisor directly with a custom manifest::

    $ PTS_RUNNER :== $DISK:[PATH.TO.OPENVMS.BUILD]PTS_RUNNER.EXE
    $ PTS_RUNNER CASES.DAT "[.RESULTS]" 1 12345 8 2000
    $ PTS_RUNNER CASES.DAT "[.RESULTS]" 100 12345 8 2000 STOP_ON_CRASH

Replace the device/directory. The output directory must already exist.
Custom manifests use the same eight-column format and reference images already
built in ``[.BUILD]``.

QIO completion and AST stress
-----------------------------

``PT_QIO_AST`` is an original supplemental test of the I/O pattern described
by Mimer. It uses public OpenVMS services and pthread interfaces; it contains
no Mimer implementation code. The normal manifest runs a short instance with
a 300-second external deadline. On non-OpenVMS hosts it reports UNSUPPORTED.

Build all test images as usual, then select just this case::

    $ @BUILD 64 BOTH
    $ @RUN_QIO 1 12345 8 200 300
    $ @RUN_QIO 10 12345 32 10000 3600
    $ @RUN_QIO 1 12345 600 10000 7200

The interface is::

    @RUN_QIO [repeats [seed [threads [iterations [timeout_seconds [CONTINUE|STOP_ON_CRASH]]]]]]

Defaults are 1 repetition, seed 1, 8 workers, 2000 submissions per submitting
worker, and a 3600-second deadline per repetition. Timeouts may be 1 through
86400 seconds. RUN_QIO creates a fresh one-case manifest under RESULTS;
the supervisor also saves it with the run. CASES.DAT remains unchanged.

For repeated QIO stress that stops and prints stderr on the first CRASH::

    $ @RUN_QIO 100 12345 32 10000 3600 STOP_ON_CRASH

The sixth argument has the same semantics as the fifth argument to RUN.
It defaults to CONTINUE; the fifth argument remains the per-repetition timeout.

The requested workers are split between submitters and waiters: half,
rounded down, submit requests; the rest wait for them. Every request is
awaited by a different thread from its submitter. Thus 600 means 300 submitting
plus 300 waiting pthreads, in addition to the initial thread and runtime
threads. The request count is ``floor(threads / 2) * iterations`` per repetition.
The 600/10000 example submits 3,000,000 requests. It does not create 600 kernel
threads explicitly, and does not represent 600 full database sessions.

The workload uses:

* 128 reusable slots, each with a persistent CV, mutex, IOSB and 4 KiB buffer.
  The 64-bit build uses _malloc64 for the slot pool, exercising long pointers
  like those in the Mimer dump; the startup output records its actual address.
  The 32-bit build uses ordinary allocation. RMS control structures remain
  in 32-bit address space as required by their pointer fields.
* Asynchronous disk SYS$QIO on an exclusively created private scratch file in
  the child case directory. Each slot owns a separate disk extent. Successful
  writes are followed by reads that verify their contents.
* An AST that publishes completion and calls pthread_cond_signal_int_np without
  taking the slot mutex. Waiters use a predicate loop with pthread_cond_wait;
  event flags and SYS$SYNCH do not replace the CV wakeup path.
* Synthetic early request errors completed by SYS$DCLAST, matching Mimer's
  error-completion mechanism. Every sixteenth request uses this path, and
  DCLAST submission status is checked.
* Both issue-before-handoff and handoff-before-issue ordering. The latter is a
  deliberate stress variation to encourage a waiter to block before completion.
  The first request waits for its consumer to enter the wait loop before issue,
  ensuring even a short successful run calls pthread_cond_wait on an I/O slot.
* Rapid reuse, independently queued work, IOSB/byte-count checks and data checks.
  The startup/summary output identifies the workload and observed wait paths.

There is no per-I/O formatted logging. Each AST also publishes a final marker
so the harness cannot reuse a slot while the callback is still updating its
diagnostic fields. A waiter checks this marker only after its CV predicate has
completed; it cannot rescue a missing CV wakeup. This extra lifetime check is
a test-harness difference from the supplied Mimer code.

Use BOTH first: it enables upcalls and multiple kernel threads, as needed to
exercise AST delivery alongside multi-VP scheduling. Run separate builds with
32-bit and 64-bit pointers, and optionally KERNEL, UPCALLS and NONE as comparison
profiles. Retain each build's listings/maps and actual RTL identification.
Native Alpha and IA64 builds also need validation; host simulations do not
validate their compiler, pointer modes, or service ABI.

Large runs require enough process/thread address space, page-file quota and
AST/direct-I/O quotas for the outstanding requests and runtime activity.
Workers request 512 KiB stacks, so 600 workers request about 300 MiB of worker
stack address space before guards and runtime overhead. Quota failures are
reported as UNRESOLVED, never as a successful reduced workload. Watch the
reported native status if setup or submission fails. The scratch file is
small and repeatedly reused; this is primarily synchronization pressure,
not a recreation of the production disk/cache or database workload.

The supervisor explicitly passes its ASTLM and DIOLM limits to each child.
A null SYS$CREPRC quota list would instead select the system defaults/minima,
which may be much smaller than the invoking process's quotas. For example,
PQL_DASTLM=24 can cause intermittent SS$_EXQUOTA (0000001C) even with eight
workers, because workers can submit multiple outstanding requests. No change
to system-wide SYSGEN defaults is needed when the invoking process already
has sufficient quotas.

The supervisor prints the requested limits, and the QIO child's STDOUT.LOG
prints its actual ASTLM/DIOLM and remaining AST/direct-I/O counts before work
begins. Remaining counts are snapshots, not peak usage or the configured
limits. If EXQUOTA still occurs, check these values and the failing service;
EXQUOTA from SYS$DCLAST specifically indicates exhausted AST quota. Account
quota changes require appropriate administration and a new login to take
effect; they are not made by the suite. Other quotas retain the existing
SYS$CREPRC behavior. The workload is not silently reduced to fit low quotas.

A successful long run does not rule out the production defect. In particular,
600 active database users need not correspond to 600 pthreads. Test both
moderate worker counts near the server's actual thread count and higher counts,
vary seeds and repeats, and compare installed and candidate RTL images. The
external supervisor reports TIMEOUT or CRASH without relying on pthreads to
remain functional. Preserve the case directory, logs and dumps on failure.

Process isolation and watchdog
------------------------------

On OpenVMS, each test image is started directly through SYS$CREPRC as a
subprocess. LOGINOUT and a command shell are not executed. Detached-process
creation is a separate planned OpenVMS integration test.

The supervisor uses a private termination mailbox for the child's final
status and a relative SYS$SETIMR timer for its deadline. Its watchdog does not
use pthreads. On timeout it requests deletion of the specific child and waits
for termination. Supervision failure stops the run.

The child reads a control file from SYS$INPUT, enters its case directory,
runs test_main, and writes a result containing its case id and per-run token.
The supervisor waits for termination before accepting the record. A PASS
record never overrides an abnormal process exit.
An atomic claim selects one result writer when several workers report errors
together. Other reporters wait for termination instead of opening competing
result files. Exclusive file creation still detects genuinely stale files.

Results and diagnostics
-----------------------

Each run atomically reserves a new directory beneath ``[.RESULTS]``. A serial
suffix handles repeated images in the same DCL process within one second;
existing directories and files are never reused::

    [.RESULTS.R_<pid>_<time>_<serial>]SUMMARY.CSV
    [.RESULTS.R_<pid>_<time>_<serial>]MANIFEST.DAT
    [.RESULTS.R_<pid>_<time>_<serial>]BUILD_INFO.TXT

Each case/repetition has a separate child directory containing::

    CONTROL.DAT
    STDOUT.LOG
    STDERR.LOG
    RESULT.PTS

The summary records the case, repetition, classification, PTS result, raw
native exit status, seed, threads, iterations, elapsed time, and artifact path.

Classifications:

* PASS: valid PTS_PASS record and clean process termination.
* FAIL: a test assertion failed.
* UNRESOLVED: setup/environment prevented a conclusion.
* UNSUPPORTED: the test explicitly reported an unsupported capability.
* UNTESTED: the test explicitly reported it did not exercise the assertion.
* CRASH: abnormal process termination.
* TIMEOUT: the external deadline expired.
* HARNESS_ERROR: absent, malformed, stale, or mismatched result, or failed
  supervision. Inspect the logs and native exit status.
* BUILD_ERROR: the expected test executable was not found.

FAIL, UNRESOLVED, CRASH, TIMEOUT, HARNESS_ERROR, and BUILD_ERROR make the overall
run fail. An entirely unsupported/untested run also fails. Unsupported and
untested counts remain visible when other cases pass.

After the child enters its case directory, dumps and pthread_dump.log should
be written there. Image/library initialization can fail before this directory
change, so also check the supervisor's default directory. Dump creation still
depends on the node's settings, quotas, and available disk space.

Testing a candidate runtime
---------------------------

Select a candidate shareable image at build time::

    $ @BUILD 64 BOTH DISK:[PTHREAD.CANDIDATE]PTHREAD$RTL.EXE

Ensure the image activator also loads the intended image in the child
processes. A process-table logical in the invoking DCL process alone is not a
reliable configuration mechanism for a raw SYS$CREPRC child. One approach is
a job-table logical visible to the supervisor and its subprocesses::

    $ DEFINE/JOB PTHREAD$RTL DISK:[PTHREAD.CANDIDATE]PTHREAD$RTL.EXE
    $ @RUN 1 12345 8 2000
    $ DEASSIGN/JOB PTHREAD$RTL

If a job-table definition existed, record and restore it instead of discarding
it. Use the matching PTHREAD$DBGSHR when analyzing dumps. Record actual runtime
image versions/link dates in a failure report; build metadata alone does not
prove which image the activator loaded.

Coverage and deferred cases
---------------------------

API_COVERAGE.CSV inventories 117 interfaces found in the reference pthread
header and 23 optional interfaces absent from it. It distinguishes
declared_vms, unix_only, obsolete, and not_declared. Absence from that reference
header is not a claim about all OpenVMS releases.

Coverage is labelled partial, exercised_only, planned, or excluded. Calling an
API does not establish complete assertion coverage. Native runtime validation
remains pending until execution on OpenVMS.

HEADER_REFERENCE.TXT identifies the reference header by hash. The header itself
is not included in this public repository. With a reference header available,
regenerate the inventory using::

    python3 TOOLS/COVERAGE.py /path/to/pthread.h

The generator's explicit UNIX-only/obsolete lists require review when changing
the header/version.

DEFERRED_CASES.CSV identifies 21 upstream cases held out after initial review.
Reasons include unsynchronized shared-variable polls, timing assumptions, and
invalid handle/object use. Their source is available at the pinned upstream
revision; these omitted cases are not reported as passes or covered assertions.

Supplemental tests cover thread lifetimes; recursive/error-check mutexes;
condition handoff, broadcast and timeout; deferred cancellation with mutex
reacquisition; concurrent once initialization and TSD destructors; reader/writer
exclusion; mutex contention; and legal concurrent condition-variable lifetimes.
The QIO/AST case adds partial native coverage of interrupt-safe CV signalling
with persistent I/O slots, cross-thread waiting, and slot reuse.

Planned work includes the full attributes/error matrix, scheduling/priorities,
additional cancellation modes and points, stack/guard boundaries, other AST cases,
detached startup, native exception/unwind behavior, register/FP preservation,
CPU-time accounting, remaining _np extensions, and target-side primitive tests.

Repository scope
----------------

This fork retains the sources used by the initial OpenVMS subset, their
assertion descriptions, licence/provenance records, and development checks.
Unrelated Linux kernel tests and unused LTP build infrastructure are removed.
Omitted upstream tests can be retrieved from the pinned upstream commit.

Keep selected upstream files unchanged. Audit synchronization, dependencies,
pointer usage, and assumptions before adding a case. Record original blob
hashes. Label newly constructed tests as supplemental.

Git author: iamcamiel <iamcamiel@gmail.com>.
Revision-history initials/name: CV, Camiel Vanderhoeven.

For changes to the supervisor, run these developer checks on a POSIX host
with Python 3 and a C compiler, from the ``OPENVMS`` directory::

    python3 TOOLS/CHECK_RUNNER.py
    python3 TOOLS/CHECK_NATIVE.py

CHECK_RUNNER executes real child processes to verify stop-on-crash, stderr
replay, retained results, and continuation after other classifications.
CHECK_NATIVE compiles the OpenVMS supervisor branch against system-service
stubs. These checks do not replace native builds and execution on OpenVMS.
