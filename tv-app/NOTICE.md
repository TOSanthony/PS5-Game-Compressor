# Notices for the Game Compressor TV app (`tv-app/`)

This directory is a native, full-screen PS5 application that drives the Game
Compressor payload (`game-compressor.elf`) through its local HTTP API on
`127.0.0.1:5910`. It is **free software under the GNU General Public License,
version 3 or (at your option) any later version** (see [LICENSE](LICENSE)).

The rest of this repository is not covered by this licence. The payload is a
separate program: it is started by the app and talks to it only over HTTP.

## Where the code comes from

This application is built on the TV app of **Orbit Store** (repository
`TOSanthony/orbit-store`, directory `app/`, GPL-3.0-or-later), which is itself
built on the **ps5-homebrew-ui** kit and **ps5-native-app-boilerplate** by
BlackBearReloaded (GPL-3.0-or-later). Copied from that tree, with their
original licence headers kept:

- `src/core`, `src/gfx`, `src/ui`, `src/audio`, `src/platform`, `src/runtime`,
  `src/third_party`: renderer, widgets, input, sound and the PS5 platform layer.
- `tooling/`: the native build and FSELF/`libc.prx` tooling (derived from
  SharpProspero, GPL-3.0).
- `tools/`: build, dependency and packaging scripts (adapted).
- `src/gc/http.*`, `json.*`, `thread.*`, `ui.*`, `starter.*`: small helpers
  adapted from Orbit Store's `app/src/orbit/` (renamed namespace, Orbit-specific
  parts removed).
- `assets/fonts`, `third_party/fonts`: the Inter typeface (SIL Open Font
  License 1.1), baked for the renderer.

Everything under `src/gc/` and `src/main.cpp` that is not listed above is new
code written for Game Compressor.

## Components used at build time

| Component | Licence | Notes |
|---|---|---|
| ps5-opengl SDK 1.0.0 (Mesa, OpenGNM, PSBC) | GPL-3.0-or-later, components under their own licences | Downloaded and hash-checked by `tools/fetch-opengl-sdk.sh`; notices in `licenses/ps5-opengl/` |
| PS5 payload SDK v0.42 | GPL-3.0-or-later and BSD-labelled files | Downloaded and hash-checked by `tools/setup-native-dependencies.sh` |
| libwebp 1.4.0 (PacBrew v0.40.2) | BSD-3-Clause | `licenses/libwebp-*` |
| LLVM libc++, libc++abi, libunwind, compiler-rt | Apache-2.0 with LLVM exceptions | `licenses/*-LLVM.txt` |
| zlib 1.3.2 | zlib licence | host packaging tool only |
| UFS2Tool | BSD-2-Clause | build-only, fetched at a pinned commit; not distributed |

The package also contains `game-compressor.elf`, built from this repository's
own sources (see the root of the repository for its terms).
