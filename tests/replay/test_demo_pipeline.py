#!/usr/bin/env python3
"""Demo-device pipeline test (tests/replay — SYNTHETIC data, NOT hardware).

Runs `capture once --device demo` through the exact same code path a real
U2Basic capture uses (library init, session, datafeed callback, raw file,
unpack stats, capture.json/events.jsonl) against the library's built-in
pattern generator. A pass here proves the software pipeline only; it is
never a substitute for the hardware acceptance test.
"""
import json
import subprocess
import sys
import os
import tempfile
import shutil

DSLAB = sys.argv[1] if len(sys.argv) > 1 else "./build/dslab.exe"

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def main():
    tmp = tempfile.mkdtemp(prefix="dslab-demo-")
    out = os.path.join(tmp, "run")
    try:
        p = subprocess.run(
            [DSLAB, "capture", "once", "--device", "demo",
             "--channels", "D0,D1", "--rate", "1M", "--duration", "5ms",
             "--output", out, "--json"],
            capture_output=True, text=True, timeout=60)
        check(p.returncode == 0, f"exit code 0, got {p.returncode}; stderr tail: "
              f"{p.stderr[-400:]!r}")
        lines = [l for l in p.stdout.splitlines() if l.strip()]
        check(len(lines) == 1, f"exactly one stdout line, got {len(lines)}")
        j = json.loads(lines[0])
        check(j["ok"] is True, "ok is true")
        check(j["device"]["model"] == "Demo Device",
              "device is explicitly the demo device (no masquerading)")
        check(j["capture"]["sample_count"] == 5000,
              f"5000 samples, got {j['capture']['sample_count']}")
        check(j["capture"]["byte_count"] >= 1248,
              f"byte count >= 1248, got {j['capture']['byte_count']}")
        check(len(j["channels"]) == 2, "two channels requested")
        for ch in ("D0", "D1"):
            check(ch in j["channels_detail"], f"{ch} stats present")
            check("transitions" in j["channels_detail"][ch], "transitions field")
        check(j["error"] is None, "error is null")

        raw = os.path.join(out, "capture.raw")
        check(os.path.isfile(raw), "capture.raw exists")
        check(os.path.getsize(raw) >= 1248, "capture.raw size")
        meta = json.load(open(os.path.join(out, "capture.json"), encoding="utf-8"))
        check(meta["capture"]["sample_rate_hz"] == 1000000, "meta rate")
        check(meta["sample_format"]["channel_slots"] == 2, "meta channel slots")
        check(meta["enabled_channels"] == ["D0", "D1"], "meta enabled channels")
        ev = [json.loads(l) for l in open(os.path.join(out, "events.jsonl"),
                                          encoding="utf-8") if l.strip()]
        names = [e["event"] for e in ev]
        for expected in ("armed_config", "collect_start", "device_running",
                         "df_end", "collect_end"):
            check(expected in names, f"event {expected} present")
        # event order sanity
        check(names.index("collect_start") < names.index("device_running")
              < names.index("collect_end"), "event ordering")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    if failures:
        print(f"{len(failures)} demo pipeline failure(s):")
        for f in failures:
            print("  -", f)
        return 1
    print("demo pipeline test: all passed (synthetic data — not hardware validation)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
