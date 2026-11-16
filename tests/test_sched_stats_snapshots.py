#!/usr/bin/env python3
"""Exercise snapshots with real C++/Rust consumers and deterministic C tests.

Run with an instrumented resource tree and a CILK_STATS=0 resource tree. No
benchmark sources are edited. All generated programs/reports use a temp dir.
"""

import argparse
from collections import Counter
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import re
import resource
import subprocess
import tempfile


CATEGORIES = {"working", "scheduling", "idling", "sleep", "sleep_uncilk",
              "cilkify_enter", "cilkify_exit", "pending_search"}
COUNTERS = ("steals", "repossessions", "reengagement_requests",
            "one_sentinel_requests", "observed_ns")
NAME = re.compile(r"cheetah_stats_(\d{8}_\d{6}_\d{6})\.json$")


def run(command, **kwargs):
    result = subprocess.run(list(map(str, command)), capture_output=True, text=True,
                            timeout=90, **kwargs)
    assert result.returncode == 0, (command, result.returncode, result.stdout, result.stderr)
    return result


def report(directory, *, unit=False):
    files = list(directory.glob("cheetah_stats_*.json"))
    if unit:
        sentinels = [p for p in files if p.read_text() == "sentinel"]
        assert len(sentinels) == 1, files
        files = [p for p in files if p not in sentinels]
    assert len(files) == 1, files
    match = NAME.fullmatch(files[0].name)
    assert match
    datetime.strptime(match[1], "%Y%m%d_%H%M%S_%f")
    data = json.loads(files[0].read_text())
    assert isinstance(data, list)
    assert [p["pool_id"] for p in data] == sorted(p["pool_id"] for p in data)
    for pool in data:
        assert pool["snapshots"][-1]["id"] == {"kind": "exit"}
        for s in pool["snapshots"]:
            total = s["total"]
            assert total["worker_count"] == len(s["workers"])
            assert total["any_worker_active"] == any(w["stats"]["active"] for w in s["workers"])
            for w in s["workers"]:
                stats = w["stats"]
                assert set(stats["times_ns"]) == set(stats["counts"]) == CATEGORIES
                assert sum(stats["times_ns"].values()) == stats["observed_ns"]
            for kind in ("times_ns", "counts"):
                for category in CATEGORIES:
                    assert total[kind][category] == sum(w["stats"][kind][category] for w in s["workers"])
            for counter in COUNTERS:
                assert total[counter] == sum(w["stats"][counter] for w in s["workers"])
            assert sum(total["times_ns"].values()) == total["observed_ns"]
    return data


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--resource", type=Path, required=True)
    parser.add_argument("--disabled-resource", type=Path, required=True)
    parser.add_argument("--cxx", required=True, help="OpenCilk clang++")
    parser.add_argument("--cc", default="clang")
    parser.add_argument("--rust", action="store_true", help="Also test Rust FFI with rustc +stage1")
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1]
    enabled = args.resource.resolve()
    disabled = args.disabled_resource.resolve()
    triple = "x86_64-unknown-linux-gnu"
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))

    with tempfile.TemporaryDirectory(prefix="cheetah-snapshots-") as temporary:
        root = Path(temporary)
        unit = root / "unit"
        run([args.cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-pthread",
             "-DCHEETAH_STATS_DIR_DEFAULT=\"embedded\"", "-I" + str(source / "include"),
             "-I" + str(source / "runtime"), source / "tests/sched_stats_snapshot_unit.c", "-o", unit])
        for use_override in (True, False):
            case = root / ("unit-override" if use_override else "unit-embedded")
            case.mkdir()
            env = os.environ.copy()
            env.pop("CHEETAH_STATS_DIR", None)
            destination = case / ("override" if use_override else "embedded")
            if use_override:
                env["CHEETAH_STATS_DIR"] = str(destination)
            run([unit], env=env, cwd=case)
            pools = report(destination, unit=True)
            assert [p["pool_id"] for p in pools] == [0, 1]
            saved = pools[0]["snapshots"]
            assert sum(s["id"] == {"kind": "user", "value": "exit"} for s in saved) == 2
            by_id = {s["id"].get("value"): s for s in saved if s["id"]["kind"] == "user"}
            assert "must-not-appear" not in by_id
            assert by_id['copy-☃"\\\n']["total"]["times_ns"]["sleep_uncilk"] == 20
            assert by_id["pending-success"]["total"]["times_ns"]["pending_search"] == 20
            assert by_id["resolved-success"]["total"]["times_ns"]["scheduling"] == 20
            assert saved[-1]["total"]["times_ns"]["working"] == 100
        print("PASS: unified interval accounting, pending/retried/abandoned searches, ownership, overflow, UTF-8, caller restrictions, pools, directory precedence, filename collision")

        binaries = {}
        for label, runtime in (("enabled", enabled), ("disabled", disabled)):
            symbols = run(["nm", "-D", "--defined-only", runtime / "lib" / triple / "libopencilk.so"]).stdout
            assert "__cilkrts_sched_stats_print" not in symbols
            assert "__cilkrts_sched_stats_snapshot_and_reset" in symbols
            binary = root / (label + "-program")
            run([args.cxx, "-std=c++17", "-O2", "-pthread", "-fopencilk",
                 "--opencilk-resource-dir=" + str(runtime),
                 source / "tests/sched_stats_snapshot_integration.cpp", "-o", binary])
            binaries[label] = binary

        def environment(runtime, directory):
            env = os.environ.copy()
            env["CILK_NWORKERS"] = "4"
            env["LD_LIBRARY_PATH"] = str(runtime / "lib" / triple) + ":" + env.get("LD_LIBRARY_PATH", "")
            env.pop("CHEETAH_STATS_DIR", None)
            if directory is not None:
                env["CHEETAH_STATS_DIR"] = str(directory)
            return env

        for mode in ("normal", "exit-only", "no-work", "directory-change", "disabled"):
            case = root / mode
            case.mkdir()
            runtime = disabled if mode == "disabled" else enabled
            binary = binaries["disabled" if mode == "disabled" else "enabled"]
            # Use a relative directory, then change cwd/env after constructor.
            destination = case / "relative/nested"
            other = case / "other"
            other.mkdir()
            env = environment(runtime, "relative/nested")
            result = run([binary, mode, other], cwd=case, env=env)
            assert "SCHEDULING STATS (SECONDS)" not in result.stderr
            if mode == "disabled":
                assert not list(case.rglob("*.json"))
                continue
            pools = report(destination)
            assert len(pools) == 1
            samples = pools[0]["snapshots"]
            if mode in ("exit-only", "no-work"):
                assert len(samples) == 1
            else:
                assert len(samples) == 15
                ids = Counter(s["id"].get("value") for s in samples[:-1])
                assert ids["exit"] == 2 and ids['quote-"\\\n-☃'] == 1
                for i in range(1, 4):
                    s = next(s for s in samples if s["id"].get("value") == f"iteration/{i}")
                    assert s["total"]["times_ns"]["working"] > 0
                assert samples[-1]["total"]["times_ns"]["sleep_uncilk"] >= 20_000_000
            assert not list(other.rglob("*.json"))
        print("PASS: C++ fork/join workload, concurrent recording, exit tail, automatic report, frozen paths, CILK_STATS=0")

        case = root / "default-cwd"
        case.mkdir()
        run([binaries["enabled"], "no-work"], cwd=case, env=environment(enabled, None))
        report(case)
        for mode in ("abort", "quick-exit"):
            case = root / mode
            case.mkdir()
            result = subprocess.run([binaries["enabled"], mode], cwd=case,
                                    env=environment(enabled, case), capture_output=True, timeout=90)
            assert result.returncode != 0 if mode == "abort" else result.returncode == 0
            assert not list(case.glob("*.json"))
        for label in ("empty", "file"):
            case = root / ("bad-directory-" + label)
            case.mkdir()
            target = case / "not-a-directory"
            target.write_text("keep this file")
            result = run([binaries["enabled"], "no-work"], cwd=case,
                         env=environment(enabled, "" if label == "empty" else target))
            assert "failed to write JSON report" in result.stderr
            assert target.read_text() == "keep this file"
            assert not list(case.glob("*.json"))
        print("PASS: cwd fallback, abort/_exit buffering semantics, deferred output errors")

        if args.rust:
            case = root / "rust"
            case.mkdir()
            library = enabled / "lib" / triple
            env = environment(enabled, case)
            env["OPENCILK_RT_SEARCH_DIR"] = str(library)
            env["OPENCILK_ABI_PATH"] = str(library / "libopencilk-abi.bc")
            binary = root / "rust-ffi"
            run(["rustc", "+stage1", "--edition=2021", "-O",
                 source / "tests/sched_stats_snapshot_ffi.rs", "-L", "native=" + str(library),
                 "-l", "static=opencilk-personality-cpp", "-l", "stdc++", "-o", binary], env=env)
            run([binary], cwd=case, env=env)
            samples = report(case)[0]["snapshots"]
            assert [s["id"] for s in samples] == [
                {"kind": "user", "value": "rust-ffi/iteration/1"}, {"kind": "exit"}]
            assert samples[0]["total"]["times_ns"]["working"] > 0
            print("PASS: ERI static linking and Rust FFI layout/ownership")


if __name__ == "__main__":
    main()
