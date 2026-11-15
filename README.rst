.. SPDX-License-Identifier: GPL-2.0-or-later

Open POSIX tests for OpenVMS
============================

OpenVMS x86-64 port of a selected Open POSIX Test Suite subset. This repository
is derived from Linux Test Project at commit
686a331b6240e54ac3170717adfd28ec94cfe100.

The initial subset includes 19 unchanged upstream conformance cases,
10 supplemental regression/stress cases, an API coverage inventory, and a
native process supervisor. Unrelated Linux kernel tests and unused LTP build
infrastructure have been removed. Upstream history and attribution are retained.

Build and run on OpenVMS
------------------------

With VSI C, native development headers and linker, from the checkout root::

    $ SET DEFAULT [.TESTCASES.OPEN_POSIX_TESTSUITE.OPENVMS]
    $ @BUILD 64 BOTH
    $ @RUN 1 1 8 2000

The build interface is::

    @BUILD [32|64] [BOTH|UPCALLS|KERNEL|NONE] [pthread_shareable_image]

The run interface is::

    @RUN [repeats [seed [threads [iterations]]]]

Examples::

    $ @BUILD 32 BOTH
    $ @RUN 1 1 4 200
    $ @BUILD 64 BOTH
    $ @RUN 100 12345 32 10000

Defaults are 64-bit test pointers, multiple kernel threads plus upcalls,
one repetition, seed 1, eight workers, and 2000 iterations. Run one build
profile before building the next. The supervisor uses independent process
isolation and timeouts. Per-case output and SUMMARY.CSV are retained below
the port directory's [.RESULTS] tree.

Read the OpenVMS_instructions_ for checkout, prerequisites, build outputs,
candidate-runtime selection, workload settings, result codes, diagnostics,
coverage limits, and development checks.

.. _OpenVMS_instructions: testcases/open_posix_testsuite/openvms/README.rst

Validation status
-----------------

The portable code is checked on Linux/WSL. Native OpenVMS compilation, linking,
execution, mailbox/timer behavior, and exception handling still require target
validation. Linux passes are not OpenVMS passes, and the selected subset is not
a complete conformance or certification suite.

Portable developer checks
-------------------------

From testcases/open_posix_testsuite/openvms on Linux or Ubuntu WSL::

    python3 TOOLS/BUILD_HOST.py --self-test --run --iterations 200

Licence and provenance
----------------------

See COPYING and testcases/open_posix_testsuite/COPYING. Original test notices
are preserved. UPSTREAM.json in the port directory records the pinned original
paths and Git blob hashes; API_COVERAGE.CSV distinguishes partial, incidental,
planned, and excluded coverage.

Upstream: https://github.com/linux-test-project/ltp
OpenVMS repository: https://github.com/iamcamiel/open-posix-testsuite-openvms
Author: iamcamiel <iamcamiel@gmail.com>
