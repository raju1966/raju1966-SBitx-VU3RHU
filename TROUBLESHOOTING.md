# Troubleshooting

| Problem | What to do |
|---|---|
| Audio garbled or gone after switching on with a USB card plugged in | Unplug the USB card and restart the Pi. Then run `bash fix_usb_audio.sh` once and restart. Check with `cat /proc/asound/cards`: the USB card must be number 5. |
| Audio is garbled (other cases) | Close every sbitx window and run one copy. Switch off NRVL, NBVL, FMNT, FVLY, AINR and the equalizers to test. Restart if needed. |
| No audio at all | Check USBS is OFF and AUDIO is not 0. Then check the sound card volume: `amixer -c 0 sset Master 95%`. |
| USB speaker or microphone will not switch on | Check the card is listed by `cat /proc/asound/cards`, then tap USBS or USBM again. The console line starting with `[USB]` says why. |
| "Device or resource busy" on the USB speaker | The desktop sound system holds the USB card. Switch USBS off, wait 10 seconds and tap it again; if it persists, unplug and replug the card. |
| Stray lines inside the menu boxes, or meter readings hidden behind MENU | Fixed on 2026-11-13. Install the current `sbitx_gtk.c`. |
| FT8 does not decode | Switch off all speech filters and equalizers. Use USB mode. |
| Voice boomy or distorted | Lower MIC gain, then lower MCMP, then reduce the 300 Hz band of the TX equalizer. |
| TX trace on the wrong side | USB is on the right and LSB on the left. If reversed, you have an old `sbitx.c`; reinstall. |
| Slow audio return after transmit | Reinstall the package `sbitx.c` and run `make clean && make`. |
| FreeDV stays on | Set FDVM to OFF and close the window with FDVW. |
| FDVM returns to OFF | codec2 is not installed. Run `bash freedv/install_freedv.sh`. |
| A new file seems to do nothing | Run `cd ~/sbitx && make clean && make && ./sbitx` so an old build is not reused. |

## Back up and go back
```
cp -r ~/sbitx ~/sbitx_backup_$(date +%Y%m%d)
cd ~ && mv sbitx sbitx_new && cp -r sbitx_backup_YYYYMMDD sbitx
cd ~/sbitx && make clean && make && ./sbitx
```

More detail: `docs/sBitx_User_Manual.pdf`.
