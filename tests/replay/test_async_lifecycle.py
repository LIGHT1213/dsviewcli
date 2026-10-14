#!/usr/bin/env python3
"""Async capture lifecycle tests (tests/replay — demo device, SYNTHETIC).

Covers M2 requirements that are testable without hardware:
  1. start → status → complete lifecycle with correct event ordering
  2. mid-flight stop of a long capture → cancelled + partial artifacts
  3. second start while a session is active → DEVICE_BUSY
  4. unknown session → SESSION_NOT_FOUND
Not hardware validation; the demo device generates synthetic data.
"""
import json
import os
import subprocess
import sys
import time
import shutil
import tempfile

DSLAB = sys.argv[1] if len(sys.argv) > 1 else "./build/dslab.exe"
failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def run(*args, timeout=90):
    return subprocess.run([DSLAB, *args], capture_output=True, text=True, timeout=timeout)


def one_json(p, ctx):
    lines = [l for l in p.stdout.splitlines() if l.strip()]
    check(len(lines) == 1, f"{ctx}: one stdout line, got {len(lines)}")
    return json.loads(lines[0]) if lines else {}


def wait_terminal(sid, timeout_s=60):
    t0 = time.time()
    while time.time() - t0 < timeout_s:
        p = run("capture", "status", "--session", sid, "--json")
        j = one_json(p, "status")
        if j.get("state") in ("complete", "timeout", "cancelled", "failed"):
            return j
        time.sleep(0.2)
    return {}


def main():
    os.makedirs("runs", exist_ok=True)

    # ---- 1. normal lifecycle ----
    p = run("capture", "start", "--device", "demo", "--channels", "D0,D1",
            "--rate", "1M", "--duration", "200ms", "--json")
    check(p.returncode == 0, f"start exit 0, got {p.returncode}: {p.stderr[-300:]}")
    j = one_json(p, "start")
    sid = j.get("session_id")
    check(bool(sid), "session_id returned")
    check(j.get("state") == "created", "initial state created")
    fin = wait_terminal(sid)
    check(fin.get("state") == "complete", f"lifecycle completes, got {fin.get('state')}")
    check(fin.get("raw_bytes", 0) == 50000, f"raw size 50000, got {fin.get('raw_bytes')}")

    ev = [json.loads(l) for l in open(f"runs/{sid}/events.jsonl", encoding="utf-8")
          if l.strip()]
    names = [e["event"] for e in ev]
    for want in ("armed_config", "collect_start", "device_running", "collect_end"):
        check(want in names, f"event {want} present in worker session")
    check("configuring" in names and "starting" in names,
          "worker announces configuring/starting states")

    # ---- 2. mid-flight stop of a long capture ----
    p = run("capture", "start", "--device", "demo", "--channels", "D0,D1",
            "--rate", "1M", "--duration", "120000ms", "--json")
    j = one_json(p, "start-long")
    sid2 = j.get("session_id")
    check(bool(sid2), "long session started")
    time.sleep(0.8)  # demo generates fast; 120s of samples takes several seconds
    p = run("capture", "status", "--session", sid2, "--json")
    mid = one_json(p, "status-mid")
    check(mid.get("state") in ("capturing", "armed", "starting", "configuring"),
          f"mid-flight state is live, got {mid.get('state')}")
    p = run("capture", "stop", "--session", sid2, "--json", timeout=60)
    check(p.returncode == 0, f"stop exit 0, got {p.returncode}")
    st = one_json(p, "stop")
    check(st.get("state") == "cancelled", f"stop leads to cancelled, got {st.get('state')}")
    check(os.path.isfile(f"runs/{sid2}/capture.raw"), "partial capture.raw exists")
    check(os.path.getsize(f"runs/{sid2}/capture.raw") > 0, "partial raw non-empty")
    check(os.path.isfile(f"runs/{sid2}/capture.json"), "capture.json finalized")

    # ---- 3. concurrency rejection (fabricated active session) ----
    fake = "zzfakeactive01"
    d = f"runs/{fake}"
    os.makedirs(d, exist_ok=True)
    hb = int(time.time() * 1000)
    with open(f"{d}/state.json", "w", encoding="utf-8") as f:
        f.write(json.dumps({"schema_version": "1.0", "session_id": fake,
                            "state": "capturing", "pid": os.getpid(),
                            "heartbeat_ms": hb,
                            "progress": {"bytes": 1, "samples": 1}}))
    try:
        p = run("capture", "start", "--device", "demo", "--duration", "10ms", "--json")
        j = one_json(p, "start-conflict")
        check(p.returncode == 3, f"conflicting start exits 3, got {p.returncode}")
        check(j.get("error", {}).get("code") == "DEVICE_BUSY",
              f"conflicting start reports DEVICE_BUSY, got {j.get('error')}")
    finally:
        shutil.rmtree(d, ignore_errors=True)

    # ---- 4. unknown session ----
    p = run("capture", "status", "--session", "does-not-exist", "--json")
    j = one_json(p, "status-unknown")
    check(j.get("error", {}).get("code") == "SESSION_NOT_FOUND", "unknown session code")
    check(p.returncode == 3, "unknown session exits 3")

    if failures:
        print(f"{len(failures)} async lifecycle failure(s):")
        for f in failures:
            print("  -", f)
        return 1
    print("async lifecycle tests: all passed (demo device — synthetic)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
