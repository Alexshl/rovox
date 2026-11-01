# Rovox

An open-source DAW for people and AI agents working together.

Rovox follows REAPER's interface and workflow closely, and adds two things REAPER doesn't have:

- **MCP built in.** An AI agent can drive the DAW as fully as a person can: tracks, items, FX, routing, transport, rendering. Every agent change shows up in the UI and can be undone in one step.
- **Several projects live at once.** Project tabs share one audio device, and any of them can keep playing in the background while you or an agent work in another.

It is designed for real audio interfaces (CoreAudio, ASIO), low-latency recording, and VST3/AU plugins hosted out of process.

The name nods to Roland.

## Status

Milestone M1: a headless engine with a project format (`.rvx`), a command layer (undo, revisions, events), import of
Zoom R20 `.zprj` folders, WAV files and REAPER `.RPP` projects, resumable uploads (tus), and offline render with preflight
checks and loudness measurement. MCP, the web UI and plugin management come in later milestones.

## Using it

```
rovox import ZOOM0001.zprj --new "My song" --rename "REC01=Guitar,REC02=Bass"
rovox render "My song" --out mix.wav --check                      # exit 0 ok, 2 checks failed, 1 error
rovox render "My song" --out mix.mp3 --check --target-lufs -14 --normalize --json
rovox import song.RPP --new "From REAPER"
rovox serve --data-dir /data --bind 0.0.0.0                       # HTTP API, tus uploads, autosave; SIGTERM saves and exits
rovox upload ZOOM0001.zprj --server http://host:4810 --token T    # resumable, run again after a dropped connection
```

A render never silently produces a wrong file. Before rendering, `--check` verifies that every clip's media is readable, that
every plugin loaded (a plugin that fails to load is bypassed by the engine without a word), and that MIDI tracks produce sound
(all output buses of an instrument are checked, not only the first pair). After rendering it checks for a silent result,
clipping, true-peak limits and a loudness target. Failures stop the render before a file is published.

Where things live: `--data-dir` (or `$ROVOX_DATA_DIR`) holds `projects/<id>/project.rvx/`, `uploads/` and `cache/`
(the engine's preferences and temp files; it would otherwise write to `$HOME/.config`). A `.rvx` project is a directory with
`edit.xml` (the Tracktion Edit), `rovox.json` (manifest) and `media/` (content-addressed audio). Set `ROVOX_CPUS` to override
the CPU count the audio graph uses; by default it follows the container's cgroup CPU quota.

## Building

Everything builds and tests inside Docker (Linux amd64 and arm64).

```
tools/init-submodules.sh                 # shallow submodules; see the note below
tools/build-in-docker.sh runtime rovox   # configure, compile, ctest, minimal runtime image
docker run --rm rovox --version
tools/check-license-headers.sh           # SPDX headers on every first-party source file
```

Anonymous clones: Tracktion Engine declares its nested JUCE submodule with an ssh URL (`git@github.com:juce-framework/JUCE.git`).
`tools/init-submodules.sh` rewrites it to https for that one command (`git -c url.https://github.com/.insteadOf=git@github.com:`),
so no fork and no change to your git config is needed. The Docker toolchain image sets the same rewrite system-wide.
CI uses the same script (`.github/workflows/ci.yml`).

## Stack

C++ on [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) and [JUCE](https://juce.com).

## License

[AGPL-3.0](LICENSE). Rovox is not affiliated with Cockos (REAPER) or Roland.
