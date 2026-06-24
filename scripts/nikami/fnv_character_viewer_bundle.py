#!/usr/bin/env python3
"""Generate a standalone FNV character/creature viewer from runtime proof cases."""

from __future__ import annotations

import argparse
import html
import json
import os
import re
from pathlib import Path
from typing import Any


PHASE_ORDER = [
    "body",
    "head",
    "face",
    "hair",
    "equipment",
    "weapon",
    "headgear",
    "talk",
    "creature-model",
    "creature-body",
    "creature-animation",
    "creature-full",
    "full",
]
ANGLE_ORDER = ["front", "front-left", "front-right"]
LAYER_ORDER = ["all", "body-skin", "head-skin", "face-organs", "hair-beard", "equipment-body", "weapon", "headgear"]
CREATURE_LAYER_ORDER = ["all", "creature-model", "creature-body", "creature-animation", "creature-bounds", "creature-kf"]
SKIN_NEEDLES = (
    "skin",
    "FaceGen",
    "EGT",
    "EGM",
    "normal map",
    "subsurface",
    "material tint",
    "diffuse",
    "detail overlay",
    "complexion",
)
HAIR_NEEDLES = ("hair", "beard", "brow", "headgear", "hat", "covered")
ANIMATION_NEEDLES = ("animation", "idle", ".kf", "mouth driver", "dialogue", "TRI morph", "weapon")


def layer_label(layer: str) -> str:
    return {
        "all": "All",
        "body-skin": "Body Skin",
        "head-skin": "Head Skin",
        "face-organs": "Eyes Mouth Teeth",
        "hair-beard": "Hair Beard",
        "equipment-body": "Body Equipment",
        "weapon": "Weapon",
        "headgear": "Headgear",
        "creature-model": "Creature Model",
        "creature-body": "Creature Body",
        "creature-animation": "Creature Animation",
        "creature-bounds": "Creature Bounds",
        "creature-kf": "Creature KF",
    }.get(layer, layer)


def compact_line(line: str) -> str:
    return re.sub(r"\s+", " ", line.strip())


def read_json(path: Path, default: Any) -> Any:
    if not path.is_file():
        return default
    return json.loads(path.read_text(encoding="utf-8-sig", errors="replace"))


def read_lines(path: Path) -> list[str]:
    if not path.is_file():
        return []
    return path.read_text(encoding="utf-8", errors="replace").splitlines()


def as_list(value: Any) -> list[Any]:
    if value is None:
        return []
    if isinstance(value, list):
        return value
    return [value]


def rel_path(path: Path, base: Path) -> str:
    try:
        return os.path.relpath(path, base).replace("\\", "/")
    except ValueError:
        return str(path).replace("\\", "/")


def unique_ordered(values: list[str]) -> list[str]:
    seen: set[str] = set()
    result: list[str] = []
    for value in values:
        if value in seen:
            continue
        seen.add(value)
        result.append(value)
    return result


def unique_dicts(values: list[dict[str, Any]], key: str) -> list[dict[str, Any]]:
    seen: set[str] = set()
    result: list[dict[str, Any]] = []
    for value in values:
        marker = str(value.get(key, ""))
        if marker in seen:
            continue
        seen.add(marker)
        result.append(value)
    return result


def shell_quote(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def phase_for_category(category: str) -> str:
    return {
        "body-skin": "body",
        "head-skin": "head",
        "face-organs": "face",
        "hair-beard": "hair",
        "equipment-body": "equipment",
        "weapon": "weapon",
        "headgear": "headgear",
        "creature-model": "creature-model",
        "creature-body": "creature-body",
        "creature-animation": "creature-animation",
    }.get(category, category or "full")


def selector_arg(name: str, value: str) -> str:
    return f" -{name} {shell_quote(value)}" if value else ""


def viewer_command(
    actor: str,
    actor_kind: str,
    phases: str,
    *,
    parts: str = "",
    part_models: str = "",
    prop_slots: str = "",
    prop_models: str = "",
    animation_group: str = "",
    dialogue_mode: str = "",
) -> str:
    command = (
        "powershell -NoProfile -ExecutionPolicy Bypass -File scripts/nikami/run-fnv-character-viewer.ps1 "
        f"-Targets {shell_quote(actor or ('Creature' if actor_kind == 'creature' else 'GSEasyPete'))} "
        f"-ActorKind {actor_kind} "
    )
    if actor_kind == "creature":
        command += "-CreatureDiagnostics "
    command += f"-Phases {shell_quote(phases)}"
    command += selector_arg("ActorKitParts", parts)
    command += selector_arg("ActorKitPartModels", part_models)
    command += selector_arg("ActorKitPropSlots", prop_slots)
    command += selector_arg("ActorKitPropModels", prop_models)
    command += selector_arg("ActorKitAnimationGroup", animation_group)
    command += selector_arg("ActorKitDialogueMode", dialogue_mode)
    command += " -OpenViewer"
    return command


def gate_controls(cases: list[dict[str, Any]], actor: str, actor_kind: str) -> dict[str, Any]:
    gates: list[dict[str, Any]] = []
    for case in cases:
        gates.extend([gate for gate in as_list(case.get("gates")) if isinstance(gate, dict)])

    if actor_kind == "creature":
        evidence = [
            item
            for case in cases
            for item in as_list(case.get("creatureEvidence"))
            if isinstance(item, dict)
        ]

        def creature_models(kind: str) -> list[str]:
            return unique_ordered(
                [
                    str(item.get("model") or item.get("path") or "")
                    for item in evidence
                    if str(item.get("kind", "")).startswith(kind) and str(item.get("model") or item.get("path") or "")
                ]
            )

        part_toggles = [
            {
                "id": layer,
                "label": layer_label(layer),
                "category": layer,
                "defaultEnabled": True,
                "models": creature_models(layer),
                "modelCount": len(creature_models(layer)),
                "runtimeSelector": {
                    "parts": layer,
                    "phase": phase_for_category(layer),
                    "command": viewer_command(
                        actor, "creature", phase_for_category(layer), parts=layer
                    ),
                },
            }
            for layer in CREATURE_LAYER_ORDER
            if layer != "all"
        ]
        phase_controls = [
            {
                "id": phase,
                "label": layer_label(phase),
                "phase": phase,
                "controlType": "assembly-phase",
                "dialogueMouthProof": False,
                "command": viewer_command(actor, "creature", phase),
            }
            for phase in ["creature-model", "creature-body", "creature-animation", "creature-full"]
        ]
        animation_proofs = [
            {
                "id": group,
                "label": group.title(),
                "phase": "creature-animation",
                "controlType": "animation-proof",
                "animationGroup": group,
                "dialogueMouthProof": False,
                "command": viewer_command(actor, "creature", "creature-animation", animation_group=group),
            }
            for group in ["idle", "walkforward", "runforward", "attackleft", "attackright", "death"]
        ]
        return {
            "partToggles": part_toggles,
            "propSlots": [
                {
                    "id": "variant-model",
                    "category": "creature-model",
                    "label": "Creature Model",
                    "allowAll": True,
                    "runtimeSelector": {
                        "parts": "creature-body",
                        "phase": "creature-body",
                        "command": viewer_command(
                            actor, "creature", "creature-body", parts="creature-body"
                        ),
                    },
                    "options": unique_dicts(
                        [
                            {
                                "id": f"creature-model:{str(item.get('model') or item.get('path') or '')}",
                                "model": str(item.get("model") or item.get("path") or ""),
                                "classification": str(item.get("kind", "")),
                                "sourcePhase": str(item.get("timestamp", "")),
                                "runtimeSelector": {
                                    "parts": "creature-body",
                                    "partModels": str(item.get("model") or item.get("path") or ""),
                                    "phase": "creature-body",
                                    "command": viewer_command(
                                        actor,
                                        "creature",
                                        "creature-body",
                                        parts="creature-body",
                                        part_models=str(item.get("model") or item.get("path") or ""),
                                    ),
                                },
                            }
                            for item in evidence
                            if str(item.get("model") or item.get("path") or "")
                        ],
                        "model",
                    ),
                }
            ],
            "animationControls": phase_controls + animation_proofs,
            "dialogueControls": [],
            "captureAngles": [{"id": angle, "label": angle} for angle in ANGLE_ORDER],
            "botCommands": [
                {
                    "id": "creature-ladder",
                    "label": "Creature Ladder",
                    "command": viewer_command(actor, "creature", "creature-model,creature-body,creature-animation,creature-full"),
                },
                {
                    "id": "creature-animation",
                    "label": "Creature Animation",
                    "command": viewer_command(actor, "creature", "creature-animation"),
                },
            ],
        }

    part_toggles: list[dict[str, Any]] = []
    for layer in LAYER_ORDER:
        if layer == "all":
            continue
        models = unique_ordered(
            [
                str(gate.get("model", ""))
                for gate in gates
                if gate.get("category") == layer and str(gate.get("model", "")) not in {"", "<none>"}
            ]
        )
        part_toggles.append(
            {
                "id": layer,
                "label": layer_label(layer),
                "category": layer,
                "defaultEnabled": True,
                "models": models,
                "modelCount": len(models),
                "runtimeSelector": {
                    "parts": layer,
                    "phase": phase_for_category(layer),
                    "command": viewer_command(actor, "npc", phase_for_category(layer), parts=layer),
                },
            }
        )

    def slot(name: str, category: str) -> dict[str, Any]:
        options = [
            {
                "id": f"{category}:{str(gate.get('model', ''))}",
                "model": str(gate.get("model", "")),
                "classification": str(gate.get("classification", "")),
                "sourcePhase": str(gate.get("phase", "")),
                "runtimeSelector": {
                    "parts": category,
                    "propSlots": category,
                    "propModels": str(gate.get("model", "")),
                    "phase": phase_for_category(category),
                    "command": viewer_command(
                        actor,
                        "npc",
                        phase_for_category(category),
                        parts=category,
                        prop_slots=category,
                        prop_models=str(gate.get("model", "")),
                    ),
                },
            }
            for gate in gates
            if gate.get("category") == category and str(gate.get("model", "")) not in {"", "<none>"}
        ]
        return {
            "id": name,
            "category": category,
            "label": layer_label(category),
            "allowAll": True,
            "options": unique_dicts(options, "model"),
            "runtimeSelector": {
                "parts": category,
                "propSlots": category,
                "phase": phase_for_category(category),
                "command": viewer_command(
                    actor,
                    "npc",
                    phase_for_category(category),
                    parts=category,
                    prop_slots=category,
                ),
            },
        }

    phase_controls = [
        {
            "id": phase,
            "label": layer_label(phase) if phase in LAYER_ORDER else phase.title(),
            "phase": phase,
            "dialogueMouthProof": phase == "talk",
            "command": viewer_command(actor, "npc", phase),
        }
        for phase in PHASE_ORDER
        if phase != "full"
    ]
    animation_proofs = [
        {
            "id": f"anim-{group}",
            "label": label,
            "phase": phase,
            "controlType": "animation-proof",
            "animationGroup": group,
            "dialogueMouthProof": False,
            "command": viewer_command(actor, "npc", phase, animation_group=group),
        }
        for group, label, phase in [
            ("idle", "Idle", "hair"),
            ("walkforward", "Walk Forward", "hair"),
            ("runforward", "Run Forward", "hair"),
            ("mtidle", "mT Idle", "hair"),
        ]
    ]
    dialogue_controls = [
        {
            "id": "talk-mouth-open",
            "label": "Talk Mouth",
            "phase": "talk",
            "controlType": "dialogue-proof",
            "dialogueMouthProof": True,
            "dialogueMode": "mouth-open",
            "command": viewer_command(actor, "npc", "talk", dialogue_mode="mouth-open"),
        },
        {
            "id": "talk-mouth-pose",
            "label": "Talk Pose",
            "phase": "talk",
            "controlType": "dialogue-proof",
            "dialogueMouthProof": True,
            "dialogueMode": "mouth-open-pose",
            "command": viewer_command(actor, "npc", "talk", dialogue_mode="mouth-open-pose"),
        },
    ]

    return {
        "partToggles": part_toggles,
        "propSlots": [
            slot("body-equipment", "equipment-body"),
            slot("weapon", "weapon"),
            slot("headgear", "headgear"),
        ],
        "animationControls": [
            control for control in phase_controls if control["phase"] in {"body", "head", "face", "hair", "equipment", "weapon", "headgear"}
        ]
        + animation_proofs,
        "dialogueControls": dialogue_controls,
        "captureAngles": [{"id": angle, "label": angle} for angle in ANGLE_ORDER],
        "botCommands": [
            {
                "id": "full-ladder",
                "label": "Full Ladder",
                "command": viewer_command(actor, "npc", "body,head,face,hair,equipment,weapon,headgear,talk"),
            },
            {
                "id": "headgear-only",
                "label": "Headgear Only",
                "command": viewer_command(actor, "npc", "headgear", parts="headgear", prop_slots="headgear"),
            },
            {
                "id": "talk-only",
                "label": "Talk Only",
                "command": viewer_command(actor, "npc", "talk", dialogue_mode="mouth-open"),
            },
            {
                "id": "idle-proof",
                "label": "Idle Proof",
                "command": viewer_command(actor, "npc", "hair", animation_group="idle"),
            },
        ],
    }


def line_matches_actor(line: str, patterns: list[str]) -> bool:
    if not patterns:
        return True
    return any(pattern and pattern in line for pattern in patterns)


def evidence_lines(lines: list[str], patterns: list[str], needles: tuple[str, ...], limit: int = 240) -> list[str]:
    result: list[str] = []
    for line in lines:
        if not any(needle.lower() in line.lower() for needle in needles):
            continue
        if not line_matches_actor(line, patterns):
            continue
        result.append(compact_line(line))
        if len(result) >= limit:
            break
    return result


def normalize_phase(phase: str) -> str:
    lowered = (phase or "full").strip().lower()
    return lowered or "full"


def sort_phases(phases: list[str]) -> list[str]:
    order = {name: index for index, name in enumerate(PHASE_ORDER)}
    return sorted(unique_ordered(phases), key=lambda value: (order.get(value, 999), value))


def sort_angles(angles: list[str]) -> list[str]:
    order = {name: index for index, name in enumerate(ANGLE_ORDER)}
    return sorted(unique_ordered(angles), key=lambda value: (order.get(value, 999), value))


def first_image(case_dir: Path, names: list[str]) -> str:
    for name in names:
        if name.lower().endswith(".png") and (case_dir / name).is_file():
            return name
    pngs = sorted(path.name for path in case_dir.glob("*.png"))
    return pngs[0] if pngs else ""


def screenshot_names(value: Any) -> list[str]:
    names: list[str] = []
    for item in as_list(value):
        name = ""
        if isinstance(item, str):
            name = item
        elif isinstance(item, dict):
            name = str(item.get("name") or item.get("path") or "")
        else:
            continue
        name = name.replace("\\", "/").split("/")[-1].strip()
        if not name or name == "{}":
            continue
        names.append(name)
    return names


def load_suite(suite_dir: Path) -> dict[str, Any]:
    suite_json = suite_dir / "character-builder-suite.json"
    raw_results = as_list(read_json(suite_json, []))
    cases: list[dict[str, Any]] = []
    actor = ""
    actor_kind = "npc"
    phases: list[str] = []
    angles: list[str] = []

    for raw in raw_results:
        if not isinstance(raw, dict):
            continue
        case_dir = Path(str(raw.get("caseDir", "")))
        if not case_dir.is_absolute():
            case_dir = suite_dir / case_dir
        report = read_json(case_dir / "character-builder-report.json", {})
        patterns = as_list(report.get("actorPatterns"))
        if not actor:
            actor = str(report.get("actor") or "")
        report_actor_kind = str(report.get("actorKind") or "").lower()
        if report_actor_kind in {"npc", "creature"}:
            actor_kind = report_actor_kind
        phase = normalize_phase(str(raw.get("phase") or report.get("phase") or "full"))
        angle = str(raw.get("angle") or "")
        phases.append(phase)
        angles.append(angle)
        log_lines = read_lines(case_dir / "openmw.log")
        screenshots = sorted(unique_ordered(screenshot_names(raw.get("screenshots")) + [p.name for p in case_dir.glob("*.png")]))
        image = first_image(case_dir, screenshots)
        merged_failures = unique_ordered(
            [str(item) for item in as_list(raw.get("failures"))]
            + [str(item) for item in as_list(report.get("failures"))]
        )
        case_data: dict[str, Any] = {
            "case": str(raw.get("case") or f"{phase}_{angle}"),
            "phase": phase,
            "angle": angle,
            "runtimeGateStatus": str(raw.get("runtimeGateStatus") or "MISSING"),
            "runtimeGateError": str(raw.get("runtimeGateError") or ""),
            "reportStatus": str(raw.get("reportStatus") or report.get("status") or "MISSING"),
            "failures": merged_failures,
            "proofDir": str(raw.get("proofDir") or report.get("proofDir") or ""),
            "caseDir": str(case_dir),
            "caseDirRelative": rel_path(case_dir, suite_dir),
            "bootstrap": raw.get("bootstrap") or {},
            "actorStage": raw.get("actorStage") or {},
            "actorCamera": raw.get("actorCamera") or {},
            "actorKitSelection": raw.get("actorKitSelection") or {},
            "openmwLog": rel_path(case_dir / "openmw.log", suite_dir),
            "reportJson": rel_path(case_dir / "character-builder-report.json", suite_dir),
            "reportMarkdown": rel_path(case_dir / "character-builder-report.md", suite_dir),
            "screenshots": [{"name": name, "path": rel_path(case_dir / name, suite_dir)} for name in screenshots],
            "mainImage": rel_path(case_dir / image, suite_dir) if image else "",
            "counts": {
                "gates": len(as_list(report.get("gates"))),
                "attachmentBounds": len(as_list(report.get("attachmentBounds"))),
                "runtimePartAudits": len(as_list(report.get("runtimePartAudits"))),
                "runtimeAuditSummary": len(as_list(report.get("runtimeAuditSummary"))),
                "runtimePartTimelines": len(as_list(report.get("runtimePartTimelines"))),
                "faceDrawables": len(as_list(report.get("faceDrawables"))),
                "morphLines": len(as_list(report.get("morphLines"))),
                "weaponLines": len(as_list(report.get("weaponLines"))),
                "actorMatches": len(as_list(report.get("actorMatches"))),
                "animationSources": len(as_list(report.get("animationSources"))),
                "animationRequests": len(as_list(report.get("animationRequests"))),
                "animationPlayback": len(as_list(report.get("animationPlayback"))),
                "creatureEvidence": len(as_list(report.get("creatureEvidence"))),
            },
            "actorKind": str(report.get("actorKind") or actor_kind),
            "actorPatterns": patterns,
            "actorMatches": as_list(report.get("actorMatches")),
            "gates": as_list(report.get("gates")),
            "attachmentBounds": as_list(report.get("attachmentBounds")),
            "runtimePartAudits": as_list(report.get("runtimePartAudits")),
            "runtimeAuditSummary": as_list(report.get("runtimeAuditSummary")),
            "runtimePartTimelines": as_list(report.get("runtimePartTimelines")),
            "faceDrawables": as_list(report.get("faceDrawables")),
            "morphLines": as_list(report.get("morphLines")),
            "weaponLines": as_list(report.get("weaponLines")),
            "animationSources": as_list(report.get("animationSources")),
            "animationRequests": as_list(report.get("animationRequests")),
            "animationPlayback": as_list(report.get("animationPlayback")),
            "animationBlockers": as_list(report.get("animationBlockers")),
            "creatureEvidence": as_list(report.get("creatureEvidence")),
            "creatureLines": as_list(report.get("creatureLines")),
            "skinLines": evidence_lines(log_lines, patterns, SKIN_NEEDLES),
            "hairLines": evidence_lines(log_lines, patterns, HAIR_NEEDLES),
            "animationLines": evidence_lines(log_lines, patterns, ANIMATION_NEEDLES),
        }
        cases.append(case_data)

    overall = "PASS"
    if not cases:
        overall = "MISSING"
    elif any(case["runtimeGateStatus"] != "PASS" or case["reportStatus"] != "PASS" for case in cases):
        overall = "FAIL"

    return {
        "schema": "nikami-fnv-character-viewer-v2",
        "schemaMarkers": ["viewer-controls-v2", "actor-profile-v1", "creature-isolation-v1", "part-timeline-v1"],
        "actorProfile": {
            "kind": actor_kind,
            "recordType": "CREA" if actor_kind == "creature" else "NPC_",
            "supportsEquipmentSlots": actor_kind == "npc",
            "supportsDialogueMouthProof": actor_kind == "npc",
            "supportsNpcFaceDrawables": actor_kind == "npc",
            "supportsCreatureDiagnostics": actor_kind == "creature",
        },
        "actor": actor,
        "suiteDir": str(suite_dir),
        "overallStatus": overall,
        "phases": sort_phases(phases),
        "angles": sort_angles(angles),
        "layers": CREATURE_LAYER_ORDER if actor_kind == "creature" else LAYER_ORDER,
        "evidenceSections": (
            ["Actor Matches", "Creature Evidence", "Animation Playback", "Runtime Drift"]
            if actor_kind == "creature"
            else ["Skin Evidence", "Hair Headgear Evidence", "Animation Talk Weapon Evidence", "Face Drawables"]
        ),
        "controls": gate_controls(cases, actor, actor_kind),
        "cases": cases,
    }


def html_doc(manifest: dict[str, Any]) -> str:
    data = json.dumps(manifest, ensure_ascii=False)
    data = data.replace("</", "<\\/")
    title = f"FNV Character Viewer - {manifest.get('actor') or 'Actor'}"
    return f"""<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{html.escape(title)}</title>
<style>
:root {{
  color-scheme: dark;
  --bg: #111316;
  --panel: #1a1d22;
  --panel2: #20242a;
  --line: #363c45;
  --text: #eceff3;
  --muted: #aeb6c2;
  --bad: #ff6f61;
  --ok: #64d488;
  --warn: #e8c86a;
  --accent: #70a7ff;
}}
* {{ box-sizing: border-box; }}
body {{ margin: 0; background: var(--bg); color: var(--text); font: 13px/1.4 Segoe UI, Arial, sans-serif; }}
header {{ display: flex; align-items: center; justify-content: space-between; gap: 16px; padding: 12px 16px; border-bottom: 1px solid var(--line); background: #15181c; position: sticky; top: 0; z-index: 5; }}
h1 {{ margin: 0; font-size: 16px; font-weight: 600; }}
main {{ padding: 14px 16px 28px; display: grid; gap: 14px; }}
.toolbar, .section {{ background: var(--panel); border: 1px solid var(--line); border-radius: 6px; }}
.toolbar {{ padding: 10px; display: flex; gap: 12px; flex-wrap: wrap; align-items: center; }}
.group {{ display: flex; gap: 6px; align-items: center; flex-wrap: wrap; }}
.label {{ color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: .04em; }}
button, select {{ border: 1px solid var(--line); border-radius: 5px; background: var(--panel2); color: var(--text); padding: 6px 9px; font: inherit; }}
button.active {{ border-color: var(--accent); background: #26354d; }}
.controlGrid {{ display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }}
.controlPanel {{ background: #15181c; border: 1px solid var(--line); border-radius: 6px; padding: 9px; min-width: 0; }}
.controlPanel h3 {{ margin: 0 0 8px; font-size: 13px; color: var(--muted); font-weight: 600; }}
.toggleList {{ display: flex; flex-wrap: wrap; gap: 7px; }}
.check {{ display: inline-flex; align-items: center; gap: 5px; border: 1px solid var(--line); border-radius: 5px; padding: 5px 7px; background: var(--panel2); }}
.check input {{ margin: 0; }}
.commandList {{ display: grid; gap: 5px; }}
.command {{ font-family: Consolas, monospace; font-size: 12px; background: #101216; border: 1px solid #2b3038; border-radius: 4px; padding: 6px; white-space: pre-wrap; overflow-wrap: anywhere; }}
.status {{ border-radius: 99px; padding: 3px 8px; font-weight: 600; }}
.PASS {{ color: #07120b; background: var(--ok); }}
.FAIL {{ color: #190502; background: var(--bad); }}
.MISSING {{ color: #181102; background: var(--warn); }}
.grid3 {{ display: grid; grid-template-columns: repeat(3, minmax(0, 1fr)); gap: 10px; }}
.pane {{ background: var(--panel); border: 1px solid var(--line); border-radius: 6px; overflow: hidden; min-width: 0; }}
.paneHead {{ display: flex; justify-content: space-between; gap: 8px; padding: 8px 10px; border-bottom: 1px solid var(--line); color: var(--muted); }}
.pane img {{ display: block; width: 100%; height: auto; background: #050607; }}
.empty {{ min-height: 240px; display: grid; place-items: center; color: var(--muted); background: #0b0d10; }}
.section {{ padding: 10px; overflow: auto; }}
.section h2 {{ margin: 0 0 8px; font-size: 14px; }}
table {{ width: 100%; border-collapse: collapse; }}
th, td {{ padding: 6px 7px; border-bottom: 1px solid var(--line); text-align: left; vertical-align: top; }}
th {{ color: var(--muted); font-weight: 600; position: sticky; top: 49px; background: var(--panel); }}
code {{ color: #d8e6ff; }}
.failures {{ color: var(--bad); white-space: pre-wrap; }}
.lineList {{ display: grid; gap: 5px; max-height: 340px; overflow: auto; }}
.line {{ font-family: Consolas, monospace; font-size: 12px; color: #dce6f7; background: #101216; border: 1px solid #2b3038; border-radius: 4px; padding: 5px 6px; }}
a {{ color: #9fc2ff; }}
@media (max-width: 980px) {{ .grid3 {{ grid-template-columns: 1fr; }} header {{ align-items: flex-start; flex-direction: column; }} }}
@media (max-width: 980px) {{ .controlGrid {{ grid-template-columns: 1fr; }} }}
</style>
</head>
<body>
<header>
  <h1>{html.escape(title)}</h1>
  <div><span id="overall" class="status"></span></div>
</header>
<main>
  <div class="toolbar">
    <div class="group"><span class="label">Phase</span><span id="phaseButtons"></span></div>
    <div class="group"><span class="label">Math Angle</span><span id="angleButtons"></span></div>
    <div class="group"><span class="label">Layer</span><select id="layerSelect"></select></div>
  </div>
  <div class="section">
    <h2>Runtime Controls</h2>
    <div class="controlGrid">
      <div class="controlPanel"><h3>Part Toggles</h3><div id="partToggleHost" class="toggleList"></div></div>
      <div class="controlPanel"><h3>Prop Slots</h3><div id="propSlotHost" class="commandList"></div></div>
      <div class="controlPanel"><h3>Animation Dialogue</h3><div id="runtimeControlHost" class="commandList"></div></div>
    </div>
  </div>
  <div class="section">
    <h2>Bot Commands</h2>
    <div id="botCommandHost" class="commandList"></div>
  </div>
  <div id="cameraGrid" class="grid3"></div>
  <div class="section">
    <h2>Case Status</h2>
    <div id="caseStatus"></div>
  </div>
  <div class="section">
    <h2>Actor Matches</h2>
    <div id="actorMatchLines" class="lineList"></div>
  </div>
  <div class="section">
    <h2>Assembly Gates</h2>
    <div id="gateTable"></div>
  </div>
  <div class="section">
    <h2>Coordinates</h2>
    <div id="coordTable"></div>
  </div>
  <div class="section">
    <h2>Runtime Drift</h2>
    <div id="driftTable"></div>
  </div>
  <div class="section">
    <h2>Part Timeline</h2>
    <div id="timelineTable"></div>
  </div>
  <div class="section">
    <h2>Skin Evidence</h2>
    <div id="skinLines" class="lineList"></div>
  </div>
  <div class="section">
    <h2>Hair Headgear Evidence</h2>
    <div id="hairLines" class="lineList"></div>
  </div>
  <div class="section">
    <h2>Animation Talk Weapon Evidence</h2>
    <div id="animLines" class="lineList"></div>
  </div>
  <div class="section">
    <h2>Creature Evidence</h2>
    <div id="creatureLines" class="lineList"></div>
  </div>
  <div class="section">
    <h2>Animation Playback</h2>
    <div id="playbackLines" class="lineList"></div>
  </div>
  <div class="section">
    <h2>Face Drawables</h2>
    <div id="faceTable"></div>
  </div>
</main>
<script>
const MANIFEST = {data};
const state = {{
  phase: MANIFEST.phases[0] || "full",
  angle: MANIFEST.angles[0] || "front",
  layer: "all",
  enabledLayers: Object.fromEntries((MANIFEST.controls?.partToggles || []).map(t => [t.category, t.defaultEnabled !== false])),
  slotFilters: Object.fromEntries((MANIFEST.controls?.propSlots || []).map(s => [s.id, "all"]))
}};
function esc(value) {{
  return String(value ?? "").replace(/[&<>"']/g, c => ({{"&":"&amp;","<":"&lt;",">":"&gt;","\\"":"&quot;","'":"&#39;"}}[c]));
}}
function statusSpan(value) {{
  const v = value || "MISSING";
  return `<span class="status ${{esc(v)}}">${{esc(v)}}</span>`;
}}
function caseFor(phase, angle) {{
  return MANIFEST.cases.find(c => c.phase === phase && c.angle === angle);
}}
function selectedCase() {{
  return caseFor(state.phase, state.angle) || MANIFEST.cases.find(c => c.phase === state.phase) || MANIFEST.cases[0];
}}
function buttonRow(id, values, current, setter) {{
  const host = document.getElementById(id);
  host.innerHTML = values.map(v => `<button type="button" class="${{v === current ? "active" : ""}}" data-value="${{esc(v)}}">${{esc(v)}}</button>`).join("");
  host.querySelectorAll("button").forEach(btn => btn.addEventListener("click", () => {{ setter(btn.dataset.value); render(); }}));
}}
function table(headers, rows) {{
  if (!rows.length) return `<div class="empty">No data</div>`;
  return `<table><thead><tr>${{headers.map(h => `<th>${{esc(h)}}</th>`).join("")}}</tr></thead><tbody>${{rows.join("")}}</tbody></table>`;
}}
function partCategoryFromText(part, cls) {{
  const text = `${{part || ""}} ${{cls || ""}}`.toLowerCase();
  if (text.includes("creature-animation") || text.includes(".kf")) return "creature-animation";
  if (text.includes("creature-bounds") || text.includes("bounds")) return "creature-bounds";
  if (text.includes("creature-kf")) return "creature-kf";
  if (text.includes("creature-body") || text.includes("body nif")) return "creature-body";
  if (text.includes("creature-model") || text.includes("creature")) return "creature-model";
  if (text.includes("weapon")) return "weapon";
  if (text.includes("headgear") || text.includes("hat") || text.includes("cowboyhat")) return "headgear";
  if (text.includes("upperbody") || text.includes("lefthand") || text.includes("righthand") || text.includes("left hand") || text.includes("right hand")) return "body-skin";
  if (text.includes("mouth") || text.includes("teeth") || text.includes("tongue") || text.includes("eye") || text.includes("brow")) return "face-organs";
  if (text.includes("hair") || text.includes("beard")) return "hair-beard";
  if (text.includes("headold") || text.includes("headhuman") || text.includes("class=head")) return "head-skin";
  if (text.includes("armor")) return "equipment-body";
  return "all";
}}
function categoryEnabled(category) {{
  return category === "all" || state.enabledLayers[category] !== false;
}}
function slotAllows(category, model) {{
  const slot = (MANIFEST.controls?.propSlots || []).find(s => s.category === category);
  if (!slot) return true;
  const selected = state.slotFilters[slot.id] || "all";
  return selected === "all" || String(model || "") === selected;
}}
function filteredGates(c) {{
  const gates = c?.gates || [];
  return gates.filter(g => (state.layer === "all" || g.category === state.layer) && categoryEnabled(g.category) && slotAllows(g.category, g.model));
}}
function filterPartRows(items, textGetter, classGetter) {{
  return (items || []).filter(item => {{
    const category = item.category || partCategoryFromText(textGetter(item), classGetter(item));
    return (state.layer === "all" || category === state.layer || category === "all") && categoryEnabled(category);
  }});
}}
function renderControls() {{
  const partHost = document.getElementById("partToggleHost");
  partHost.innerHTML = (MANIFEST.controls?.partToggles || []).map(t => `<div><label class="check"><input type="checkbox" data-category="${{esc(t.category)}}" ${{state.enabledLayers[t.category] !== false ? "checked" : ""}}> ${{esc(t.label)}} <span class="label">${{esc(t.modelCount)}} </span></label>${{t.runtimeSelector?.command ? `<div class="command">${{esc(t.runtimeSelector.command)}}</div>` : ""}}</div>`).join("");
  partHost.querySelectorAll("input").forEach(input => input.addEventListener("change", () => {{
    state.enabledLayers[input.dataset.category] = input.checked;
    render();
  }}));

  const propHost = document.getElementById("propSlotHost");
  propHost.innerHTML = (MANIFEST.controls?.propSlots || []).map(slot => {{
    const options = [`<option value="all">all</option>`].concat((slot.options || []).map(o => `<option value="${{esc(o.model)}}">${{esc(o.model)}}</option>`)).join("");
    const selected = state.slotFilters[slot.id] || "all";
    const selectedOption = (slot.options || []).find(o => String(o.model || "") === selected);
    const command = selectedOption?.runtimeSelector?.command || slot.runtimeSelector?.command || "";
    return `<label><span class="label">${{esc(slot.label)}}</span><br><select data-slot="${{esc(slot.id)}}">${{options}}</select></label>${{command ? `<div class="command">${{esc(command)}}</div>` : ""}}`;
  }}).join("");
  propHost.querySelectorAll("select").forEach(select => {{
    select.value = state.slotFilters[select.dataset.slot] || "all";
    select.addEventListener("change", () => {{ state.slotFilters[select.dataset.slot] = select.value; render(); }});
  }});

  const runtime = [...(MANIFEST.controls?.animationControls || []), ...(MANIFEST.controls?.dialogueControls || [])];
  document.getElementById("runtimeControlHost").innerHTML = runtime.map(control => `<button type="button" data-phase="${{esc(control.phase)}}">${{esc(control.label)}}</button><div class="command">${{esc(control.command)}}</div>`).join("");
  document.getElementById("runtimeControlHost").querySelectorAll("button").forEach(button => button.addEventListener("click", () => {{
    state.phase = button.dataset.phase;
    render();
  }}));
  document.getElementById("botCommandHost").innerHTML = (MANIFEST.controls?.botCommands || []).map(command => `<div><span class="label">${{esc(command.label)}}</span><div class="command">${{esc(command.command)}}</div></div>`).join("");
}}
function renderImages() {{
  const host = document.getElementById("cameraGrid");
  host.innerHTML = (MANIFEST.angles || []).map(angle => {{
    const c = caseFor(state.phase, angle);
    const image = c?.mainImage || "";
    const body = image ? `<a href="${{esc(image)}}"><img src="${{esc(image)}}" alt="${{esc(c.case)}}"></a>` : `<div class="empty">No screenshot</div>`;
    const failures = (c?.failures || []).join("\\n");
    return `<div class="pane">
      <div class="paneHead"><span>${{esc(angle)}}</span><span>${{statusSpan(c?.reportStatus || "MISSING")}}</span></div>
      ${{body}}
      ${{failures ? `<div class="section failures">${{esc(failures)}}</div>` : ""}}
    </div>`;
  }}).join("");
}}
function renderStatus(c) {{
  const rows = (MANIFEST.angles || []).map(angle => {{
    const item = caseFor(state.phase, angle);
    return `<tr><td>${{esc(item?.case || angle)}}</td><td>${{statusSpan(item?.runtimeGateStatus)}}</td><td>${{statusSpan(item?.reportStatus)}}</td><td><a href="${{esc(item?.openmwLog || "#")}}">log</a></td><td><a href="${{esc(item?.reportJson || "#")}}">json</a></td></tr>`;
  }});
  document.getElementById("caseStatus").innerHTML = table(["Case", "Runtime", "Report", "Log", "JSON"], rows);
}}
function renderGates(c) {{
  const rows = filteredGates(c).map(g => `<tr><td>${{esc(g.action)}}</td><td>${{esc(g.category)}}</td><td>${{esc(g.classification)}}</td><td><code>${{esc(g.model)}}</code></td></tr>`);
  document.getElementById("gateTable").innerHTML = table(["Action", "Category", "Classification", "Model"], rows);
}}
function renderCoords(c) {{
  const bounds = filterPartRows(c?.attachmentBounds || [], b => b.model, b => b.parent);
  const audits = filterPartRows(c?.runtimePartAudits || [], a => a.part, a => a.class);
  const caseRows = [];
  if (c?.bootstrap && Object.keys(c.bootstrap).length) {{
    caseRows.push(`<tr><td>bootstrap</td><td><code>${{esc(c.bootstrap.cell || "")}}</code></td><td>player/cell</td><td>${{esc(JSON.stringify({{x:c.bootstrap.x,y:c.bootstrap.y,z:c.bootstrap.z}}))}}</td><td>${{esc(JSON.stringify({{x:c.bootstrap.rotX,y:c.bootstrap.rotY,z:c.bootstrap.rotZ,hour:c.bootstrap.hour}}))}}</td><td>source</td></tr>`);
  }}
  if (c?.actorStage && Object.keys(c.actorStage).length) {{
    caseRows.push(`<tr><td>stage</td><td><code>${{esc(MANIFEST.actor)}}</code></td><td>actor</td><td>${{esc(JSON.stringify({{x:c.actorStage.x,y:c.actorStage.y,z:c.actorStage.z}}))}}</td><td>${{esc(JSON.stringify({{x:c.actorStage.rotX,y:c.actorStage.rotY,z:c.actorStage.rotZ}}))}}</td><td>source</td></tr>`);
  }}
  if (c?.actorCamera && Object.keys(c.actorCamera).length) {{
    caseRows.push(`<tr><td>camera</td><td><code>${{esc(c.actorCamera.angle || c.angle)}}</code></td><td>actor-local</td><td>${{esc(JSON.stringify({{x:c.actorCamera.offsetX,y:c.actorCamera.offsetY,z:c.actorCamera.offsetZ}}))}}</td><td>${{esc(JSON.stringify({{targetZ:c.actorCamera.targetZ,local:c.actorCamera.localOffset}}))}}</td><td>capture</td></tr>`);
  }}
  if (c?.actorKitSelection && Object.keys(c.actorKitSelection).length) {{
    caseRows.push(`<tr><td>actor-kit</td><td><code>runtime selector</code></td><td>parts/slots</td><td>${{esc(JSON.stringify({{parts:c.actorKitSelection.parts,partModels:c.actorKitSelection.partModels}}))}}</td><td>${{esc(JSON.stringify({{propSlots:c.actorKitSelection.propSlots,propModels:c.actorKitSelection.propModels}}))}}</td><td>proof-input</td></tr>`);
  }}
  const boundRows = bounds.map(b => `<tr><td>attachment</td><td><code>${{esc(b.model)}}</code></td><td>${{esc(b.parent)}}</td><td>${{esc(JSON.stringify(b.headRel))}}</td><td>${{esc(JSON.stringify(b.extent))}}</td><td>${{esc(b.verdict)}}</td></tr>`);
  const auditRows = audits.map(a => `<tr><td>runtime</td><td><code>${{esc(a.part)}}</code></td><td>${{esc(a.class)}}</td><td>${{esc(JSON.stringify(a.relLocal))}}</td><td>${{esc(a.distance)}} / ${{esc(a.limit)}}</td><td>${{esc(a.verdict)}}</td></tr>`);
  document.getElementById("coordTable").innerHTML = table(["Kind", "Part", "Parent/Class", "Position/Rel", "Rotation/Extent", "Verdict"], caseRows.concat(boundRows).concat(auditRows));
}}
function renderDrift(c) {{
  const rows = filterPartRows(c?.runtimeAuditSummary || [], a => a.part, a => a.class).map(a => `<tr><td><code>${{esc(a.part)}}</code></td><td>${{esc(a.class)}}</td><td>${{esc(a.firstVerdict)}} -> ${{esc(a.lastVerdict)}}</td><td>${{esc(a.badCount)}} / ${{esc(a.count)}}</td><td>${{esc(a.firstSampleIndex)}} -> ${{esc(a.lastSampleIndex)}}</td><td>${{esc(a.firstAnimationTime)}} -> ${{esc(a.lastAnimationTime)}}</td><td>${{esc(a.maxDistance)}}</td><td>${{esc(JSON.stringify(a.deltaRelLocal || []))}}</td><td>${{esc(JSON.stringify(a.deltaPartInAnchorTrans || []))}}</td><td>${{esc(a.firstBadSampleIndex || "")}}</td></tr>`);
  document.getElementById("driftTable").innerHTML = table(["Part", "Class", "Verdict", "Bad", "Samples", "Anim Time", "Max Distance", "Delta Rel", "Delta Anchor", "First Bad"], rows);
}}
function renderTimeline(c) {{
  const rows = [];
  for (const timeline of filterPartRows(c?.runtimePartTimelines || [], t => t.part, t => t.class)) {{
    for (const sample of (timeline.samples || []).slice(0, 12)) {{
      rows.push(`<tr><td><code>${{esc(timeline.part)}}</code></td><td>${{esc(timeline.class)}}</td><td>${{esc(sample.sampleIndex)}}</td><td>${{esc(sample.animationGroup)}}@${{esc(sample.animationTime)}}</td><td>${{esc(sample.distance)}} / ${{esc(sample.limit)}}</td><td>${{esc(sample.verdict)}}</td><td>${{esc(JSON.stringify(sample.relLocal || []))}}</td><td>${{esc(JSON.stringify(sample.partInAnchorTrans || []))}}</td><td>${{esc(sample.anchorAngleDeg ?? "")}}</td></tr>`);
    }}
  }}
  document.getElementById("timelineTable").innerHTML = table(["Part", "Class", "Sample", "Animation", "Distance", "Verdict", "Rel Local", "Part In Anchor", "Anchor Deg"], rows);
}}
function renderLines(id, lines) {{
  document.getElementById(id).innerHTML = (lines || []).length
    ? lines.map(line => `<div class="line">${{esc(line)}}</div>`).join("")
    : `<div class="empty">No evidence lines</div>`;
}}
function renderFace(c) {{
  const rows = filterPartRows(c?.faceDrawables || [], f => f.model, f => f.drawable).map(f => `<tr><td><code>${{esc(f.model)}}</code></td><td>${{esc(f.drawable)}}</td><td>${{esc(f.texture)}}</td><td>${{esc(f.sourceVertices)}}/${{esc(f.renderVertices)}}</td><td>${{esc(JSON.stringify(f.sourceExtent))}}</td><td>${{esc(JSON.stringify(f.renderExtent))}}</td></tr>`);
  document.getElementById("faceTable").innerHTML = table(["Model", "Drawable", "Texture", "Verts", "Source Extent", "Render Extent"], rows);
}}
function render() {{
  document.getElementById("overall").className = `status ${{MANIFEST.overallStatus || "MISSING"}}`;
  document.getElementById("overall").textContent = MANIFEST.overallStatus || "MISSING";
  buttonRow("phaseButtons", MANIFEST.phases, state.phase, value => state.phase = value);
  buttonRow("angleButtons", MANIFEST.angles, state.angle, value => state.angle = value);
  const select = document.getElementById("layerSelect");
  select.innerHTML = MANIFEST.layers.map(layer => `<option value="${{esc(layer)}}">${{esc(layer)}}</option>`).join("");
  select.value = state.layer;
  select.onchange = () => {{ state.layer = select.value; render(); }};
  const c = selectedCase();
  renderControls();
  renderImages();
  renderStatus(c);
  renderLines("actorMatchLines", (c?.actorMatches || []).map(item => item.line || JSON.stringify(item)));
  renderGates(c);
  renderCoords(c);
  renderDrift(c);
  renderTimeline(c);
  renderLines("skinLines", c?.skinLines);
  renderLines("hairLines", c?.hairLines);
  renderLines("animLines", [...(c?.animationLines || []), ...(c?.morphLines || []), ...(c?.weaponLines || [])]);
  renderLines("creatureLines", (c?.creatureLines || []).concat((c?.creatureEvidence || []).map(item => item.line || JSON.stringify(item))));
  renderLines("playbackLines", (c?.animationRequests || []).map(item => item.line || JSON.stringify(item)).concat((c?.animationPlayback || []).map(item => item.line || JSON.stringify(item))).concat(c?.animationBlockers || []));
  renderFace(c);
}}
render();
</script>
</body>
</html>
"""


def actor_kit_manifest(manifest: dict[str, Any]) -> dict[str, Any]:
    cases = []
    for case in manifest.get("cases", []):
        if not isinstance(case, dict):
            continue
        cases.append(
            {
                "case": case.get("case", ""),
                "phase": case.get("phase", ""),
                "angle": case.get("angle", ""),
                "runtimeGateStatus": case.get("runtimeGateStatus", ""),
                "reportStatus": case.get("reportStatus", ""),
                "bootstrap": case.get("bootstrap", {}),
                "actorStage": case.get("actorStage", {}),
                "actorCamera": case.get("actorCamera", {}),
                "actorKitSelection": case.get("actorKitSelection", {}),
                "animationRequests": case.get("animationRequests", []),
                "runtimePartTimelines": case.get("runtimePartTimelines", []),
                "openmwLog": case.get("openmwLog", ""),
                "reportJson": case.get("reportJson", ""),
                "screenshots": case.get("screenshots", []),
            }
        )
    return {
        "schema": "nikami-fnv-actor-kit-v1",
        "payloadPolicy": "generated proof metadata and commands only; no retail asset payload bytes",
        "actor": manifest.get("actor", ""),
        "actorProfile": manifest.get("actorProfile", {}),
        "phases": manifest.get("phases", []),
        "angles": manifest.get("angles", []),
        "layers": manifest.get("layers", []),
        "controls": manifest.get("controls", {}),
        "cases": cases,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--suite-dir", required=True, type=Path)
    parser.add_argument("--out-json", type=Path)
    parser.add_argument("--out-html", type=Path)
    parser.add_argument("--out-kit-json", type=Path)
    args = parser.parse_args()

    suite_dir = args.suite_dir.resolve()
    manifest = load_suite(suite_dir)
    out_json = args.out_json or (suite_dir / "character-viewer-manifest.json")
    out_html = args.out_html or (suite_dir / "character-viewer.html")
    out_kit_json = args.out_kit_json or (suite_dir / "character-actor-kit.json")
    out_json.write_text(json.dumps(manifest, indent=2), encoding="utf-8")
    out_kit_json.write_text(json.dumps(actor_kit_manifest(manifest), indent=2), encoding="utf-8")
    out_html.write_text(html_doc(manifest), encoding="utf-8")
    print(f"viewer-html={out_html}")
    print(f"viewer-json={out_json}")
    print(f"actor-kit-json={out_kit_json}")
    print(f"status={manifest['overallStatus']} cases={len(manifest['cases'])}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
