#!/bin/bash
# Standalone FreeDV program - ONE-COMMAND installer
# Usage:  cd <this folder> && bash install_standalone.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
echo "=== Step A: codec2 (10-20 minutes the first time) ==="
if ldconfig -p 2>/dev/null | grep -q libcodec2 && [ -f /usr/local/include/codec2/freedv_api.h -o -f /usr/include/codec2/freedv_api.h ]; then
  echo "codec2 already installed - skipping."
else
  bash "$HERE/build_codec2.sh"
fi
echo "=== Step B: copy to ~/sbitx_freedv and build ==="
DEST="$HOME/sbitx_freedv"
if [ "$HERE" != "$DEST" ]; then mkdir -p "$DEST"; cp -rf "$HERE"/. "$DEST"/; fi
cd "$DEST" && make clean && make
echo "=== Step C: loopback sound cards ==="
sudo modprobe snd-aloop enable=1,1,1 index=1,2,3 || echo "(loopback cards could not be loaded - the sBitx install normally has them)"
bash "$HERE/../fix_usb_audio.sh" || true
echo "Done. Start the program:  cd ~/sbitx_freedv && ./sbitx_freedv"
