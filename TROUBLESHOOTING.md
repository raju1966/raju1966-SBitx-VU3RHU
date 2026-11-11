# Troubleshooting

| Problem | What to do |
|---|---|
| Audio is garbled | Close every sbitx window and run one copy. Plug in the USB card only after the radio starts. Check the filter settings (switch off NRVL, NBVL, FMNT, FVLY, AINR and the equalizers to test). Restart if needed. |
| No audio at all | Check USBS is OFF and AUDIO is not 0. Then check the sound card volume: `amixer -c 0 sset Master 95%`. |
| USB speaker or microphone will not switch on | Start the radio first, plug the USB card in, then tap USBS or USBM. The console line starting with `[USB]` says why. |
| "Device or resource busy" on the USB speaker | The desktop sound system holds the USB card. Plug the card in after the radio is running. |
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
