#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 iamcamiel
# X-1 CV Camiel Vanderhoeven 15-Nov-2026: portable validation build.
"""Build the selected sources and POSIX runner backend; not a VMS validation."""
import argparse
import csv
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

PORT = Path(__file__).resolve().parent.parent
SUITE = PORT.parent


def cases(path=PORT / "CASES.DAT"):
    rows = []
    for line in path.read_text().splitlines():
        if not line or line.startswith("#"):
            continue
        fields = line.split("|")
        if len(fields) != 8 or fields[-1] != "ready":
            raise ValueError(f"Invalid case row: {line}")
        rows.append(dict(zip(("id", "source", "timeout", "category", "apis",
                              "assertion", "blob", "state"), fields)))
    if len({r["id"] for r in rows}) != len(rows):
        raise ValueError("Duplicate case ids")
    return rows


def blob_sha(path):
    content = path.read_bytes().replace(b"\r\n", b"\n")
    return hashlib.sha1(b"blob " + str(len(content)).encode() + b"\0" + content).hexdigest()


def build(cc="cc"):
    builddir = PORT / "BUILD"
    builddir.mkdir(exist_ok=True)
    (builddir / "BUILD_STATE.DAT").write_text("BUILDING\n")
    (PORT / "RESULTS").mkdir(exist_ok=True)
    original = SUITE / "include/posixtest.h"
    shutil.copyfile(original, PORT / "PTS_ORIGINAL.H")
    shutil.copyfile(PORT / "POSIXTEST.H", builddir / "posixtest.h")
    provenance = json.loads((PORT / "UPSTREAM.json").read_text())
    flags = ["-std=c99", "-D_POSIX_C_SOURCE=200809L", "-D_XOPEN_SOURCE=700",
             "-Wall", "-Wextra", "-Wno-unused-parameter", "-pthread",
             "-I" + str(PORT), "-I" + str(builddir), "-I" + str(PORT / "LOCAL")]

    def compile_file(source, name, extra=()):
        obj = builddir / (name + ".o")
        command = [cc, *flags, *extra, "-x", "c", "-c", str(source), "-o", str(obj)]
        subprocess.run(command, check=True, timeout=60)
        return obj

    def link(objects, name, pthread=True):
        command = [cc, *map(str, objects), *(["-pthread"] if pthread else []),
                   "-o", str(builddir / (name + ".EXE"))]
        subprocess.run(command, check=True, timeout=60)

    common = compile_file(PORT / "PTS_PORT.C", "PTS_PORT", ["-Werror"])
    main = compile_file(PORT / "PTS_MAIN.C", "PTS_MAIN", ["-Werror"])
    support = compile_file(PORT / "LOCAL/PTS_SUPPORT.C", "PTS_SUPPORT", ["-Werror"])
    runner = compile_file(PORT / "PTS_RUNNER.C", "PTS_RUNNER", ["-Werror"])
    link([runner, common], "PTS_RUNNER", pthread=False)

    for row in cases():
        source = SUITE / row["source"]
        if not source.is_file():
            raise ValueError(f"Missing {source}")
        if row["blob"] != "local" and blob_sha(source) != row["blob"]:
            raise ValueError(f"Upstream changed: {row['source']}; re-audit before enabling")
        obj = compile_file(source, row["id"],
                           ["-Werror"] if row["blob"] == "local" else [])
        link([obj, main, common, support], row["id"])
    for kind in [0, 1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 99]:
        obj = compile_file(PORT / "LOCAL/PT_FIXTURE.C", f"FIXTURE_{kind}",
                           [f"-DFIXTURE_KIND={kind}", "-Werror"])
        link([obj, main, common], f"FIXTURE_{kind}")

    info = {
        "validation_platform": sys.platform,
        "upstream_commit": provenance["commit"],
        "cc": subprocess.check_output([cc, "--version"], text=True).splitlines()[0],
        "flags": flags,
        "cases_sha256": hashlib.sha256((PORT / "CASES.DAT").read_bytes()).hexdigest(),
        "note": "POSIX host build; no native OpenVMS claim",
    }
    (builddir / "BUILD_INFO.TXT").write_text(json.dumps(info, indent=2) + "\n")
    (builddir / "BUILD_STATE.DAT").write_text("READY\n")
    print(f"Built {len(cases())} selected cases and supervisor", flush=True)


def self_test():
    import tempfile
    builddir = PORT / "BUILD"
    expected = {
        0: "PASS", 1: "FAIL", 2: "UNRESOLVED", 4: "UNSUPPORTED",
        5: "UNTESTED", 6: "CRASH", 7: "TIMEOUT", 8: "HARNESS_ERROR",
        9: "CRASH", 10: "HARNESS_ERROR", 11: "HARNESS_ERROR",
        99: "HARNESS_ERROR",
    }
    # The runner stops after HARNESS_ERROR; give each fixture its own run.
    with tempfile.TemporaryDirectory(prefix="pts_runner_") as temp:
        root = Path(temp)
        for kind, want in expected.items():
            manifest = root / f"fixture_{kind}.dat"
            manifest.write_text(
                f"FIXTURE_{kind}|fixture|1|selftest|none|runner_fixture|local|ready\n")
            result = subprocess.run(
                [str(builddir / "PTS_RUNNER.EXE"), str(manifest), str(root)],
                cwd=PORT, capture_output=True, text=True, timeout=15)
            summaries = sorted(root.glob("R_*/SUMMARY.CSV"))
            assert summaries, result.stdout + result.stderr
            # Each run has a different supervisor PID, so select its case.
            records = [row for path in summaries
                       for row in csv.DictReader(path.open())
                       if row["case"] == f"FIXTURE_{kind}"]
            assert len(records) == 1, records
            assert records[0]["result"] == want, (kind, records, result.stdout)
            assert result.returncode == (0 if kind == 0 else 1), (kind, result)
        bad = root / "invalid.dat"
        bad.write_text("../escape|fixture|1|selftest|none|x|local|ready\n")
        result = subprocess.run([str(builddir / "PTS_RUNNER.EXE"), str(bad), str(root)],
                                cwd=PORT, capture_output=True, timeout=5)
        assert result.returncode != 0
    print("PASS: runner pass/fail/unresolved/unsupported/untested/crash/timeout/"
          "missing-record/invalid-code/invalid-manifest checks", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default=os.environ.get("CC", "cc"))
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--repeats", default="1")
    parser.add_argument("--seed", default="1")
    parser.add_argument("--threads", default="8")
    parser.add_argument("--iterations", default="200")
    args = parser.parse_args()
    build(args.cc)
    if args.self_test:
        self_test()
    if args.run:
        return subprocess.call(
            [str(PORT / "BUILD/PTS_RUNNER.EXE"), "CASES.DAT", "RESULTS",
             args.repeats, args.seed, args.threads, args.iterations], cwd=PORT)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
