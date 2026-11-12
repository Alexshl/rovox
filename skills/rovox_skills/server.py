# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""A tiny MCP server over stdio exposing the skills (skill.list, skill.run) to an agent that also talks to the core. No API key is ever in a response."""
import json
import os
import re
import sys

from . import __version__
from . import analysis
from .accompany.tools import ACCOMPANY_TOOLS, wrap_handler as _accompany_wrap
from .core import Core, CoreError
from .skills import describe, run_skill
from .notation import NOTATION_TOOLS, call_tool as call_notation, tool_specs as notation_specs

TOOLS = [
    {"name": "skill.list", "description": "The skills of the rovox-skills sidecar with their parameters.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "skill.run", "description": "Runs a skill. Its steps are calls to the Rovox core with the origin skill:<name>; they appear in the activity log and can be undone.",
     "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}, "params": {"type": "object"}}, "required": ["name"]}},
    {"name": "analysis.tempo_map", "description": "Estimates the tempo of the master or a track (renders it through the core) and optionally applies it with tempo.set.",
     "inputSchema": {"type": "object", "properties": {"project_id": {"type": "string"}, "source": {"description": "\"master\" or {track_id}"}, "range": {"type": "object"}, "apply": {"type": "boolean"}}, "required": ["project_id"]}},
]

TOOLS += notation_specs()

_SECRET_NAME = re.compile(r"(_KEY|_TOKEN|SECRET|PASSWORD|PASS|CREDENTIAL)", re.I)
_MIN_SECRET = 12


def _secrets(extra=()):
    values = {v for k, v in os.environ.items() if (_SECRET_NAME.search(k) or k == "ROVOX_TOKEN") and len(v) >= _MIN_SECRET}
    values.update(v for v in extra if v and len(v) >= _MIN_SECRET)
    return sorted(values, key=len, reverse=True)


def redact(value, extra=()):
    """Replaces the values of env variables named like *_KEY, *_TOKEN, *SECRET*, *PASS*, *PASSWORD*, *CREDENTIAL* (12+ characters), ROVOX_TOKEN and the given bearer token with [redacted], in every string."""
    secrets = _secrets(extra)

    def walk(v):
        if isinstance(v, str):
            for s in secrets:
                v = v.replace(s, "[redacted]")
            return v
        if isinstance(v, dict):
            return {walk(k): walk(x) for k, x in v.items()}
        if isinstance(v, (list, tuple)):
            return [walk(x) for x in v]
        return v
    return walk(value) if secrets else value


def _result(ok, data=None, error=None):
    envelope = {"ok": ok}
    envelope["data" if ok else "error"] = data if ok else error
    return {"content": [{"type": "text", "text": json.dumps(envelope)}], "structuredContent": envelope, "isError": not ok}


def handle(message, core_factory, token=None):
    try:
        response = _handle(message, core_factory)
    except Exception as e:      # one bad request (wave.Error, EOFError, ...) must not end the stdio loop
        mid = message.get("id") if isinstance(message, dict) else None
        text = "%s: %s" % (type(e).__name__, e)
        if mid is None:
            response = None
        elif isinstance(message, dict) and message.get("method") == "tools/call":
            response = {"jsonrpc": "2.0", "id": mid, "result": _result(False, error={"code": "INTERNAL_ERROR", "message": text})}
        else:
            response = {"jsonrpc": "2.0", "id": mid, "error": {"code": -32603, "message": text}}
    return redact(response, (token,))


def _handle(message, core_factory):
    method, mid = message.get("method"), message.get("id")
    if mid is None:
        return None
    if method == "initialize":
        return {"jsonrpc": "2.0", "id": mid, "result": {"protocolVersion": "2025-06-18", "capabilities": {"tools": {}}, "serverInfo": {"name": "rovox-skills", "version": __version__}}}
    if method == "tools/list":
        return {"jsonrpc": "2.0", "id": mid, "result": {"tools": TOOLS}}
    if method == "tools/call":
        p = message.get("params", {})
        args = p.get("arguments", {})
        if p.get("name") == "skill.list":
            return {"jsonrpc": "2.0", "id": mid, "result": _result(True, {"skills": describe()})}
        if p.get("name") == "skill.run":
            try:
                core = core_factory("skill:" + args["name"])
                return {"jsonrpc": "2.0", "id": mid, "result": _result(True, run_skill(core, args["name"], args.get("params", {})))}
            except CoreError as e:
                return {"jsonrpc": "2.0", "id": mid, "result": _result(False, error={"code": e.code, "message": str(e), "details": e.details})}
            except (KeyError, ValueError, RuntimeError, OSError) as e:
                return {"jsonrpc": "2.0", "id": mid, "result": _result(False, error={"code": "SKILL_FAILED", "message": str(e)})}
        if p.get("name") == "analysis.tempo_map":
            try:
                core = core_factory("skill:analysis.tempo_map")
                return {"jsonrpc": "2.0", "id": mid, "result": _result(True, analysis.tempo_map(core, args))}
            except CoreError as e:
                return {"jsonrpc": "2.0", "id": mid, "result": _result(False, error={"code": e.code, "message": str(e), "details": e.details})}
            except (KeyError, ValueError, RuntimeError, OSError) as e:
                return {"jsonrpc": "2.0", "id": mid, "result": _result(False, error={"code": "SKILL_FAILED", "message": str(e)})}
        if p.get("name") in NOTATION_TOOLS:
            ok, payload = call_notation(p["name"], args, core_factory)
            return {"jsonrpc": "2.0", "id": mid, "result": _result(True, payload) if ok else _result(False, error=payload)}
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "unknown tool"}}
    if method == "ping":
        return {"jsonrpc": "2.0", "id": mid, "result": {}}
    return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}}


TOOLS, _handle = TOOLS + ACCOMPANY_TOOLS, _accompany_wrap(_handle)      # adds accompany.styles / .generate / .apply


def serve(url=None, token=None, stdin=sys.stdin, stdout=sys.stdout):
    cores = {}      # one core session per origin name, reused by every call of this process and deleted at exit

    def core_factory(name):
        if name not in cores:
            cores[name] = Core(url, token, name=name)
        return cores[name]

    try:
        for line in stdin:
            line = line.strip()
            if not line:
                continue
            try:
                message = json.loads(line)
            except ValueError:
                stdout.write(json.dumps({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse error"}}) + "\n")
                stdout.flush()
                continue
            response = handle(message, core_factory, token)
            if response is not None:
                stdout.write(json.dumps(response) + "\n")
                stdout.flush()
    finally:
        for core in cores.values():
            close = getattr(core, "close", None)
            if close is not None:
                close()
