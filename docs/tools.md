# Tools

## Screenshots — `tools/screenshot.py`

Photographing a wall panel gives reflections and moiré; the panel can
instead hand over the exact frame it is showing. One run of the script,
one PNG:

```sh
python tools/screenshot.py --port COM7 --env-file path/to/ha.env --node bms-panel
```

The script calls the panel's `esphome.<node>_screenshot` service through the
Home Assistant REST API (`HA_URL` and `HA_TOKEN` from `--env-file` or the
environment), and the firmware renders the active LVGL screen into a PSRAM
buffer and drains it into its own serial log as base64 — a few lines per loop
pass, so the panel stays responsive. The script reassembles the frame,
verifies the CRC, and writes the PNG. The whole round trip is about a
minute. Whatever screen is currently shown is what gets captured, so
navigate the panel there first.

With `--no-trigger` the script only listens on the serial port; fire the
service yourself from HA *Developer Tools → Actions*.

Firmware prerequisites, both build-time. The example config ships with them
commented out — this is debug tooling, not part of a production panel — so
uncomment both and reflash to use it:

```yaml
api:
  custom_services: true    # compiles in the api's user-service machinery
power_flow:
  debug_screenshot: true   # registers the service, enables LVGL's snapshot
```

Host prerequisites: `pyserial` and `Pillow`; `numpy` is optional but makes
decoding instant.
