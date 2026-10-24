"""Tests for the shotref bridge (mock backend). Run: python3 -m unittest discover bridge"""

import http.client
import json
import sys
import threading
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import shotref_bridge as sb  # noqa: E402

PAGE = Path(__file__).resolve().parent.parent / "index.html"
ASTRO = {"mode": "M", "fnumber": "widest", "shutter": '15"', "iso": 3200, "wb": 4000, "focus": "MF", "drive": "timer_2s"}
PORTRAIT = {"mode": "A", "fnumber": 2, "shutter": None, "iso": "auto", "wb": "auto", "focus": "AF-C", "drive": "single"}


class ValidationTest(unittest.TestCase):
    def test_valid_presets(self):
        self.assertEqual(sb.validate_preset(ASTRO)["fnumber"], "widest")
        self.assertIsNone(sb.validate_preset(PORTRAIT)["shutter"])

    def test_rejects_bad_values(self):
        bad = [
            {**ASTRO, "mode": "S"},
            {**ASTRO, "fnumber": 0.5},
            {**ASTRO, "fnumber": True},
            {**ASTRO, "shutter": "1/0.5; rm -rf"},
            {**ASTRO, "shutter": None},
            {**ASTRO, "iso": 10},
            {**ASTRO, "wb": 20000},
            {**ASTRO, "focus": "AF-X"},
            {**ASTRO, "drive": "burst"},
            "not a dict",
        ]
        for p in bad:
            with self.subTest(p=p), self.assertRaises(sb.PresetError):
                sb.validate_preset(p)

    def test_manual_steps(self):
        steps = sb.manual_steps(sb.validate_preset(PORTRAIT), "M")
        self.assertIn("Turn the mode dial to A.", steps)
        self.assertEqual(sb.manual_steps(sb.validate_preset(ASTRO), "M"), ["If your lens has an AF/MF switch, set it to MF."])

    def test_host_guard(self):
        for ok in ["192.168.1.20:8765", "localhost:8765", "shotref.local", "[::1]:8765"]:
            self.assertTrue(sb.host_is_local(ok), ok)
        for bad in ["evil.example.com", "", "attacker.com:8765"]:
            self.assertFalse(sb.host_is_local(bad), bad)


class HttpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.server = sb.ThreadingHTTPServer(("127.0.0.1", 0), sb.make_handler(sb.Bridge(sb.MockCamera(), PAGE)))
        cls.port = cls.server.server_address[1]
        threading.Thread(target=cls.server.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()

    def req(self, method, path, body=None, headers=None):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        h = {"Host": f"127.0.0.1:{self.port}"}
        if body is not None:
            body = json.dumps(body)
            h["Content-Type"] = "application/json"
        h.update(headers or {})
        c.request(method, path, body=body, headers=h)
        r = c.getresponse()
        data = r.read()
        c.close()
        return r.status, data

    def test_serves_page_with_bridge_marker(self):
        status, data = self.req("GET", "/")
        self.assertEqual(status, 200)
        self.assertIn(sb.BRIDGE_META.encode(), data)

    def test_status(self):
        status, data = self.req("GET", "/api/status")
        st = json.loads(data)
        self.assertEqual((status, st["connected"], st["backend"]), (200, True, "mock"))

    def test_apply(self):
        status, data = self.req("POST", "/api/apply", {"genre": "astro", "preset": ASTRO},
                                {"Origin": f"http://127.0.0.1:{self.port}"})
        out = json.loads(data)
        self.assertEqual(status, 200)
        self.assertTrue(all(r["ok"] for r in out["results"]))
        self.assertIn("f/1.8", [r["message"] for r in out["results"]])

    def test_rejects_cross_origin(self):
        status, _ = self.req("POST", "/api/apply", {"preset": ASTRO}, {"Origin": "https://evil.example.com"})
        self.assertEqual(status, 403)

    def test_rejects_foreign_host(self):
        status, _ = self.req("GET", "/api/status", headers={"Host": "evil.example.com"})
        self.assertEqual(status, 403)

    def test_rejects_non_json_and_bad_preset(self):
        c = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        c.request("POST", "/api/apply", body="preset=x", headers={"Content-Type": "text/plain"})
        self.assertEqual(c.getresponse().status, 415)
        c.close()
        status, data = self.req("POST", "/api/apply", {"preset": {**ASTRO, "iso": 1}})
        self.assertEqual(status, 400)
        self.assertIn("iso", json.loads(data)["error"])


if __name__ == "__main__":
    unittest.main()
