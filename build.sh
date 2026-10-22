#!/bin/sh
# Translates a Windows executable to C and builds it natively (macOS):
#   ./build.sh <game.exe> <functions.json>     (functions.json: see ghidra/export.sh)
# -> build/librecomp.dylib (the recompiled code, for tests/difftest.py) and build/recomp (the launcher).
# Only generated files whose content changed are recompiled. TRACE=1 builds build/librecomp_trace.dylib
# instead (every instruction calls trace_insn). PROFILE=path/to/profile.h picks the game profile
# (default host/profile.h). PY, WGPU and SDL2 override the Python, wgpu-native and SDL2 locations.
set -e
R=$(cd "$(dirname "$0")" && pwd)
EXE=${1:?usage: build.sh <game.exe> <functions.json>}
FUNCS=${2:?usage: build.sh <game.exe> <functions.json>}
PY=${PY:-python3}
WGPU=${WGPU:-$R/third_party/wgpu}
SDL2=${SDL2:-$(brew --prefix)}  # brew --prefix sdl2 now points at sdl2-compat
PFLAG=; [ -n "$PROFILE" ] && PFLAG="-DRC_PROFILE=\"$(cd "$(dirname "$PROFILE")" && pwd)/$(basename "$PROFILE")\""
OPT=${OPT:--O1}
FPFLAGS="-frounding-math"  # the x87 rounding mode is mirrored into the host FPU at runtime
if [ "${TRACE:-0}" = 1 ]; then OUT=out_trace; OBJ=obj_trace; LIB=librecomp_trace.dylib; TFLAG=--trace
else OUT=out_all; OBJ=obj_all; LIB=librecomp.dylib; TFLAG=; fi
export OUT OBJ  # used inside the per-file compile command below

"$PY" -I "$R/translator/x86toc.py" "$EXE" "$FUNCS" "$R/$OUT" --all --per-file 300 $TFLAG | head -1
mkdir -p "$R/build/$OBJ"
cd "$R"
ls $OUT/*.c | while read f; do
    b=$(basename "$f" .c)
    h=$( (cat "$f"; echo "$OPT $FPFLAGS") | shasum | cut -c1-40)  # flags are part of the cache key
    if [ ! -f "build/$OBJ/$b.o" ] || [ "$(cat "build/$OBJ/$b.hash" 2>/dev/null)" != "$h" ]; then
        echo "$f $b $h"
    fi
done | xargs -r -P 10 -L 1 sh -c 'clang '"$OPT $FPFLAGS"' -w -c -Iruntime -I$OUT "$0" -o "build/$OBJ/$1.o" && echo "$2" > "build/$OBJ/$1.hash" && echo "  compiled $1"'
for o in build/$OBJ/*.o; do  # drop objects whose source no longer exists
    [ -f "$OUT/$(basename "$o" .o).c" ] || rm -f "$o" "${o%.o}.hash"
done
clang $OPT $FPFLAGS -w -c -Iruntime runtime/runtime.c -o build/runtime.o
clang $OPT $FPFLAGS -w -c -Iruntime runtime/interp.c -o build/interp.o
clang $OPT -w -c -Iruntime tests/difftest_api.c -o build/difftest_api.o
# link to a temp file and rename, so running test processes keep their mapped copy intact
clang -dynamiclib -o build/$LIB.tmp build/runtime.o build/interp.o build/difftest_api.o build/$OBJ/*.o -lm
mv -f build/$LIB.tmp build/$LIB
echo "built build/$LIB"

# Native launcher (normal build only): the Windows API layer in host/ + the recompiled code
if [ "${TRACE:-0}" != 1 ]; then
    mkdir -p build/host
    for f in host/*.c; do
        clang $OPT -Wall -Wno-unused-function -Wno-misleading-indentation -c -Iruntime -Ihost $PFLAG \
            -I"$WGPU/include" -I"$SDL2/include" "$f" -o "build/host/$(basename "$f" .c).o" || exit 1
    done
    clang -o build/recomp.tmp build/runtime.o build/interp.o build/host/*.o build/$OBJ/*.o -lm -framework AudioToolbox \
        "$WGPU/lib/libwgpu_native.a" -L"$SDL2/lib" -lSDL2 \
        -lz -framework Metal -framework QuartzCore -framework Cocoa -framework IOKit -framework CoreFoundation
    mv -f build/recomp.tmp build/recomp
    echo "built build/recomp"
fi
