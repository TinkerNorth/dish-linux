#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Tests scripts/measurements-report.py against bench JSON in the shape
# tools/bench/input_path_bench.cpp writes. With DISH_BENCH_JSON naming a file
# the bench really wrote (a --smoke run's), that file is read too.
#
#   python3 -m unittest discover -s tests -p 'test_measurements_report.py'

import contextlib
import importlib.util
import io
import json
import os
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent.parent / "scripts" / "measurements-report.py"
SPEC = importlib.util.spec_from_file_location("measurements_report", SCRIPT)
report = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(report)


def latency_row(scenario: str, producer: str, p999: float = 30.0, max_us: float = 50.0) -> dict:
    return {"mode": "synthetic", "scenario": scenario, "producer": producer, "rateHz": 1000,
            "sent": 3000, "frames": 3000, "p50Us": 12.0, "p99Us": 20.0, "p999Us": p999,
            "maxUs": max_us, "overOneMs": 0, "longestGapMs": 1.1, "datagramsPerReport": 1.0,
            "drops": 0, "duplicates": 0, "pacingEarly": 0, "pacingMedianLatenessUs": 2.0}


def lock_row(scenario: str, lock: str, role: str, acquisitions: int) -> dict:
    return {"scenario": scenario, "lock": lock, "role": role, "acquisitions": acquisitions,
            "contended": 0, "tryFailures": 0, "maxWaitUs": 0.2, "p99WaitUs": 0.1,
            "maxHoldUs": 1.0}


def bench_json(lock_stats: bool, mic_hz: int, rows: list[dict], locks: list[dict]) -> dict:
    return {
        "bench": "input_path",
        "settings": {"mode": "synthetic", "rateHz": 1000, "secs": 3, "reps": 3, "micHz": mic_hz,
                     "receiver": "spin", "smoke": False, "scenarios": []},
        "machine": {"cpu": "Test CPU", "logicalCpus": 4, "os": "Test OS", "kernel": "Linux 6",
                    "build": "Release", "lockStats": lock_stats},
        "rows": rows,
        "extras": [{"scenario": r["scenario"], "rumblesSent": 0, "lightbarsSent": 0,
                    "queuedForSdl": 0, "micSent": 0, "micReceived": 0, "pingsReceived": 0,
                    "toggles": 0} for r in rows],
        "locks": locks, "skipped": False, "notes": [], "problems": [],
    }


MIC = "S5 (mic 1000 Hz)"


def write_runs(directory: Path, input_locks: int = 3000, mic_p999: float = 30.0) -> None:
    default_rows = [latency_row("S1", "A"), latency_row("S2", "A"), latency_row("S2", "B")]
    mic_rows = [latency_row(MIC, "A", p999=mic_p999), latency_row(MIC, "B")]
    locks = [lock_row("S1", "input.processor", "SdlInput", input_locks),
             lock_row("S1", "app.routing", "Gui", 10)]
    files = {
        "bench_input_path.json": bench_json(False, 50, default_rows, []),
        "bench_input_path-mic1000.json": bench_json(False, 1000, mic_rows, []),
        "bench_input_path_lockstats.json": bench_json(True, 50, default_rows, locks),
        "bench_input_path_lockstats-mic1000.json":
            bench_json(True, 1000, mic_rows, [lock_row(MIC, "satellite.send", "AudioCapture", 9)]),
    }
    directory.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        (directory / name).write_text(json.dumps(data), encoding="utf-8")


def run(*args: str) -> tuple[int, str]:
    out = io.StringIO()
    with contextlib.redirect_stdout(out):
        code = report.main(list(args))
    return code, out.getvalue()


class MeasurementsReportTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        self.raw = self.root / "raw"
        self.out = self.root / "out"
        os.environ.pop("GITHUB_STEP_SUMMARY", None)

    def tearDown(self) -> None:
        self.tmp.cleanup()

    def report(self, version: str = "2.5.0", *extra: str) -> tuple[int, str]:
        return run("--input-dir", str(self.raw), "--output-dir", str(self.out), "--version",
                   version, "--commit", "0123456789abcdef", *extra)

    def written(self, version: str = "2.5.0") -> dict:
        return json.loads((self.out / f"measurements-{version}.json").read_text(encoding="utf-8"))

    def test_writes_the_json_and_the_summary_for_four_runs(self) -> None:
        write_runs(self.raw)
        code, _ = self.report()
        self.assertEqual(code, 0)
        data = self.written()
        self.assertEqual(data["schema"], report.SCHEMA)
        self.assertEqual(data["commit"], "0123456789abcdef")
        self.assertEqual(len(data["latency"]), 10)
        self.assertEqual({s["target"] for s in data["sources"]},
                         {"bench_input_path", "bench_input_path_lockstats"})
        markdown = (self.out / "measurements-2.5.0.md").read_text(encoding="utf-8")
        self.assertIn("# Measurements 2.5.0", markdown)
        self.assertIn("`0123456`", markdown)
        self.assertIn("| bench_input_path | S5 (mic 1000 Hz) | A |", markdown)

    def test_the_lock_gate_fails_without_failing_the_run_until_it_is_enforced(self) -> None:
        write_runs(self.raw)
        code, printed = self.report()
        self.assertEqual(code, 0)
        gate = self.written()["lockGate"]
        self.assertFalse(gate["passed"])
        self.assertFalse(gate["enforced"])
        self.assertEqual(gate["failures"], [
            "bench_input_path_lockstats default S1: the SdlInput role acquired "
            "input.processor 3000 times"])
        self.assertIn("::notice title=Lock law gate::", printed)
        markdown = (self.out / "measurements-2.5.0.md").read_text(encoding="utf-8")
        self.assertIn("FAIL (not enforced", markdown)

        code, printed = self.report("2.5.0", "--enforce-lock-gate")
        self.assertEqual(code, 1)
        self.assertIn("::error title=Lock law gate::", printed)

    def test_the_lock_gate_passes_when_the_three_roles_take_no_lock(self) -> None:
        write_runs(self.raw, input_locks=0)
        code, _ = self.report("2.5.0", "--enforce-lock-gate")
        self.assertEqual(code, 0)
        self.assertTrue(self.written()["lockGate"]["passed"])

    def test_a_lockstats_run_without_locks_cannot_be_judged(self) -> None:
        write_runs(self.raw)
        path = self.raw / "bench_input_path_lockstats-mic1000.json"
        data = json.loads(path.read_text(encoding="utf-8"))
        data["locks"] = []
        path.write_text(json.dumps(data), encoding="utf-8")
        self.report()
        self.assertIn("bench_input_path_lockstats mic1000 recorded no lock at all, so its lock "
                      "laws cannot be judged", self.written()["lockGate"]["failures"])

    def test_the_headline_counts_the_input_threads_locks_per_report(self) -> None:
        write_runs(self.raw, input_locks=15000)
        self.report()
        top = self.written()["headline"]
        self.assertEqual(top["p50Us"], 12.0)
        self.assertAlmostEqual(top["inputLocksPerReport"], 5.0)
        self.assertEqual(top["noiseFloor"], {"maxUs": 50.0, "longestGapMs": 1.1})

    def test_a_missing_run_is_an_error_unless_partial_runs_are_allowed(self) -> None:
        write_runs(self.raw)
        (self.raw / "bench_input_path-mic1000.json").unlink()
        code, printed = self.report()
        self.assertEqual(code, 2)
        self.assertIn("missing runs: bench_input_path mic1000", printed)
        code, _ = self.report("2.5.0", "--allow-partial")
        self.assertEqual(code, 0)

    def test_two_files_with_the_same_rows_are_an_error(self) -> None:
        write_runs(self.raw)
        copy = (self.raw / "bench_input_path.json").read_text(encoding="utf-8")
        (self.raw / "bench_input_path-again.json").write_text(copy, encoding="utf-8")
        code, printed = self.report()
        self.assertEqual(code, 2)
        self.assertIn("two bench files cover the same row", printed)

    def test_a_version_label_with_a_slash_is_refused(self) -> None:
        write_runs(self.raw)
        code, printed = self.report("feature/x")
        self.assertEqual(code, 2)
        self.assertIn("may only hold letters", printed)

    def test_the_trend_warns_only_when_a_row_is_both_twice_and_100_us_slower(self) -> None:
        previous = self.root / "previous"
        write_runs(self.raw, mic_p999=30.0)
        self.report("2.4.0")
        previous.mkdir()
        (self.out / "measurements-2.4.0.json").rename(previous / "measurements-2.4.0.json")

        write_runs(self.raw, mic_p999=59.0)
        self.report("2.5.0", "--previous-dir", str(previous))
        self.assertEqual(self.written()["trend"]["warnings"], [])

        write_runs(self.raw, mic_p999=200.0)
        code, printed = self.report("2.5.0", "--previous-dir", str(previous))
        self.assertEqual(code, 0)
        warnings = self.written()["trend"]["warnings"]
        self.assertEqual(len(warnings), 2)
        self.assertTrue(all("S5 (mic 1000 Hz) A p99.9 is 200.0 us, was 30.0 us in 2.4.0" in w
                            for w in warnings))
        self.assertIn("::warning title=Measurement trend::", printed)
        markdown = (self.out / "measurements-2.5.0.md").read_text(encoding="utf-8")
        self.assertIn("## Trend against 2.4.0", markdown)
        self.assertIn("p99.9 us in 2.4.0", markdown)

    def test_the_newest_earlier_release_is_the_one_compared_with(self) -> None:
        previous = self.root / "previous"
        previous.mkdir()
        for version in ("2.4.0", "2.10.0", "2.9.1", "2.11.0"):
            write_runs(self.raw)
            self.report(version)
            (self.out / f"measurements-{version}.json").rename(
                previous / f"measurements-{version}.json")
        (previous / "measurements-junk.json").write_text("not json", encoding="utf-8")
        write_runs(self.raw)
        self.report("2.11.0", "--previous-dir", str(previous))
        self.assertEqual(self.written("2.11.0")["trend"]["previousVersion"], "2.10.0")

    def test_the_summary_is_appended_to_the_job_summary(self) -> None:
        write_runs(self.raw)
        summary = self.root / "summary.md"
        summary.write_text("earlier step\n", encoding="utf-8")
        os.environ["GITHUB_STEP_SUMMARY"] = str(summary)
        try:
            self.report()
        finally:
            os.environ.pop("GITHUB_STEP_SUMMARY", None)
        text = summary.read_text(encoding="utf-8")
        self.assertTrue(text.startswith("earlier step\n# Measurements 2.5.0"))

    def test_a_file_the_bench_really_wrote_is_read(self) -> None:
        path = os.environ.get("DISH_BENCH_JSON")
        if not path:
            self.skipTest("DISH_BENCH_JSON names no bench output")
        run = report.read_bench_file(Path(path))
        self.assertTrue(run["rows"])
        for row in run["rows"]:
            for name in report.LATENCY_FIELDS:
                self.assertIn(name, row)
        self.assertEqual(run["problems"], [])


if __name__ == "__main__":
    unittest.main()
