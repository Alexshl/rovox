// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <utility>

namespace rovox
{

using json = nlohmann::json;

/** Machine-readable error codes shared by the command layer, MCP and the CLI. */
namespace errc
{
inline constexpr const char* invalidParams       = "INVALID_PARAMS";
inline constexpr const char* notFound            = "NOT_FOUND";
inline constexpr const char* revisionConflict    = "REVISION_CONFLICT";
inline constexpr const char* preconditionFailed  = "PRECONDITION_FAILED";
inline constexpr const char* busy                = "BUSY";
inline constexpr const char* internal            = "INTERNAL";
inline constexpr const char* preflightFailed     = "PREFLIGHT_FAILED";
inline constexpr const char* unsupported         = "UNSUPPORTED";
inline constexpr const char* io                  = "IO_ERROR";
inline constexpr const char* cancelled           = "CANCELLED";
inline constexpr const char* notImplemented      = "NOT_IMPLEMENTED";
inline constexpr const char* serverRestarted     = "SERVER_RESTARTED";
inline constexpr const char* resyncRequired      = "RESYNC_REQUIRED";
inline constexpr const char* forbiddenPath       = "FORBIDDEN_PATH";
} // namespace errc

struct Error
{
    std::string code = errc::internal;
    std::string message;
    json details = nullptr;     // e.g. {"path": "/volume_db"} or {"current_revision": 12}

    json toJson() const
    {
        json j { { "code", code }, { "message", message } };

        if (! details.is_null())
            j["details"] = details;

        return j;
    }
};

/** Value-or-error. `Result<void>` carries only success or an Error. */
template <typename T>
class Result
{
public:
    Result (T v) : value_ (std::move (v)) {}
    Result (Error e) : error_ (std::move (e)) {}

    bool ok() const noexcept            { return value_.has_value(); }
    explicit operator bool() const      { return ok(); }
    T& value()                          { return *value_; }
    const T& value() const              { return *value_; }
    const Error& error() const          { return error_; }

private:
    std::optional<T> value_;
    Error error_;
};

template <>
class Result<void>
{
public:
    Result() = default;
    Result (Error e) : failed_ (true), error_ (std::move (e)) {}

    bool ok() const noexcept            { return ! failed_; }
    explicit operator bool() const      { return ok(); }
    const Error& error() const          { return error_; }

private:
    bool failed_ = false;
    Error error_;
};

inline Error makeError (std::string code, std::string message, json details = nullptr)
{
    return { std::move (code), std::move (message), std::move (details) };
}

} // namespace rovox
