#!/bin/sh
# apply.sh DIR NAME: write a fetched blob (DIR/NAME.gz or DIR/NAME.partNN, plus DIR/NAME.gz.idx) into
# the sparse disc.
WORK=${OUTBREAK_WORK:-$HOME/outbreak-work}
D=$1; N=$2
if [ -f "$D/$N.gz" ]; then gunzip -c "$D/$N.gz" > "$WORK/blob.tmp"; else cat "$D/$N".part* | gunzip > "$WORK/blob.tmp"; fi
python3 "$(dirname "$0")/sparse.py" apply "$WORK/blob.tmp" "$D/$N.gz.idx" && rm "$WORK/blob.tmp"
