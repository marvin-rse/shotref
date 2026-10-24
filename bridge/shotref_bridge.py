#!/usr/bin/env python3
"""shotref bridge: serves the shotref app on the local network and applies presets to a Sony camera.

    iPhone (shotref in the browser) --Wi-Fi--> this bridge --Sony Camera Remote SDK--> camera

The bridge serves index.html itself, so the phone talks to it same-origin. A page loaded from
https://github.io could not call a plain-http device on the local network (mixed content).

Backends
  mock  Simulated camera. Runs anywhere, no SDK needed. Use it to try the whole flow.
  sdk   Calls the small C++ helper in sdk-helper/, which links Sony's Camera Remote SDK.

Only the Python standard library is used, so it runs the same on a laptop and a Raspberry Pi.
"""

import argparse
import ipaddress
import json
import logging
import re
import socket
import subprocess
import threading
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import urlsplit

log = logging.getLogger("shotref-bridge")

MAX_BODY = 4096
BRIDGE_META = '<meta name="shotref-bridge" content="1">'

MODES = {"M", "A"}
WB_PRESETS = {"auto", "daylight", "shade", "cloudy", "tungsten", "fluorescent"}
FOCUS_MODES = {"AF-S", "AF-C", "MF"}
DRIVES = {"single", "cont_lo", "cont_hi_plus", "timer_2s"}
SHUTTER_RE = re.compile(r'^(1/\d{1,4}|\d{1,2}(\.\d)?"|bulb)$')

# Settings the camera can take remotely, in the order they are applied.
SETTABLE = ["fnumber", "shutter", "iso", "wb", "focus", "drive"]


class PresetError(ValueError):
    pass


def validate_preset(raw):
    """Return a cleaned preset or raise PresetError. Never trust what the page sends."""
    if not isinstance(raw, dict):
        raise PresetError("preset must be an object")
    p = {}
    mode = raw.get("mode")
    if mode not in MODES:
        raise PresetError("mode must be M or A")
    p["mode"] = mode

    f = raw.get("fnumber")
    if f == "widest":
        p["fnumber"] = "widest"
    elif isinstance(f, (int, float)) and not isinstance(f, bool) and 0.95 <= f <= 32:
        p["fnumber"] = round(float(f), 1)
    else:
        raise PresetError("fnumber must be 'widest' or between 0.95 and 32")

    s = raw.get("shutter")
    if s is None:
        if mode == "M":
            raise PresetError("shutter is required in M mode")
        p["shutter"] = None
    elif isinstance(s, str) and SHUTTER_RE.match(s):
        p["shutter"] = s
    else:
        raise PresetError('shutter must look like 1/250, 15" or bulb')

    iso = raw.get("iso")
    if iso == "auto" or (isinstance(iso, int) and not isinstance(iso, bool) and 50 <= iso <= 204800):
        p["iso"] = iso
    else:
        raise PresetError("iso must be 'auto' or 50-204800")

    wb = raw.get("wb")
    if wb in WB_PRESETS or (isinstance(wb, int) and not isinstance(wb, bool) and 2500 <= wb <= 9900):
        p["wb"] = wb
    else:
        raise PresetError("wb must be a preset name or 2500-9900 K")

    if raw.get("focus") not in FOCUS_MODES:
        raise PresetError("focus must be AF-S, AF-C or MF")
    p["focus"] = raw["focus"]

    if raw.get("drive") not in DRIVES:
        raise PresetError("unknown drive mode")
    p["drive"] = raw["drive"]
    return p


def manual_steps(preset, dial_mode):
    """Things the camera cannot be told remotely, worded for the photographer."""
    steps = []
    if dial_mode != preset["mode"]:
        steps.append(f"Turn the mode dial to {preset['mode']}.")
    if preset["focus"] == "MF":
        steps.append("If your lens has an AF/MF switch, set it to MF.")
    if preset["shutter"] == "bulb":
        steps.append("Shutter is on BULB: time the exposure with a remote or the self-timer.")
    return steps


# ---------------------------------------------------------------- backends

class MockCamera:
    """Simulated α7C II so the whole flow can be tried without a camera or the SDK."""

    name = "mock"

    def __init__(self):
        self.state = {"dial": "M", "fnumber": 4.0, "shutter": "1/125", "iso": "auto",
                      "wb": "auto", "focus": "AF-S", "drive": "single"}
        self.widest = 1.8

    def status(self):
        return {"connected": True, "model": "ILCE-7CM2 (simulated)", "dial": self.state["dial"]}

    def apply(self, preset):
        results = []
        for key in SETTABLE:
            value = preset[key]
            if value is None:
                continue
            if key == "fnumber" and value == "widest":
                value = self.widest
            if key == "shutter" and preset["mode"] == "A":
                results.append({"setting": key, "ok": False, "message": "not used in A mode"})
                continue
            self.state[key] = value
            results.append({"setting": key, "ok": True, "message": _fmt(key, value)})
        return {"results": results, "dial": self.state["dial"]}


class SdkHelperCamera:
    """Talks to sdk-helper (C++, links Sony's SDK) via JSON on stdin/stdout.

    Contract:
      shotref-cam status          -> {"connected": bool, "model": str, "dial": "M"|"A"|...}
      shotref-cam apply < preset  -> {"results": [{"setting","ok","message"}], "dial": str}
    """

    name = "sdk"

    def __init__(self, helper, timeout=20):
        self.helper = str(helper)
        self.timeout = timeout

    def _run(self, *args, payload=None):
        try:
            proc = subprocess.run([self.helper, *args], input=json.dumps(payload) if payload else None,
                                  capture_output=True, text=True, timeout=self.timeout)
        except FileNotFoundError:
            raise RuntimeError(f"SDK helper not found at {self.helper}. Build it first (see sdk-helper/README.md).")
        except subprocess.TimeoutExpired:
            raise RuntimeError("camera did not answer in time")
        if proc.returncode != 0:
            raise RuntimeError((proc.stderr or "SDK helper failed").strip().splitlines()[-1])
        return json.loads(proc.stdout)

    def status(self):
        try:
            return self._run("status")
        except RuntimeError as e:
            return {"connected": False, "error": str(e)}

    def apply(self, preset):
        return self._run("apply", payload=preset)


def _fmt(key, v):
    if key == "fnumber":
        return f"f/{v:g}"
    if key == "shutter":
        return v.replace('"', " s") if '"' in v else (v if v == "bulb" else v + " s")
    if key == "iso":
        return "ISO auto" if v == "auto" else f"ISO {v}"
    if key == "wb":
        return f"{v} K" if isinstance(v, int) else v
    if key == "drive":
        return {"single": "Single", "cont_lo": "Continuous Lo", "cont_hi_plus": "Continuous Hi+",
                "timer_2s": "Self-timer 2 s"}.get(v, v)
    return str(v)


# ---------------------------------------------------------------- http

def host_is_local(host_header, extra_names=()):
    """Guard against DNS rebinding: only answer to IPs, localhost, *.local or configured names."""
    host = urlsplit("//" + (host_header or "")).hostname or ""
    if host in ("localhost", *extra_names) or host.endswith(".local"):
        return True
    try:
        ipaddress.ip_address(host)
        return True
    except ValueError:
        return False


class Bridge:
    def __init__(self, camera, page_path, extra_names=()):
        self.camera = camera
        self.page_path = Path(page_path)
        self.extra_names = tuple(extra_names)
        self.lock = threading.Lock()  # one camera conversation at a time

    def page(self):
        html = self.page_path.read_text(encoding="utf-8")
        return html.replace("</head>", BRIDGE_META + "\n</head>", 1).encode("utf-8")


def make_handler(bridge):
    class Handler(BaseHTTPRequestHandler):
        server_version = "shotref-bridge"
        sys_version = ""

        def log_message(self, fmt, *args):
            log.info("%s %s", self.address_string(), fmt % args)

        def _send(self, status, body, ctype="application/json"):
            if isinstance(body, (dict, list)):
                body = json.dumps(body).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.send_header("X-Content-Type-Options", "nosniff")
            self.send_header("Referrer-Policy", "no-referrer")
            self.end_headers()
            self.wfile.write(body)

        def _guard(self):
            if not host_is_local(self.headers.get("Host"), bridge.extra_names):
                self._send(HTTPStatus.FORBIDDEN, {"error": "unexpected host"})
                return False
            return True

        def do_GET(self):
            if not self._guard():
                return
            path = urlsplit(self.path).path
            if path in ("/", "/index.html"):
                self._send(HTTPStatus.OK, bridge.page(), "text/html; charset=utf-8")
            elif path == "/api/status":
                with bridge.lock:
                    st = bridge.camera.status()
                st["backend"] = bridge.camera.name
                self._send(HTTPStatus.OK, st)
            elif path == "/robots.txt":
                self._send(HTTPStatus.OK, b"User-agent: *\nDisallow: /\n", "text/plain")
            else:
                self._send(HTTPStatus.NOT_FOUND, {"error": "not found"})

        def do_POST(self):
            if not self._guard():
                return
            if urlsplit(self.path).path != "/api/apply":
                return self._send(HTTPStatus.NOT_FOUND, {"error": "not found"})
            # Same-origin only. A JSON content type also forces a CORS preflight for any
            # cross-site page, which this server never approves.
            origin = self.headers.get("Origin")
            if origin and urlsplit(origin).netloc != self.headers.get("Host"):
                return self._send(HTTPStatus.FORBIDDEN, {"error": "cross-origin request refused"})
            if (self.headers.get("Content-Type") or "").split(";")[0].strip() != "application/json":
                return self._send(HTTPStatus.UNSUPPORTED_MEDIA_TYPE, {"error": "send JSON"})
            try:
                length = int(self.headers.get("Content-Length") or 0)
            except ValueError:
                length = -1
            if not 0 < length <= MAX_BODY:
                return self._send(HTTPStatus.BAD_REQUEST, {"error": "bad request size"})
            try:
                body = json.loads(self.rfile.read(length))
                preset = validate_preset(body.get("preset"))
            except (json.JSONDecodeError, AttributeError):
                return self._send(HTTPStatus.BAD_REQUEST, {"error": "invalid JSON"})
            except PresetError as e:
                return self._send(HTTPStatus.BAD_REQUEST, {"error": str(e)})
            try:
                with bridge.lock:
                    out = bridge.camera.apply(preset)
            except RuntimeError as e:
                log.warning("apply failed: %s", e)
                return self._send(HTTPStatus.BAD_GATEWAY, {"error": str(e)})
            out["manual"] = manual_steps(preset, out.get("dial"))
            log.info("applied %s: %s", body.get("genre", "?"),
                     ", ".join(f"{r['setting']}={'ok' if r['ok'] else 'FAIL'}" for r in out["results"]))
            self._send(HTTPStatus.OK, out)

    return Handler


def local_ip():
    """Best guess at this machine's LAN address, for the startup message. Sends no packets."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("192.0.2.1", 9))  # TEST-NET address, never routed
        return s.getsockname()[0]
    except OSError:
        return "127.0.0.1"
    finally:
        s.close()


def main():
    here = Path(__file__).resolve().parent
    ap = argparse.ArgumentParser(description="Serve shotref on the local network and apply presets to a Sony camera.")
    ap.add_argument("--backend", choices=["mock", "sdk"], default="mock", help="mock = simulated camera (default)")
    ap.add_argument("--helper", default=str(here / "sdk-helper" / "build" / "shotref-cam"), help="path to the SDK helper")
    ap.add_argument("--host", default="0.0.0.0", help="address to listen on (default: all)")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--page", default=str(here.parent / "index.html"), help="path to shotref's index.html")
    ap.add_argument("--name", action="append", default=[], help="extra hostname to accept, e.g. shotref.lan")
    args = ap.parse_args()

    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(message)s", datefmt="%H:%M:%S")
    camera = MockCamera() if args.backend == "mock" else SdkHelperCamera(args.helper)
    bridge = Bridge(camera, args.page, args.name)
    server = ThreadingHTTPServer((args.host, args.port), make_handler(bridge))
    log.info("shotref bridge (%s backend) running", camera.name)
    log.info("Open on your phone (same Wi-Fi): http://%s:%d", local_ip(), args.port)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
