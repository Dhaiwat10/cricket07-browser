#!/bin/sh
# Analyses an executable with Ghidra (headless) and exports the function list the translator needs.
#   GHIDRA=/path/to/ghidra_12.x_PUBLIC ghidra/export.sh <game.exe> <out/functions.json>
# Also recovers MSVC RTTI class names and names functions after the assert strings they use; both
# only affect naming (the CSV reports land next to the JSON).
set -e
G=${GHIDRA:?set GHIDRA to your Ghidra install folder}
EXE=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
OUT=$(cd "$(dirname "$2")" && pwd)/$(basename "$2")
D=$(dirname "$OUT")
S=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$D/ghidra_project"
"$G/support/analyzeHeadless" "$D/ghidra_project" recomp -import "$EXE" -overwrite -scriptPath "$S" \
  -postScript NameFromAsserts.java "$D/assert_names.csv" \
  -postScript RecoverRttiClasses.java "$D/rtti_classes.csv" \
  -postScript ExportFunctions.java "$OUT"
