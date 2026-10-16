# Full-screen launch: feasibility notes

Branch: `fullscreen-app`. Status: **experimental, builds, not tested on a console.**

## Why the tile is not full screen

The home-screen tile (`PSGC50001`) has no `eboot.bin`. Its `param.json` only
contains a `deeplinkUri` (`http://127.0.0.1:5910/`), so the system opens the
URL in its small built-in web-view window. The size of that window is chosen by
the system, not by the page, so no HTML/CSS change can make it full screen.

The payload ELF itself has no window: it runs the web server and the worker.
Packaging it as an ffpkg would only change how it is installed or mounted.

## Options

1. **Open the full system web browser app** (implemented here as a prototype).
   `sceSystemServiceLaunchWebBrowser(url, NULL)` is the call used by the
   ps5-payload-sdk `browser` sample. The browser is a full application, not
   the small web-view window. Whether it satisfies "full screen" (it still has
   the browser's own UI) is unverified.
2. **Real application with its own `eboot.bin`.** The SDK `install_app` sample
   shows how to build a fake-signed `eboot.bin` and install a title with
   `sceAppInstUtilAppInstallTitleDir`. A native UI would need its own renderer
   (VideoOut / GPU libraries are present in the SDK); re-implementing the
   existing HTML interface natively is a large project and has no path to
   embedding a web view that I could confirm.
3. **Other `param.json` values.** No field that sets the web-view size is known.

## What this branch adds

- `gc_launcher_open_system_browser()` in `src/gc_app_installer.c`: resolves
  `sceSystemServiceLaunchWebBrowser` from `libSceSystemService` and opens the
  web UI URL.
- `GET /api/control/open-browser` in `src/gc_websrv.c`: calls the function and
  answers `{"ok":true|false,"rc":N}`. Nothing calls it automatically, so the
  default behaviour of the app is unchanged.
- `tools/fullscreen-launcher/`: a freestanding, syscall-only `eboot.bin` that
  connects to `127.0.0.1:5910`, requests `/api/control/open-browser` and exits.
  It uses the linker script and `make_fself.py` from the SDK samples.

## Build

```sh
export PS5_PAYLOAD_SDK=/path/to/ps5-payload-sdk   # v0.43 or newer
make                                              # game-compressor.elf
make -C tools/fullscreen-launcher                 # out/eboot.bin
```

Both build cleanly with SDK v0.43 and Clang 18.

## Test without installing anything

1. Send `game-compressor.elf` to the console as usual.
2. From any device on the network, request
   `http://<ps5-ip>:5910/api/control/open-browser`.
3. If the system browser opens on the console, option 1 works. Check the
   response `rc` and `/data/.../log` entries starting with
   `open system browser` otherwise.

## Not done / open questions

- Not run on a console. It is unknown whether the call works from the
  payload's web-server thread (the SDK sample calls it from `main` after
  `sceUserServiceInitialize`).
- The eboot is not wired into the tile installer. Installing a title with an
  `eboot.bin` (files under `/system_ex/app/<TITLE_ID>`, as in the SDK sample)
  changes how the tile is installed and needs on-console testing first.
- Whether the system browser app is acceptable as "full screen".
