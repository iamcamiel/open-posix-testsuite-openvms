$! SPDX-License-Identifier: GPL-2.0-or-later
$! X-1 CV Camiel Vanderhoeven 15-Nov-2026: Target-side supervisor checks.
$! Run @BUILD first. These fixtures deliberately fail, crash, and time out.
$ saved_default = f$environment("DEFAULT")
$ on error then goto failed
$ on control_y then goto failed
$ here = f$parse(f$environment("PROCEDURE"),,,"DEVICE") + -
         f$parse(f$environment("PROCEDURE"),,,"DIRECTORY")
$ set default 'here'
$ build_dir = f$extract(0,f$length(here)-1,here) + ".BUILD]"
$ runner := $'build_dir'PTS_RUNNER.EXE
$ kinds = "0,1,2,4,5,6,7,8,9,10,11,99"
$ expected = "PASS,FAIL,UNRESOLVED,UNSUPPORTED,UNTESTED,CRASH,TIMEOUT,HARNESS_ERROR,CRASH,HARNESS_ERROR,HARNESS_ERROR,HARNESS_ERROR"
$ index = 0
$next:
$ kind = f$element(index,",",kinds)
$ if kind .eqs. "," then goto done
$ want = f$element(index,",",expected)
$ open/write manifest [.BUILD]FIXTURE_RUN.DAT
$ write manifest "FIXTURE_" + kind + "|fixture|1|selftest|none|fixture|local|ready"
$ close manifest
$ define/user SYS$OUTPUT [.BUILD]SELFTEST_OUTPUT.TXT
$ set noon
$ runner "[.BUILD]FIXTURE_RUN.DAT" "[.RESULTS]"
$ run_status = $status
$ set on
$ on error then goto failed
$ open/read output [.BUILD]SELFTEST_OUTPUT.TXT
$ read/end=bad_output output first_line
$ close output
$ if f$extract(0,9,first_line) .nes. "Results: " then goto bad_output
$ summary_name = f$extract(9,f$length(first_line)-9,first_line)
$ open/read report 'summary_name'
$ read/end=bad_report report heading
$ read/end=bad_report report row
$ close report
$ got = f$element(2,",",row)
$ if got .nes. want then goto mismatch
$ if want .eqs. "PASS" .and. .not. (run_status .and. 1) then goto mismatch
$ if want .nes. "PASS" .and. (run_status .and. 1) then goto mismatch
$ write sys$output "Fixture " + kind + ": " + got + " (expected)"
$ index = index + 1
$ goto next
$done:
$ write sys$output "PASS: native runner classifications, abnormal shutdown, and watchdog"
$ set default 'saved_default'
$ exit 1
$bad_output:
$ write sys$error "Missing runner output during fixture " + kind
$ goto failed
$bad_report:
$ write sys$error "Missing runner summary during fixture " + kind
$ goto failed
$mismatch:
$ write sys$error "Fixture " + kind + " expected " + want + ", got " + got
$failed:
$ set default 'saved_default'
$ exit %x10000002
