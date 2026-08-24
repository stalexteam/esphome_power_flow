"""One run, one screenshot.

Triggers the panel's `esphome.<node>_screenshot` service over the Home
Assistant REST API, captures the base64 frame the firmware dumps into its
serial log, and writes a PNG. The firmware side is `debug_screenshot: true`
on the `power_flow:` block (see tools.md).

Usage:
    python screenshot.py --port COM7 --env-file ../DEV/ha.env --node bms-panel
    python screenshot.py --port COM7 --no-trigger        # trigger by hand in HA

HA_URL and HA_TOKEN come from --env-file or the environment. Requires
pyserial and Pillow; numpy is optional but makes decoding instant.
"""

import argparse
import base64
import os
import re
import sys
import time
import urllib.request
import zlib

import serial

# ESPHome colours its log lines; strip the ANSI escapes before matching.
ANSI_RE = re.compile(r"\x1b\[[0-9;]*m")
LINE_RE = re.compile(r"\[pf\.screenshot:\d+\]:\s*(.*)$")
BEGIN_RE = re.compile(
    r"BEGIN w=(\d+) h=(\d+) stride=(\d+) fmt=(\S+) len=(\d+) crc=([0-9a-f]{8})"
)


def read_env_file(path):
    values = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line and not line.startswith("#") and "=" in line:
                key, _, value = line.partition("=")
                value = value.strip()
                if value[:1] in "'\"" and value.find(value[0], 1) > 0:
                    value = value[1 : value.find(value[0], 1)]
                else:
                    value = value.split("#", 1)[0].strip()
                values[key.strip()] = value
    return values


def trigger(ha_url, token, node):
    service = "esphome/{}_screenshot".format(node.replace("-", "_"))
    req = urllib.request.Request(
        "{}/api/services/{}".format(ha_url.rstrip("/"), service),
        data=b"{}",
        headers={
            "Authorization": "Bearer " + token,
            "Content-Type": "application/json",
        },
    )
    urllib.request.urlopen(req, timeout=10).read()


def receive(s, timeout, on_stall=None):
    """Read the serial log until one complete BEGIN..END frame arrives.

    A service call that lands while the panel's HA connection is
    re-establishing is dropped silently, so if no BEGIN shows up within 15 s
    `on_stall` is invoked once to fire the trigger again."""
    start = time.time()
    deadline = start + timeout
    meta, chunks, buffer = None, [], b""
    while time.time() < deadline:
        if on_stall and meta is None and time.time() > start + 15:
            print("no frame yet, re-triggering once...")
            on_stall()
            on_stall = None
        buffer += s.read(4096)
        *lines, buffer = buffer.split(b"\n")
        for raw in lines:
            line = ANSI_RE.sub("", raw.decode("utf-8", "replace")).strip()
            m = LINE_RE.search(line)
            if m is None:
                continue
            payload = m.group(1)
            if payload.startswith("BEGIN"):
                b = BEGIN_RE.search(payload)
                if b is None:
                    raise SystemExit("unparseable BEGIN line: " + payload)
                meta, chunks = b.groups(), []
            elif payload.startswith("D") and meta is not None:
                chunks.append(payload[1:])
            elif payload.startswith("END") and meta is not None:
                return meta, "".join(chunks)
    raise SystemExit(
        "timed out after {}s without a complete frame - is "
        "`debug_screenshot: true` flashed, and did the service fire?".format(timeout)
    )


def decode(meta, b64):
    """Return (pixels, w, h, stride); raise ValueError if the capture lost
    bytes in transit (retryable), SystemExit on a protocol mismatch."""
    w, h, stride, fmt, length, crc = meta
    w, h, stride, length = int(w), int(h), int(stride), int(length)
    if fmt != "RGB565LE":
        raise SystemExit("unexpected pixel format: " + fmt)
    try:
        data = base64.b64decode(b64)
    except Exception as err:
        raise ValueError("base64 damaged in transit ({})".format(err)) from err
    if len(data) != length:
        raise ValueError("got {} bytes, expected {}".format(len(data), length))
    if zlib.crc32(data) & 0xFFFFFFFF != int(crc, 16):
        raise ValueError("CRC mismatch")
    return data, w, h, stride


def to_image(data, w, h, stride):
    from PIL import Image

    try:
        import numpy as np

        v = (
            np.frombuffer(data, dtype=np.uint8)
            .reshape(h, stride)[:, : w * 2]
            .reshape(h, w, 2)
            .astype(np.uint16)
        )
        v = v[:, :, 0] | (v[:, :, 1] << 8)
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        rgb = np.dstack(
            [(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)]
        ).astype(np.uint8)
        return Image.fromarray(rgb, "RGB")
    except ImportError:
        out = bytearray(w * h * 3)
        i = 0
        for y in range(h):
            row = data[y * stride : y * stride + w * 2]
            for x in range(w):
                v = row[2 * x] | (row[2 * x + 1] << 8)
                r, g, b = (v >> 11) & 0x1F, (v >> 5) & 0x3F, v & 0x1F
                out[i] = (r << 3) | (r >> 2)
                out[i + 1] = (g << 2) | (g >> 4)
                out[i + 2] = (b << 3) | (b >> 2)
                i += 3
        return Image.frombytes("RGB", (w, h), bytes(out))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--port", default="COM7", help="serial port (default COM7)")
    ap.add_argument("--node", default="bms-panel", help="ESPHome node name")
    ap.add_argument("--env-file", help="file with HA_URL=... and HA_TOKEN=...")
    ap.add_argument("-o", "--out", help="output PNG (default screenshot-<ts>.png)")
    ap.add_argument("--timeout", type=float, default=90, help="seconds to wait")
    ap.add_argument(
        "--no-trigger",
        action="store_true",
        help="only listen; fire the service yourself from HA Developer Tools",
    )
    args = ap.parse_args()

    env = dict(os.environ)
    if args.env_file:
        env.update(read_env_file(args.env_file))
    if not args.no_trigger and not (env.get("HA_URL") and env.get("HA_TOKEN")):
        ap.error("need HA_URL and HA_TOKEN (via --env-file or environment), "
                 "or --no-trigger")

    # The port must be open before the service fires, or the head of the
    # dump is lost.
    fire = None
    if not args.no_trigger:
        fire = lambda: trigger(env["HA_URL"], env["HA_TOKEN"], args.node)
    s = serial.Serial(args.port, 115200, timeout=1)
    try:
        s.reset_input_buffer()
        if args.no_trigger:
            print("listening on {} - fire esphome.{}_screenshot in HA".format(
                args.port, args.node.replace("-", "_")))
        for attempt in range(3):
            if fire is not None:
                fire()
                print("triggered, receiving (takes ~1 min)...")
            try:
                frame = decode(*receive(s, args.timeout, on_stall=fire))
                break
            except ValueError as err:
                # The serial link dropped bytes; the frame is gone, ask again.
                if fire is None or attempt == 2:
                    raise SystemExit("{} - giving up".format(err))
                print("{}; retrying...".format(err))
    finally:
        s.close()

    out = args.out or "screenshot-{}.png".format(time.strftime("%Y%m%d-%H%M%S"))
    to_image(*frame).save(out)
    print(out)


if __name__ == "__main__":
    main()
