# shotref bridge

Apply a shotref preset to your camera with one tap.

```
iPhone (shotref in the browser) ──Wi-Fi──▶ bridge (Mac, PC or Raspberry Pi) ──Sony SDK──▶ α7C II
```

A browser can't talk to the camera directly (the camera speaks PTP, and Sony's SDK is a native
C/C++ library with no iOS/Android version). The bridge sits in between: it serves shotref to your
phone and turns a preset into SDK calls. It serves the page itself, so the phone talks to it
same-origin — an `https://` page could not call a plain-`http` device on your network.

The public shotref site is unchanged: without the bridge it makes no network requests at all.

## Try it now (no camera, no SDK)

```bash
python3 bridge/shotref_bridge.py
```

Open the address it prints (e.g. `http://192.168.1.20:8765`) on your phone, in the same Wi-Fi.
Open any genre → **Send to camera**. The mock backend simulates an α7C II. Only Python 3 is
needed, no extra packages.

Tests: `python3 -m unittest discover bridge`

## With the real camera

Written and compiled against **Camera Remote SDK v2.02.00**.

### 1. Build the helper (once)

Unpack the SDK zip for your platform **outside this repository** (the SDK must not be published,
see below), then unpack `RemoteCli.zip` from it. On a Mac:

```bash
brew install cmake
cd ~/CrSDK && unzip CrSDK_v2.02.00_*_Mac.zip && unzip RemoteCli.zip -d RemoteCli
cd /path/to/shotref/bridge/sdk-helper
cmake -B build -DCRSDK_DIR=$HOME/CrSDK/RemoteCli
cmake --build build
```

On macOS, the SDK libraries may be quarantined after download. If the helper won't start, see
*Removing macOS Quarantine Attributes* in the SDK's API reference.

### 2. Prepare the camera

- **USB** (most reliable): `MENU → Setup → USB → USB Connection Mode → Remote Shoot (PC Remote)`
- **Wi-Fi Direct**: `MENU → Network → Cnct./PC Remote → PC Remote Function → PC Remote: On`, then
  join the camera's Wi-Fi with the Mac (SSID/password under *Wi-Fi Direct Info.*). The camera must
  be re-connected from its menu after every power-on.
- **Wi-Fi access point** (camera and Mac in your home Wi-Fi): uses SSH. Set user and password under
  `Network → Network Option → Access Authen. Settings` and start the bridge with
  `SHOTREF_CAM_PASSWORD=... SHOTREF_CAM_USER=...`. The camera's SSH fingerprint is remembered on
  first connect; a changed fingerprint is refused.

First check with the SDK's own `RemoteCli` sample that the camera connects at all.

### 3. Run

```bash
python3 bridge/shotref_bridge.py --backend sdk
```

## How it behaves on the α7C II

- **Exposure mode is set remotely.** The helper switches the camera to *PC Remote* priority, so
  M/A follow the app instead of the mode dial — no need to turn the dial.
- **While connected, the dials don't win.** Tap *Give the dials back to the camera* in the app
  (or stop the bridge) to hand control back. The camera also does this itself when the session ends.
- The helper runs as one long-lived process so the session stays open between taps.
- Values are always picked from what the camera currently offers (e.g. *widest aperture* is read
  from the attached lens), and the app shows exactly what was applied.
- A lens' **AF/MF switch** is physical and still wins; the app reminds you when MF is needed.

## Raspberry Pi gadget: USB only

Sony's compatibility table: the α7C II supports Wi-Fi control, **but not with the Linux ARM
builds of the SDK** (Raspberry Pi). A Pi gadget therefore connects to the camera by **USB**; its
Wi-Fi is only for your phone. Sony lists the Raspberry Pi 4 (64-bit) as tested; a Pi Zero 2 W uses
the same CPU architecture but isn't on Sony's list.

## Licence notes (Sony Camera Remote SDK)

Not legal advice — summary of the licence that comes with the SDK:

- You may build and distribute (also sell) software that embeds the SDK library in binary form.
- **Never commit the SDK** (headers, libraries, zips) to this public repository. `.gitignore`
  excludes the usual paths.
- Before others use software built with the SDK, they must be told and agree that Sony considers
  the camera **out of manufacturer warranty** once it is controlled through such software, and
  agree to the licence's prohibited-use clause. Don't present the software as made or supported
  by Sony.

## Security

- Answers only to IP addresses, `localhost`, `*.local` or names passed with `--name`
  (protection against DNS rebinding).
- Accepts only same-origin JSON; every preset value is checked against a whitelist.
- No accounts, no cloud. Don't run it on public Wi-Fi: anyone on the same network can open it.

## API

| Request | Answer |
|---|---|
| `GET /` | shotref, with `<meta name="shotref-bridge">` injected |
| `GET /api/status` | `{"connected": true, "model": "...", "backend": "sdk"}` |
| `POST /api/apply` `{"genre": "astro", "preset": {...}}` | `{"results": [...], "manual": [...]}` |
| `POST /api/release` `{}` | `{"ok": true}` — dials back to the camera |
