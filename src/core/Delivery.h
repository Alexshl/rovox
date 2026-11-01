// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Delivery adapters (task 510): file, telegram, s3, webhook. Secrets come from the environment or from files named in the
// config, are read when a send happens and never appear in a result, an error message or a log line.
#pragma once

#include "Result.h"

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rovox
{

struct DeliveryRequest
{
    std::filesystem::path file;
    std::string caption, title, performer;
    json options = json::object();      // per-call adapter options: {"method": "sendDocument"} {"key": "mixes/v3.mp3"} ...
};

/** A place where a rendered file can be sent. send() must be callable from any thread. */
class DeliveryAdapter
{
public:
    virtual ~DeliveryAdapter() = default;

    const std::string& id() const       { return id_; }
    virtual std::string kind() const = 0;
    /** Safe to show to a client: id, kind, a short description. Never a token, a chat id, a bucket credential or a URL with a secret. */
    json describe() const;

    /** Receipt on success (message_id, etag, url ...), never containing a secret. `cancelled` is polled between phases. */
    virtual Result<json> send (const DeliveryRequest&, const std::function<bool()>& cancelled) = 0;
    /** Transient failures (network, 5xx, 429) are retried by the caller; the error code is then BUSY. */
    virtual std::vector<std::string> secretValues() const = 0;

protected:
    explicit DeliveryAdapter (std::string id, std::string description) : id_ (std::move (id)), description_ (std::move (description)) {}
    std::string id_, description_;
};

/** Where a secret lives: an environment variable, or a file (first line, trimmed). */
struct SecretRef
{
    std::string env, file;
    std::string resolve() const;        // empty if unset
    bool configured() const             { return ! env.empty() || ! file.empty(); }
};

/** Replaces every occurrence of a non-empty secret in `text` with "***". */
std::string scrubSecrets (std::string text, const std::vector<std::string>& secrets);

constexpr std::uintmax_t telegramUploadLimitBytes = 50u * 1024u * 1024u;

class DeliveryRegistry
{
public:
    /** Reads [delivery.<kind>.<name>] tables of a TOML file (missing file = no configured adapters) and adds the built-in
        `file.exports` (directory `exportsDir`) and, if ROVOX_TELEGRAM_TOKEN and ROVOX_TELEGRAM_CHAT_ID are set, `telegram.default`. */
    static DeliveryRegistry load (const std::filesystem::path& tomlFile, const std::filesystem::path& exportsDir);

    std::shared_ptr<DeliveryAdapter> find (const std::string& id) const;
    std::vector<std::shared_ptr<DeliveryAdapter>> all() const;
    const std::vector<std::string>& warnings() const    { return warnings_; }

    void add (std::unique_ptr<DeliveryAdapter>);

private:
    std::map<std::string, std::shared_ptr<DeliveryAdapter>> adapters_;
    std::vector<std::string> warnings_;
};

/** Adapter constructors, used by the registry and directly by tests. */
std::unique_ptr<DeliveryAdapter> makeFileAdapter (std::string id, std::filesystem::path dir);
std::unique_ptr<DeliveryAdapter> makeTelegramAdapter (std::string id, SecretRef token, std::string chatId, std::string apiBase);
struct S3Config
{
    std::string endpoint, bucket, region = "us-east-1", prefix;
    SecretRef accessKey, secretKey, sessionToken;
    bool pathStyle = true;
    int presignTtlS = 3600;
};
std::unique_ptr<DeliveryAdapter> makeS3Adapter (std::string id, S3Config);
std::unique_ptr<DeliveryAdapter> makeWebhookAdapter (std::string id, std::string url, SecretRef hmacSecret, std::string mode = "multipart");

} // namespace rovox
