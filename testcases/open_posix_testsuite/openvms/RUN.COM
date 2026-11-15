$! SPDX-License-Identifier: GPL-2.0-or-later
$! X-1 CV Camiel Vanderhoeven 15-Nov-2026: Native test invocation.
$! @RUN [repeats [seed [threads [iterations]]]]
$ saved_default = f$environment("DEFAULT")
$ on error then goto failed
$ on control_y then goto failed
$ here = f$parse(f$environment("PROCEDURE"),,,"DEVICE") + -
         f$parse(f$environment("PROCEDURE"),,,"DIRECTORY")
$ set default 'here'
$ if p1 .eqs. "" then p1 = "1"
$ if p2 .eqs. "" then p2 = "1"
$ if p3 .eqs. "" then p3 = "8"
$ if p4 .eqs. "" then p4 = "2000"
$ if f$search("RESULTS.DIR") .eqs. "" then create/directory [.RESULTS]
$ build_dir = f$extract(0,f$length(here)-1,here) + ".BUILD]"
$ runner := $'build_dir'PTS_RUNNER.EXE
$! The runner validates numeric arguments and rejects malformed case ids.
$ runner CASES.DAT "[.RESULTS]" 'p1' 'p2' 'p3' 'p4'
$ run_status = $status
$ set default 'saved_default'
$ exit 'run_status'
$failed:
$ failure_status = $status
$ set default 'saved_default'
$ exit 'failure_status'
