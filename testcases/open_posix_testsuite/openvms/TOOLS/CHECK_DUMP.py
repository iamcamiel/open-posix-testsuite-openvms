#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 iamcamiel
# Copyright 2026 VMS Software, Inc.
# X-1 CV Camiel Vanderhoeven 17-Nov-2026: Crash artifact verifier regression.
"""Exercise artifact checking with synthetic files; never cause an actual crash."""
import argparse
import csv
from pathlib import Path
import shlex
import subprocess
import tempfile

PORT = Path(__file__).resolve().parent.parent
HEADER = ("case,repeat,result,pts_code,native_status,seed,threads,iterations,"
          "seconds,directory").split(",")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="pts-dump-check-") as temp:
        root = Path(temp)
        binary = root / "dump_check"
        subprocess.run(shlex.split(args.cc) + [
            "-std=c99", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
            "-Werror", "-I" + str(PORT), "-x", "c",
            str(PORT / "PTS_DUMP_CHECK.C"), str(PORT / "PTS_PORT.C"),
            "-o", str(binary)], check=True, timeout=60)
        checks = [
            ("bugcheck", True), ("accvio", True), ("versioned", True),
            ("quoted_path", True), ("missing_dump", False),
            ("empty_dump", False), ("wrong_suffix", False),
            ("missing_log", False), ("empty_log", False),
            ("wrong_reason", False), ("aborting", False),
            ("condition", False), ("missing_stderr", False),
            ("wrong_token", False), ("marker_prefix", False),
            ("no_marker", False), ("wrong_id", False),
            ("wrong_control_id", False), ("wrong_control_path", False),
            ("wrong_seed", False), ("unarmed", False),
            ("pass", False), ("timeout", False), ("extra_row", False),
            ("bad_header", False), ("truncated", False),
            ("bad_csv", False), ("long_log_record", False),
        ]
        for index, (kind, wanted) in enumerate(checks):
            run = root / str(index)
            run.mkdir()
            work = run / ('case,"quoted"' if kind == "quoted_path" else "case")
            work.mkdir()
            mode = "ACCVIO" if kind == "accvio" else "BUGCHECK"
            identity = "PT_CRASH_" + mode
            token = "P123_R1_C0_S1"
            control_id = "OTHER" if kind == "wrong_control_id" else identity
            control_path = str(root) if kind == "wrong_control_path" else str(work)
            (work / "CONTROL.DAT").write_text(
                f"PTS1\n{control_id}\n{token}\n{work / 'RESULT.PTS'}\n"
                f"{control_path}\n1\n1\n1\n")
            marker = f"PTS_CRASH_TRIGGER|{mode}|{token}"
            if kind == "wrong_token":
                marker = f"PTS_CRASH_TRIGGER|{mode}|OLD_TOKEN"
            elif kind == "marker_prefix":
                marker += "_OLD"
            elif kind == "no_marker":
                marker = "Probe setup failed before triggering"
            (work / "STDOUT.LOG").write_text("Probe setup\n" + marker + "\n")
            stderr = "%SYSTEM-F-ACCVIO, access violation\n"
            if kind == "aborting":
                stderr += "IMGDMP aborting\n"
            elif kind == "condition":
                stderr += "Improperly handled condition\n"
            elif kind == "long_log_record":
                stderr += "x" * 5000 + "IMGDMP aborting\n"
            if kind != "missing_stderr":
                (work / "STDERR.LOG").write_text(stderr)
            dump_name = "CRASH.DMP;1" if kind == "versioned" else "crash.dmp"
            if kind == "wrong_suffix":
                dump_name += ".txt"
            if kind != "missing_dump":
                (work / dump_name).write_bytes(b"" if kind == "empty_dump" else b"synthetic")
            if kind not in ("accvio", "missing_log"):
                log_name = "pthread_dump.log;1" if kind == "versioned" else "PTHREAD_DUMP.LOG"
                reason = "Reason: deb_get: bad item code 2147483647\n"
                if kind == "empty_log":
                    reason = ""
                elif kind == "wrong_reason":
                    reason = "Reason: unrelated failure\n"
                (work / log_name).write_text(reason)
            result = "PASS" if kind == "pass" else "TIMEOUT" if kind == "timeout" else "CRASH"
            row = ["OTHER" if kind == "wrong_id" else identity, "1", result,
                   "-1", "0x0000000c", "2" if kind == "wrong_seed" else "1",
                   "2" if kind == "unarmed" else "1", "1", "0.5", str(work)]
            summary = run / "SUMMARY.CSV"
            with summary.open("w", newline="") as out:
                writer = csv.writer(out)
                writer.writerow(["bad"] if kind == "bad_header" else HEADER)
                writer.writerow(row)
                if kind == "extra_row":
                    writer.writerow(row)
            if kind == "truncated":
                summary.write_bytes(summary.read_bytes().rstrip(b"\r\n"))
            elif kind == "bad_csv":
                summary.write_text(",".join(HEADER) + "\n" + ",".join(row[:9]) + ',"unterminated\n')
            completed = subprocess.run([str(binary), str(summary), identity],
                                       capture_output=True, text=True, timeout=10)
            assert (completed.returncode == 0) == wanted, (
                kind, completed.returncode, completed.stdout, completed.stderr)
            if wanted:
                assert "SDA validation" in completed.stdout
            print(f"PASS: dump verifier {kind}", flush=True)
        usage = subprocess.run([str(binary)], capture_output=True, timeout=10)
        assert usage.returncode != 0
    print("PASS: artifact verification only; no native dump generation or SDA validation")


if __name__ == "__main__":
    main()
