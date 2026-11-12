# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
"""The one error type of the notation tools: a stable code, a message for the agent and optional details ({line, element, measure})."""


class NotationError(Exception):
    def __init__(self, code, message, details=None):
        super().__init__(message)
        self.code = code
        self.message = message
        self.details = details

    def payload(self):
        out = {"code": self.code, "message": self.message}
        if self.details:
            out["details"] = self.details
        return out


def invalid(message, **details):
    return NotationError("INVALID_PARAMS", message, {k: v for k, v in details.items() if v is not None} or None)


def unavailable(message):
    return NotationError("SKILL_UNAVAILABLE", message)
