# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
import argparse
import json
import sys

from .core import Core, CoreError
from .skills import describe, run_skill


def _parse_param(text):
    key, _, value = text.partition("=")
    try:
        return key, json.loads(value)
    except ValueError:
        return key, value


def main(argv=None):
    ap = argparse.ArgumentParser(prog="rovox-skills", description="Skills for the Rovox DAW server (no LLM)")
    sub = ap.add_subparsers(dest="command", required=True)
    sub.add_parser("list", help="list the skills")
    run = sub.add_parser("run", help="run a skill")
    run.add_argument("name")
    run.add_argument("--server", help="MCP URL of the core (default $ROVOX_URL)")
    run.add_argument("--token", help="bearer token (default $ROVOX_TOKEN)")
    run.add_argument("--project", help="project id")
    run.add_argument("--param", action="append", default=[], help="key=value (value as JSON when it parses)")
    serve = sub.add_parser("serve", help="MCP server over stdio exposing skill.list / skill.run")
    serve.add_argument("--server")
    serve.add_argument("--token")
    args = ap.parse_args(argv)

    if args.command == "list":
        print(json.dumps(describe(), indent=2))
        return 0
    if args.command == "serve":
        from .server import serve as run_server
        run_server(args.server, args.token)
        return 0
    params = dict(_parse_param(p) for p in args.param)
    if args.project:
        params["project_id"] = args.project
    try:
        print(json.dumps(run_skill(Core(args.server, args.token, name="skill:" + args.name), args.name, params), indent=2))
    except CoreError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2
    except (KeyError, ValueError, RuntimeError) as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
