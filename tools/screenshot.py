"""One request, one screenshot.

Fetches `/pf-screenshot.bmp` from the panel — the active LVGL screen, captured
on demand and served as a 16-bit BMP — and writes it out as a PNG. The
firmware side is `debug_screenshot: true` on the `power_flow:` block, which
also serves a one-page viewer at `/pf-screenshot` (see tools.md).

Usage:
    python screenshot.py --host bms-panel.local
    python screenshot.py --host 192.168.1.42 -o shot.png
    python screenshot.py --url http://panel/pf-screenshot.bmp --user admin --password s3cr3t

Requires Pillow; numpy is optional but makes decoding instant.
"""

import argparse
import base64
import struct
import time
import urllib.error
import urllib.parse
import urllib.request

BMP_PATH = "/pf-screenshot.bmp"
# BITMAPFILEHEADER + BITMAPINFOHEADER + the three BI_BITFIELDS masks, which is
# also where the pixels start in the file the firmware writes.
HEADER_LEN = 14 + 40 + 12
BI_BITFIELDS = 3
RGB565_MASKS = (0xF800, 0x07E0, 0x001F)


def endpoint(target):
    """Turn whatever the caller typed into the BMP URL.

    A bare host, a host with a scheme, a trailing slash — all mean the same
    thing here, and the panel answers a URL it has no handler for by closing
    the connection with no response at all, which is a baffling thing to be
    told when the only mistake was leaving the path off."""
    if "://" not in target:
        target = "http://" + target
    parts = urllib.parse.urlsplit(target)
    if parts.path in ("", "/"):
        parts = parts._replace(path=BMP_PATH)
    return urllib.parse.urlunsplit(parts)


def fetch(url, user, password, timeout):
    req = urllib.request.Request(url)
    if user is not None:
        token = base64.b64encode("{}:{}".format(user, password or "").encode()).decode()
        req.add_header("Authorization", "Basic " + token)
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        return resp.read()


def parse_bmp(data):
    """Return (pixels, w, h, stride) for the firmware's RGB565 BMP.

    Everything here is a property of the file the panel writes, so a mismatch
    means the two halves of this tool have drifted apart, not that a capture
    went wrong: say which field disagrees rather than guessing at the pixels."""
    if len(data) < HEADER_LEN or data[:2] != b"BM":
        raise SystemExit("not a BMP - got {} bytes starting {!r}".format(len(data), data[:16]))
    offbits, hdr_size, w, h, planes, bpp, compression = struct.unpack_from("<IIiiHHI", data, 10)
    masks = struct.unpack_from("<3I", data, 14 + hdr_size)
    if hdr_size != 40 or planes != 1:
        raise SystemExit("unexpected BMP header: size={} planes={}".format(hdr_size, planes))
    if bpp != 16 or compression != BI_BITFIELDS or masks != RGB565_MASKS:
        raise SystemExit(
            "expected RGB565 bitfields, got bpp={} compression={} masks={}".format(
                bpp, compression, tuple(hex(m) for m in masks)
            )
        )
    top_down, h = h < 0, abs(h)
    stride = ((w * bpp + 31) // 32) * 4
    expected = offbits + stride * h
    if len(data) != expected:
        raise SystemExit("truncated: {} bytes, header says {}".format(len(data), expected))
    pixels = data[offbits:]
    if not top_down:
        pixels = b"".join(pixels[y * stride : (y + 1) * stride] for y in range(h - 1, -1, -1))
    return pixels, w, h, stride


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
    ap.add_argument("--host", help="panel hostname or IP, e.g. bms-panel.local")
    ap.add_argument("--url", help="full URL; a bare host works too, and a URL "
                                  "with no path gets " + BMP_PATH)
    ap.add_argument("--user", help="username, if the web server has auth")
    ap.add_argument("--password", help="password, if the web server has auth")
    ap.add_argument("-o", "--out", help="output PNG (default screenshot-<ts>.png)")
    ap.add_argument("--timeout", type=float, default=20, help="seconds per request")
    ap.add_argument("--retries", type=int, default=3, help="attempts before giving up")
    args = ap.parse_args()

    if args.url or args.host:
        url = endpoint(args.url or args.host)
    else:
        ap.error("need --host or --url")

    for attempt in range(args.retries):
        try:
            data = fetch(url, args.user, args.password, args.timeout)
            break
        except urllib.error.HTTPError as err:
            # 503 is the panel saying another capture is in flight, or that its
            # main loop has not answered yet; both are worth one more try.
            retryable = err.code == 503
            body = err.read().decode("utf-8", "replace").strip()
            message = "{} {} - {}".format(err.code, err.reason, body)
        except OSError as err:
            retryable = True
            message = str(err)
            if "closed connection" in message:
                # What the panel does with a URL no handler claims. Reaching
                # this after the retries almost always means the wrong path.
                message += " (no handler for this URL - expected {})".format(BMP_PATH)
        if not retryable or attempt == args.retries - 1:
            raise SystemExit("{}: {}".format(url, message))
        print("{}; retrying...".format(message))
        time.sleep(1)

    out = args.out or "screenshot-{}.png".format(time.strftime("%Y%m%d-%H%M%S"))
    to_image(*parse_bmp(data)).save(out)
    print(out)


if __name__ == "__main__":
    main()
