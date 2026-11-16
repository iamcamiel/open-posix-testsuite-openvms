#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright 2026 iamcamiel
# X-2 CV Camiel Vanderhoeven 16-Nov-2026: QIO/AST coverage and portable LF output.
# X-1 CV Camiel Vanderhoeven 15-Nov-2026: public API coverage inventory.
"""Generate coverage from a reference pthread.h; never copy the header."""
import argparse
import csv
import hashlib
import re
from pathlib import Path
from BUILD_HOST import PORT, SUITE, cases

UNIX_ONLY = {
    "pthread_sigmask", "pthread_kill", "pthread_mutexattr_getprioceiling",
    "pthread_mutexattr_setprioceiling", "pthread_mutexattr_getprotocol",
    "pthread_mutexattr_setprotocol", "pthread_mutex_getprioceiling",
    "pthread_mutex_setprioceiling",
}
OBSOLETE = {"pthread_debug", "pthread_debug_cmd"}
ABSENT = [
    "pthread_mutex_timedlock", "pthread_rwlock_timedrdlock",
    "pthread_rwlock_timedwrlock", "pthread_condattr_getclock",
    "pthread_condattr_setclock", "pthread_mutexattr_getrobust",
    "pthread_mutexattr_setrobust", "pthread_mutex_consistent",
    "pthread_barrier_init", "pthread_barrier_wait", "pthread_barrier_destroy",
    "pthread_barrierattr_init", "pthread_barrierattr_destroy",
    "pthread_barrierattr_getpshared", "pthread_barrierattr_setpshared",
    "pthread_spin_init", "pthread_spin_destroy", "pthread_spin_lock",
    "pthread_spin_trylock", "pthread_spin_unlock", "pthread_setschedprio",
    "pthread_getcpuclockid", "pthread_atfork",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("header", type=Path)
    args = parser.parse_args()
    raw = args.header.read_bytes()
    text = re.sub(r"/\*.*?\*/", "", raw.decode("utf-8", errors="replace"),
                  flags=re.S)
    apis = set(re.findall(r"\b(pthread_\w+)\s*\(", text))
    # Macro definitions and inline functions are part of the public interface.
    apis -= {"pthread_t", "pthread_attr_t"}
    asserted = {}
    exercised = {}
    for row in cases():
        for api in row["apis"].split(","):
            asserted.setdefault(api, []).append(row["id"])
        src = (SUITE / row["source"]).read_text()
        src = re.sub(r"/\*.*?\*/|//[^\n]*", "", src, flags=re.S)
        for api in set(re.findall(r"\b(pthread_\w+)\s*\(", src)):
            exercised.setdefault(api, []).append(row["id"])
    with (PORT / "API_COVERAGE.CSV").open("w", newline="") as out:
        writer = csv.writer(out, lineterminator="\n")
        writer.writerow(["api", "header_status", "coverage",
                         "asserted_cases", "exercised_cases", "notes"])
        for api in sorted(apis | set(ABSENT)):
            state = ("unix_only" if api in UNIX_ONLY else
                     "obsolete" if api in OBSOLETE else
                     "not_declared" if api not in apis else "declared_vms")
            covered = asserted.get(api, [])
            incidental = exercised.get(api, [])
            coverage = ("partial" if covered else
                        "exercised_only" if incidental else "planned")
            if state != "declared_vms":
                coverage = "excluded"
            note = ("Native QIO/AST stress; target validation pending"
                    if api == "pthread_cond_signal_int_np" and covered else
                    "VMS extension; requires dedicated semantics review"
                    if api.endswith("_np") else
                    "No claim of complete assertion coverage or VMS runtime validation")
            writer.writerow([api, state, coverage, ",".join(covered),
                             ",".join(incidental), note])
    reference = (
        "Reference: baseos/PTHREAD/PTHREAD.H (API names only; header not vendored)\n"
        f"SHA256: {hashlib.sha256(raw).hexdigest()}\n"
        "Classification: declarations plus explicit UNIX-only/obsolete exclusions.\n"
        "not_declared means absent from this header, not a runtime ENOTSUP claim.\n"
        "Verify the deployment node's headers and optional features separately.\n")
    with (PORT / "HEADER_REFERENCE.TXT").open("w", newline="\n") as out:
        out.write(reference)
    print(f"Inventoried {len(apis)} header interfaces and {len(set(ABSENT)-apis)} "
          "optional interfaces absent from the reference header")


if __name__ == "__main__":
    main()
