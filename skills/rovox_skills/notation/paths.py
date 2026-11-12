# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""Where the notation tools may read and write files: ROVOX_SKILLS_DIRS (os.pathsep separated) or, by default, the temp directory and the working directory."""
import os
import tempfile

from .errors import invalid


def allowed_roots():
    raw = os.environ.get("ROVOX_SKILLS_DIRS")
    roots = [r for r in raw.split(os.pathsep) if r] if raw else [tempfile.gettempdir(), os.getcwd()]
    return [os.path.realpath(r) for r in roots]


def default_out_dir():
    base = allowed_roots()[0] if os.environ.get("ROVOX_SKILLS_DIRS") else os.path.realpath(tempfile.gettempdir())
    return os.path.join(base, "rovox-skills-out")


def _inside(path, root):
    return path == root or path.startswith(root.rstrip(os.sep) + os.sep)


def resolve_in(path):
    """An existing input file inside an allowed directory."""
    if not isinstance(path, str) or not path:
        raise invalid("file must be a path string", element="file")
    real = os.path.realpath(path)
    if not any(_inside(real, r) for r in allowed_roots()):
        raise invalid("the file is outside the directories this sidecar may read", file=path)
    return real


def resolve_out(path, default_name):
    """An output path inside an allowed directory (relative paths and the default go to the output directory); parent directories are created."""
    if path:
        real = os.path.realpath(path if os.path.isabs(path) else os.path.join(default_out_dir(), path))
    else:
        real = os.path.join(default_out_dir(), default_name)
    if not any(_inside(real, r) for r in allowed_roots()):
        raise invalid("the output path is outside the directories this sidecar may write", output=path)
    try:
        os.makedirs(os.path.dirname(real), exist_ok=True)
    except OSError as e:
        raise invalid("cannot create the output directory: %s" % e.strerror)
    return real
