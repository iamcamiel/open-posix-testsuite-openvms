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

Disabled-AST regressions
------------------------

Three original supplemental cases check deferred AST delivery in a threaded
process using the public SYS$SETAST and SYS$DCLAST services:

* ``PT_AST_MAIN`` disables ASTs in the initial thread, queues callbacks, and
  makes progress through worker handoffs while delivery remains disabled.
  Repeated disabling is checked for idempotence: one enable must suffice.
* ``PT_AST_WORKER`` has two helpers disable ASTs concurrently. Re-enabling
  the first helper must not release queued callbacks; re-enabling the last
  helper must release them.
* ``PT_AST_CALLBACK`` queues a follower from an AST and disables delivery
  before returning. The initial thread must regain control, make progress,
  and later re-enable delivery of the follower.

Each case checks for premature, missing, or duplicate callbacks. Disabled-window
handshakes use ordinary thread synchronization and do not wait for AST completion.
AST callbacks use no ordinary pthread locking or waiting routines. These tests
exercise the AST delivery path without adding logging to the runtime.

All three are built by ``@BUILD`` and included in ``@RUN``. To run only these
cases, from the ``OPENVMS`` directory::

    $ @BUILD 64 BOTH
    $ @RUN_AST 1 12345 200 300
    $ @RUN_AST 100 12345 2000 300 STOP_ON_CRASH

The interface is::

    @RUN_AST [repeats [seed [iterations [timeout_seconds [CONTINUE|STOP_ON_CRASH]]]]]

Defaults are one repetition, seed 1, 200 iterations per case, and a 300-second
deadline per case. There is no worker-count argument: each case uses the initial
thread and two helpers. Every iteration repeats the scenario; the seed varies
yield injection. The supervisor runs each case in a separate process and retains
its STDOUT.LOG, STDERR.LOG, and SUMMARY.CSV entry under ``[.RESULTS]``.

Use ``BOTH`` or ``UPCALLS``. Builds with ``KERNEL`` or ``NONE`` and non-OpenVMS
host builds report these cases UNSUPPORTED. The tests use public services and
are intended for Alpha, IA64, and x86, with 32-bit or 64-bit test pointers.
Successful host checks do not establish native correctness on any architecture.
Rebuild and run each pointer/profile combination separately, for example::

    $ @BUILD 32 BOTH
    $ @RUN_AST 1 12345 200 300
    $ @BUILD 64 UPCALLS
    $ @RUN_AST 1 12345 200 300

The external watchdog is essential: a runtime spinning in AST delivery may
never return to the test. That case is recorded as TIMEOUT. A timeout alone
does not prove the cause; retain the logs and inspect the process while it is
still running if a stack trace is needed. ``STOP_ON_CRASH`` stops on CRASH,
as elsewhere in the suite; it does not stop on TIMEOUT or FAIL.

These are AST integration regressions, not additional POSIX conformance
assertions. SYS$SETAST reports the previous enable state with SS$_WASSET or
SS$_WASCLR; see the `OpenVMS system services reference
<https://docs.vmssoftware.com/vsi-openvms-system-services-reference-manual-getutc-z/>`_.

QIO completion and AST stress
-----------------------------

``PT_QIO_AST`` is an original supplemental test of asynchronous disk I/O
completion. It uses public OpenVMS services and pthread interfaces. The normal
manifest runs a short instance with
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
  above the 32-bit address range; startup output records its actual address.
  The 32-bit build uses ordinary allocation. RMS control structures remain
  in 32-bit address space as required by their pointer fields.
* Asynchronous disk SYS$QIO on an exclusively created private scratch file in
  the child case directory. Each slot owns a separate disk extent. Successful
  writes are followed by reads that verify their contents.
* An AST that publishes completion and calls pthread_cond_signal_int_np without
  taking the slot mutex. Waiters use a predicate loop with pthread_cond_wait;
  event flags and SYS$SYNCH do not replace the CV wakeup path.
* Synthetic early request errors completed by SYS$DCLAST so both disk and
  error completions run in AST context. Every sixteenth request uses this path, and
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
an additional test-harness lifetime safeguard.

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

Mixed QIO and communication stress
----------------------------------

``PT_QIO_MIXED`` is a separate supplemental workload. The original
``PT_QIO_AST`` and ``@RUN_QIO`` remain unchanged for direct comparisons.
Build with upcalls enabled and select the mixed case explicitly::

    $ @BUILD 64 BOTH
    $ @RUN_QIO_MIXED 1 12345 8 200 300 STOP_ON_CRASH
    $ @RUN_QIO_MIXED 10000 12345 32 40000 3600 STOP_ON_CRASH

The six arguments and defaults match ``@RUN_QIO``::

    @RUN_QIO_MIXED [repeats [seed [threads [iterations [timeout_seconds [CONTINUE|STOP_ON_CRASH]]]]]]

The normal manifest also includes a short mixed run. TCP/IP must be started
and support the socket/QIO interfaces. The listener binds only to loopback
(127.0.0.1), using an automatically assigned port. No external client or server
is required. Setup and quota failures are reported as UNRESOLVED.

Three workloads start together:

* Disk: the same 128-slot asynchronous QIO, cross-thread completion waits,
  read-back validation, and declared-AST error completion as the original case.
* Local communication: 16 persistent message slots, native interlocked queues,
  and a dedicated communication thread using SYS$HIBER. A sender inserts a batch
  and calls SYS$WAKE; the communication thread dispatches it using ordinary
  pthread mutexes and condition variables. A worker verifies each message and
  acknowledges it before the slot can be reused.
* TCP communication: 16 loopback connections with outstanding SYS$QIO reads.
  Each message is sent in two pieces. Completion ASTs accumulate partial reads,
  enqueue a complete message, and call pthread_cond_signal_int_np to wake a
  worker. The worker validates contents and acknowledges completion only after
  the AST has published its final marker. TCP may coalesce the sends; partial
  completions are handled but not guaranteed on every native run.

The worker argument still counts disk submitters and waiters, split as in
``RUN_QIO``. Five additional pthreads implement the local sender, communication
thread and worker, plus the TCP sender and worker. Each requests a 512 KiB stack.
Per pass, disk requests total ``floor(threads / 2) * iterations`` and local
messages total ``16 * iterations``. TCP sends at most ``16 * iterations``
messages, stopping new batches when all disk workers finish. Any TCP batch
already started (up to 16 messages) completes both send pieces, receives all
remaining data, and waits for worker acknowledgements after AST retirement
before sockets are closed. Pending reads are not cancelled.

With 32 workers and 40000 iterations, disk and local paths each process 640000
requests/messages; the TCP total depends on how much work completes alongside
disk I/O. The final report prints the actual TCP message and round counts.
A very short run can finish disk I/O before TCP starts and report zero TCP
messages. Local messaging still completes its fixed workload, so it can
continue after disk and TCP stop. Rebuild with ``@BUILD 64 BOTH`` after
updating the test source.

The local queue uses memory shared within this process; it does not create
cross-process global sections. Communication workers are separate from the disk
workers. Connection count and message size are bounded test settings, not a model
of thousands of production sessions. The output records counts, actual partial
reads, waits, and queue completion checks without per-request formatted logging.

Use ``BOTH`` first. ``UPCALLS`` is also supported by the test selection. Builds
with ``KERNEL`` or ``NONE`` report this case UNSUPPORTED because the in-process
HIBER/WAKE workload needs pthread upcalls to avoid blocking its own producers.
``BUILD.COM`` records that link-profile choice in the test compile definitions.
All three architectures and both pointer modes still require native validation;
host service simulations cannot validate AST delivery or OpenVMS socket ABI.

Intentional crash and dump checks
--------------------------------

These diagnostics deliberately terminate one isolated child process. They are
built by ``@BUILD`` but live in ``CRASH_CASES.DAT``, outside the normal ``@RUN``
manifest, and are never started by ``@RUN_QIO`` or ``@RUN_QIO_MIXED``. An explicit
mode is required; ``@RUN_CRASH`` without a mode only prints usage.

Build and run from the ``OPENVMS`` directory::

    $ @BUILD 64 BOTH
    $ @RUN_CRASH BUGCHECK
    $ @RUN_CRASH ACCVIO

The interface is::

    @RUN_CRASH BUGCHECK|ACCVIO [timeout_seconds]

The default external deadline is 600 seconds, including image-dump creation;
it can be set from 1 through 86400. Use a separate checkout/build directory
when another stress campaign is running, so its executable set stays fixed.

``BUGCHECK`` creates a pthread worker and calls the exported diagnostic routine
``pthread_dbg_get`` with deliberately invalid item code 2147483647. The compatibility entry is declared locally with its inspected native ABI; no
private pthread header is required. The reviewed runtime enters its fatal-error handler with reason
``deb_get: bad item code 2147483647``. This exercises the runtime's own diagnostic
log and image-dump path without modifying the runtime or its private objects.
It is an implementation diagnostic, not a POSIX conformance assertion. If a
runtime instead rejects the item and returns, the probe reports UNRESOLVED.

``ACCVIO`` creates a pthread worker and deliberately faults in application code.
This tests the supervisor's ``PRC$M_IMGDMP`` setting and the image-dump path for
an unhandled access violation. It does not require a pthread diagnostic log.

The expected raw supervisor classification is CRASH, with a failing supervisor
exit status. ``RUN_CRASH`` then runs a separate checker, linked without pthreads,
after child termination. The checker requires:

* Exactly one CRASH result for the selected probe.
* A matching control record and flushed trigger marker with that run's token.
* A nonempty ``*.DMP`` file in that fresh case directory.
* For BUGCHECK, ``PTHREAD_DUMP.LOG`` containing the expected runtime reason.
* No recognized image-dump-aborting diagnostic in STDERR.LOG.

The procedure succeeds only if those artifact checks succeed. TIMEOUT,
UNRESOLVED, a missing/empty file, a stale token, or an image-dump failure does
not count as a successful diagnostic. It preserves the raw CRASH row and all
artifacts; it does not turn expected crashes into normal suite PASS results.
The correct runtime log filename is ``PTHREAD_DUMP.LOG``.

A nonempty dump proves that a file was written; it does not prove that the dump
is complete or readable by SDA. Inspect the reported dump with SDA separately.
If dump placement is redirected outside the case directory, the checker reports
the missing local artifact rather than accepting a file from another run.
Retain STDOUT.LOG and STDERR.LOG even when no dump is produced.

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

The native supervisor explicitly enables image dumps for every child with
``PRC$M_IMGDMP`` on ``SYS$CREPRC``. An unhandled condition such as ACCVIO can
therefore produce an image ``.DMP`` even when it does not enter pthread's
bugcheck handler. Rebuild the supervisor with ``@BUILD`` after updating;
setting ``SET PROCESS/DUMP`` in the invoking DCL process is not a substitute
for enabling dumps in the directly created child.

After the child enters its case directory, its image dump should be written
there. Image/library initialization can fail before this directory change,
so also check the supervisor's default directory. Dump creation still depends
on the node's settings, quotas, write access, and available disk space.
``PTHREAD_DUMP.LOG`` is a separate pthread bugcheck report, not a report
generated for every access violation. Preserve the entire failing case
directory, the test executable, and the exact RTL images used for analysis.

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
API does not establish complete assertion coverage. New cases require native runtime validation
on OpenVMS; host builds and simulations alone do not establish it.

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
with persistent I/O slots, cross-thread waiting, and slot reuse. The separate
mixed case adds loopback TCP AST completion and local HIBER/WAKE dispatch.
The disabled-AST cases check SETAST state, overlapping disabling threads,
progress while delivery is disabled, and deferred callback delivery.
Intentional crash probes are diagnostic checks, excluded from API coverage.

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

For the supplemental native workloads and dump diagnostics, also run::

    python3 TOOLS/CHECK_QIO.py
    python3 TOOLS/CHECK_QIO_MIXED.py
    python3 TOOLS/CHECK_AST.py
    python3 TOOLS/CHECK_CRASH.py
    python3 TOOLS/CHECK_DUMP.py

The mixed checker uses real host loopback sockets with simulated VMS queue,
QIO and AST services. It also forces disk completion before TCP starts, during
a partial TCP frame, and after TCP reaches its iteration limit to check shutdown
and draining without relying on timing. Crash triggers use harmless substitutes
during host validation; the dump checker uses synthetic artifact trees. These checks do
not create or validate an actual OpenVMS image dump.

CHECK_AST compiles the native AST cases against a host service model with real
pthread helpers. Fault injection checks the test assertions as well as successful
delivery. It does not emulate the OpenVMS scheduler, AST ABI, or memory ordering.
