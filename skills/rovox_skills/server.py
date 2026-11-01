# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""A tiny MCP server over stdio exposing the skills (skill.list, skill.run) to an agent that also talks to the core. No API key is ever in a response."""
import json
import sys

from . import __version__
from .core import Core, CoreError
from .skills import describe, run_skill

TOOLS = [
    {"name": "skill.list", "description": "The skills of the rovox-skills sidecar with their parameters.", "inputSchema": {"type": "object", "properties": {}}},
    {"name": "skill.run", "description": "Runs a skill. Its steps are calls to the Rovox core with the origin skill:<name>; they appear in the activity log and can be undone.",
     "inputSchema": {"type": "object", "properties": {"name": {"type": "string"}, "params": {"type": "object"}}, "required": ["name"]}},
]


def _result(ok, data=None, error=None):
    envelope = {"ok": ok}
    envelope["data" if ok else "error"] = data if ok else error
    return {"content": [{"type": "text", "text": json.dumps(envelope)}], "structuredContent": envelope, "isError": not ok}


def handle(message, core_factory):
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
            except (KeyError, ValueError, RuntimeError) as e:
                return {"jsonrpc": "2.0", "id": mid, "result": _result(False, error={"code": "SKILL_FAILED", "message": str(e)})}
        return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32602, "message": "unknown tool"}}
    if method == "ping":
        return {"jsonrpc": "2.0", "id": mid, "result": {}}
    return {"jsonrpc": "2.0", "id": mid, "error": {"code": -32601, "message": "method not found"}}


def serve(url=None, token=None, stdin=sys.stdin, stdout=sys.stdout):
    for line in stdin:
        line = line.strip()
        if not line:
            continue
        response = handle(json.loads(line), lambda name: Core(url, token, name=name))
        if response is not None:
            stdout.write(json.dumps(response) + "\n")
            stdout.flush()
