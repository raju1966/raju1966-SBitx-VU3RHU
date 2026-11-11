#!/bin/bash
# sBitx custom build WITH FreeDV in the radio. Run from the repo:  bash freedv/install_freedv.sh
set -e
REPO="$(cd "$(dirname "$0")/.." && pwd)"
if ldconfig -p 2>/dev/null | grep -q libcodec2 && [ -f /usr/local/include/codec2/freedv_api.h -o -f /usr/include/codec2/freedv_api.h ]; then
  echo "codec2 already installed - skipping."
else
  bash "$REPO/freedv/build_codec2.sh"     # clones drowe67/codec2 from GitHub and builds it
fi
DEST="$HOME/sbitx"; STAMP=$(date +%Y%m%d_%H%M)
[ -d "$DEST" ] || { echo "ERROR: $DEST not found (install the sBitx base software first)"; exit 1; }
cp -r "$DEST" "$HOME/sbitx_backup_$STAMP"
for f in sbitx.c sbitx_gtk.c ainr.c ainr.h usbaudio.c usbaudio.h; do cp -f "$REPO/$f" "$DEST/"; done
cp -f "$REPO/freedv/freedv_ext.c" "$REPO/freedv/freedv_ext.h" "$DEST/"
cp -f "$REPO/freedv/Makefile.freedv" "$DEST/Makefile"
mkdir -p "$DEST/data"; cp -rf "$REPO/rnnoise" "$DEST/"; cp -f "$REPO/data/rnnoise_weights.bin" "$DEST/data/"
cd "$DEST" && make clean && make
echo "Done. Backup: $HOME/sbitx_backup_$STAMP   Start: cd ~/sbitx && ./sbitx"
