# Third-party notices

Rovox is licensed under the GNU Affero General Public License v3.0 or later (see `LICENSE`).

## Bundled as git submodules

| Component | Location | License |
|---|---|---|
| Tracktion Engine | `third_party/tracktion_engine` | GPL-3.0-or-later (or commercial); see its `LICENSE.md` |
| JUCE 9 | `third_party/tracktion_engine/modules/juce` | AGPL-3.0 (or commercial JUCE EULA); see its `LICENSE.md` |

Tracktion Engine in turn ships third-party code under `third_party/tracktion_engine/modules/3rd_party`
(for example SoundTouch, Signalsmith Stretch, choc, libsamplerate, magic_enum, Airwindows).
Their individual licenses apply and are kept in their respective directories.
The complete per-component audit is still to be done before the first release.

Rovox is distributed under AGPL-3.0-or-later, which is compatible with GPL-3.0 components.

## Fetched at configure time (CMake FetchContent, linked statically)

| Component | Use | License |
|---|---|---|
| ysfx (jpcima/ysfx, pinned commit in the top-level `CMakeLists.txt`) | runs REAPER JS effects (JSFX) as the `jsfx` plugin type | Apache-2.0 |
| WDL / EEL2 (Cockos, vendored inside ysfx) | the EEL2 compiler and runtime that ysfx is built on | zlib-style (WDL license) |
| dr_libs (mackron, a submodule of ysfx) | WAV / FLAC reading for JSFX file functions | public domain / MIT-0 |

The JSFX effect files themselves are not part of Rovox or of its image: they are read at run time from the directories of `ROVOX_JSFX_PATH`
(for example a read-only mount of the REAPER `Effects` folder) and keep their own licenses.

## System libraries used by the Rovox executables

Installed from the distribution (Debian packages), not bundled. Linked dynamically unless noted.

| Component | Use | License |
|---|---|---|
| nlohmann/json | JSON (header only) | MIT |
| CLI11 | command line parsing (header only) | BSD-3-Clause |
| cpp-httplib | HTTP server and client | MIT |
| libebur128 | ITU-R BS.1770-4 loudness | MIT |
| LAME (libmp3lame) | MP3 encoding, dynamic linking | LGPL-2.1-or-later |
| libopus | Opus encoding of the browser audio stream, dynamic linking | BSD-3-Clause |
| toml++ (libtomlplusplus3) | reading `rovox.toml` (delivery adapters), dynamic linking | MIT |
| OpenSSL (libssl3, through cpp-httplib) | HTTPS for the Telegram, S3 and webhook delivery adapters, dynamic linking | Apache-2.0 |

## Web UI (`web/`)

Built in the Docker image (Node 22) and served as static files by `rovox serve`. Runtime content of the bundle:

| Component | Use | License |
|---|---|---|
| Barlow Condensed (`@fontsource/barlow-condensed`) | display font, self-hosted woff2 | SIL OFL 1.1 |
| IBM Plex Sans, IBM Plex Mono (`@fontsource/ibm-plex-*`) | UI and readout fonts, self-hosted woff2 | SIL OFL 1.1 |

Build and test tools only (not shipped): Vite, TypeScript, Vitest, jsdom, node-canvas (optional, canvas tests) — MIT.

## LV2 plugins in the runtime image

The runtime image installs these Debian packages so that the agent has instruments and effects out of the box. They are separate programs loaded
at run time (in child processes), not linked into Rovox and not part of this repository; each keeps its own license.

| Package | Provides | License |
|---|---|---|
| zynaddsubfx-lv2 | synthesizer (instrument) and effects | GPL-2.0-or-later |
| avldrums.lv2 (+ avldrums.lv2-soundfont) | drum kits (instrument) | GPL-2.0-or-later |
| calf-plugins | synthesizers and studio effects | LGPL-2.1-or-later |
| lsp-plugins-lv2 | dynamics, EQ, reverbs | LGPL-3.0-or-later |
| x42-plugins | EQ, compressor, limiter, meters | GPL-2.0-or-later |

Commercial plugins are never bypassed: a plugin that needs an activation or a license that the server cannot satisfy simply fails to load and is
reported (`PLUGIN_LOAD_FAILED`, blacklist), see `plugin.catalog` and `plugin.probe`.

## Drum kit samples (not in the repository, not in the image)

The built-in drum sampler (`rovox:drums`) plays a kit that is converted offline by `tools/drumkit/build_drskit.py` and kept in the object store
(`<S3_PREFIX>assets/drumkits/drskit/<sha>/`) or in a directory named by `ROVOX_DRUMKITS_PATH`. No sample audio is committed.

| Kit | Source | License |
|---|---|---|
| DRSKit 2.1 | DrumGizmo team and Jes Eiler (DRSDrums), https://drumgizmo.org/wiki/doku.php?id=kits:drskit | CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/) |

The converted kit is a derivative (a stereo mix-down of the multi-microphone recordings, shortened, re-encoded as FLAC). Credit required by the license:
"Drum samples: DRSKit by the DrumGizmo team and Jes Eiler (DRSDrums), CC BY 4.0". The line is shown as the tooltip of the plugin in the web mixer, is
returned in the plugin state (`attribution`) and is written into the kit's `kit.json`. Anything that publishes audio made with this kit (for example
a delivered render) must keep that credit.

## rovox-skills (`skills/`)

Python 3 standard library only; no third-party dependency is installed. Optional extras (`analysis`: numpy, soundfile, librosa) are not part of
this version.
