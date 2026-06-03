# α7C II Shot Reference
-test-
A free, offline shooting-settings reference for the Sony α7C II. Installable as a home-screen app on iPhone and iPad. Works fully offline once installed. No tracking, no analytics, no cost.

**[→ Open the app](https://marvin-rse.github.io/shotref/)**

<img width="1600" height="1101" alt="image" src="https://github.com/user-attachments/assets/d1a872f9-47cd-4cef-acab-28cd1c5bbd97" />


## What it does

Pick a genre and get the recommended camera settings, the exact menu path to find them on the α7C II, and a practical field tip. Eight genres cover most situations:

- Portrait — skin tones, Eye AF
- Street — quick, candid, sharp
- Landscape — sharp edge to edge
- Family & kids — fast indoors with subject detection
- Astrophotography — Milky Way and stars
- Long exposure — silky water, light trails
- Wildlife & sport — action and birds
- Macro — close-up detail

You can also upload your own reference photos per genre (stored locally on your device, never uploaded anywhere). Tap a photo to see it full-screen with its EXIF metadata.

## Install on iPhone or iPad

1. Open **[the link above](https://marvin-rse.github.io/shotref/)** in **Safari** (not Chrome — Add to Home Screen needs Safari).
2. Tap the **Share** button.
3. Scroll down and tap **Add to Home Screen**.
4. Done — the app launches full-screen, works offline, has its own icon.

## Themes

Three themes you can switch from Settings:

- **Black** — OLED-friendly dark mode for general use
- **White** — bright daylight reading
- **Red** — preserves your night vision when shooting astro or long exposures (auto-engages for those genres)

## Privacy

This app does not phone home. Once Safari has loaded the page, it makes zero network requests. Your uploaded reference photos live in your device's local storage and never leave. The page is marked `noindex` so search engines won't list it. A Content Security Policy locks the app to its own resources only.

## Tech

A single self-contained HTML file (~60 KB). Vanilla JavaScript, no dependencies, no build step. EXIF parsing is done in-browser when you upload a JPEG.

## Disclaimer

Not affiliated with Sony. The recommended settings are sensible defaults for typical situations; they are not gospel. Adjust to taste and conditions. The α and α7C II names are trademarks of Sony Group Corporation.

## License

Personal-use project. Take what's useful.
