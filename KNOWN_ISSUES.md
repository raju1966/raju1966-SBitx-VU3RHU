# Known issues and limits

## USB sound card
- **Fixed on 2026-11-13:** the radio lost its audio when switched on with a USB sound card plugged in.
  The installers now run `fix_usb_audio.sh`, which keeps a USB sound card at number 5. Restart the Pi once
  after installing, then check with `cat /proc/asound/cards` (radio card 0, Loopback 1 to 3, USB card 5).
  This was confirmed on the author's radio with one USB card; other cards and Pi models are untested.
- If you installed before that date, run `bash fix_usb_audio.sh` once and restart the Pi.
- Switch USBS and USBM off before unplugging the card.
- USBS ON mutes the radio's own speaker and earphone output.

## Audio at switch-on
- On some starts the radio comes up with no audio until it is switched off and on again. The cause is not
  confirmed yet (possibly the radio program starting before the sound cards are ready). Under investigation.

## Audio after transmit
- After releasing PTT the receive audio level returns quickly in testing on the author's radio, but some users may
  still notice a slow return on busy bands. Reports with the console `[TXRX]` lines are welcome.

## FreeDV
- The RADE modes are listed but **do not work on Debian Buster**; they need newer system libraries. 700D, 700E and 1600 work.
- codec2 must be built once on the Pi (the installer does this and takes 10 to 20 minutes).

## Not implemented yet
- BW shift display on the screen during RX and TX.
- Separate RX and TX bandwidth selection.

## Licence
- The licence of the original sBitx code is pending confirmation from its authors. See `NOTICE.md`.
