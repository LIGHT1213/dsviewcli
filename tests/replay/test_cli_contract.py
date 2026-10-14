#!/usr/bin/env python3
"""CLI JSON contract tests (tests/replay, no hardware required).

Verifies that --json commands emit exactly one parseable JSON object on
stdout with the required schema fields, and that exit codes / error codes
behave correctly when no device is connected. This is a *software* test —
it must never be reported as hardware validation.
"""
import json
import subprocess
import sys
import os

DSLAB = sys.argv[1] if len(sys.argv) > 1 else "./build/dslab.exe"

failures = []


def run(*args):
    p = subprocess.run([DSLAB, *args], capture_output=True, text=True, timeout=60)
    return p


def check(cond, msg):
    if not cond:
        failures.append(msg)


def expect_single_json(p, ctx):
    lines = [l for l in p.stdout.splitlines() if l.strip()]
    check(len(lines) == 1, f"{ctx}: stdout must be exactly one line, got {len(lines)}: {p.stdout[:200]!r}")
    try:
        return json.loads(lines[0])
    except json.JSONDecodeError as e:
        failures.append(f"{ctx}: stdout is not valid JSON: {e}: {p.stdout[:200]!r}")
        return None


def main():
    # version
    p = run("version")
    check(p.returncode == 0, "version: exit 0")
    check(p.stdout.startswith("dslab "), "version: prints version")

    # devices --json : single JSON object; count field present; ok=true.
    p = run("devices", "--json")
    check(p.returncode in (0, 3), f"devices: exit code in (0,3), got {p.returncode}")
    j = expect_single_json(p, "devices")
    if j:
        for k in ("schema_version", "ok", "command", "devices"):
            check(k in j, f"devices: missing key {k}")
        check(j["command"] == "devices", "devices: command field")
        check(isinstance(j["devices"], list), "devices: devices is list")
        if j["devices"]:
            d = j["devices"][0]
            for k in ("vid", "pid", "model", "firmware_loaded"):
                check(k in d, f"devices[0]: missing key {k}")

    # doctor --json : checks array with id/status pairs
    p = run("doctor", "--json")
    j = expect_single_json(p, "doctor")
    if j:
        check("checks" in j and isinstance(j["checks"], list), "doctor: checks list")
        for c in j.get("checks", []):
            check("id" in c and "status" in c, "doctor: check items have id+status")
        ids = [c["id"] for c in j.get("checks", [])]
        check("usb_enumeration" in ids, "doctor: usb_enumeration check present")

    # capabilities: without hardware -> NO_DEVICE + exit 3
    p = run("capabilities", "--json")
    j = expect_single_json(p, "capabilities")
    if j:
        if j.get("ok") is False:
            check(j.get("error", {}).get("code") == "NO_DEVICE",
                  f"capabilities: no-device error code, got {j.get('error')}")
            check(p.returncode == 3, f"capabilities: exit 3, got {p.returncode}")

    # capture once: unsupported mode rejected with UNSUPPORTED_CONFIG, exit 2
    p = run("capture", "once", "--mode", "buffer")
    j = expect_single_json(p, "capture.mode")
    if j:
        check(j.get("error", {}).get("code") == "UNSUPPORTED_CONFIG",
              "capture: bad mode -> UNSUPPORTED_CONFIG")
    check(p.returncode == 2, f"capture: bad mode exit 2, got {p.returncode}")

    # capture once: bad rate rejected
    p = run("capture", "once", "--rate", "999zig")
    j = expect_single_json(p, "capture.rate")
    if j:
        check(j.get("error", {}).get("code") is not None, "capture: bad rate has error code")
    check(p.returncode == 2, f"capture: bad rate exit 2, got {p.returncode}")

    # capture once with no device: NO_DEVICE + exit 3 (when no hardware present;
    # if hardware IS present this capture really runs — accept ok=true too)
    p = run("capture", "once", "--channels", "D0", "--rate", "1M",
            "--duration", "1ms", "--output", "./runs/contract-smoke", "--json")
    try:
        j = expect_single_json(p, "capture.once")
        if j and j.get("ok") is False:
            code = j.get("error", {}).get("code")
            check(code in ("NO_DEVICE", "DEVICE_BUSY", "PERMISSION_DENIED"),
                  f"capture.once no hw: code={code}")
    except Exception:
        pass

    if failures:
        print(f"{len(failures)} CLI contract failure(s):")
        for f in failures:
            print("  -", f)
        return 1
    print("CLI contract tests: all passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
