#!/bin/bash
# Builds and installs the codec2 library on the Pi (Debian Buster has no working apt for it).
# Uses the codec2-main.zip that sits next to this script, so NO download is needed
# (only cmake, which this script installs with pip if it is missing).
# Takes about 10-20 minutes on a Pi 4.  Run once:   bash build_codec2.sh
set -e
export PATH="$HOME/.local/bin:$PATH"
HERE="$(cd "$(dirname "$0")" && pwd)"
if ! command -v cmake >/dev/null; then
  echo "cmake not found - installing it with pip (no apt needed, needs internet for this one step)"
  pip3 install --user cmake
fi
for t in cmake gcc make unzip; do
  command -v $t >/dev/null || { echo "MISSING: $t"; exit 1; }
done
cmake --version | head -1
W="$HOME/codec2_src"
rm -rf "$W"; mkdir -p "$W"
if [ -f "$HERE/codec2-main.zip" ]; then
  unzip -q "$HERE/codec2-main.zip" -d "$W"
  SRC="$W/codec2-main"
else
  git clone https://github.com/drowe67/codec2.git "$W/codec2-main"
  SRC="$W/codec2-main"
fi
mkdir -p "$SRC/build_linux" && cd "$SRC/build_linux"
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j2
sudo make install
sudo ldconfig
ls /usr/local/include/codec2/freedv_api.h && echo "codec2 installed OK"
