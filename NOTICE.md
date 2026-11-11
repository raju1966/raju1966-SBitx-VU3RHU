# Notice: credits, licences and what is covered by what

This repository adds new features to the sBitx v3.021 firmware. It combines work by several
authors. Each part keeps its own licence. Nothing here relicenses anyone else's work.

## Licence summary

| Part | Author | Licence | Status |
|---|---|---|---|
| New files: `ainr.c/.h`, `usbaudio.c/.h`, `freedv_ext.c/.h`, `standalone/fdvapp.c`, install scripts, manual, README | Rajesh Huddar (VU3RHU) | MIT (see `LICENSE`) | In force |
| New-feature additions inside `sbitx.c`, `sbitx_gtk.c`, `Makefile` | Rajesh Huddar (VU3RHU) | MIT, to the extent they can be separated from the upstream code | In force |
| Original sBitx code in `sbitx.c`, `sbitx_gtk.c`, `Makefile` | Ashhar Farhan, HF Signals and contributors | **To be confirmed by the upstream authors** | **Pending** (see section 1) |
| RNNoise (`rnnoise/`, `data/rnnoise_weights.bin`) | Xiph.Org, Mozilla, Jean-Marc Valin and others | BSD-style, see `rnnoise/COPYING` | In force |
| codec2 / FreeDV (downloaded, not included) | David Rowe and contributors | LGPL-2.1 | In force |
| `ft8_lib` (part of upstream sBitx) | Karlis Goba | its own licence | In force |

## 1. Upstream project: sBitx v3.021 (licence pending)
Source: https://github.com/afarhan/sbitx (Ashhar Farhan, HF Signals, and contributors).

The upstream repository currently shows no LICENSE file. The permission and licence terms for the
original sBitx code have been requested from its authors. **Until they reply, the original sBitx code
in this repository is not relicensed and stays under whatever terms its authors choose.**
When the upstream authors state a licence, it applies to the upstream portions and takes precedence
over anything stated here. This notice will then be updated.

Upstream licence: _pending reply from the sBitx authors_.

This repository is an independent community project. It is not made or endorsed by HF Signals.

## 2. What the MIT licence covers
New features by Rajesh Huddar: six-band RX/TX equalizers, NRVL / NBVL / FMNT / FVLY noise reduction,
AI noise reduction (AINR), USB speaker/microphone/PTT, PWR/SWR/ALC/DRV meters, SWR sweep statistics,
voice recorder with waveform and WAV export, TX spectrum trace, faster TX/RX switching and AGC recovery,
and FreeDV (in the radio and standalone). These were developed with the assistance of Claude (Anthropic).

## 3. Third-party libraries
* **RNNoise**: BSD-style licence. Keep `rnnoise/COPYING` with the code. The model file
  `data/rnnoise_weights.bin` is the RNNoise trained model published by Xiph.Org.
* **codec2 / FreeDV**: LGPL-2.1. Not included here; the scripts download it from
  https://github.com/drowe67/codec2 and link to it as a shared library.
* **ft8_lib**: part of the upstream sBitx project, under its own licence.

## 4. Amateur radio use
You are responsible for operating within your licence conditions and local regulations.
Test transmit functions into a dummy load first. Software is provided without warranty.
