#!/bin/bash
# sBitx v3.021 custom build - one-command installer (no FreeDV)
# Usage:  cd <this folder> && bash install.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
DEST="$HOME/sbitx"
if [ ! -d "$DEST" ]; then echo "ERROR: $DEST not found. Install the sBitx base software first."; exit 1; fi
STAMP=$(date +%Y%m%d_%H%M)
echo "[1/4] Backing up $DEST to $HOME/sbitx_backup_$STAMP ..."
cp -r "$DEST" "$HOME/sbitx_backup_$STAMP"
echo "[2/4] Copying files ..."
for f in sbitx.c sbitx_gtk.c ainr.c ainr.h usbaudio.c usbaudio.h Makefile; do cp -f "$HERE/$f" "$DEST/"; done
for f in freedv_ext.c freedv_ext.h; do [ -f "$HERE/$f" ] && cp -f "$HERE/$f" "$DEST/"; done
mkdir -p "$DEST/data"
cp -rf "$HERE/rnnoise" "$DEST/"
cp -f "$HERE/data/rnnoise_weights.bin" "$DEST/data/"
echo "[3/4] Building (a few minutes) ..."
cd "$DEST" && make clean && make
echo "[4/4] Done. Backup is in $HOME/sbitx_backup_$STAMP"
echo "Start the radio with:  cd ~/sbitx && ./sbitx"
