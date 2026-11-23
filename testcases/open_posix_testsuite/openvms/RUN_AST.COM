$! SPDX-License-Identifier: GPL-2.0-or-later
$! Copyright 2026 VMS Software, Inc.
$! X-3 CV Camiel Vanderhoeven 23-Nov-2026: Select both final-AST regressions.
$! X-2 CV Camiel Vanderhoeven 17-Nov-2026: Include timer/timed-wait cases.
$! X-1 CV Camiel Vanderhoeven 17-Nov-2026: Disabled-AST regression selection.
$! @RUN_AST [repeats [seed [iterations [timeout_seconds [CONTINUE|STOP_ON_CRASH]]]]]
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
$ if p3 .eqs. "" then p3 = "200"
$ if p4 .eqs. "" then p4 = "300"
$ if p5 .eqs. "" then p5 = "CONTINUE"
$ p5 = f$edit(p5,"UPCASE")
$ if p5 .nes. "CONTINUE" .and. p5 .nes. "STOP_ON_CRASH" then goto usage
$ if p6 .nes. "" .or. p7 .nes. "" .or. p8 .nes. "" then goto usage
$ digit = 0
$ if f$length(p4) .gt. 5 then goto usage
$check_digit:
$ if digit .ge. f$length(p4) then goto digits_done
$ if f$locate(f$extract(digit,1,p4),"0123456789") .ge. 10 then goto usage
$ digit = digit + 1
$ goto check_digit
$digits_done:
$ timeout = f$integer(p4)
$ if timeout .lt. 1 .or. timeout .gt. 86400 then goto usage
$ if f$search("RESULTS.DIR") .eqs. "" then create/directory [.RESULTS]
$ serial = 0
$find_manifest:
$ manifest = "[.RESULTS]AST_" + f$getjpi("","PID") + "_" + -
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
$ write selected "# Disabled-AST cases; copied by the supervisor into its run."
$ found_main = 0
$ found_worker = 0
$ found_callback = 0
$ found_timer = 0
$ found_timer_printf = 0
$ found_timer_flush = 0
$ found_timer_crtl = 0
$ found_timer_enable = 0
$ found_last_run = 0
$ found_last_join = 0
$next_case:
$ read/end=cases_done cases row
$ id = f$element(0,"|",row)
$ if id .nes. "PT_AST_MAIN" .and. id .nes. "PT_AST_WORKER" .and. -
     id .nes. "PT_AST_CALLBACK" .and. id .nes. "PT_AST_TIMER" .and. -
     id .nes. "PT_AST_TIMER_PRINTF" .and. id .nes. "PT_AST_TIMER_CRTL" .and. -
     id .nes. "PT_AST_TIMER_ENABLE" .and. id .nes. "PT_AST_TIMER_FLUSH" .and. -
     id .nes. "PT_AST_LAST_RUN" .and. id .nes. "PT_AST_LAST_JOIN" then goto next_case
$ if id .eqs. "PT_AST_MAIN" then found_main = 1
$ if id .eqs. "PT_AST_WORKER" then found_worker = 1
$ if id .eqs. "PT_AST_CALLBACK" then found_callback = 1
$ if id .eqs. "PT_AST_TIMER" then found_timer = 1
$ if id .eqs. "PT_AST_TIMER_PRINTF" then found_timer_printf = 1
$ if id .eqs. "PT_AST_TIMER_FLUSH" then found_timer_flush = 1
$ if id .eqs. "PT_AST_TIMER_CRTL" then found_timer_crtl = 1
$ if id .eqs. "PT_AST_TIMER_ENABLE" then found_timer_enable = 1
$ if id .eqs. "PT_AST_LAST_RUN" then found_last_run = 1
$ if id .eqs. "PT_AST_LAST_JOIN" then found_last_join = 1
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
$ if .not. found_main .or. .not. found_worker .or. .not. found_callback then goto missing
$ if .not. found_timer .or. .not. found_timer_crtl then goto missing
$ if .not. found_timer_printf .or. .not. found_timer_enable then goto missing
$ if .not. found_timer_flush then goto missing
$ if .not. found_last_run .or. .not. found_last_join then goto missing
$ build_dir = f$extract(0,f$length(here)-1,here) + ".BUILD]"
$ runner := $'build_dir'PTS_RUNNER.EXE
$! Scenarios use the initial thread and up to two helpers.
$ runner 'manifest' "[.RESULTS]" 'p1' 'p2' 3 'p3' "''p5'"
$ run_status = $status
$ set default 'saved_default'
$ exit 'run_status'
$missing:
$ write sys$error "Disabled-AST cases are missing from CASES.DAT. Update and rebuild."
$ goto failed
$usage:
$ write sys$error "Usage: @RUN_AST [repeats [seed [iterations [timeout_seconds [CONTINUE|STOP_ON_CRASH]]]]]"
$ write sys$error "Timeout must be an integer from 1 through 86400 seconds."
$failed:
$ failure_status = $status
$ if failure_status .and. 1 then failure_status = %x10000002
$ if input_open then close cases
$ if output_open then close selected
$ set default 'saved_default'
$ exit 'failure_status'
