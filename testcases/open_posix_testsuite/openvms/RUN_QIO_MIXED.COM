$! SPDX-License-Identifier: GPL-2.0-or-later
$! X-1 CV Camiel Vanderhoeven 17-Nov-2026: Mixed disk and communication QIO stress.
$! @RUN_QIO_MIXED [repeats [seed [threads [iterations [timeout_seconds [CONTINUE|STOP_ON_CRASH]]]]]]
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
$ if p5 .eqs. "" then p5 = "3600"
$ if p6 .eqs. "" then p6 = "CONTINUE"
$ p6 = f$edit(p6,"UPCASE")
$ if p6 .nes. "CONTINUE" .and. p6 .nes. "STOP_ON_CRASH" then goto usage
$ if p7 .nes. "" .or. p8 .nes. "" then goto usage
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
$ open/read cases CASES.DAT
$ input_open = 1
$find_case:
$ read/end=missing cases row
$ if f$element(0,"|",row) .nes. "PT_QIO_MIXED" then goto find_case
$ close cases
$ input_open = 0
$ if f$search("RESULTS.DIR") .eqs. "" then create/directory [.RESULTS]
$ serial = 0
$find_manifest:
$ manifest = "[.RESULTS]QIO_MIXED_" + f$getjpi("","PID") + "_" + -
             f$string(serial) + ".DAT;1"
$ if f$search(manifest) .eqs. "" then goto manifest_ready
$ serial = serial + 1
$ if serial .lt. 100000 then goto find_manifest
$ goto failed
$manifest_ready:
$ open/write selected 'manifest'
$ output_open = 1
$ write selected "# Mixed QIO-only selection; the supervisor copies this into its run."
$ write selected f$element(0,"|",row) + "|" + f$element(1,"|",row) + -
    "|" + f$string(timeout) + "|" + f$element(3,"|",row) + "|" + -
    f$element(4,"|",row) + "|" + f$element(5,"|",row) + "|" + -
    f$element(6,"|",row) + "|" + f$element(7,"|",row)
$ close selected
$ output_open = 0
$ build_dir = f$extract(0,f$length(here)-1,here) + ".BUILD]"
$ runner := $'build_dir'PTS_RUNNER.EXE
$ runner 'manifest' "[.RESULTS]" 'p1' 'p2' 'p3' 'p4' "''p6'"
$ run_status = $status
$ set default 'saved_default'
$ exit 'run_status'
$missing:
$ write sys$error "PT_QIO_MIXED is absent from CASES.DAT. Update and rebuild the suite."
$ goto failed
$usage:
$ write sys$error "Usage: @RUN_QIO_MIXED [repeats [seed [threads [iterations [timeout_seconds [CONTINUE|STOP_ON_CRASH]]]]]]"
$ write sys$error "Timeout must be an integer from 1 through 86400 seconds."
$failed:
$ failure_status = $status
$ if failure_status .and. 1 then failure_status = %x10000002
$ if input_open then close cases
$ if output_open then close selected
$ set default 'saved_default'
$ exit 'failure_status'
