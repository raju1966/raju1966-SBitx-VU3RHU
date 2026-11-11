#!/bin/bash
# Builds RADE (the latest FreeDV) for the sBitx and tells you whether this Pi
# can run it in real time. Needs internet once (to fetch RADE, Opus and the
# Opus neural-network model). Takes roughly 10-20 minutes on a Pi 4.
#
#   cd ~/sbitx/rade && ./build_rade.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
W="$HOME/rade_c"
sudo apt-get install -y git cmake build-essential autoconf automake libtool sox

[ -d "$W" ] || git clone https://github.com/drowe67/rade_c.git "$W" 2>/dev/null || \
               git clone https://github.com/freedv/rade_c.git "$W"
mkdir -p "$W/build" && cd "$W/build"
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j4

OPUS="$W/build/build_opus-prefix/src/build_opus"
[ -f "$OPUS/.libs/libopus.a" ] || { echo "Opus build not found at $OPUS"; exit 1; }

# the wrapper the radio loads at run time
gcc -O3 -mcpu=native -shared -fPIC -Wl,-Bsymbolic -DHAVE_CONFIG_H -DHAVE_RADEX \
    -I"$OPUS" -I"$W/src" -I"$OPUS/dnn" -I"$OPUS/celt" -I"$OPUS/include" \
    "$HERE/radex.c" -o "$HERE/libradex.so" \
    -L"$W/build/src" -lrade -Wl,-rpath,"$W/build/src" "$OPUS/.libs/libopus.a" -lm
echo "built $HERE/libradex.so"

# ---- real-time check: encode and decode 30 s of speech and time it --------
T=$(mktemp -d)
sox "$W/input_sample.wav" -r 16000 -c 1 -b 16 "$T/in16.wav" trim 0 30 2>/dev/null || cp "$W/input_sample.wav" "$T/in16.wav"
S=$(soxi -D "$T/in16.wav")
t0=$(date +%s.%N); "$W/build/src/rade_tx_wav" -v 0 "$T/in16.wav" "$T/mod.wav"; t1=$(date +%s.%N)
"$W/build/src/rade_rx_wav" -v 0 "$T/mod.wav" "$T/out.wav"; t2=$(date +%s.%N)
python3 - "$S" "$t0" "$t1" "$t2" <<'PY'
import sys
s,t0,t1,t2=map(float,sys.argv[1:])
tx=(t1-t0)/s; rx=(t2-t1)/s
print(f"\nSpeech length {s:.1f} s")
print(f"TRANSMIT uses {tx*100:.0f}% of real time ({'OK' if tx<0.6 else 'TOO SLOW'})")
print(f"RECEIVE  uses {rx*100:.0f}% of real time ({'OK' if rx<0.6 else 'TOO SLOW'})")
print("Below about 60% is comfortable because the radio's own DSP shares the Pi.")
PY
rm -rf "$T"
