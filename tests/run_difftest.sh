#!/bin/sh
# Runs the differential tester in N parallel shards and merges the results.
# Usage: tests/run_difftest.sh <game.exe> <functions.json> [N] [extra difftest.py args...]
# (build.sh must have built build/librecomp.dylib from the same executable)
R=$(cd "$(dirname "$0")/.." && pwd)
PY=${PY:-python3}
EXE=${1:?usage: run_difftest.sh <game.exe> <functions.json> [N]}; FUNCS=${2:?}; shift 2
N=${1:-8}; [ $# -gt 0 ] && shift
OUT="$R/tests/results"; mkdir -p "$OUT"; rm -f "$OUT"/shard_*
for k in $(seq 0 $((N - 1))); do
  "$PY" -I "$R/tests/difftest.py" "$EXE" "$FUNCS" "$R/build/librecomp.dylib" \
    --shard "$k/$N" --report "$OUT/shard_$k.json" "$@" > "$OUT/shard_$k.log" 2>&1 &
done
wait
"$PY" -I - "$OUT" <<'PY'
import json, glob, sys, collections
out = sys.argv[1]; tot = collections.defaultdict(list)
for f in glob.glob(f"{out}/shard_*.json"):
    for k, v in json.load(open(f)).items(): tot[k] += v
json.dump(tot, open(f"{out}/merged.json", "w"), indent=1)
print("  ".join(f"{k}={len(v)}" for k, v in tot.items()))
for e, n, why in sorted(tot["fail"]): print("FAIL", e, n, why)
PY
