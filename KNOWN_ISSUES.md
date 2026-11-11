# Known issues and limits

## USB sound card
- **Plug the USB card in after the radio has started**, then tap USBS / USBM (menu M4).
  A card plugged in before the radio starts can be held by the desktop sound system. The USB speaker may then
  report "busy", and in some cases the radio's own receive and transmit audio became garbled or silent until restart.
  The exact cause has not been fully identified.
- Switch USBS and USBM off before unplugging the card.
- USBS ON mutes the radio's own speaker and earphone output.

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
