# Notice: credits, licences and what is covered by what

This repository combines work by several authors. Each part keeps its own
licence. Nothing here relicenses anyone else's work.

## 1. Upstream project: sBitx v3.021
Source: https://github.com/afarhan/sbitx (Ashhar Farhan, HF Signals, and contributors).

`sbitx.c`, `sbitx_gtk.c` and the `Makefile` are **modified versions of upstream files**.
The upstream parts remain under the upstream project's terms. At the time of writing the
upstream repository shows no LICENSE file, so its authors' permission is being requested
(see issue linked from the README). If the upstream authors state a licence, it applies
to the upstream portions of these files and takes precedence over anything stated here.

The sBitx hardware and software are products of HF Signals; this repository is an
independent community enhancement and is not endorsed by them.

## 2. Original contributions of Rajesh Huddar (VU3RHU), MIT licence (see LICENSE)
Entirely new files:
* `ainr.c`, `ainr.h` (AI noise reduction using RNNoise)
* `usbaudio.c`, `usbaudio.h` (USB speaker, microphone and PTT)
* `freedv_ext.c`, `freedv_ext.h` (FreeDV inside the radio)
* `standalone/fdvapp.c` and `standalone/Makefile` (standalone FreeDV program)
* the install scripts (`install.sh`, `freedv/install_freedv.sh`, `standalone/install_standalone.sh`, `freedv/build_codec2.sh`)
* the manual in `docs/`, `README.md`

Added or changed parts inside `sbitx.c`, `sbitx_gtk.c` and `Makefile` (equalizers, NRVL/NBVL/FMNT/FVLY,
meters, SWR sweep statistics, recorder, TX trace, TX/RX timing and AGC recovery): the MIT licence
applies to these additions **only to the extent they can be separated from the upstream code**.

These contributions were developed with the assistance of Claude (Anthropic).

## 3. Third-party libraries
* **RNNoise** (Xiph.Org Foundation, Mozilla, Jean-Marc Valin and others): BSD-style licence.
  Full text in `rnnoise/COPYING`. Keep that file with the code. The model file
  `data/rnnoise_weights.bin` is the RNNoise trained model published by Xiph.Org.
* **codec2 / FreeDV** (David Rowe and contributors): LGPL-2.1. Not included here; the scripts download
  it from https://github.com/drowe67/codec2 and link to it as a shared library.
* **FT8 library** used by sBitx (`ft8_lib`, Karlis Goba): part of the upstream project, under its own licence.

## 4. Amateur radio use
You are responsible for operating within your licence conditions and local regulations.
Test transmit functions into a dummy load first. Software is provided without warranty.
