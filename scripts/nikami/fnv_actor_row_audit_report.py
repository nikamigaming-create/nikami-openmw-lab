#!/usr/bin/env python3
"""Summarize FNV actor parity burn-down rows against generated row-run proof."""

from __future__ import annotations

import argparse
import csv
import html
import json
import os
from collections import Counter
from datetime import datetime
from pathlib import Path
from typing import Any


ALLOWED_CLASSIFICATIONS = {
    "runtime-supported",
    "loaded-pending-runtime",
    "known-blocked",
    "non-runtime-support-file",
    "intentionally-excluded-with-proof",
}

RUNTIME_REQUIRED = {"runtime-supported", "loaded-pending-runtime"}


def as_text(value: Any) -> str:
    return "" if value is None else str(value)


def as_list(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def norm_path(value: str) -> str:
    if not value:
        return ""
    return os.path.normcase(os.path.abspath(value))


def load_json(path: Path) -> Any:
    with path.open("r", encoding="utf-8-sig") as handle:
        return json.load(handle)


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8") as handle:
        json.dump(data, handle, indent=2)
        handle.write("\n")


def latest_burndown(proof_root: Path) -> Path:
    root = proof_root / "fnv-actor-parity-burndown"
    candidates = sorted(
        (candidate / "actor-parity-burndown.json" for candidate in root.glob("*") if candidate.is_dir()),
        key=lambda path: path.stat().st_mtime if path.is_file() else 0,
        reverse=True,
    )
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    raise SystemExit(f"No actor parity burn-down JSON found under {root}")


def latest_out_dir(proof_root: Path) -> Path:
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    return proof_root / "fnv-actor-row-audit-report" / stamp


def validate_burndown(burn: dict[str, Any], path: Path) -> list[str]:
    errors: list[str] = []
    if burn.get("schema") != "nikami-fnv-actor-parity-burndown-v1":
        errors.append(f"unexpected burn-down schema: {burn.get('schema')}")
    rows = [row for row in as_list(burn.get("rows")) if isinstance(row, dict)]
    if not rows:
        errors.append("burn-down has no rows")
    invalid = [as_text(row.get("id")) for row in rows if as_text(row.get("classification")) not in ALLOWED_CLASSIFICATIONS]
    unclassified = [as_text(row.get("id")) for row in rows if not as_text(row.get("classification"))]
    if invalid:
        errors.append(f"invalid classifications: {len(invalid)}")
    if unclassified:
        errors.append(f"unclassified rows: {len(unclassified)}")
    if burn.get("status") != "PASS":
        errors.append(f"burn-down status is not PASS: {burn.get('status')}")
    if errors:
        errors.append(f"path={path}")
    return errors


def iter_run_jsons(run_root: Path) -> list[Path]:
    if not run_root.is_dir():
        return []
    return sorted(
        run_root.glob("*/actor-parity-burndown-run.json"),
        key=lambda path: path.stat().st_mtime,
    )


def select_run_results(run_paths: list[Path], burn_path: Path, include_all_runs: bool) -> dict[str, dict[str, Any]]:
    selected: dict[str, dict[str, Any]] = {}
    burn_norm = norm_path(str(burn_path))
    for run_path in run_paths:
        try:
            run = load_json(run_path)
        except Exception:
            continue
        if run.get("schema") != "nikami-fnv-actor-parity-burndown-run-v1":
            continue
        run_burn = norm_path(as_text(run.get("burnDownJson")))
        if not include_all_runs and run_burn != burn_norm:
            continue
        for result in as_list(run.get("results")):
            if not isinstance(result, dict):
                continue
            row_id = as_text(result.get("id"))
            if not row_id:
                continue
            item = dict(result)
            item["_runPath"] = str(run_path)
            item["_runMtime"] = run_path.stat().st_mtime
            selected[row_id] = item
    return selected


def classify_result(row: dict[str, Any], result: dict[str, Any] | None) -> dict[str, Any]:
    source_class = as_text(row.get("classification"))
    runtime_required = source_class in RUNTIME_REQUIRED
    if result is None:
        if runtime_required:
            effective = "loaded-pending-runtime"
            audit_status = "UNRUN"
            proof_mode = "not-run"
            missing = ["runtime-row-run"]
        else:
            effective = source_class
            audit_status = "ACCOUNTED"
            proof_mode = "source-classification"
            missing = []
        return {
            "runMatched": False,
            "runStatus": "UNRUN",
            "rowAuditStatus": audit_status,
            "rowGateProofMode": proof_mode,
            "effectiveClassification": effective,
            "missingEvidenceKinds": missing,
            "observedEvidenceKinds": [],
            "runPath": "",
            "viewerJson": "",
            "viewerIndex": "",
            "error": "",
        }

    audit = result.get("rowGateAudit") if isinstance(result.get("rowGateAudit"), dict) else {}
    run_status = as_text(result.get("status")) or "UNKNOWN"
    audit_status = as_text(audit.get("status")) or run_status
    effective = (
        as_text(result.get("rowRuntimeClassification"))
        or as_text(audit.get("classification"))
        or source_class
    )
    if run_status == "FAIL" or audit_status == "FAIL":
        effective = "known-blocked"
    elif audit_status == "PASS":
        effective = "runtime-supported"
    elif audit_status in {"PENDING", "DRY-RUN", "NOT-RUN"} and runtime_required:
        effective = "loaded-pending-runtime"
    if effective not in ALLOWED_CLASSIFICATIONS:
        effective = "known-blocked"

    return {
        "runMatched": True,
        "runStatus": run_status,
        "rowAuditStatus": audit_status,
        "rowGateProofMode": as_text(result.get("rowGateProofMode")) or "unknown",
        "effectiveClassification": effective,
        "missingEvidenceKinds": [as_text(item) for item in as_list(audit.get("missingEvidenceKinds")) if as_text(item)],
        "observedEvidenceKinds": [as_text(item) for item in as_list(audit.get("observedEvidenceKinds")) if as_text(item)],
        "runPath": as_text(result.get("_runPath")),
        "viewerJson": as_text(result.get("viewerJson")),
        "viewerIndex": as_text(result.get("viewerIndex")),
        "error": as_text(result.get("error")) or "; ".join(as_text(item) for item in as_list(audit.get("errors")) if as_text(item)),
    }


def build_report(burn_path: Path, run_root: Path, include_all_runs: bool) -> dict[str, Any]:
    burn = load_json(burn_path)
    validation_errors = validate_burndown(burn, burn_path)
    rows = [row for row in as_list(burn.get("rows")) if isinstance(row, dict)]
    run_paths = iter_run_jsons(run_root)
    latest_results = select_run_results(run_paths, burn_path, include_all_runs)

    report_rows: list[dict[str, Any]] = []
    effective_counts: Counter[str] = Counter()
    source_counts: Counter[str] = Counter()
    audit_counts: Counter[str] = Counter()
    phase_counts: Counter[str] = Counter()
    gate_counts: Counter[str] = Counter()
    missing_counts: Counter[str] = Counter()

    for row in rows:
        row_id = as_text(row.get("id"))
        result = latest_results.get(row_id)
        classified = classify_result(row, result)
        source_class = as_text(row.get("classification"))
        source_counts[source_class] += 1
        effective_counts[classified["effectiveClassification"]] += 1
        audit_counts[classified["rowAuditStatus"]] += 1
        phase_counts[as_text(row.get("phase"))] += 1
        gate_counts[as_text(row.get("gate"))] += 1
        for missing in classified["missingEvidenceKinds"]:
            missing_counts[missing] += 1
        report_rows.append(
            {
                "id": row_id,
                "entryId": as_text(row.get("entryId")),
                "priority": as_text(row.get("priority")),
                "actorKind": as_text(row.get("actorKind")),
                "target": as_text(row.get("target")),
                "runtimeTarget": as_text(row.get("runtimeTarget")),
                "phase": as_text(row.get("phase")),
                "gate": as_text(row.get("gate")),
                "runtimeStates": [as_text(item) for item in as_list(row.get("runtimeStates")) if as_text(item)],
                "sourceClassification": source_class,
                "effectiveClassification": classified["effectiveClassification"],
                "firstFailingGate": as_text(row.get("firstFailingGate")),
                "runMatched": classified["runMatched"],
                "runStatus": classified["runStatus"],
                "rowAuditStatus": classified["rowAuditStatus"],
                "rowGateProofMode": classified["rowGateProofMode"],
                "missingEvidenceKinds": classified["missingEvidenceKinds"],
                "observedEvidenceKinds": classified["observedEvidenceKinds"],
                "runPath": classified["runPath"],
                "viewerJson": classified["viewerJson"],
                "viewerIndex": classified["viewerIndex"],
                "error": classified["error"],
                "payloadPolicy": "generated row/audit metadata and proof links only; no retail payload bytes",
            }
        )

    runtime_required = [row for row in report_rows if row["sourceClassification"] in RUNTIME_REQUIRED]
    unrun_runtime = [row for row in runtime_required if not row["runMatched"]]
    failed_rows = [row for row in report_rows if row["runStatus"] == "FAIL" or row["rowAuditStatus"] == "FAIL"]
    supported_rows = [row for row in report_rows if row["effectiveClassification"] == "runtime-supported"]
    pending_rows = [row for row in report_rows if row["effectiveClassification"] == "loaded-pending-runtime"]

    runtime_status = "PASS"
    if failed_rows:
        runtime_status = "FAIL"
    elif pending_rows or unrun_runtime:
        runtime_status = "PARTIAL"

    return {
        "schema": "nikami-fnv-actor-row-audit-report-v1",
        "status": "FAIL" if validation_errors else "PASS",
        "runtimeStatus": runtime_status,
        "createdAt": datetime.now().isoformat(timespec="seconds"),
        "burnDownJson": str(burn_path),
        "runRoot": str(run_root),
        "includeAllRuns": include_all_runs,
        "payloadPolicy": "generated row/audit metadata and proof links only; no retail payload bytes",
        "validationErrors": validation_errors,
        "counts": {
            "burnDownRows": len(rows),
            "runtimeRequiredRows": len(runtime_required),
            "runMatchedRows": sum(1 for row in report_rows if row["runMatched"]),
            "unrunRuntimeRequiredRows": len(unrun_runtime),
            "runtimeSupportedRows": len(supported_rows),
            "loadedPendingRuntimeRows": len(pending_rows),
            "failedRows": len(failed_rows),
            "runJsonFilesScanned": len(run_paths),
        },
        "sourceClassificationCounts": dict(sorted(source_counts.items())),
        "effectiveClassificationCounts": dict(sorted(effective_counts.items())),
        "rowAuditStatusCounts": dict(sorted(audit_counts.items())),
        "phaseCounts": dict(sorted(phase_counts.items())),
        "gateCounts": dict(sorted(gate_counts.items())),
        "missingEvidenceCounts": dict(sorted(missing_counts.items())),
        "rows": report_rows,
    }


def write_csv_report(path: Path, rows: list[dict[str, Any]]) -> None:
    fieldnames = [
        "id",
        "priority",
        "actorKind",
        "runtimeTarget",
        "phase",
        "gate",
        "sourceClassification",
        "effectiveClassification",
        "runMatched",
        "runStatus",
        "rowAuditStatus",
        "rowGateProofMode",
        "missingEvidenceKinds",
        "runPath",
        "viewerJson",
        "error",
    ]
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        for row in rows:
            writable = dict(row)
            writable["missingEvidenceKinds"] = ",".join(row.get("missingEvidenceKinds", []))
            writer.writerow({field: writable.get(field, "") for field in fieldnames})


def write_markdown(path: Path, report: dict[str, Any]) -> None:
    counts = report["counts"]
    lines = [
        "# FNV Actor Row Audit Report",
        "",
        f"Status: **{report['status']}**",
        f"Runtime status: **{report['runtimeStatus']}**",
        f"Burn-down: `{report['burnDownJson']}`",
        f"Run root: `{report['runRoot']}`",
        f"Policy: {report['payloadPolicy']}",
        "",
        "## Counts",
        "",
        f"- Burn-down rows: {counts['burnDownRows']}",
        f"- Runtime-required rows: {counts['runtimeRequiredRows']}",
        f"- Run matched rows: {counts['runMatchedRows']}",
        f"- Unrun runtime-required rows: {counts['unrunRuntimeRequiredRows']}",
        f"- Runtime-supported rows: {counts['runtimeSupportedRows']}",
        f"- Loaded-pending-runtime rows: {counts['loadedPendingRuntimeRows']}",
        f"- Failed rows: {counts['failedRows']}",
        "",
        "## Missing Evidence",
        "",
    ]
    if report["missingEvidenceCounts"]:
        for key, value in sorted(report["missingEvidenceCounts"].items(), key=lambda item: (-item[1], item[0])):
            lines.append(f"- `{key}`: {value}")
    else:
        lines.append("- none")
    lines.extend(["", "## Rows", "", "| Runtime | Audit | Target | Phase | Gate | Missing Evidence | Run |", "|---|---|---|---|---|---|---|"])
    for row in report["rows"][:500]:
        missing = ",".join(row.get("missingEvidenceKinds", []))
        lines.append(
            f"| {row['effectiveClassification']} | {row['rowAuditStatus']} | `{row['runtimeTarget']}` | {row['phase']} | {row['gate']} | `{missing}` | `{row['runStatus']}` |"
        )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def write_html(path: Path, report: dict[str, Any]) -> None:
    def esc(value: Any) -> str:
        return html.escape(as_text(value))

    row_html = []
    for row in report["rows"][:1000]:
        missing = ",".join(row.get("missingEvidenceKinds", []))
        run_link = esc(row.get("runPath", ""))
        row_html.append(
            "<tr>"
            f"<td>{esc(row['effectiveClassification'])}</td>"
            f"<td>{esc(row['rowAuditStatus'])}</td>"
            f"<td><code>{esc(row['runtimeTarget'])}</code></td>"
            f"<td>{esc(row['phase'])}</td>"
            f"<td>{esc(row['gate'])}</td>"
            f"<td><code>{esc(missing)}</code></td>"
            f"<td><code>{run_link}</code></td>"
            "</tr>"
        )
    body = f"""<!doctype html>
<html lang=\"en\">
<head>
<meta charset=\"utf-8\">
<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">
<title>FNV Actor Row Audit Report</title>
<style>
body{{margin:0;background:#111316;color:#eceff3;font:13px/1.4 Segoe UI,Arial,sans-serif}}
main{{padding:16px;display:grid;gap:14px}}
.panel{{border:1px solid #363c45;border-radius:6px;background:#1a1d22;padding:12px}}
.meta{{display:flex;gap:10px;flex-wrap:wrap}}
.pill{{border:1px solid #363c45;border-radius:999px;padding:4px 8px;background:#20242a}}
table{{border-collapse:collapse;width:100%}}
td,th{{border-bottom:1px solid #363c45;padding:7px;text-align:left;vertical-align:top}}
th{{color:#aeb6c2}}
code{{color:#d8e6ff;overflow-wrap:anywhere}}
</style>
</head>
<body><main>
<h1>FNV Actor Row Audit Report</h1>
<section class=\"panel meta\">
<span class=\"pill\">Status: {esc(report['status'])}</span>
<span class=\"pill\">Runtime: {esc(report['runtimeStatus'])}</span>
<span class=\"pill\">Rows: {esc(report['counts']['burnDownRows'])}</span>
<span class=\"pill\">Supported: {esc(report['counts']['runtimeSupportedRows'])}</span>
<span class=\"pill\">Pending: {esc(report['counts']['loadedPendingRuntimeRows'])}</span>
<span class=\"pill\">Unrun: {esc(report['counts']['unrunRuntimeRequiredRows'])}</span>
<span class=\"pill\">Failed: {esc(report['counts']['failedRows'])}</span>
</section>
<section class=\"panel\"><div>Burn-down: <code>{esc(report['burnDownJson'])}</code></div><div>Policy: {esc(report['payloadPolicy'])}</div></section>
<section class=\"panel\"><table>
<thead><tr><th>Runtime Class</th><th>Audit</th><th>Target</th><th>Phase</th><th>Gate</th><th>Missing Evidence</th><th>Run</th></tr></thead>
<tbody>{''.join(row_html)}</tbody>
</table></section>
</main></body></html>
"""
    path.write_text(body, encoding="utf-8")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--proof-root", type=Path, required=True)
    parser.add_argument("--burn-down-json", type=Path, default=None)
    parser.add_argument("--run-root", type=Path, default=None)
    parser.add_argument("--out-dir", type=Path, default=None)
    parser.add_argument("--include-all-runs", action="store_true")
    parser.add_argument("--require-runtime-pass", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    proof_root = args.proof_root
    burn_path = args.burn_down_json or latest_burndown(proof_root)
    run_root = args.run_root or (proof_root / "fnv-actor-parity-burndown-run")
    out_dir = args.out_dir or latest_out_dir(proof_root)
    out_dir.mkdir(parents=True, exist_ok=True)

    report = build_report(burn_path, run_root, args.include_all_runs)
    report["artifacts"] = {
        "json": str(out_dir / "actor-row-audit-report.json"),
        "markdown": str(out_dir / "actor-row-audit-report.md"),
        "html": str(out_dir / "actor-row-audit-report.html"),
        "csv": str(out_dir / "actor-row-audit-report.csv"),
    }

    write_json(out_dir / "actor-row-audit-report.json", report)
    write_markdown(out_dir / "actor-row-audit-report.md", report)
    write_html(out_dir / "actor-row-audit-report.html", report)
    write_csv_report(out_dir / "actor-row-audit-report.csv", report["rows"])

    print(
        f"{report['status']} runtime={report['runtimeStatus']} "
        f"rows={report['counts']['burnDownRows']} supported={report['counts']['runtimeSupportedRows']} "
        f"pending={report['counts']['loadedPendingRuntimeRows']} failed={report['counts']['failedRows']} "
        f"unrun={report['counts']['unrunRuntimeRequiredRows']} {out_dir / 'actor-row-audit-report.json'}"
    )
    if report["status"] != "PASS":
        return 1
    if args.require_runtime_pass and report["runtimeStatus"] != "PASS":
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
