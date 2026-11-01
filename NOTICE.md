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

## Web UI (`web/`)

Built in the Docker image (Node 22) and served as static files by `rovox serve`. Runtime content of the bundle:

| Component | Use | License |
|---|---|---|
| Barlow Condensed (`@fontsource/barlow-condensed`) | display font, self-hosted woff2 | SIL OFL 1.1 |
| IBM Plex Sans, IBM Plex Mono (`@fontsource/ibm-plex-*`) | UI and readout fonts, self-hosted woff2 | SIL OFL 1.1 |

Build and test tools only (not shipped): Vite, TypeScript, Vitest, jsdom, node-canvas (optional, canvas tests) — MIT.
