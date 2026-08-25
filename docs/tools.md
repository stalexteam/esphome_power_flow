# Tools

## Screenshots — `tools/screenshot.py`

Photographing a wall panel gives reflections and moiré; the panel can
instead hand over the exact frame it is showing. One run of the script,
one PNG:

```sh
python tools/screenshot.py --host bms-panel.local -o shot.png
```

`--host` and `--url` both accept a bare name, an IP or a full URL, and a URL
with no path of its own gets `/pf-screenshot.bmp` appended — the panel answers
a URL no handler claims by closing the connection with no response at all,
which is an unhelpful thing to be told for a missing path.

The panel serves the active LVGL screen at `/pf-screenshot.bmp` as a 16-bit
BMP — one capture per request, nothing cached — and the script converts it to
a PNG. A 480 × 800 frame is 768 KB and arrives in about a second. Whatever
screen is currently shown is what gets captured, so navigate the panel there
first.

`/pf-screenshot` is the same thing for a browser: a page with the image and a
**Capture** button. It takes one shot per press and one per reload, and
nothing else — the auto-refresh line is in the page source, commented out, so
that a tab left open on a wall panel cannot quietly keep re-rendering its
screen forever.

Firmware prerequisite, build-time. The example configs ship with it commented
out — this is debug tooling, not part of a production panel — so uncomment it
and reflash to use it:

```yaml
power_flow:
  debug_screenshot: true
```

That one line pulls in `web_server_base` and starts an HTTP listener on port
80 (shared with `web_server:` and `captive_portal:` if either is present), and
it compiles in LVGL's snapshot module. If the web server has authentication
configured, pass `--user` and `--password`.

Host prerequisites: `Pillow`; `numpy` is optional but makes decoding instant.

### How it works, and why that way

The snapshot has to happen on the main loop — ESPHome's LVGL has no lock and
the HTTP handler runs on the httpd task — so the handler parks on a state flag
while `loop()` renders into a PSRAM buffer, then sends it and frees it. Two
consequences worth knowing:

- a second request while one is in flight gets **503**; the script retries;
- if the main loop does not answer within five seconds the handler gives up
  with a 503 and leaves the buffer for `loop()` to free, so a wedged loop
  cannot leak a frame per request.

The BMP is `BI_BITFIELDS` RGB565 with a negative height — the byte order LVGL
already produced, with a 66-byte header written into the slack in front of it,
so nothing is copied or converted on the panel. Browsers and Pillow both read
it as it stands.

Before 2026-08-25 this went over the serial log as base64. That transport is
gone: the ESP-IDF USB-Serial/JTAG console silently drops characters once its
512-byte TX ring fills — it retries a full line once, waits 50 ms, and then
throws bytes away — so roughly one capture in three came back short or with
the base64 mangled, and a minute of dumping was wasted each time. HTTP is TCP;
there is nothing left to lose.
