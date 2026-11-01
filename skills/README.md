# rovox-skills

An optional sidecar for the Rovox server. It is an MCP client of the core (it drives projects with the same tools as any agent, with the
origin `skill:<name>`, so every step shows in the activity log and can be undone) and a small MCP server of its own (`skill.list`,
`skill.run`). It uses no LLM and no third-party package.

```
rovox-skills list
rovox-skills run render_and_deliver --server http://127.0.0.1:4810/mcp --project p_... --param adapter=telegram.me --param target_lufs=-14
rovox-skills run rhythm_section --project p_... --param seed=7 --param bars=2
rovox-skills serve --server http://127.0.0.1:4810/mcp        # MCP over stdio, for an agent that wants skill.run
```

The server URL defaults to `$ROVOX_URL`, the token to `$ROVOX_TOKEN`.

Skills: `render_and_deliver`, `rhythm_section` (a seeded drum + bass pattern written as MIDI for the instruments on the project's tracks),
`add_reverb_bus` (a reverb return and sends), `mastering_check` (renders and reports loudness against a target without changing anything).
Tests: `python3 -m unittest discover -s tests` (a fake core, no server needed).
