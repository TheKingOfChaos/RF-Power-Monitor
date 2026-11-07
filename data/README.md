# LittleFS Data Directory

This folder is the source for `pio run -t uploadfs`, which writes it to the Pico's flash filesystem.

**It is not needed for normal use.** The firmware creates its settings on first boot. Uploading this folder replaces the whole filesystem and erases the settings stored on the device.

The files here are from the old v1 storage scheme and are kept for reference only. Current storage (two copies with a save sequence number, `data/settings_a.json` and `data/settings_b.json` on the device) is described in [docs/REFERENCE.md](../docs/REFERENCE.md#settings-storage).
