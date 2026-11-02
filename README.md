# Rovox

An open-source DAW for people and AI agents working together.

Rovox follows REAPER's interface and workflow closely, and adds two things REAPER doesn't have:

- **MCP built in.** An AI agent can drive the DAW as fully as a person can: tracks, items, FX, routing, transport, rendering. Every agent change shows up in the UI and can be undone in one step.
- **Several projects live at once.** Project tabs share one audio device, and any of them can keep playing in the background while you or an agent work in another.

It is designed for real audio interfaces (CoreAudio, ASIO), low-latency recording, and VST3/AU plugins hosted out of process.

The name nods to Roland.

## Status

Milestone M5: a headless engine with a project format (`.rvx`), a command layer (undo, revisions, events), import of
Zoom R20 `.zprj` folders, WAV files and REAPER `.RPP` projects, resumable uploads (tus), offline render with preflight
checks and loudness measurement, an MCP server (stdio and Streamable HTTP, protocol 2025-06-18) with jobs for long work, and a
web DAW (arrange view, mixer, transport, render dialog) that plays through the browser and shows what an MCP agent changes, live.
Plugins (LV2, VST3) have a catalog, are tested and rendered in child processes (a crashing plugin does not take the server down), and are
played by MIDI that an agent writes; sends/returns, project templates, recipes, and delivery of renders (file, Telegram, S3, webhook) are in.
The `rovox-skills` sidecar (`skills/`, Python, standard library only) runs skills against the core over MCP.

## Web UI

```
docker run --rm -p 4810:4810 -v rovox-data:/data -v ~/songs:/in:ro rovox serve --bind 0.0.0.0 --audio browser --allow-path /in
# prints: generated access token ... open http://<host>:4810/?token=...      (or set ROVOX_TOKEN)
```

REAPER JS effects (JSFX) run through ysfx. Mount the REAPER `Effects` folder read-only at `/jsfx` (or name your own directories in `ROVOX_JSFX_PATH`,
colon separated) and they show up in `plugin.catalog` as format `JSFX` (uid `jsfx:<path>`, for example `jsfx:loser/Saturation`), `fx.add` takes them by uid,
and the `<JS ...>` effects of an imported `.RPP` are loaded with the slider values of the project. The files are never copied into the image or the project:

```
docker run --rm -p 4810:4810 -v rovox-data:/data -v ~/songs:/in:ro \
  -v "$HOME/Library/Application Support/REAPER/Effects:/jsfx:ro" rovox serve --bind 0.0.0.0 --audio browser --allow-path /in
```

Open the printed URL. The shell is served by `rovox serve` itself (built from `web/`, TypeScript + Vite, Canvas for the arrange view,
no UI framework), with COOP/COEP headers. Everything the UI does is a command of the same layer MCP uses; the page has one WebSocket
(`/ws`) for commands, events, 30 Hz meters and playhead, and the Opus audio of the browser stream (`--audio browser`: 48 kHz stereo,
decoded with WebCodecs, 150 ms jitter buffer; click "Увімкнути звук" once, the browser wants a gesture before it plays). With
`--audio none` Play renders a 30 s preview and plays that file.
Anything an MCP agent changes is tinted `agent-cyan`, with the agent's name; "Undo агента" takes back its last steps, "Прийняти" clears the tint.
`--allow-path DIR` (repeatable, or `$ROVOX_ALLOWED_PATHS`) lists the server directories the API may read besides the data dir (a mounted `/in`);
any other path is refused with `FORBIDDEN_PATH`. `GET /source` answers where the running source code is (AGPL section 13).

```
cd web && npm ci && npm test && npm run build      # dist/ is what `rovox serve --web-dir dist` serves
python3 tools/web_e2e.py --token T stream --project "My song" --seconds 20     # packets, gaps, jitter, 150 ms underruns, from a plain client
python3 tools/web_e2e.py --token T agent  --project "My song"                  # change things like Claude Code would, while the UI is open
```

## Using it

```
rovox import ZOOM0001.zprj --new "My song" --rename "REC01=Guitar,REC02=Bass"
rovox render "My song" --out mix.wav --check                      # exit 0 ok, 2 checks failed, 1 error
rovox render "My song" --out mix.mp3 --check --target-lufs -14 --normalize --json
rovox import song.RPP --new "From REAPER"                          # FX: JSFX from ROVOX_JSFX_PATH, Rea* settings from song.rovoxfx.json if present
rovox serve --data-dir /data --bind 0.0.0.0                       # HTTP API, tus uploads, autosave; SIGTERM saves and exits
rovox upload ZOOM0001.zprj --server http://host:4810 --token T    # resumable, run again after a dropped connection
```

### REAPER effect settings

An `.RPP` stores the settings of REAPER's own effects (ReaEQ, ReaComp, ReaLimit, ReaGate, ReaVerbate, ReaDelay) in a binary chunk that Rovox does not read.
To carry them over, open the project in REAPER and run `tools/reaper/rovox_export_fx.lua` once (Actions > Show action list > Load ReaScript). It writes
`song.rovoxfx.json` next to `song.RPP`; `import.rpp` (and `rovox import`) finds it there, or takes `fx_sidecar` explicitly, and translates the values REAPER
shows into the Rovox stock effects (the report lists what was `translated` and what is `untranslated`). Without the file those effects get default parameters.

## MCP

```
rovox mcp --data-dir ~/rovox-data                 # stdio, for Claude Code:  claude mcp add rovox -- rovox mcp --data-dir ~/rovox-data
rovox serve --token T                             # the same tools over Streamable HTTP at http://127.0.0.1:4810/mcp
python3 tools/mcp_e2e.py --zprj /in/song.zprj --http http://127.0.0.1:4810/mcp --token T     # scripted end-to-end check
```

About 47 tools (`project.*`, `track.*`, `item.*`, `marker.*`, `tempo.*`, `fx.*`, `undo`/`redo`, `batch`, `render.*`, `analyze.audio`,
`job.*`, `project.diff`, `events.*`). Every project tool takes `project_id`; ids are GUIDs, times are `{s}`, `{bar,beat,tick}` or `{qn}`,
levels are in dB. One call is one undo step; `if_revision` guards against stale writes; responses carry `external_changes` (edits made by
others since your last call). `render.start`, `render.preview`, `analyze.audio` and `import.plan` run as jobs: pass `wait_s` to wait,
or poll `job.get`. `--tool-names underscore` lists `track_list` instead of `track.list`; `--reaper-compat` adds the tool names of the old
REAPER bridge. Binding beyond loopback requires `--token`; browser `Origin` headers other than localhost are refused unless `--allow-origin` lists them.

A render never silently produces a wrong file. Before rendering, `--check` verifies that every clip's media is readable, that
every plugin loaded (a plugin that fails to load is bypassed by the engine without a word), and that MIDI tracks produce sound
(all output buses of an instrument are checked, not only the first pair). After rendering it checks for a silent result,
clipping, true-peak limits and a loudness target. Failures stop the render before a file is published.

Where things live: `--data-dir` (or `$ROVOX_DATA_DIR`) holds `projects/<id>/project.rvx/`, `uploads/` and `cache/`
(the engine's preferences and temp files; it would otherwise write to `$HOME/.config`). A `.rvx` project is a directory with
`edit.xml` (the Tracktion Edit), `rovox.json` (manifest) and `media/` (content-addressed audio). Set `ROVOX_CPUS` to override
the CPU count the audio graph uses; by default it follows the container's cgroup CPU quota.

## Plugins, MIDI, sends, delivery

```
plugin.catalog {kind: "instrument", query: "drum"}      # what can be loaded; fx.add {track_id, plugin: {plugin_uid}} loads it (tested in a child process first)
item.create_midi {track_id, notes: [{pitch: "C2", start_qn: 0, length_qn: 1}]}   # the track's instrument plays the notes
routing.send_create {track_id, to_track_id, gain_db: -12}                        # aux send to a reverb bus
template.apply {template_id: "audio-band"}   recipe.run {recipe_id: "render_and_deliver", params: {adapter: "telegram.me"}}
delivery.send {project_id, source: {job_id}, adapter: "telegram.me", caption: "..."}   # project.update {delivery_hold: true} queues instead
```

Delivery adapters are configured in `<data>/config/rovox.toml` (`--config`, `$ROVOX_CONFIG`) as `[delivery.telegram.me]`, `[delivery.s3.backup]`,
`[delivery.webhook.n8n]`, `[delivery.file.share]`. Secrets are never written there: use `bot_token_env` / `bot_token_file`, `access_key_env`,
`secret_key_env`, `hmac_secret_env` (the Telegram token defaults to `$ROVOX_TELEGRAM_TOKEN`, the chat to `$ROVOX_TELEGRAM_CHAT_ID`).
Plugins are found in the system LV2/VST3 directories and in `--plugins` / `/plugins`. Plugins are used as licensed: nothing here bypasses
an activation, and a plugin that cannot start is reported and blacklisted, not worked around.

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
