# shotref bridge

Apply a shotref preset to your camera with one tap.

```
iPhone (shotref in the browser) ──Wi-Fi──▶ bridge (laptop or Raspberry Pi) ──Sony SDK──▶ α7C II
```

A browser can't talk to the camera directly (the camera speaks a binary protocol, and Sony's SDK
is a native library). The bridge sits in between: it serves shotref to your phone and translates
a preset into SDK calls. It also serves the page itself, so the phone talks to it same-origin —
an `https://` page could not call a plain-`http` device on your network.

The public shotref site is unchanged: without the bridge it makes no network requests at all.

## Try it now (no camera, no SDK)

```bash
python3 bridge/shotref_bridge.py
```

Open the address it prints (e.g. `http://192.168.1.20:8765`) on your phone, in the same Wi-Fi.
Open any genre → **Send to camera**. The mock backend simulates an α7C II, so you can try the
whole flow. Only Python 3 is needed, no extra packages.

Run the tests: `python3 -m unittest discover bridge`

## With the real camera

1. **Get the SDK.** Apply for the Camera Remote SDK on Sony's developer site and unpack it.
2. **First test with Sony's own sample.** Build the `RemoteCli` sample that ships with the SDK and
   check it can change a setting on the α7C II:
   - over USB: camera `MENU → Setup → USB Connection → PC Remote`
   - over Wi-Fi: `MENU → Network → Cnct./PC Remote → PC Remote Function → PC Remote: On`,
     then connect via Wi-Fi Direct (SSID and password under *Wi-Fi Direct Info.*)

   If Wi-Fi fails but USB works, use USB — the bridge works the same either way.
3. **Build the helper:**
   ```bash
   cd bridge/sdk-helper
   cmake -B build -DCRSDK_DIR=/path/to/CrSDK && cmake --build build
   ```
   `main.cpp` is a skeleton: aperture, shutter and ISO are written out, white balance, focus
   mode and drive mode are marked `TODO`. Every SDK call marked `VERIFY` must be checked
   against the API reference of your SDK version — it could not be compiled without the SDK.
4. **Run the bridge with the SDK backend:**
   ```bash
   python3 bridge/shotref_bridge.py --backend sdk
   ```

## What can't be set remotely

The **mode dial** (M / A) and the lens' **AF/MF switch** are physical. The bridge tells you on the
phone when you need to turn them.

## Security

- Listens on your local network only and answers only to IP addresses, `localhost`, `*.local` or
  names passed with `--name` (protection against DNS rebinding).
- Accepts presets only as same-origin JSON; every value is checked against a whitelist before it
  reaches the camera.
- No accounts, no cloud, no data leaves your network.

Don't run it on public Wi-Fi: anyone on the same network can open the page.

## API

| Request | Answer |
|---|---|
| `GET /` | shotref, with `<meta name="shotref-bridge">` injected |
| `GET /api/status` | `{"connected": true, "model": "...", "backend": "mock"}` |
| `POST /api/apply` `{"genre": "astro", "preset": {...}}` | `{"results": [...], "manual": [...]}` |
