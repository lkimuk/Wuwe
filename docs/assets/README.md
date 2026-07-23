# Brand assets

The asset tree separates deployable brand files from visual source exports:

- `brand/` contains the assets used by the README and documentation website.
- `source/` contains the original reference exports and is never published by
  the website build.

Use the transparent assets under `brand/` in product and documentation UI:

| Asset | Intended use |
| --- | --- |
| `brand/banner.png` | README and website social banner |
| `brand/wuwe-logo-light-transparent.png` | Full logo on light backgrounds |
| `brand/wuwe-logo-dark-transparent.png` | Full logo on dark backgrounds |
| `brand/wuwe-mark-light-transparent.png` | Standalone mark on light backgrounds |
| `brand/wuwe-mark-dark-transparent.png` | Standalone mark on dark backgrounds |
| `brand/favicon.ico` | Browser favicon |
| `brand/favicon-light.png` | Browser favicon for light browser chrome |
| `brand/favicon-dark.png` | Browser favicon for dark browser chrome |
| `brand/favicon-32.png` | Compact browser and UI icon |
| `brand/favicon-64.png` | High-density compact UI icon |
| `brand/favicon-192.png` | Web app and mobile icon |
| `brand/favicon-512.png` | High-resolution web app icon |

The files under `source/` have a baked checkerboard background. Keep them as
visual references, but do not use them directly in product or documentation UI.
