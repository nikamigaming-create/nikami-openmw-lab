#!/usr/bin/env python3
"""Loopback live server for the generated FNV character viewer.

The server serves generated proof files from --root and exposes one local-only
actor-kit endpoint that can run the generated viewer rerun commands. It stores
job metadata under --run-dir and never serves or writes retail asset payloads.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import threading
import time
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import unquote, urlparse


ALLOWED_PREFIX = [
    "powershell",
    "-NoProfile",
    "-ExecutionPolicy",
    "Bypass",
    "-File",
    "scripts/nikami/run-fnv-character-viewer.ps1",
]
FORBIDDEN_CHARS = set("&;|<>`")


def split_powershell_command(command: str) -> list[str]:
    tokens: list[str] = []
    current: list[str] = []
    in_single = False
    index = 0
    while index < len(command):
        char = command[index]
        if in_single:
            if char == "'":
                if index + 1 < len(command) and command[index + 1] == "'":
                    current.append("'")
                    index += 2
                    continue
                in_single = False
            else:
                current.append(char)
            index += 1
            continue
        if char == "'":
            in_single = True
        elif char.isspace():
            if current:
                tokens.append("".join(current))
                current = []
        else:
            current.append(char)
        index += 1
    if in_single:
        raise ValueError("unterminated single quote")
    if current:
        tokens.append("".join(current))
    return tokens


def command_to_args(command: str, runner: Path) -> list[str]:
    if any(char in FORBIDDEN_CHARS for char in command):
        raise ValueError("command contains shell metacharacters outside the actor-kit allowlist")
    tokens = split_powershell_command(command)
    if len(tokens) < len(ALLOWED_PREFIX):
        raise ValueError("command is too short")
    normalized = tokens[: len(ALLOWED_PREFIX)]
    normalized[0] = normalized[0].lower().removesuffix(".exe")
    if normalized != ALLOWED_PREFIX:
        raise ValueError("command does not match generated actor-kit runner prefix")
    args = tokens[:]
    args[0] = "powershell"
    args[5] = str(runner)
    args = [arg for arg in args if arg != "-OpenViewer"]
    args = [arg for arg in args if arg != "-Serve"]
    args = [arg for arg in args if arg != "-LiveServe"]
    return args


def proof_url(root: Path, path: str) -> str:
    if not path:
        return ""
    target = Path(path)
    if not target.is_absolute():
        return ""
    try:
        relative = target.resolve().relative_to(root.resolve())
    except ValueError:
        return ""
    return "/" + str(relative).replace(os.sep, "/")


def parse_run_output(stdout: str, root: Path) -> dict[str, str]:
    viewer = ""
    manifest = ""
    actor_kit = ""
    suite = ""
    for line in stdout.splitlines():
        if "Viewer for " in line and ": " in line:
            viewer = line.split(": ", 1)[1].strip()
        elif "Manifest for " in line and ": " in line:
            manifest = line.split(": ", 1)[1].strip()
        elif line.startswith("viewer-html="):
            viewer = line.split("=", 1)[1].strip()
        elif line.startswith("viewer-json="):
            manifest = line.split("=", 1)[1].strip()
        elif line.startswith("actor-kit-json="):
            actor_kit = line.split("=", 1)[1].strip()
        elif "SuiteDir:" in line:
            suite = line.split("SuiteDir:", 1)[1].strip()
    status_match = re.search(r"status=(?P<status>[A-Z]+)\s+cases=(?P<cases>[0-9]+)", stdout)
    return {
        "status": status_match.group("status") if status_match else "",
        "cases": status_match.group("cases") if status_match else "",
        "viewerHtml": viewer,
        "viewerUrl": proof_url(root, viewer),
        "viewerJson": manifest,
        "viewerJsonUrl": proof_url(root, manifest),
        "actorKitJson": actor_kit,
        "actorKitUrl": proof_url(root, actor_kit),
        "suiteDir": suite,
    }


class JobStore:
    def __init__(self, run_dir: Path, root: Path, repo_root: Path, runner: Path) -> None:
        self.run_dir = run_dir
        self.root = root
        self.repo_root = repo_root
        self.runner = runner
        self.jobs_dir = run_dir / "live-jobs"
        self.jobs_dir.mkdir(parents=True, exist_ok=True)
        self.lock = threading.Lock()
        self.jobs: dict[str, dict[str, Any]] = {}

    def write_job(self, job: dict[str, Any]) -> None:
        job_path = self.jobs_dir / f"{job['id']}.json"
        job_path.write_text(json.dumps(job, indent=2), encoding="utf-8")

    def start(self, command: str) -> dict[str, Any]:
        args = command_to_args(command, self.runner)
        job_id = time.strftime("%Y%m%d_%H%M%S") + f"_{len(self.jobs) + 1:04d}"
        job: dict[str, Any] = {
            "id": job_id,
            "state": "running",
            "startedAt": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "finishedAt": "",
            "command": command,
            "args": args,
            "returnCode": None,
            "stdout": "",
            "stderr": "",
            "result": {},
            "policy": {
                "loopbackOnly": True,
                "generatedProofOutputsOnly": True,
                "noRetailAssetsCommitted": True,
                "allowedRunner": "scripts/nikami/run-fnv-character-viewer.ps1",
            },
        }
        with self.lock:
            self.jobs[job_id] = job
            self.write_job(job)
        thread = threading.Thread(target=self._run, args=(job_id,), daemon=True)
        thread.start()
        return job

    def _run(self, job_id: str) -> None:
        with self.lock:
            job = self.jobs[job_id]
            args = list(job["args"])
        try:
            completed = subprocess.run(
                args,
                cwd=self.repo_root,
                text=True,
                capture_output=True,
                check=False,
            )
            result = parse_run_output(completed.stdout, self.root)
            with self.lock:
                job = self.jobs[job_id]
                job["state"] = "complete" if completed.returncode == 0 else "failed"
                job["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                job["returnCode"] = completed.returncode
                job["stdout"] = completed.stdout[-20000:]
                job["stderr"] = completed.stderr[-20000:]
                job["result"] = result
                self.write_job(job)
        except Exception as exc:  # pragma: no cover - defensive runtime guard
            with self.lock:
                job = self.jobs[job_id]
                job["state"] = "failed"
                job["finishedAt"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
                job["stderr"] = str(exc)
                self.write_job(job)

    def get(self, job_id: str) -> dict[str, Any] | None:
        with self.lock:
            return self.jobs.get(job_id)

    def list(self) -> list[dict[str, Any]]:
        with self.lock:
            return list(self.jobs.values())[-50:]


class LiveHandler(SimpleHTTPRequestHandler):
    server_version = "NikamiFNVCharacterViewerLive/1.0"

    def send_json(self, status: int, payload: dict[str, Any] | list[dict[str, Any]]) -> None:
        data = json.dumps(payload, indent=2).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def do_GET(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path == "/nikami/actor-kit/jobs":
            self.send_json(200, self.server.job_store.list())  # type: ignore[attr-defined]
            return
        if parsed.path.startswith("/nikami/actor-kit/jobs/"):
            job_id = unquote(parsed.path.rsplit("/", 1)[-1])
            job = self.server.job_store.get(job_id)  # type: ignore[attr-defined]
            if job is None:
                self.send_json(404, {"error": "unknown job"})
                return
            self.send_json(200, job)
            return
        super().do_GET()

    def do_POST(self) -> None:  # noqa: N802
        parsed = urlparse(self.path)
        if parsed.path != "/nikami/actor-kit/run":
            self.send_json(404, {"error": "unknown endpoint"})
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
            payload = json.loads(self.rfile.read(length).decode("utf-8"))
            command = str(payload.get("command") or "")
            job = self.server.job_store.start(command)  # type: ignore[attr-defined]
            self.send_json(202, job)
        except Exception as exc:
            self.send_json(400, {"error": str(exc)})


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=Path)
    parser.add_argument("--repo-root", required=True, type=Path)
    parser.add_argument("--run-dir", required=True, type=Path)
    parser.add_argument("--runner", required=True, type=Path)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", required=True, type=int)
    args = parser.parse_args()

    root = args.root.resolve()
    repo_root = args.repo_root.resolve()
    run_dir = args.run_dir.resolve()
    runner = args.runner.resolve()
    os.chdir(root)
    handler = lambda *handler_args, **handler_kwargs: LiveHandler(  # noqa: E731
        *handler_args, directory=str(root), **handler_kwargs
    )
    httpd = ThreadingHTTPServer((args.host, args.port), handler)
    httpd.job_store = JobStore(run_dir, root, repo_root, runner)  # type: ignore[attr-defined]
    print(f"live-viewer-server=http://{args.host}:{args.port}", flush=True)
    httpd.serve_forever()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
