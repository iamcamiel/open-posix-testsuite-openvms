$! SPDX-License-Identifier: GPL-2.0-or-later
$! X-3 CV Camiel Vanderhoeven 23-Nov-2026: Prototype legacy thread entry points.
$! X-2 CV Camiel Vanderhoeven 17-Nov-2026: Build opt-in dump diagnostics and checker.
$! X-1 CV Camiel Vanderhoeven 15-Nov-2026: Native OpenVMS build.
$! @BUILD [32|64] [BOTH|UPCALLS|KERNEL|NONE] [pthread shareable image]
$ saved_default = f$environment("DEFAULT")
$ case_open = 0
$ on error then goto failed
$ on control_y then goto failed
$ here = f$parse(f$environment("PROCEDURE"),,,"DEVICE") + -
         f$parse(f$environment("PROCEDURE"),,,"DIRECTORY")
$ set default 'here'
$ pointer = p1
$ if pointer .eqs. "" then pointer = "64"
$ if pointer .nes. "32" .and. pointer .nes. "64" then goto usage
$ profile = f$edit(p2,"UPCASE")
$ if profile .eqs. "" then profile = "BOTH"
$ thread_qual = ""
$ if profile .eqs. "BOTH" then thread_qual = "/THREADS_ENABLE=(MULTIPLE_KERNEL_THREADS,UPCALLS)"
$ if profile .eqs. "UPCALLS" then thread_qual = "/THREADS_ENABLE=UPCALLS"
$ if profile .eqs. "KERNEL" then thread_qual = "/THREADS_ENABLE=MULTIPLE_KERNEL_THREADS"
$ if profile .eqs. "NONE" then thread_qual = "/NOTHREADS_ENABLE"
$ if thread_qual .eqs. "" then goto usage
$ native_upcalls = "0"
$ if profile .eqs. "BOTH" .or. profile .eqs. "UPCALLS" then native_upcalls = "1"
$ rtl = p3
$ if rtl .eqs. "" then rtl = "SYS$SHARE:PTHREAD$RTL.EXE"
$ if f$search("BUILD.DIR") .eqs. "" then create/directory [.BUILD]
$ if f$search("RESULTS.DIR") .eqs. "" then create/directory [.RESULTS]
$ open/write state [.BUILD]BUILD_STATE.DAT
$ write state "BUILDING"
$ close state
$ copy [-.INCLUDE]POSIXTEST.H []PTS_ORIGINAL.H
$ open/write opt [.BUILD]PTHREAD_LIBRARY.OPT
$ write opt rtl + "/SHAREABLE"
$ close opt
$ includes = "/INCLUDE=([],[.BUILD],[.LOCAL])"
$ common = "/STANDARD=RELAXED_ANSI/DEBUG/MAIN=POSIX_EXIT/DEFINE=(_POSIX_EXIT=1)" + includes
$ runner_quals = common + "/POINTER_SIZE=32/REENTRANCY=NONE"
$ test_quals = common + "/POINTER_SIZE=" + pointer + "/REENTRANCY=MULTITHREAD" + -
               "/DEFINE=(_POSIX_EXIT=1,PTS_NATIVE_UPCALLS=" + native_upcalls + ")"
$ cc 'runner_quals'/OBJECT=[.BUILD]PTS_PORT_RUN.OBJ PTS_PORT.C
$ cc 'runner_quals'/OBJECT=[.BUILD]PTS_RUNNER.OBJ PTS_RUNNER.C
$ cc 'runner_quals'/OBJECT=[.BUILD]PTS_DUMP_CHECK.OBJ PTS_DUMP_CHECK.C
$ link/NOTHREADS_ENABLE/EXE=[.BUILD]PTS_DUMP_CHECK.EXE -
       [.BUILD]PTS_DUMP_CHECK.OBJ,[.BUILD]PTS_PORT_RUN.OBJ
$ link/NOTHREADS_ENABLE/EXE=[.BUILD]PTS_RUNNER.EXE -
       [.BUILD]PTS_RUNNER.OBJ,[.BUILD]PTS_PORT_RUN.OBJ
$ cc 'test_quals'/OBJECT=[.BUILD]PTS_PORT_TEST.OBJ PTS_PORT.C
$ cc 'test_quals'/OBJECT=[.BUILD]PTS_MAIN.OBJ PTS_MAIN.C
$ cc 'test_quals'/OBJECT=[.BUILD]PTS_SUPPORT.OBJ [.LOCAL]PTS_SUPPORT.C
$ manifest_index = 0
$ manifests = "CASES.DAT,CRASH_CASES.DAT"
$next_manifest:
$ case_manifest = f$element(manifest_index,",",manifests)
$ open/read cases 'case_manifest'
$ case_open = 1
$next_case:
$ read/end=cases_done cases row
$ if f$extract(0,1,row) .eqs. "#" .or. row .eqs. "" then goto next_case
$ id = f$element(0,"|",row)
$ source = f$element(1,"|",row)
$ if f$element(7,"|",row) .nes. "ready" then goto failed
$! Translate suite-relative paths to native file specifications.
$ component = f$element(0,"/",source)
$ index = 0
$ source_dir = "[-"
$ if component .nes. "openvms" then goto source_parts
$ source_dir = "["
$ index = 1
$source_parts:
$ component = f$element(index,"/",source)
$ next_component = f$element(index+1,"/",source)
$ if next_component .eqs. "/" then goto source_filename
$ source_dir = source_dir + "." + component
$ index = index + 1
$ goto source_parts
$source_filename:
$ native_source = source_dir + "]" + component
$ copy 'native_source' [.BUILD]'id'.C
$ case_quals = test_quals
$ if id .eqs. "PT_CREATE_1_1" .or. id .eqs. "PT_JOIN_1_1" .or. -
     id .eqs. "PT_EQUAL_1_1" .or. id .eqs. "PT_CLEANUP_PUSH_1_1"
$ then
$     case_quals = case_quals + "/FIRST_INCLUDE=[.LOCAL]PTS_THREAD_PROTO.H"
$ endif
$ cc 'case_quals'/OBJECT=[.BUILD]'id'.OBJ/LIST=[.BUILD]'id'.LIS [.BUILD]'id'.C
$ link 'thread_qual'/EXE=[.BUILD]'id'.EXE/MAP=[.BUILD]'id'.MAP -
       [.BUILD]'id'.OBJ,[.BUILD]PTS_MAIN.OBJ,[.BUILD]PTS_PORT_TEST.OBJ,-
       [.BUILD]PTS_SUPPORT.OBJ,[.BUILD]PTHREAD_LIBRARY.OPT/OPTIONS
$ goto next_case
$cases_done:
$ close cases
$ case_open = 0
$ manifest_index = manifest_index + 1
$ if manifest_index .lt. 2 then goto next_manifest
$! Harness fixtures are built separately and never added to CASES.DAT.
$ fixture_list = "0,1,2,4,5,6,7,8,9,10,11,99"
$ fixture_index = 0
$fixture_loop:
$ kind = f$element(fixture_index,",",fixture_list)
$ if kind .eqs. "," then goto fixtures_done
$ cc 'test_quals'/DEFINE=(_POSIX_EXIT=1,FIXTURE_KIND='kind') -
     /OBJECT=[.BUILD]FIXTURE_'kind'.OBJ [.LOCAL]PT_FIXTURE.C
$ link 'thread_qual'/EXE=[.BUILD]FIXTURE_'kind'.EXE -
       [.BUILD]FIXTURE_'kind'.OBJ,[.BUILD]PTS_MAIN.OBJ,-
       [.BUILD]PTS_PORT_TEST.OBJ,[.BUILD]PTHREAD_LIBRARY.OPT/OPTIONS
$ fixture_index = fixture_index + 1
$ goto fixture_loop
$fixtures_done:
$ open/write info [.BUILD]BUILD_INFO.TXT
$ write info "Open POSIX OpenVMS port: 2026-11-15"
$ write info "Upstream commit: 686a331b6240e54ac3170717adfd28ec94cfe100"
$ write info "Built: " + f$time()
$ write info "OpenVMS: " + f$getsyi("VERSION")
$ write info "Architecture: " + f$getsyi("ARCH_NAME")
$ write info "Test qualifiers: " + test_quals
$ write info "Link profile: " + thread_qual
$ write info "Pthread image: " + rtl
$ write info "Supervisor: 32-bit pointers; no explicit PTHREAD$RTL dependency"
$ close info
$ define/user SYS$OUTPUT [.BUILD]COMPILER_VERSION.TXT
$ cc/version
$ open/write state [.BUILD]BUILD_STATE.DAT
$ write state "READY"
$ close state
$ write sys$output "Built Open POSIX core subset. Run @RUN from this directory."
$ set default 'saved_default'
$ exit 1
$usage:
$ write sys$error "Usage: @BUILD [32|64] [BOTH|UPCALLS|KERNEL|NONE] [pthread image]"
$ goto failed
$failed:
$ failure_status = $status
$ if failure_status .and. 1 then failure_status = %x10000002
$ if case_open then close cases
$ set default 'saved_default'
$ exit 'failure_status'
