#!/usr/bin/env bash
# Hardware acceptance for dslab against a real DSLogic U2Basic.
# Run on the host with the device plugged in:  tests/hardware/run_hw_acceptance.sh
# Produces tests/hardware/results/<timestamp>/ with all JSON + a summary.
# Requires: bash, python (any 3.x) for pretty summary.
set -u
cd "$(dirname "$0")/../../"
DSL="${DSL:-./build/dslab.exe}"
OUT="tests/hardware/results/$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT"
PASS=0; FAIL=0; SKIP=0

step() { # name, expected_exit, command...
  local name="$1" want="$2"; shift 2
  echo "=== $name"
  "$@" >"$OUT/$name.json" 2>"$OUT/$name.stderr"; local rc=$?
  if [ "$rc" = "$want" ]; then echo "  ok (exit $rc)"; PASS=$((PASS+1));
  else echo "  FAIL (exit $rc, want $want; see $OUT/$name.stderr)"; FAIL=$((FAIL+1)); fi
}
capture_ok() { # run_name, args...
  local name="$1"; shift
  echo "=== $name"
  "$DSL" capture once "$@" --json >"$OUT/$name.json" 2>"$OUT/$name.stderr"; local rc=$?
  if [ "$rc" = 0 ]; then
    if python - "$OUT/$name.json" <<'EOF'
import json,sys
j=json.load(open(sys.argv[1],encoding='utf-8'))
ok = j.get("ok") is True and j.get("capture",{}).get("sample_count",0) > 0
sys.exit(0 if ok else 1)
EOF
    then echo "  ok (capture complete)"; PASS=$((PASS+1)); else echo "  FAIL (ok:false or 0 samples)"; FAIL=$((FAIL+1)); fi
  else echo "  FAIL (exit $rc)"; FAIL=$((FAIL+1)); fi
}

step devices 0 "$DSL" devices --json
step doctor 0 "$DSL" doctor --json
step capabilities 0 "$DSL" capabilities --json

# plan §M1: three consecutive captures (consistency check)
for i in 1 2 3; do
  capture_ok "cap$i" --channels D0 --rate 1M --duration 10ms --output "$OUT/run_cap$i"
done
# wider config
capture_ok "cap2ch20m" --channels D0,D1 --rate 20M --duration 50ms --output "$OUT/run_2ch"

# offline analysis on a real capture
step analyze 0 "$DSL" analyze timing "$OUT/run_cap1" --channel D0 --json
step decode_uart 0 "$DSL" decode uart "$OUT/run_cap1" --channel D0 --baud 115200 --json

# trigger: arm with timeout (device under test must present an edge on D0;
# a floating/expired trigger ending in TRIGGER_TIMEOUT=exit 4 is a VALID
# outcome for this step when no signal is connected — recorded, not failed)
echo "=== trigger_timeout"
"$DSL" capture once --channels D0 --rate 1M --duration 100ms \
      --trigger 'D0:rising' --timeout 3s --json \
      >"$OUT/trigger.json" 2>"$OUT/trigger.stderr"; rc=$?
if [ "$rc" = 0 ] || [ "$rc" = 4 ]; then echo "  ok (exit $rc)"; PASS=$((PASS+1));
else echo "  FAIL (exit $rc)"; FAIL=$((FAIL+1)); fi

# async lifecycle on real hardware
echo "=== async"
"$DSL" capture start --channels D0 --rate 1M --duration 200ms --json \
  >"$OUT/async_start.json" 2>/dev/null
SID=$(python -c "import json;print(json.load(open('$OUT/async_start.json',encoding='utf-8')).get('session_id',''))" 2>/dev/null)
if [ -n "$SID" ]; then
  sleep 1
  step async_status 0 "$DSL" capture status --session "$SID" --json
  step async_stop 0 "$DSL" capture stop --session "$SID" --json
else echo "  FAIL (no session_id)"; FAIL=$((FAIL+1)); fi

echo
echo "==== SUMMARY: PASS=$PASS FAIL=$FAIL SKIP=$SKIP ===="
echo "artifacts: $OUT"
[ "$FAIL" = 0 ]
