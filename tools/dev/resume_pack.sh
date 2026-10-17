#!/bin/sh
# resume_pack.sh: pack what a new session can't get from git (your disc's extracted files in
# the decomp's orig/, its tools/compilers, the sparse disc and saves in $WORK) into
# $WORK/pack/resume.tar.gz.partNN (25 MB parts). These contain game data: they stay on your machine.
PC=$(cd "$(dirname "$0")/../.." && pwd)
WORK=${OUTBREAK_WORK:-$HOME/outbreak-work}
DECOMP=${OUTBREAK_DECOMP:-$(grep -s "^OUTBREAK_DECOMP_DIR" "$PC/build/linux32/CMakeCache.txt" | cut -d= -f2)}
DECOMP=${DECOMP:-$PC/decomp}
rm -rf "$WORK/pack"; mkdir -p "$WORK/pack"
tar -C / -cSf - "${DECOMP#/}/orig" "${DECOMP#/}/tools/compilers" \
    "${WORK#/}/f1disc" "${WORK#/}/f1disc.have.json" "${WORK#/}/saves" \
    | gzip -1 | split -b 25M -d - "$WORK/pack/resume.tar.gz.part"
ls -la "$WORK/pack"
