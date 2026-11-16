# sBitx v3.021 with new features (VU3RHU)

*Firmware enhancements for the HF Signals sBitx, written with **Claude (Anthropic)** —
six-band equalizers, AI noise reduction, USB audio, a voice recorder and FreeDV,
designed and debugged in conversation against a live radio.*

New features for the HF Signals **sBitx v3** (Raspberry Pi, GTK3, Debian Buster).
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
git clone https://github.com/raju1966/raju1966-SBitx-VU3RHU.git
cd raju1966-SBitx-VU3RHU
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

## USB audio
The installer keeps a USB sound card at card number 5, so it can stay plugged in when the radio is switched on.
Restart the Pi once after installing, and check with `cat /proc/asound/cards`. If you installed before 13 November 2026,
run `bash fix_usb_audio.sh` once and restart. Then tap USBS / USBM (menu M4).

## Manual
Full illustrated user manual: `docs/sBitx_User_Manual.pdf`.

## More information
* [User manual (PDF)](docs/sBitx_User_Manual.pdf): all features, recommended settings and install steps
* [CHANGELOG](CHANGELOG.md): what is new in each release
* [KNOWN_ISSUES](KNOWN_ISSUES.md): limits and open items
* [TROUBLESHOOTING](TROUBLESHOOTING.md): common problems and fixes
* [TESTED_ON](TESTED_ON.md): hardware and software tested
* [CONTRIBUTING](CONTRIBUTING.md) and [BUG_REPORT_TEMPLATE](BUG_REPORT_TEMPLATE.md): reporting problems and sending changes
* [NOTICE](NOTICE.md) and [LICENSE](LICENSE): credits and licences

## Credits

Based on **sBitx** by Ashhar Farhan and HF Signals, https://github.com/afarhan/sbitx (GPL-3.0).

The new features here were developed by **Rajesh Huddar (VU3RHU)** working with
**Claude (Anthropic)**. Rajesh specified what the radio needed, tested every change
on the air and judged the results; Claude wrote the code. The DSP design, the GTK
work and most of the debugging were done in conversation, against a live radio on
40 and 20 metres.

**RNNoise** (Xiph.Org, Mozilla, Jean-Marc Valin) provides the AI noise reduction.
**codec2 / FreeDV** (David Rowe) provides the digital voice modes.

## Licence

GPL-3.0. See `LICENSE` for the full text and `NOTICE.md` for what covers what.

The sBitx software this is built on is released under the GPL-3.0 by Ashhar Farhan,
so this repository is too. The source is all here, you may modify and redistribute
it, and any version you pass on must carry the same licence and its source.

RNNoise keeps its own BSD-style licence, and codec2 its LGPL-2.1.

73 de VU3RHU (MK68JW).
