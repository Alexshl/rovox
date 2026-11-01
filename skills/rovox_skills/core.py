# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""MCP client of the Rovox core over Streamable HTTP (standard library only).

Every tool result of the core is an envelope {ok, data|error, revision, external_changes}; `Core.call` returns `data` or raises `CoreError`.
"""
import json
import os
import urllib.error
import urllib.request

PROTOCOL_VERSION = "2025-06-18"


class CoreError(Exception):
    def __init__(self, tool, error):
        self.tool = tool
        self.code = (error or {}).get("code", "UNKNOWN")
        self.details = (error or {}).get("details")
        super().__init__("%s failed: %s: %s" % (tool, self.code, (error or {}).get("message", "")))


class Core:
    def __init__(self, url=None, token=None, name="skill:rovox-skills", timeout=180):
        self.url = url or os.environ.get("ROVOX_URL", "http://127.0.0.1:4810/mcp")
        self.token = token if token is not None else os.environ.get("ROVOX_TOKEN", "")
        self.name = name
        self.timeout = timeout
        self.session = None
        self._id = 0
        self.last_revision = {}

    # -- transport -----------------------------------------------------------------------------------------------
    def _post(self, message):
        headers = {"Content-Type": "application/json", "Accept": "application/json, text/event-stream", "MCP-Protocol-Version": PROTOCOL_VERSION}
        if self.token:
            headers["Authorization"] = "Bearer " + self.token
        if self.session:
            headers["Mcp-Session-Id"] = self.session
        req = urllib.request.Request(self.url, json.dumps(message).encode(), headers)
        try:
            return urllib.request.urlopen(req, timeout=self.timeout)
        except urllib.error.HTTPError as e:
            raise RuntimeError("the core answered HTTP %d: %s" % (e.code, e.read().decode(errors="replace")[:300]))

    def _request(self, method, params):
        self._id += 1
        message = {"jsonrpc": "2.0", "id": self._id, "method": method, "params": params}
        with self._post(message) as r:
            if r.headers.get("Mcp-Session-Id"):
                self.session = r.headers["Mcp-Session-Id"]
            body = r.read().decode()
            if "text/event-stream" in r.headers.get("Content-Type", ""):
                for block in body.split("\n\n"):
                    for line in block.splitlines():
                        if line.startswith("data:"):
                            m = json.loads(line[5:])
                            if m.get("id") == message["id"]:
                                return m
                raise RuntimeError("no response in the event stream")
            return json.loads(body)

    def initialize(self):
        if self.session:
            return
        r = self._request("initialize", {"protocolVersion": PROTOCOL_VERSION, "capabilities": {}, "clientInfo": {"name": self.name, "version": "0.1.0"}})
        if "error" in r:
            raise RuntimeError("initialize failed: %s" % r["error"])
        with self._post({"jsonrpc": "2.0", "method": "notifications/initialized"}):
            pass

    # -- tools ---------------------------------------------------------------------------------------------------
    def envelope(self, tool, **arguments):
        """The full structured result of a tool call (ok, data, error, revision)."""
        self.initialize()
        r = self._request("tools/call", {"name": tool, "arguments": arguments})
        if "error" in r:
            raise RuntimeError("%s: protocol error %s" % (tool, r["error"]))
        return r["result"]["structuredContent"]

    def call(self, tool, **arguments):
        env = self.envelope(tool, **arguments)
        if not env.get("ok"):
            raise CoreError(tool, env.get("error"))
        if "revision" in env and arguments.get("project_id"):
            self.last_revision[arguments["project_id"]] = env["revision"]
        return env.get("data", {})

    def tools(self):
        self.initialize()
        return [t["name"] for t in self._request("tools/list", {})["result"]["tools"]]
