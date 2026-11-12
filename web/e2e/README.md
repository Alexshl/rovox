# Browser end-to-end tests

Playwright 1.64.1 (`@playwright/test` and the container image `mcr.microsoft.com/playwright:v1.64.1-noble` are pinned together). The tests drive the
real UI served by the Rovox runtime image and seed it over MCP.

## Run

```
export ROVOX_TOKEN=any-random-string
docker build -f docker/Dockerfile --target runtime -t rovox:ci .          # or E2E_IMAGE=<image>
docker compose -f docker/compose.e2e.yml up --abort-on-container-exit --exit-code-from runner runner
docker compose -f docker/compose.e2e.yml down -v
```

The runner installs the npm packages (`npm install` inside the container, a named volume holds `node_modules`), `globalSetup.ts` seeds the projects
(`seed.ts`: `e2e-demo` with 8 generated tracks, tempo map, markers, FX; `e2e-second`; `e2e-click`), signs in once with `?token=` and forces the UI language
to Ukrainian. Reports land in `web/e2e/playwright-report` and `web/e2e/test-results`.

Projects: `chromium` (1440x900), `firefox`, `phone` (390x844, visual specs only). The viewport, `uk-UA`, UTC, reduced motion, animations off and
`document.fonts.ready` are fixed. One worker, no parallelism (one server, shared state).

| Spec | What |
|---|---|
| `visual.spec.ts` | arrange, mixer, MCP panel with agent highlights, FX popover, project picker, render dialog, import wizard (step 1 and the plan step after a small real upload), the phone layout (menu sheet, mixer sheet, agents drawer), both themes |
| `perf.spec.ts` | 5000 items on 64 tracks, 5 s of scroll and zoom, rAF frame times. Gate: >= 30 fps headless. The 50 fps target is the owner's run in real Chrome |
| `stream.spec.ts` | `--audio browser`: 60 s of playback (10 min with `E2E_LONG=1`), zero underruns, the playhead advances; tab mute and background project via an analyser tapped on the page's output; click onsets vs the playhead (median <= 50 ms); with `E2E_NETEM=1` (netem profile) at most 1 underrun per minute |
| `audionone.spec.ts` | the second server with `--audio none`: Play renders a preview and an `<audio>` element plays it |
| `a11y.spec.ts` | keyboard-only walk (menu, tab, transport, fader) and an axe-core scan |

The app does not publish its output node, so `lib/audiotap.ts` wraps `AudioNode.connect` in the page (init script) to tap what reaches the destination.
Underruns are read from the tooltip of the sound button ("Обриви звуку (underrun): N"); nothing was added to `web/src`.

## Reference screenshots

They are **not in git**. Rendering depends on the OS, fonts and browser build, so they are generated in this same container and the owner approves them once:

```
docker compose -f docker/compose.e2e.yml run --rm runner npx playwright test visual --update-snapshots
```

The images appear in `web/e2e/__screenshots__/<project>-<platform>/`. The owner compares them with the design (the mockup is not in the repo), then commits
the folder. Until then the visual tests skip (CI prints a notice) and the rest of the suite runs. Any later UI change that moves pixels fails the visual
tests: look at the diff in the CI artifact `playwright-report`, and regenerate with `--update-snapshots` when the change is intended.

## Owner-only checks (cannot be automated)

- 50 fps on the 5000-item project in his own Chrome: `E2E_HEADED=1 E2E_URL=http://localhost:4810 E2E_TOKEN=... npx playwright test --project=chrome-headed perf`
- playhead against what is audible on real outputs (Bluetooth and OS output latency cannot be simulated), Safari 26 on macOS and iPad.

## Packet loss

`E2E_NETEM=1 docker compose -f docker/compose.e2e.yml --profile netem up --abort-on-container-exit --exit-code-from runner`: the `netem` sidecar (NET_ADMIN, shares
the server's network namespace) adds 40 +- 10 ms delay and 2 % loss to the server's egress.
