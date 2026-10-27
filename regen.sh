#!/usr/bin/env bash
# Re-run the xboxrecomp pipeline for Spider-Man and rebuild the exe.
#
#   ./regen.sh           # disasm -> func_id -> abi -> names -> lift to C -> build
#   ./regen.sh lift      # lift to C -> build (analysis unchanged: a new hook
#                        # or override in src/recomp_manual.c)
#   ./regen.sh nobuild   # full analysis and lift, no build
#
# Hand-maintained function seeds live in seeds/manual_seeds.json. The Ghidra
# names are re-applied after every disassembly, because tools.disasm rewrites
# functions.json and drops them. src/recomp_manual.c is scanned by the lifter
# (--exclude-manual) for hand-written bodies and sub_X_enter hooks. Logs go to
# logs/.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TK="$(cd "$HERE/../xboxrecomp" && pwd)"
XBE=game_files/default.xbe
LOGS="$HERE/logs"
MODE="${1:-all}"
mkdir -p "$LOGS"

step() { echo "[$(date +%H:%M:%S)] $*"; }

cd "$TK"
if [ "$MODE" != "lift" ]; then
    step "disassemble";  py -3 -m tools.disasm "$XBE" --force \
                           --seed-functions "$HERE/seeds/manual_seeds.json" > "$LOGS/disasm.txt" 2>&1
    step "identify";     py -3 -m tools.func_id "$XBE" -v              > "$LOGS/func_id.txt" 2>&1
    step "abi";          py -3 -m tools.abi_analysis "$XBE" -v         > "$LOGS/abi.txt" 2>&1
    step "names";        py -3 tools/ghidra_naming/merge_names.py --apply > "$LOGS/names.txt" 2>&1
fi
step "lift to C";    py -3 -m tools.recomp "$XBE" --all --split 250 \
                       --exclude-manual "$HERE/src/recomp_manual.c" \
                       --gen-dir "$HERE/src/recomp/gen"            > "$LOGS/recomp.txt" 2>&1
tail -1 "$LOGS/recomp.txt"

[ "$MODE" = "nobuild" ] && exit 0

# A game left running keeps the exe locked and the link fails (LNK1104).
taskkill //F //IM spiderman_recomp.exe > /dev/null 2>&1 || true

cd "$HERE"
step "build"
"/c/Program Files/CMake/bin/cmake.exe" --build build --config Release --parallel 2 \
    > "$LOGS/build.txt" 2>&1 || { grep -E "error [A-Z]+[0-9]+" "$LOGS/build.txt" | head -20; exit 1; }
step "done: build/Release/spiderman_recomp.exe"
