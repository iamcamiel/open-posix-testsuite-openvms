$! SPDX-License-Identifier: GPL-2.0-or-later
$! Copyright 2026 VMS Software, Inc.
$! X-1 CV Camiel Vanderhoeven 23-Nov-2026: Select RMS read completion AST stress cases.
$! @RUN_RMS_AST [repeats [seed [threads [iterations [timeout_seconds [CONTINUE|STOP_ON_CRASH [BOTH|SPIN|YIELD]]]]]]]
$ saved_default = f$environment("DEFAULT")
$ input_open = 0
$ output_open = 0
$ on error then goto failed
$ on control_y then goto failed
$ here = f$parse(f$environment("PROCEDURE"),,,"DEVICE") + -
         f$parse(f$environment("PROCEDURE"),,,"DIRECTORY")
$ set default 'here'
$ if p1 .eqs. "" then p1 = "1"
$ if p2 .eqs. "" then p2 = "1"
$ if p3 .eqs. "" then p3 = "8"
$ if p4 .eqs. "" then p4 = "2000"
$ if p5 .eqs. "" then p5 = "300"
$ if p6 .eqs. "" then p6 = "CONTINUE"
$ if p7 .eqs. "" then p7 = "BOTH"
$ p6 = f$edit(p6,"UPCASE")
$ p7 = f$edit(p7,"UPCASE")
$ if p6 .nes. "CONTINUE" .and. p6 .nes. "STOP_ON_CRASH" then goto usage
$ if p7 .nes. "BOTH" .and. p7 .nes. "SPIN" .and. p7 .nes. "YIELD" then goto usage
$ if p8 .nes. "" then goto usage
$! Validate before using the timeout in the generated manifest.
$ digit = 0
$ if f$length(p5) .gt. 5 then goto usage
$check_digit:
$ if digit .ge. f$length(p5) then goto digits_done
$ if f$locate(f$extract(digit,1,p5),"0123456789") .ge. 10 then goto usage
$ digit = digit + 1
$ goto check_digit
$digits_done:
$ timeout = f$integer(p5)
$ if timeout .lt. 1 .or. timeout .gt. 86400 then goto usage
$ want_spin = p7 .nes. "YIELD"
$ want_yield = p7 .nes. "SPIN"
$ if f$search("RESULTS.DIR") .eqs. "" then create/directory [.RESULTS]
$ serial = 0
$find_manifest:
$ manifest = "[.RESULTS]RMS_AST_" + f$getjpi("","PID") + "_" + -
             f$string(serial) + ".DAT;1"
$ if f$search(manifest) .eqs. "" then goto manifest_ready
$ serial = serial + 1
$ if serial .lt. 100000 then goto find_manifest
$ goto failed
$manifest_ready:
$ open/read cases CASES.DAT
$ input_open = 1
$ open/write selected 'manifest'
$ output_open = 1
$ write selected "# RMS AST selection; copied by the supervisor into its run."
$ found_spin = 0
$ found_yield = 0
$next_case:
$ read/end=cases_done cases row
$ id = f$element(0,"|",row)
$ if id .eqs. "PT_RMS_AST_SPIN"
$ then
$     if .not. want_spin then goto next_case
$     found_spin = 1
$ else
$     if id .nes. "PT_RMS_AST_YIELD" .or. .not. want_yield then goto next_case
$     found_yield = 1
$ endif
$ write selected id + "|" + f$element(1,"|",row) + -
    "|" + f$string(timeout) + "|" + f$element(3,"|",row) + "|" + -
    f$element(4,"|",row) + "|" + f$element(5,"|",row) + "|" + -
    f$element(6,"|",row) + "|" + f$element(7,"|",row)
$ goto next_case
$cases_done:
$ close cases
$ input_open = 0
$ close selected
$ output_open = 0
$ if (want_spin .and. .not. found_spin) .or. (want_yield .and. .not. found_yield) then goto missing
$ build_dir = f$extract(0,f$length(here)-1,here) + ".BUILD]"
$ runner := $'build_dir'PTS_RUNNER.EXE
$ runner 'manifest' "[.RESULTS]" 'p1' 'p2' 'p3' 'p4' "''p6'"
$ run_status = $status
$ set default 'saved_default'
$ exit 'run_status'
$missing:
$ write sys$error "Requested RMS AST cases are missing from CASES.DAT. Update and rebuild."
$ goto failed
$usage:
$ write sys$error "Usage: @RUN_RMS_AST [repeats [seed [threads [iterations [timeout_seconds [CONTINUE|STOP_ON_CRASH [BOTH|SPIN|YIELD]]]]]]]"
$ write sys$error "Timeout must be an integer from 1 through 86400 seconds."
$failed:
$ failure_status = $status
$ if failure_status .and. 1 then failure_status = %x10000002
$ if input_open then close cases
$ if output_open then close selected
$ set default 'saved_default'
$ exit 'failure_status'
