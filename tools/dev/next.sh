#!/bin/sh
# next.sh: build the 32-bit Linux build, run it (INPUT=..., T=seconds) on the sparse disc, and show
# the first function the game calls that isn't decompiled yet, with its m2c draft. When the run
# read disc parts the sparse copy doesn't have, they're listed in $WORK/req.txt (fetch them with
# fetch_ranges.py on the disc's machine, then apply.sh).
PC=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${OUTBREAK_WORK:-$HOME/outbreak-work}
DECOMP=${OUTBREAK_DECOMP:-$(grep -s "^OUTBREAK_DECOMP_DIR" "$PC/build/linux32/CMakeCache.txt" | cut -d= -f2)}
DECOMP=${DECOMP:-$PC/decomp}
mkdir -p "$WORK/frames" "$WORK/saves"
cd "$PC" && cmake --build build/linux32 2>&1 | grep -E "error|undefined|isn.t|owners|inside|multiple" | grep -v "^FAILED: gen" | head -20
rm -f "$WORK"/frames/*.ppm "$WORK/reads.log"
OUTBREAK_READLOG="$WORK/reads.log" ${PRE} timeout ${T:-60} ./build/linux32/outbreak-f1 ${INPUT:+--input $INPUT} \
    --saves "$WORK/saves" --dump-frames "$WORK/frames" "$WORK/f1disc" > "$WORK/run.log" 2>&1
grep -v "display: wrote" "$WORK/run.log" | tail -${N:-6}
python3 "$PC/tools/dev/sparse.py" need "$WORK/reads.log" > "$WORK/req.txt"
[ -s "$WORK/req.txt" ] && echo "NEED DISC DATA: $(wc -l < "$WORK/req.txt") ranges in $WORK/req.txt"
f=$(grep -o "the game called [a-z_0-9A-F]*" "$WORK/run.log" | tail -1 | awk '{print $4}')
[ -z "$f" ] && exit 0
ovl=main; case $f in *__*) ovl=${f%%__*}; f=${f#*__};; esac
cd "$DECOMP" && timeout 120 python3 -I tools/draft.py f1.$ovl $f 2>&1 | grep -v "^M2C_UNK func\|^s32 func_\|^extern\|^$"
