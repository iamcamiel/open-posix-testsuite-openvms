$! SPDX-License-Identifier: GPL-2.0-or-later
$! X-1 CV Camiel Vanderhoeven 17-Nov-2026: Opt-in crash and dump artifact checks.
$! @RUN_CRASH BUGCHECK|ACCVIO [timeout_seconds]
$ saved_default = f$environment("DEFAULT")
$ input_open = 0
$ output_open = 0
$ capture_open = 0
$ on error then goto failed
$ on control_y then goto failed
$ here = f$parse(f$environment("PROCEDURE"),,,"DEVICE") + -
         f$parse(f$environment("PROCEDURE"),,,"DIRECTORY")
$ set default 'here'
$ mode = f$edit(p1,"UPCASE")
$ if mode .nes. "BUGCHECK" .and. mode .nes. "ACCVIO" then goto usage
$ if p3 .nes. "" .or. p4 .nes. "" .or. p5 .nes. "" then goto usage
$ if p6 .nes. "" .or. p7 .nes. "" .or. p8 .nes. "" then goto usage
$ if p2 .eqs. "" then p2 = "600"
$ if f$length(p2) .gt. 5 then goto usage
$ digit = 0
$check_digit:
$ if digit .ge. f$length(p2) then goto digits_done
$ if f$locate(f$extract(digit,1,p2),"0123456789") .ge. 10 then goto usage
$ digit = digit + 1
$ goto check_digit
$digits_done:
$ timeout = f$integer(p2)
$ if timeout .lt. 1 .or. timeout .gt. 86400 then goto usage
$ id = "PT_CRASH_" + mode
$ open/read cases CRASH_CASES.DAT
$ input_open = 1
$find_case:
$ read/end=missing cases row
$ if f$element(0,"|",row) .nes. id then goto find_case
$ close cases
$ input_open = 0
$ if f$search("RESULTS.DIR") .eqs. "" then create/directory [.RESULTS]
$ serial = 0
$find_files:
$ prefix = "[.RESULTS]CRASH_" + f$getjpi("","PID") + "_" + f$string(serial)
$ manifest = prefix + ".DAT;1"
$ capture = prefix + ".TXT;1"
$ if f$search(manifest) .eqs. "" .and. f$search(capture) .eqs. "" then goto files_ready
$ serial = serial + 1
$ if serial .lt. 100000 then goto find_files
$ goto failed
$files_ready:
$ open/write selected 'manifest'
$ output_open = 1
$ write selected "# Explicit intentional crash probe; never a normal test selection."
$ write selected f$element(0,"|",row) + "|" + f$element(1,"|",row) + -
    "|" + f$string(timeout) + "|" + f$element(3,"|",row) + "|" + -
    f$element(4,"|",row) + "|" + f$element(5,"|",row) + "|" + -
    f$element(6,"|",row) + "|" + f$element(7,"|",row)
$ close selected
$ output_open = 0
$ build_dir = f$extract(0,f$length(here)-1,here) + ".BUILD]"
$ runner := $'build_dir'PTS_RUNNER.EXE
$ checker := $'build_dir'PTS_DUMP_CHECK.EXE
$ write sys$output "Deliberately crashing one isolated pthread child: " + mode
$ write sys$output "The supervisor's CRASH result is expected; artifact checks follow."
$ define/user SYS$OUTPUT 'capture'
$ set noon
$ runner 'manifest' "[.RESULTS]" 1 1 1 1 "STOP_ON_CRASH"
$ run_status = $status
$ set on
$ on error then goto failed
$ if f$search(capture) .eqs. "" then goto bad_output
$ type 'capture'
$ if run_status .and. 1 then goto unexpected_success
$ open/read output 'capture'
$ capture_open = 1
$ read/end=bad_output output first_line
$ close output
$ capture_open = 0
$ if f$extract(0,9,first_line) .nes. "Results: " then goto bad_output
$ summary_name = f$extract(9,f$length(first_line)-9,first_line)
$ set noon
$ checker "''summary_name'" "''id'"
$ check_status = $status
$ set on
$ set default 'saved_default'
$ exit 'check_status'
$missing:
$ write sys$error "Requested probe is absent from CRASH_CASES.DAT. Update and rebuild."
$ goto failed
$unexpected_success:
$ write sys$error "FAIL: the intentional crash did not produce a failing supervisor status."
$ goto failed
$bad_output:
$ write sys$error "FAIL: missing fresh supervisor output or summary path; inspect " + capture
$ goto failed
$usage:
$ write sys$error "Usage: @RUN_CRASH BUGCHECK|ACCVIO [timeout_seconds]"
$ write sys$error "Explicit mode required. Timeout defaults to 600; valid range is 1..86400."
$failed:
$ if input_open then close cases
$ if output_open then close selected
$ if capture_open then close output
$ set default 'saved_default'
$ exit %x10000002
