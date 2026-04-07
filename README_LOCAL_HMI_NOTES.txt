
This refactor is based on the currently stable master branch with:
- filename-only firmware validation
- batch OTA on master
- options page
- lazy firmware list loading

New modules added:
- storage_backend.* : wraps onboard SD_MMC on Waveshare 7B with SPI SD fallback
- timekeeper.*     : NTP + last-known-time fallback via NVS
- local_ui.*       : primary local HMI for the 7B using LVGL and Waveshare helper symbols

Important dependency note:
The local HMI path expects the official Waveshare 7B Arduino demo libraries + LVGL 8.4 to be installed.
If the helper symbols are not linked, the firmware still runs and the Web UI remains available, but the local HMI stays disabled.
