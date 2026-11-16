$! SPDX-License-Identifier: GPL-2.0-or-later
$! X-2 CV Camiel Vanderhoeven 16-Nov-2026: Optional stop and stderr on crash.
$! X-1 CV Camiel Vanderhoeven 15-Nov-2026: Native test invocation.
$! @RUN [repeats [seed [threads [iterations [CONTINUE|STOP_ON_CRASH]]]]]
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
$ if p5 .eqs. "" then p5 = "CONTINUE"
$ p5 = f$edit(p5,"UPCASE")
$ if p5 .nes. "CONTINUE" .and. p5 .nes. "STOP_ON_CRASH" then goto usage
$ if p6 .nes. "" .or. p7 .nes. "" .or. p8 .nes. "" then goto usage
$ if f$search("RESULTS.DIR") .eqs. "" then create/directory [.RESULTS]
$ build_dir = f$extract(0,f$length(here)-1,here) + ".BUILD]"
$ runner := $'build_dir'PTS_RUNNER.EXE
$! The runner validates numeric arguments and rejects malformed case ids.
$ runner CASES.DAT "[.RESULTS]" 'p1' 'p2' 'p3' 'p4' "''p5'"
$ run_status = $status
$ set default 'saved_default'
$ exit 'run_status'
$usage:
$ write sys$error "Usage: @RUN [repeats [seed [threads [iterations [CONTINUE|STOP_ON_CRASH]]]]]"
$ set default 'saved_default'
$ exit %x10000002
$failed:
$ failure_status = $status
$ set default 'saved_default'
$ exit 'failure_status'
