#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
# Copyright (C) 2026 Dish contributors.
#
# Turns the input-path bench's JSON (tools/bench, one file per run) into a
# release's measurements: measurements-<version>.json and the job summary
# measurements-<version>.md. Ported from the satellite's
# scripts/measurements-report.ps1.
#
# Two gates. The lock law gate: in the lock-statistics runs, the SdlInput,
# UsbReader, Receive and AudioCapture roles take no lock in any scenario, the
# rule tests/test_lock_laws.cpp holds them to. It fails the run unless
# --no-enforce-lock-gate reports it only. The trend gate warns, never fails: a
# row whose p99.9 or max is more than --trend-ratio times the previous
# release's and more than --trend-min-delta-us above it.
#
#   scripts/measurements-report.py --input-dir raw --output-dir out \
#       --version 2.5.0 [--previous-dir prev] [--commit SHA] [--no-enforce-lock-gate]
#
# Exit codes: 0 written (and the lock gate passed, when enforced), 1 a gate
# that is enforced failed, 2 the input could not be read.

import argparse
import datetime
import json
import os
import re
import sys
from pathlib import Path

SCHEMA = 1
BENCH = "input_path"
LATENCY_TARGET = "bench_input_path"
LOCKSTATS_TARGET = "bench_input_path_lockstats"
DEFAULT_MIC_HZ = 50
DEFAULT_VARIANT = "default"
EXPECTED_TARGETS = (LATENCY_TARGET, LOCKSTATS_TARGET)
EXPECTED_VARIANTS = (DEFAULT_VARIANT, "mic1000")
MIC_SCENARIO = "S5"
LOCK_FREE_ROLES = ("SdlInput", "UsbReader", "Receive", "AudioCapture")
INPUT_ROLE = "SdlInput"
HEADLINE_SCENARIO = "S1"
HEADLINE_PRODUCER = "A"
TREND_METRICS = {"p999Us": "p99.9", "maxUs": "max"}
VERSION_PATTERN = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._+-]*$")
SHORT_COMMIT = 7

LATENCY_FIELDS = {
    "rateHz": float, "sent": int, "frames": int, "p50Us": float, "p99Us": float,
    "p999Us": float, "maxUs": float, "overOneMs": int, "longestGapMs": float,
    "datagramsPerReport": float, "drops": int, "duplicates": int, "pacingEarly": int,
    "pacingMedianLatenessUs": float,
}
REQUIRED_LATENCY = ("scenario", "producer", "frames", "p50Us", "p99Us", "p999Us", "maxUs")
EXTRA_FIELDS = ("rumblesSent", "lightbarsSent", "queuedForSdl", "micSent", "micReceived",
                "pingsReceived", "toggles")
LOCK_FIELDS = {
    "acquisitions": int, "contended": int, "tryFailures": int, "maxWaitUs": float,
    "p99WaitUs": float, "maxHoldUs": float,
}


class ReportError(Exception):
    """The input cannot be turned into a report."""


def version_sort_key(name: str) -> str:
    return "".join(digits.zfill(8) for digits in re.findall(r"\d+", name))


def fixed(value: float, decimals: int = 1) -> str:
    return f"{value:.{decimals}f}"


def run_variant(settings: dict) -> str:
    mic_hz = int(settings.get("micHz", DEFAULT_MIC_HZ))
    return DEFAULT_VARIANT if mic_hz == DEFAULT_MIC_HZ else f"mic{mic_hz}"


def scenario_kept(variant: str, scenario: str) -> bool:
    return variant == DEFAULT_VARIANT or scenario.startswith(MIC_SCENARIO)


def figure(row: dict, name: str, kind) -> float | int:
    value = row.get(name)
    return kind(0) if value is None else kind(value)


def read_bench_file(path: Path) -> dict:
    try:
        data = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as error:
        raise ReportError(f"{path.name} is not valid JSON: {error}") from error
    if not isinstance(data, dict) or data.get("bench") != BENCH:
        raise ReportError(f"{path.name} is not {BENCH} bench output")
    settings = data.get("settings")
    machine = data.get("machine")
    if not isinstance(settings, dict) or "micHz" not in settings:
        raise ReportError(f"{path.name} has no settings.micHz, so its run cannot be named")
    if not isinstance(machine, dict):
        raise ReportError(f"{path.name} has no machine")
    if data.get("skipped"):
        raise ReportError(f"{path.name} is a skipped run: {'; '.join(data.get('notes', []))}")
    target = LOCKSTATS_TARGET if machine.get("lockStats") else LATENCY_TARGET
    variant = run_variant(settings)
    rows = []
    for row in data.get("rows") or []:
        for name in REQUIRED_LATENCY:
            if row.get(name) is None:
                raise ReportError(f"{path.name} has a row without {name}")
        if not scenario_kept(variant, row["scenario"]):
            continue
        out = {"target": target, "variant": variant, "mode": str(row.get("mode", "synthetic")),
               "scenario": str(row["scenario"]), "producer": str(row["producer"])}
        out.update({name: figure(row, name, kind) for name, kind in LATENCY_FIELDS.items()})
        rows.append(out)
    if not rows:
        raise ReportError(f"{path.name} has no rows")
    extras = [{"target": target, "variant": variant, "scenario": str(extra["scenario"]),
               **{name: int(extra.get(name, 0)) for name in EXTRA_FIELDS}}
              for extra in data.get("extras") or []
              if scenario_kept(variant, str(extra.get("scenario", "")))]
    locks = [{"target": target, "variant": variant, "scenario": str(lock["scenario"]),
              "lock": str(lock["lock"]), "role": str(lock["role"]),
              **{name: figure(lock, name, kind) for name, kind in LOCK_FIELDS.items()}}
             for lock in data.get("locks") or []
             if scenario_kept(variant, str(lock.get("scenario", "")))]
    return {"file": path.name, "target": target, "variant": variant, "settings": settings,
            "machine": machine, "rows": rows, "extras": extras, "locks": locks,
            "problems": [f"{path.name}: {p}" for p in data.get("problems") or []],
            "notes": [f"{path.name}: {n}" for n in data.get("notes") or []]}


def read_bench_directory(directory: Path) -> list[dict]:
    if not directory.is_dir():
        raise ReportError(f"input directory {directory} does not exist")
    files = sorted(directory.glob("*.json"))
    if not files:
        raise ReportError(f"no bench JSON files in {directory}")
    return [read_bench_file(path) for path in files]


def row_key(row: dict) -> str:
    return f"{row['target']}|{row['variant']}|{row['scenario']}|{row['producer']}"


def missing_runs(runs: list[dict]) -> list[str]:
    present = {(run["target"], run["variant"]) for run in runs}
    return [f"{target} {variant}" for target in EXPECTED_TARGETS for variant in EXPECTED_VARIANTS
            if (target, variant) not in present]


def build_measurements(runs: list[dict], version: str, commit: str, generated_at: str) -> dict:
    seen = set()
    latency, extras, locks, problems, notes, sources = [], [], [], [], [], []
    for run in runs:
        for row in run["rows"]:
            key = row_key(row)
            if key in seen:
                raise ReportError(f"two bench files cover the same row: {key}")
            seen.add(key)
            latency.append(row)
        extras.extend(run["extras"])
        locks.extend(run["locks"])
        problems.extend(run["problems"])
        notes.extend(run["notes"])
        sources.append({"file": run["file"], "target": run["target"], "variant": run["variant"],
                        "settings": run["settings"]})
    first = next((r for r in runs if r["variant"] == DEFAULT_VARIANT), runs[0])
    machine = first["machine"]
    return {
        "schema": SCHEMA, "bench": BENCH, "version": version, "commit": commit,
        "generatedAt": generated_at,
        "machine": {"cpu": machine.get("cpu", "unknown"),
                    "logicalCpus": machine.get("logicalCpus", 0),
                    "os": machine.get("os", "unknown"), "kernel": machine.get("kernel", "unknown"),
                    "build": machine.get("build", "unknown"),
                    "runnerImage": os.environ.get("ImageVersion", "")},
        "settings": first["settings"], "sources": sources, "latency": latency, "extras": extras,
        "locks": locks, "problems": problems, "notes": notes,
    }


def lock_law_gate(measurements: dict, enforced: bool) -> dict:
    failures = []
    recorded = {(lock["target"], lock["variant"]) for lock in measurements["locks"]}
    stats_runs = sorted({(row["target"], row["variant"]) for row in measurements["latency"]
                         if row["target"] == LOCKSTATS_TARGET})
    for target, variant in stats_runs:
        if (target, variant) not in recorded:
            failures.append(f"{target} {variant} recorded no lock at all, so its lock laws "
                            "cannot be judged")
    for lock in measurements["locks"]:
        if lock["role"] not in LOCK_FREE_ROLES or lock["acquisitions"] <= 0:
            continue
        times = "time" if lock["acquisitions"] == 1 else "times"
        failures.append(f"{lock['target']} {lock['variant']} {lock['scenario']}: the "
                        f"{lock['role']} role acquired {lock['lock']} {lock['acquisitions']} "
                        f"{times}")
    return {"name": "lockLaws", "roles": list(LOCK_FREE_ROLES), "enforced": enforced,
            "checked": len(measurements["locks"]), "passed": not failures, "failures": failures}


def read_previous(directory: Path | None, exclude_version: str) -> dict | None:
    if directory is None or not directory.is_dir():
        return None
    found = None
    for path in sorted(directory.glob("measurements-*.json"), key=lambda p: version_sort_key(p.name)):
        try:
            data = json.loads(path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            print(f"::notice::skipping {path.name}, which is not valid JSON")
            continue
        usable = isinstance(data, dict) and data.get("schema") == SCHEMA and \
            data.get("bench") == BENCH and isinstance(data.get("latency"), list)
        if not usable:
            print(f"::notice::skipping {path.name}, which is not a schema {SCHEMA} {BENCH} file")
            continue
        if str(data.get("version")) == exclude_version:
            continue
        found = data
    return found


def regressed(now: float, was: float, ratio: float, min_delta_us: float) -> bool:
    return now > was * ratio and now - was > min_delta_us


def trend_comparison(measurements: dict, previous: dict | None, ratio: float,
                     min_delta_us: float) -> dict:
    if previous is None:
        return {"available": False, "previousVersion": "", "ratio": ratio,
                "minDeltaUs": min_delta_us, "warnings": [], "previousValues": {}}
    before = {row_key(row): row for row in previous["latency"]}
    warnings, previous_values = [], {}
    for row in measurements["latency"]:
        key = row_key(row)
        if key not in before:
            continue
        for metric, label in TREND_METRICS.items():
            now, was = float(row[metric]), float(before[key].get(metric, 0))
            previous_values[f"{key}|{metric}"] = was
            if regressed(now, was, ratio, min_delta_us):
                warnings.append(f"{row['target']} {row['scenario']} {row['producer']} {label} is "
                                f"{fixed(now)} us, was {fixed(was)} us in "
                                f"{previous['version']}, +{fixed(now - was)} us")
    return {"available": True, "previousVersion": str(previous["version"]), "ratio": ratio,
            "minDeltaUs": min_delta_us, "warnings": warnings, "previousValues": previous_values}


def find_row(rows: list[dict], target: str, scenario: str, producer: str) -> dict | None:
    return next((r for r in rows if r["target"] == target and r["variant"] == DEFAULT_VARIANT
                 and r["scenario"] == scenario and r["producer"] == producer), None)


def headline(measurements: dict) -> dict:
    out: dict = {"scenario": HEADLINE_SCENARIO, "producer": HEADLINE_PRODUCER}
    plain = find_row(measurements["latency"], LATENCY_TARGET, HEADLINE_SCENARIO, HEADLINE_PRODUCER)
    if plain is not None:
        out.update({"p50Us": plain["p50Us"], "p99Us": plain["p99Us"], "p999Us": plain["p999Us"],
                    "noiseFloor": {"maxUs": plain["maxUs"],
                                   "longestGapMs": plain["longestGapMs"]}})
    counted = find_row(measurements["latency"], LOCKSTATS_TARGET, HEADLINE_SCENARIO,
                       HEADLINE_PRODUCER)
    if counted is not None and counted["sent"] > 0:
        taken = sum(lock["acquisitions"] for lock in measurements["locks"]
                    if lock["target"] == LOCKSTATS_TARGET and lock["variant"] == DEFAULT_VARIANT
                    and lock["scenario"] == HEADLINE_SCENARIO and lock["role"] == INPUT_ROLE)
        out["inputLocksPerReport"] = taken / counted["sent"]
    return out


def table(headers: list[str], rows: list[list[str]], text_columns: int) -> str:
    rule = ["---" if i < text_columns else "---:" for i in range(len(headers))]
    lines = ["| " + " | ".join(headers) + " |", "| " + " | ".join(rule) + " |"]
    lines += ["| " + " | ".join(cells) + " |" for cells in rows]
    return "\n".join(lines)


def scenario_order(scenario: str) -> tuple:
    digits = re.findall(r"\d+", scenario)
    return (int(digits[0]) if digits else 0, scenario)


def latency_table(measurements: dict, trend: dict) -> str:
    headers = ["target", "scenario", "producer", "rate Hz", "frames", "p50 us", "p99 us",
               "p99.9 us", "max us", "over 1 ms", "longest gap ms", "dgrams/report", "drops"]
    if trend["available"]:
        headers += [f"p99.9 us in {trend['previousVersion']}",
                    f"max us in {trend['previousVersion']}"]
    rows = []
    ordered = sorted(measurements["latency"],
                     key=lambda r: (r["target"], r["variant"], scenario_order(r["scenario"])))
    for r in ordered:
        cells = [r["target"], r["scenario"], r["producer"], fixed(r["rateHz"], 0), str(r["frames"]),
                 fixed(r["p50Us"]), fixed(r["p99Us"]), fixed(r["p999Us"]), fixed(r["maxUs"]),
                 str(r["overOneMs"]), fixed(r["longestGapMs"], 3),
                 fixed(r["datagramsPerReport"], 2), str(r["drops"])]
        if trend["available"]:
            for metric in TREND_METRICS:
                was = trend["previousValues"].get(f"{row_key(r)}|{metric}")
                cells.append("-" if was is None else fixed(was))
        rows.append(cells)
    return table(headers, rows, 3)


def extras_table(measurements: dict) -> str:
    headers = ["target", "scenario", "rumbles sent", "light-bars sent", "queued for SDL",
               "mic sent", "mic received", "pings", "toggles"]
    rows = [[e["target"], e["scenario"]] + [str(e[name]) for name in EXTRA_FIELDS]
            for e in sorted(measurements["extras"],
                            key=lambda e: (e["target"], e["variant"], scenario_order(e["scenario"])))]
    return table(headers, rows, 2)


def lock_table(measurements: dict) -> str:
    headers = ["scenario", "lock", "role", "acquisitions", "contended", "try fails",
               "max wait us", "p99 wait us", "max hold us"]
    rows = [[lock["scenario"], lock["lock"], lock["role"], str(lock["acquisitions"]),
             str(lock["contended"]), str(lock["tryFailures"]), fixed(lock["maxWaitUs"]),
             fixed(lock["p99WaitUs"]), fixed(lock["maxHoldUs"])]
            for lock in sorted(measurements["locks"],
                               key=lambda k: (k["variant"], scenario_order(k["scenario"]),
                                              k["lock"], k["role"]))]
    return table(headers, rows, 3)


def gate_markdown(gate: dict) -> list[str]:
    roles = ", ".join(gate["roles"])
    lines = ["## Lock law gate", ""]
    if gate["passed"]:
        lines.append(f"PASS: the {roles} roles took no lock in any scenario "
                     f"({gate['checked']} lock rows checked).")
        return lines
    state = "enforced" if gate["enforced"] else (
        "not enforced: this run was asked to report it only (enforce_lock_gate off)")
    lines.append(f"FAIL ({state}). The {roles} roles must take no lock:")
    lines.append("")
    lines += [f"- {failure}" for failure in gate["failures"]]
    return lines


def trend_markdown(trend: dict) -> list[str]:
    if not trend["available"]:
        return ["## Trend", "", "No earlier release carries measurements, so nothing was compared."]
    lines = [f"## Trend against {trend['previousVersion']}", "",
             f"A row warns when its p99.9 or max is more than {trend['ratio']:g} times the "
             f"previous release's and more than {trend['minDeltaUs']:g} us above it."]
    if not trend["warnings"]:
        lines += ["", "No row regressed."]
        return lines
    lines.append("")
    lines += [f"- {warning}" for warning in trend["warnings"]]
    return lines


def render_markdown(measurements: dict, gate: dict, trend: dict, top: dict) -> str:
    machine = measurements["machine"]
    settings = measurements["settings"]
    commit = measurements["commit"][:SHORT_COMMIT] if measurements["commit"] else "unknown"
    lines = [f"# Measurements {measurements['version']}", "",
             f"Commit `{commit}`, generated {measurements['generatedAt']}. {machine['cpu']}, "
             f"{machine['logicalCpus']} logical CPUs, {machine['os']} ({machine['kernel']}), "
             f"{machine['build']} build. Runs of {settings.get('secs')} s x {settings.get('reps')} "
             f"reps, pad A at {settings.get('rateHz')} Hz, receivers "
             f"{settings.get('receiver', 'spin')}; latencies in microseconds.", ""]
    lines += ["## Headline", ""]
    if "p50Us" in top:
        lines.append(f"{top['scenario']} pad {top['producer']}, one pad and nothing else: "
                     f"{fixed(top['p50Us'])} us median, {fixed(top['p99Us'])} us p99, "
                     f"{fixed(top['p999Us'])} us p99.9 from publish to the satellite.")
        floor = top["noiseFloor"]
        lines.append(f"Runner noise floor, the same run: max {fixed(floor['maxUs'])} us, longest "
                     f"gap {fixed(floor['longestGapMs'], 3)} ms. A shared runner is noisy; a "
                     "difference inside that is not a finding.")
    if "inputLocksPerReport" in top:
        lines.append(f"The SdlInput thread took {fixed(top['inputLocksPerReport'], 2)} locks per "
                     f"report in {top['scenario']}.")
    lines.append("")
    lines += gate_markdown(gate) + [""] + trend_markdown(trend) + [""]
    lines += ["## Latency", "", latency_table(measurements, trend), ""]
    if measurements["extras"]:
        lines += ["## Side traffic", "", extras_table(measurements), ""]
    if measurements["locks"]:
        lines += ["## Locks", "", f"From {LOCKSTATS_TARGET}, for the reported rep of each "
                  "scenario.", "", lock_table(measurements), ""]
    if measurements["problems"] or measurements["notes"]:
        lines += ["## Problems and notes", ""]
        lines += [f"- PROBLEM: {p}" for p in measurements["problems"]]
        lines += [f"- note: {n}" for n in measurements["notes"]]
        lines.append("")
    return "\n".join(lines)


def write_report(args: argparse.Namespace) -> int:
    if not VERSION_PATTERN.match(args.version):
        raise ReportError(f"version label '{args.version}' may only hold letters, digits, '.', "
                          "'_', '+' and '-'")
    runs = read_bench_directory(Path(args.input_dir))
    missing = missing_runs(runs)
    if missing and not args.allow_partial:
        raise ReportError(f"missing runs: {', '.join(missing)}")
    generated_at = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    measurements = build_measurements(runs, args.version, args.commit, generated_at)
    gate = lock_law_gate(measurements, args.enforce_lock_gate)
    previous = read_previous(Path(args.previous_dir) if args.previous_dir else None, args.version)
    trend = trend_comparison(measurements, previous, args.trend_ratio, args.trend_min_delta_us)
    top = headline(measurements)
    measurements["lockGate"] = gate
    measurements["headline"] = top
    measurements["trend"] = {key: trend[key] for key in
                             ("previousVersion", "ratio", "minDeltaUs", "warnings")}
    markdown = render_markdown(measurements, gate, trend, top)
    out_dir = Path(args.output_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    json_path = out_dir / f"measurements-{args.version}.json"
    md_path = out_dir / f"measurements-{args.version}.md"
    json_path.write_text(json.dumps(measurements, indent=2) + "\n", encoding="utf-8",
                         newline="\n")
    md_path.write_text(markdown, encoding="utf-8", newline="\n")
    summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if summary:
        with open(summary, "a", encoding="utf-8", newline="\n") as out:
            out.write(markdown + "\n")
    for warning in trend["warnings"]:
        print(f"::warning title=Measurement trend::{warning}")
    if gate["enforced"]:
        for failure in gate["failures"]:
            print(f"::error title=Lock law gate::{failure}")
    elif not gate["passed"]:
        print(f"::notice title=Lock law gate::FAIL, not enforced: {len(gate['failures'])} "
              "acquisitions by roles that must take no lock (see the summary)")
    print(f"measurements: {len(runs)} bench files, {len(measurements['latency'])} latency rows, "
          f"{len(measurements['locks'])} lock rows")
    print(f"lock law gate: {'PASS' if gate['passed'] else 'FAIL'}"
          f"{'' if gate['enforced'] else ' (not enforced)'}")
    print(f"trend: {len(trend['warnings'])} warning(s) against {trend['previousVersion']}"
          if trend["available"] else "trend: no earlier release to compare with")
    print(f"wrote {json_path}")
    print(f"wrote {md_path}")
    return 1 if gate["enforced"] and not gate["passed"] else 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input-dir", required=True)
    parser.add_argument("--output-dir", required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--previous-dir", default="")
    parser.add_argument("--commit", default=os.environ.get("GITHUB_SHA", ""))
    parser.add_argument("--trend-ratio", type=float, default=2.0)
    parser.add_argument("--trend-min-delta-us", type=float, default=100.0)
    parser.add_argument("--enforce-lock-gate", action=argparse.BooleanOptionalAction,
                        default=True)
    parser.add_argument("--allow-partial", action="store_true")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    try:
        return write_report(args)
    except ReportError as error:
        print(f"::error title=Measurements report::{error}")
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
