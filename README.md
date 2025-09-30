# ESP32-C5 – Wi-Fi 6 TWT + BLE 5 Long Range (LE Coded) Test

Minimal working setup I would actually ship into a lab device: one binary showcasing **Wi-Fi 6 iTWT** on STA and **BLE 5 LE Coded** extended advertising for long range beacons. (Theoretically ~100m in open space).

## What it does

- Joins your **Wi-Fi 6** AP as STA, enables **modem power save**, negotiates **individual TWT** (~10.5 s service period, ~200 ms awake).
- Every ~10 s performs a tiny HTTP HEAD request to illustrate "do useful work during awake window".
- Starts BLE **Extended Advertising** on **LE Coded PHY** (primary + secondary) with a minimal payload (Flags + 0x181A UUID). Non-connectable, non-scannable -> beacon-like.

## Hardware / SDK

- Target: **ESP32-C5**
- **ESP-IDF ≥ 5.5.x** (tested with 5.5 series)
- Your AP must support **Wi-Fi 6 TWT**.

## Build & flash

```bash
# 1) New project or copy to your existing tree
idf.py set-target esp32c5

# 2) Configure Wi-Fi creds (or edit in main.c)
idf.py menuconfig

# 3) Build/flash/monitor
idf.py build flash monitor
