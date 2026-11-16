# Notice: credits and licences

This repository adds new features to the sBitx v3.021 firmware. It combines
work by several authors. The project as a whole is distributed under the
**GNU General Public License v3.0** — see `LICENSE`.

## Licence summary

| Part | Author | Licence |
|---|---|---|
| Original sBitx code in `sbitx.c`, `sbitx_gtk.c`, `Makefile` | Ashhar Farhan, HF Signals and contributors | GPL-3.0 |
| New-feature additions inside those files | Rajesh Huddar (VU3RHU) | GPL-3.0 |
| New files: `ainr.c/.h`, `usbaudio.c/.h`, `freedv_ext.c/.h`, `standalone/fdvapp.c`, install scripts, manual, README | Rajesh Huddar (VU3RHU) | GPL-3.0, and additionally available under the MIT licence at the author's option (see section 3) |
| RNNoise (`rnnoise/`, `data/rnnoise_weights.bin`) | Xiph.Org, Mozilla, Jean-Marc Valin and others | BSD-style, see `rnnoise/COPYING` |
| codec2 / FreeDV (downloaded, not included) | David Rowe and contributors | LGPL-2.1 |
| `ft8_lib` (part of upstream sBitx) | Karlis Goba | its own licence |

## 1. Upstream project: sBitx v3.021

Source: https://github.com/afarhan/sbitx (Ashhar Farhan, HF Signals, and contributors).

The sBitx software is released under the **GPL-3.0**, as confirmed by Ashhar Farhan
in the sBitx users' group in November 2026. `sbitx.c`, `sbitx_gtk.c` and the
`Makefile` here are modified versions of his files.

This repository is an independent community project. It is not made or endorsed
by HF Signals.

## 2. What the GPL means for this repository

Because the work is derived from GPL-3.0 code, the combined work is distributed
under the GPL-3.0. In short:

* The complete source is published here, which is what the licence requires.
* You may use, study, modify and redistribute it.
* If you distribute a modified version, binary or source, you must do so under
  the GPL-3.0 and make your source available too.
* There is no warranty. See sections 15 and 16 of `LICENSE`.

Each source file that has been modified carries a note saying so.

## 3. The new features

Six-band RX/TX equalizers, NRVL / NBVL / FMNT / FVLY noise reduction, AI noise
reduction (AINR), USB speaker / microphone / PTT, PWR / SWR / ALC / DRV meters,
SWR sweep statistics, the five-channel voice recorder with WAV export, the TX
spectrum trace, faster TX/RX switching and AGC recovery, and FreeDV in the radio
and standalone. Written by Rajesh Huddar with the assistance of Claude (Anthropic).

The files listed as entirely new in the table above are the author's own work and
are offered under the MIT licence as well, for anyone who wants to reuse them
outside a GPL project. That dual offer does not change the licence of this
repository as a whole, which is GPL-3.0.

## 4. Third-party libraries

* **RNNoise**: BSD-style licence. Keep `rnnoise/COPYING` with the code. The model
  file `data/rnnoise_weights.bin` is the trained model published by Xiph.Org.
* **codec2 / FreeDV**: LGPL-2.1. Not included here; the scripts fetch it from
  https://github.com/drowe67/codec2 and link to it as a shared library.
* **ft8_lib**: part of the upstream sBitx project, under its own licence.

## 5. Amateur radio use

You are responsible for operating within your licence conditions and local
regulations. Test transmit functions into a dummy load first. The software comes
with no warranty of any kind.
