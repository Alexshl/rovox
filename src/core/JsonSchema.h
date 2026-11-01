// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include "Result.h"

namespace rovox
{

/** Validates `value` against the subset of JSON Schema that command params use:
    type (single or list), enum, required, properties, additionalProperties (false), items,
    minimum / maximum, minLength / maxLength, minItems / maxItems.
    On failure the Error has code INVALID_PARAMS and details {"path": "/a/0/b"} (JSON pointer). */
Result<void> validateJson (const json& schema, const json& value);

} // namespace rovox
