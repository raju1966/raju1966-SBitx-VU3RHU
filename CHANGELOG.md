# Changelog

All notable changes to this project. Dates are in the format YYYY-MM-DD.

## Update 2026-11-13
Files changed: `sbitx_gtk.c`, the install scripts, the new `fix_usb_audio.sh`, the manual and these documents.
`sbitx.c` and `usbaudio.c` are unchanged.

### Fixed
- **No audio, or garbled audio, when the radio was switched on with a USB sound card plugged in.**
  The USB card could take one of the sound-card numbers the radio needs (its own card at 0, the Loopback
  cards at 1, 2 and 3). The new `fix_usb_audio.sh` (run by every installer) makes the Pi give a USB sound
  card number 5 every time, through `/etc/modprobe.d/sbitx-usb-audio.conf`. The card can now stay plugged in.
  Confirmed on the author's radio. Restart the Pi once after installing.
- **Stray vertical lines inside the menu boxes after changing menu page** (seen on M6 after M5, and on M5 after M4).
  Pages with six narrow boxes and pages with four wider boxes left parts of each other's outlines on screen.
  The whole menu row is now redrawn in one piece on every page change (`sbitx_gtk.c`).
- **SWR, ALC and DRV readings partly hidden behind the MENU box.** The meter bars now stop short of the
  MENU box (`sbitx_gtk.c`).

### Changed
- Manual revised (section 10, USB audio; troubleshooting), now titled "with New Features".

## Release v3.021-vu3rhu-nov2026 (2026-11-11)
Based on sBitx v3.021 (https://github.com/afarhan/sbitx).

### Added
- Six-band RX equalizer and six-band TX equalizer (300, 600, 1000, 1500, 2000, 2800 Hz, +/-20 dB), in a touch-screen popup (menu M8).
- Noise reduction without the metallic sound: NRVL (spectral noise reducer), NBVL (impulse blanker), FMNT and FVLY (formant filters).
- AINR: AI noise reduction using RNNoise, for SSB and AM speech.
- PWR, SWR, ALC and DRV meters shown together on the main screen.
- SWR sweep reports the average SWR and the standard deviation.
- USB speaker, USB microphone and USB PTT key (menu M4).
- Five-channel voice recorder (60 s per channel) with waveform, erase and WAV export to a USB stick (menu M5).
- TX spectrum trace on the main screen (USB to the right of the centre line, LSB to the left).
- Optional FreeDV (700D, 700E, 1600) inside the radio (menu M9), and a standalone FreeDV program.
- One-command installers with automatic backup, and an illustrated user manual (`docs/`).

### Changed
- TX to RX release shortened to about 10 ms.
- Faster audio recovery (AGC) after releasing PTT.
- Microphone gain for the USB microphone no longer changes the radio's own codec "Capture" level.

### Fixed
- TX spectrum trace appeared on the wrong side of the centre line.
- USB speaker and microphone start-up: retries and release of the desktop sound system when the card is busy.

### Not done yet
- BW shift display on the screen during RX and TX.
- Separate RX and TX bandwidth selection.
