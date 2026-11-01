# Rovox

An open-source DAW for people and AI agents working together.

Rovox follows REAPER's interface and workflow closely, and adds two things REAPER doesn't have:

- **MCP built in.** An AI agent can drive the DAW as fully as a person can: tracks, items, FX, routing, transport, rendering. Every agent change shows up in the UI and can be undone in one step.
- **Several projects live at once.** Project tabs share one audio device, and any of them can keep playing in the background while you or an agent work in another.

It is designed for real audio interfaces (CoreAudio, ASIO), low-latency recording, and VST3/AU plugins hosted out of process.

The name nods to Roland.

## Status

Phase 0: research and design. No code yet.

## Stack

C++ on [Tracktion Engine](https://github.com/Tracktion/tracktion_engine) and [JUCE](https://juce.com).

## License

[AGPL-3.0](LICENSE). Rovox is not affiliated with Cockos (REAPER) or Roland.
