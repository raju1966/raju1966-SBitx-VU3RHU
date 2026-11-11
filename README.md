# sBitx custom build (VU3RHU)

Enhanced firmware for the HF Signals **sBitx v3** (Raspberry Pi, GTK3, Debian Buster).
Built on https://github.com/afarhan/sbitx. Only these files differ from upstream:
`sbitx.c`, `sbitx_gtk.c`, `usbaudio.c/.h`, `ainr.c/.h`, `freedv_ext.c/.h` and the `Makefile`.

## Features
* Six-band RX and TX equalizers (touch-screen popup, +/-20 dB)
* Noise reduction without the metallic sound: NRVL, NBVL, FMNT, FVLY, plus AINR (RNNoise AI)
* PWR / SWR / ALC / DRV meters together; TX spectrum trace (USB right, LSB left)
* SWR sweep with average and standard deviation
* USB speaker, USB microphone and USB PTT
* Five-channel voice recorder with waveform, erase and WAV export
* About 10 ms TX to RX release and a fast AGC recovery after transmit
* Optional FreeDV (700D, 700E, 1600) inside the radio, or as a standalone program

Not yet done: BW shift display on screen, separate RX/TX bandwidth selection.

## Install (radio only)
Needs the sBitx base software in `~/sbitx`.
```
git clone https://github.com/<your-username>/sbitx-vu3rhu.git
cd sbitx-vu3rhu
bash install.sh
cd ~/sbitx && ./sbitx
```
The installer backs up `~/sbitx` first (`~/sbitx_backup_<date>`).

## Install with FreeDV in the radio
```
bash freedv/install_freedv.sh
```
Builds codec2 from GitHub (10-20 minutes, once), then installs and builds the radio.

## Standalone FreeDV
```
bash standalone/install_standalone.sh
```

## Back up and go back
```
cp -r ~/sbitx ~/sbitx_backup_$(date +%Y%m%d)
cd ~ && mv sbitx sbitx_new && cp -r sbitx_backup_YYYYMMDD sbitx
```

## USB audio caution
Start the radio first, then plug in the USB sound card, then tap USBS / USBM (menu M4).

## Manual
Full illustrated user manual: `docs/sBitx_User_Manual.pdf`.

## Licence
My original additions are MIT licensed (`LICENSE`). Code from the upstream sBitx project, RNNoise and codec2 stays
under its own terms. See `NOTICE.md` for exactly what is covered by what.

73 de VU3RHU (MK68JW).
