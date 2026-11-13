#!/bin/bash
# Keeps a USB sound card out of the sound-card slots the sBitx needs.
# The radio expects its own card at 0 and the Loopback cards at 1, 2 and 3.
# A USB card present at switch-on could take one of those slots, and the radio
# then had garbled or no audio. This gives the USB card slot 5 every time.
# Run once:  bash fix_usb_audio.sh     then restart the Pi.
# Undo:      sudo rm /etc/modprobe.d/sbitx-usb-audio.conf   then restart.
CONF=/etc/modprobe.d/sbitx-usb-audio.conf
if [ -f "$CONF" ] && grep -q "snd-usb-audio index=5" "$CONF"; then
  echo "USB sound card fix already in place ($CONF)."
else
  echo "options snd-usb-audio index=5" | sudo tee "$CONF" >/dev/null \
    && echo "USB sound card fix installed ($CONF). Restart the Pi for it to take effect." \
    || echo "WARNING: could not write $CONF. Run:  echo \"options snd-usb-audio index=5\" | sudo tee $CONF"
fi
echo "Check after the restart with:  cat /proc/asound/cards   (the USB card should be number 5)"
