#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 iamcamiel
# Copyright 2026 VMS Software, Inc.
# X-1 CV Camiel Vanderhoeven 16-Nov-2026: Verify stop-on-crash and stderr replay.
"""Exercise the production runner with real child processes on a POSIX host."""
import csv
import os
from pathlib import Path
import subprocess
import sys
import tempfile

from BUILD_HOST import PORT


CHILD = r'''
import os
from pathlib import Path
import resource
import signal
import sys
import time

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
control = sys.stdin.read().splitlines()
assert len(control) == 8 and control[0] == "PTS1", control
_, case, token, result, directory, seed, threads, iterations = control
os.chdir(directory)
Path("STARTED").write_text(case)
if case == "TIMEOUT":
    time.sleep(60)
if case.startswith("CRASH") and not (case == "CRASH_LATE" and seed == "1"):
    if case == "CRASH_EMPTY":
        pass
    elif case == "CRASH_MISSING":
        Path("STDERR.LOG").unlink()
    elif case == "CRASH_READ_ERROR":
        Path("STDERR.LOG").unlink()
        Path("STDERR.LOG").mkdir()
    else:
        sys.stderr.write("CRASH STDERR: " + "x" * 9000 + "\nlast line without newline")
        sys.stderr.flush()
    if case == "CRASH_WITH_PASS":
        Path(result).write_text(f"PTS1|{case}|{token}|0\n")
    os.kill(os.getpid(), signal.SIGABRT)
codes = {"FAIL": 1, "UNRESOLVED": 2, "UNSUPPORTED": 4, "UNTESTED": 5}
Path(result).write_text(f"PTS1|{case}|{token}|{codes.get(case, 0)}\n")
'''

CRASH_TEXT = "CRASH STDERR: " + "x" * 9000 + "\nlast line without newline"


def main():
    checks = 0
    with tempfile.TemporaryDirectory(prefix="pts_stop_on_crash_") as temp:
        root = Path(temp)
        build = root / "BUILD"
        build.mkdir()
        (build / "BUILD_STATE.DAT").write_text("READY\n")
        (build / "BUILD_INFO.TXT").write_text("POSIX runner regression fixtures\n")
        binary = build / "PTS_RUNNER.EXE"
        subprocess.run(
            [os.environ.get("CC", "cc"), "-std=c99", "-Wall", "-Wextra",
             "-Werror", "-I" + str(PORT), "-x", "c",
             str(PORT / "PTS_RUNNER.C"), str(PORT / "PTS_PORT.C"),
             "-o", str(binary)], check=True, timeout=60)

        def run(case_ids, option=None, repeats=2, extra=()):
            nonlocal checks
            checks += 1
            results = root / f"RESULTS_{checks}"
            results.mkdir()
            manifest = root / f"CASES_{checks}.DAT"
            manifest.write_text("".join(
                f"{case}|fixture|1|selftest|none|runner_fixture|local|ready\n"
                for case in case_ids))
            for case in case_ids:
                child = build / (case + ".EXE")
                child.write_text("#!" + sys.executable + "\n" + CHILD)
                child.chmod(0o700)
            args = [str(binary), str(manifest), str(results),
                    str(repeats), "1", "8", "200"]
            if option is not None:
                args.append(option)
            process = subprocess.run(args + list(extra), cwd=root,
                                     capture_output=True, text=True, timeout=15)
            summaries = list(results.glob("R_*/SUMMARY.CSV"))
            rows = []
            if summaries:
                assert len(summaries) == 1, summaries
                with summaries[0].open(newline="") as stream:
                    rows = list(csv.DictReader(stream))
                assert len(list(summaries[0].parent.glob("T_*"))) == len(rows)
                for row in rows:
                    assert (Path(row["directory"]) / "STARTED").read_text() == row["case"]
            return process, rows, summaries

        def identities(rows):
            return [(row["case"], int(row["repeat"]), row["result"]) for row in rows]

        # Existing behavior remains available both implicitly and explicitly.
        for option in (None, "CONTINUE", "continue"):
            process, rows, _ = run(["PASS_BEFORE", "CRASH", "PASS_AFTER"], option)
            assert process.returncode == 1, process
            assert identities(rows) == [
                (case, repeat, outcome)
                for repeat in (1, 2)
                for case, outcome in (("PASS_BEFORE", "PASS"),
                                      ("CRASH", "CRASH"), ("PASS_AFTER", "PASS"))]
            assert "Stopping after CRASH" not in process.stderr
            assert CRASH_TEXT not in process.stderr
            assert "Totals: PASS=4 NONPASS=2" in process.stdout

        for option in ("STOP_ON_CRASH", "stop_on_crash", "Stop_On_Crash"):
            process, rows, _ = run(["PASS_BEFORE", "CRASH", "PASS_AFTER"], option)
            assert process.returncode == 1, process
            assert identities(rows) == [("PASS_BEFORE", 1, "PASS"), ("CRASH", 1, "CRASH")]
            crash = rows[-1]
            error = Path(crash["directory"]) / "STDERR.LOG"
            assert error.read_text() == CRASH_TEXT
            assert f"Stopping after CRASH: CRASH repeat=1 native_status={crash['native_status']}" in process.stderr
            assert f"----- BEGIN STDERR.LOG: {error} -----\n" in process.stderr
            assert CRASH_TEXT + "\n----- END STDERR.LOG -----" in process.stderr
            assert "Totals: PASS=1 NONPASS=1" in process.stdout

        # A crash in a later repeat must stop that repeat and all following ones.
        process, rows, _ = run(["PASS_BEFORE", "CRASH_LATE", "PASS_AFTER"],
                               "STOP_ON_CRASH", repeats=3)
        assert process.returncode == 1, process
        assert identities(rows) == [("PASS_BEFORE", 1, "PASS"),
                                    ("CRASH_LATE", 1, "PASS"),
                                    ("PASS_AFTER", 1, "PASS"),
                                    ("PASS_BEFORE", 2, "PASS"),
                                    ("CRASH_LATE", 2, "CRASH")]
        assert "CRASH_LATE repeat=2" in process.stderr
        assert "Totals: PASS=4 NONPASS=1" in process.stdout

        # A written PASS cannot hide abnormal shutdown or bypass this option.
        process, rows, _ = run(["CRASH_WITH_PASS", "PASS_AFTER"], "STOP_ON_CRASH")
        assert process.returncode == 1, process
        assert identities(rows) == [("CRASH_WITH_PASS", 1, "CRASH")]
        assert (Path(rows[0]["directory"]) / "RESULT.PTS").read_text().endswith("|0\n")

        for case, diagnostic in (("CRASH_EMPTY", "(stderr is empty)"),
                                 ("CRASH_MISSING", "Cannot open stderr log:"),
                                 ("CRASH_READ_ERROR", "Error reading stderr log:")):
            process, rows, _ = run([case, "PASS_AFTER"], "STOP_ON_CRASH")
            assert process.returncode == 1, process
            assert identities(rows) == [(case, 1, "CRASH")]
            assert diagnostic in process.stderr, process.stderr
            assert "Totals: PASS=0 NONPASS=1" in process.stdout

        # This option means CRASH, not every non-PASS classification.
        process, rows, _ = run(["FAIL", "UNRESOLVED", "UNSUPPORTED", "UNTESTED",
                               "TIMEOUT", "PASS_AFTER"], "STOP_ON_CRASH")
        assert process.returncode == 1, process
        assert identities(rows) == [
            (case, repeat, "PASS" if case == "PASS_AFTER" else case)
            for repeat in (1, 2)
            for case in ("FAIL", "UNRESOLVED", "UNSUPPORTED", "UNTESTED", "TIMEOUT", "PASS_AFTER")]
        assert "Stopping after CRASH" not in process.stderr
        assert "Totals: PASS=2 NONPASS=6 UNSUPPORTED_OR_UNTESTED=4" in process.stdout

        process, rows, _ = run(["PASS_BEFORE", "PASS_AFTER"], "STOP_ON_CRASH")
        assert process.returncode == 0, process
        assert len(rows) == 4 and all(row["result"] == "PASS" for row in rows)
        assert not process.stderr, process.stderr

        for option, extra in (("STOP", ()), ("STOP_ON_CRASHx", ()), ("", ()),
                              ("CONTINUE", ("extra",))):
            process, rows, summaries = run(["PASS_AFTER"], option, extra=extra)
            assert process.returncode == 1, process
            assert "Usage:" in process.stderr
            assert not rows and not summaries

    print(f"PASS: stop-on-crash runner integration ({checks} scenarios)")
    print("Verified both loops, stderr replay/preservation, unchanged classifications, and arguments.")
    print("POSIX child execution; native DCL and OpenVMS execution still need target validation.")


if __name__ == "__main__":
    main()
