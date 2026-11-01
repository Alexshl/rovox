// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Delivery.h"

#include "Hmac.h"
#include "Sha256.h"

#include <httplib.h>

#include <toml++/toml.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rovox
{

namespace fs = std::filesystem;

json DeliveryAdapter::describe() const
{
    return { { "id", id_ }, { "kind", kind() }, { "description", description_ } };
}

std::string SecretRef::resolve() const
{
    if (! env.empty())
        if (const char* v = std::getenv (env.c_str()); v != nullptr && *v != 0)
            return v;

    if (! file.empty())
    {
        std::ifstream in (file);
        std::string line;

        if (in && std::getline (in, line))
        {
            const auto b = line.find_first_not_of (" \t\r\n");
            const auto e = line.find_last_not_of (" \t\r\n");
            return b == std::string::npos ? std::string() : line.substr (b, e - b + 1);
        }
    }

    return {};
}

std::string scrubSecrets (std::string text, const std::vector<std::string>& secrets)
{
    for (auto& s : secrets)
    {
        if (s.size() < 4)
            continue;

        for (size_t pos = 0; (pos = text.find (s, pos)) != std::string::npos;)
        {
            text.replace (pos, s.size(), "***");
            pos += 3;
        }
    }

    return text;
}

namespace
{
std::string lowerExt (const fs::path& p)
{
    auto e = p.extension().string();
    std::transform (e.begin(), e.end(), e.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return e;
}

std::string mimeFor (const fs::path& p)
{
    const auto e = lowerExt (p);
    if (e == ".mp3")    return "audio/mpeg";
    if (e == ".wav")    return "audio/wav";
    if (e == ".flac")   return "audio/flac";
    if (e == ".m4a")    return "audio/mp4";
    if (e == ".ogg" || e == ".opus") return "audio/ogg";
    if (e == ".json")   return "application/json";
    if (e == ".zip")    return "application/zip";
    return "application/octet-stream";
}

std::string readFile (const fs::path& p)
{
    std::ifstream in (p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

struct Url
{
    std::string scheme, host, path, origin;     // origin = scheme://host[:port]
    int port = 0;
    bool valid = false;
};

Url parseUrl (const std::string& url)
{
    Url u;
    const auto schemeEnd = url.find ("://");

    if (schemeEnd == std::string::npos)
        return u;

    u.scheme = url.substr (0, schemeEnd);

    if (u.scheme != "http" && u.scheme != "https")
        return u;

    const auto hostStart = schemeEnd + 3;
    const auto pathStart = url.find ('/', hostStart);
    auto hostPort = url.substr (hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    u.path = pathStart == std::string::npos ? "" : url.substr (pathStart);
    u.origin = u.scheme + "://" + hostPort;
    u.port = u.scheme == "https" ? 443 : 80;

    if (const auto colon = hostPort.rfind (':'); colon != std::string::npos && hostPort.find (']') == std::string::npos)
    {
        u.port = std::atoi (hostPort.c_str() + colon + 1);
        hostPort = hostPort.substr (0, colon);
    }

    u.host = hostPort;
    u.valid = ! u.host.empty();
    return u;
}

std::string trimTrailingSlash (std::string s)
{
    while (! s.empty() && s.back() == '/')
        s.pop_back();

    return s;
}

/** Network / 5xx / 429 are worth retrying (BUSY); everything else is final. */
Error httpFailure (const httplib::Result& r, const std::string& what, const std::vector<std::string>& secrets)
{
    if (! r)
        return makeError (errc::busy, scrubSecrets (what + ": " + httplib::to_string (r.error()), secrets));

    std::string body = r->body.substr (0, 300);
    const bool transient = r->status >= 500 || r->status == 429;
    return makeError (transient ? errc::busy : errc::io, scrubSecrets (what + " answered HTTP " + std::to_string (r->status) + (body.empty() ? "" : ": " + body), secrets),
                      json { { "http_status", r->status } });
}

//==============================================================================
class FileAdapter : public DeliveryAdapter
{
public:
    FileAdapter (std::string id, fs::path dir) : DeliveryAdapter (std::move (id), "Copy to a directory on the server"), dir_ (std::move (dir)) {}

    std::string kind() const override                   { return "file"; }
    std::vector<std::string> secretValues() const override  { return {}; }

    Result<json> send (const DeliveryRequest& req, const std::function<bool()>&) override
    {
        std::error_code ec;
        fs::create_directories (dir_, ec);
        const json options = req.options.is_object() ? req.options : json::object();
        auto name = options.value ("filename", req.file.filename().string());
        name = fs::path (name).filename().string();         // no directory components from the caller
        const auto target = dir_ / name;
        const auto tmp = dir_ / (name + ".part");
        fs::copy_file (req.file, tmp, fs::copy_options::overwrite_existing, ec);

        if (ec)
            return makeError (errc::io, "cannot copy to the delivery directory: " + ec.message());

        fs::rename (tmp, target, ec);

        if (ec)
            return makeError (errc::io, "cannot publish the delivered file: " + ec.message());

        return json { { "file", name }, { "size", (std::uint64_t) fs::file_size (target) } };
    }

private:
    fs::path dir_;
};

//==============================================================================
class TelegramAdapter : public DeliveryAdapter
{
public:
    TelegramAdapter (std::string id, SecretRef token, std::string chatId, std::string apiBase)
        : DeliveryAdapter (std::move (id), "Telegram chat via the Bot API"), token_ (std::move (token)), chatId_ (std::move (chatId)), apiBase_ (trimTrailingSlash (std::move (apiBase)))
    {}

    std::string kind() const override                   { return "telegram"; }

    std::vector<std::string> secretValues() const override
    {
        return { token_.resolve(), chatId_ };
    }

    Result<json> send (const DeliveryRequest& req, const std::function<bool()>& cancelled) override
    {
        const auto token = token_.resolve();

        if (token.empty())
            return makeError (errc::preconditionFailed, "the Telegram bot token is not set (environment variable " + (token_.env.empty() ? std::string ("ROVOX_TELEGRAM_TOKEN") : token_.env) + ")");

        if (chatId_.empty())
            return makeError (errc::preconditionFailed, "the Telegram chat id is not configured");

        std::error_code ec;
        const auto size = fs::file_size (req.file, ec);

        if (ec)
            return makeError (errc::notFound, "no file to deliver: " + req.file.filename().string());

        if (size > telegramUploadLimitBytes)
            return makeError ("FILE_TOO_LARGE", "the file is " + std::to_string (size / (1024 * 1024)) + " MB; the Telegram Bot API accepts uploads up to 50 MB. Render as mp3 or deliver through an s3 adapter",
                              json { { "size", size }, { "limit", telegramUploadLimitBytes } });

        const auto ext = lowerExt (req.file);
        const json options = req.options.is_object() ? req.options : json::object();
        std::string method = options.value ("method", "");

        if (method.empty())
            method = (ext == ".mp3" || ext == ".m4a" || ext == ".wav" || ext == ".flac" || ext == ".ogg") ? "sendAudio" : "sendDocument";

        if (method != "sendAudio" && method != "sendDocument")
            return makeError (errc::invalidParams, "method must be sendAudio or sendDocument");

        const auto base = parseUrl (apiBase_);

        if (! base.valid)
            return makeError (errc::preconditionFailed, "the Telegram API base URL is invalid");

        if (cancelled && cancelled())
            return makeError (errc::cancelled, "cancelled");

        const std::string field = method == "sendAudio" ? "audio" : "document";
        httplib::MultipartFormDataItems items {
            { "chat_id", chatId_, "", "" },
            { field, readFile (req.file), req.file.filename().string(), mimeFor (req.file) },
        };

        if (! req.caption.empty())
            items.push_back ({ "caption", req.caption.substr (0, 1024), "", "" });

        if (method == "sendAudio" && ! req.title.empty())
            items.push_back ({ "title", req.title, "", "" });

        if (method == "sendAudio" && ! req.performer.empty())
            items.push_back ({ "performer", req.performer, "", "" });

        httplib::Client client (base.origin);
        client.set_connection_timeout (10, 0);
        client.set_read_timeout (120, 0);
        client.set_write_timeout (120, 0);
        auto res = client.Post (base.path + "/bot" + token + "/" + method, items);

        if (! res || res->status != 200)
            return httpFailure (res, "Telegram", secretValues());

        auto doc = json::parse (res->body, nullptr, false);

        if (doc.is_discarded() || ! doc.value ("ok", false))
            return makeError (errc::io, scrubSecrets ("Telegram refused the file: " + (doc.is_discarded() ? std::string ("unreadable answer") : doc.value ("description", std::string ("no description"))), secretValues()));

        json receipt { { "method", method } };

        if (doc.contains ("result") && doc["result"].is_object() && doc["result"].contains ("message_id"))
            receipt["message_id"] = doc["result"]["message_id"];

        return receipt;
    }

private:
    SecretRef token_;
    std::string chatId_, apiBase_;
};

//==============================================================================
class S3Adapter : public DeliveryAdapter
{
public:
    S3Adapter (std::string id, S3Config c) : DeliveryAdapter (std::move (id), "S3-compatible bucket (" + c.bucket + ")"), c_ (std::move (c)) {}

    std::string kind() const override   { return "s3"; }

    std::vector<std::string> secretValues() const override
    {
        return { c_.secretKey.resolve(), c_.sessionToken.resolve() };
    }

    Result<json> send (const DeliveryRequest& req, const std::function<bool()>& cancelled) override
    {
        const auto access = c_.accessKey.resolve(), secret = c_.secretKey.resolve();

        if (access.empty() || secret.empty())
            return makeError (errc::preconditionFailed, "the S3 access key / secret key are not set (environment)");

        const auto endpoint = parseUrl (trimTrailingSlash (c_.endpoint));

        if (! endpoint.valid)
            return makeError (errc::preconditionFailed, "the S3 endpoint URL is invalid");

        std::error_code ec;
        const auto size = fs::file_size (req.file, ec);

        if (ec)
            return makeError (errc::notFound, "no file to deliver: " + req.file.filename().string());

        const auto hash = sha256File (req.file);

        if (! hash)
            return makeError (errc::io, "cannot read the file to deliver");

        const json options = req.options.is_object() ? req.options : json::object();
        std::string key = options.value ("key", c_.prefix + req.file.filename().string());

        while (! key.empty() && key.front() == '/')
            key.erase (0, 1);

        const std::string hostHeader = endpoint.host + ((endpoint.scheme == "https" && endpoint.port == 443) || (endpoint.scheme == "http" && endpoint.port == 80) ? "" : ":" + std::to_string (endpoint.port));
        const std::string connectHost = c_.pathStyle ? hostHeader : c_.bucket + "." + hostHeader;
        const std::string path = endpoint.path + (c_.pathStyle ? "/" + c_.bucket : "") + "/" + key;

        sigv4::Credentials cred { access, secret, c_.sessionToken.resolve(), c_.region, "s3" };
        sigv4::Request sr;
        sr.method = "PUT";
        sr.host = connectHost;
        sr.path = path;
        sr.payloadHash = *hash;
        sr.headers["content-type"] = mimeFor (req.file);
        std::map<std::string, std::string> signedHeaders;
        const auto now = (std::int64_t) std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now().time_since_epoch()).count();
        const auto authorization = sigv4::authorize (cred, sr, now, signedHeaders);

        httplib::Headers headers;

        for (auto& [k, v] : signedHeaders)
            if (k != "host" && k != "content-type")
                headers.emplace (k, v);

        headers.emplace ("Authorization", authorization);

        if (cancelled && cancelled())
            return makeError (errc::cancelled, "cancelled");

        httplib::Client client (endpoint.scheme + "://" + connectHost);
        client.set_connection_timeout (10, 0);
        client.set_read_timeout (300, 0);
        client.set_write_timeout (300, 0);
        std::ifstream in (req.file, std::ios::binary);

        auto res = client.Put (sigv4::uriEncode (path, true), headers, (size_t) size,
                               [&in] (size_t offset, size_t length, httplib::DataSink& sink)
                               {
                                   std::string buf (length, '\0');
                                   in.clear();
                                   in.seekg ((std::streamoff) offset);
                                   in.read (buf.data(), (std::streamsize) length);
                                   sink.write (buf.data(), (size_t) in.gcount());
                                   return in.gcount() > 0;
                               },
                               mimeFor (req.file));

        if (! res || res->status / 100 != 2)
            return httpFailure (res, "S3", secretValues());

        json receipt { { "bucket", c_.bucket }, { "key", key }, { "size", size } };

        if (res->has_header ("ETag"))
            receipt["etag"] = res->get_header_value ("ETag");

        sigv4::Request get;
        get.method = "GET";
        get.host = connectHost;
        get.path = path;
        const int ttl = std::clamp (options.value ("presign_ttl_s", c_.presignTtlS), 1, 7 * 24 * 3600);
        receipt["url"] = endpoint.scheme + "://" + connectHost + sigv4::uriEncode (path, true) + "?" + sigv4::presignQuery (cred, get, now, ttl);
        receipt["url_expires_in_s"] = ttl;
        return receipt;
    }

private:
    S3Config c_;
};

//==============================================================================
class WebhookAdapter : public DeliveryAdapter
{
public:
    WebhookAdapter (std::string id, std::string url, SecretRef secret, std::string mode)
        : DeliveryAdapter (std::move (id), "Webhook (" + mode + " POST)"), url_ (std::move (url)), secret_ (std::move (secret)), mode_ (std::move (mode))
    {}

    std::string kind() const override   { return "webhook"; }

    std::vector<std::string> secretValues() const override
    {
        return { secret_.resolve() };
    }

    Result<json> send (const DeliveryRequest& req, const std::function<bool()>& cancelled) override
    {
        const auto url = parseUrl (url_);

        if (! url.valid)
            return makeError (errc::preconditionFailed, "the webhook URL is invalid");

        std::error_code ec;
        const auto size = fs::file_size (req.file, ec);

        if (ec)
            return makeError (errc::notFound, "no file to deliver: " + req.file.filename().string());

        const auto hash = sha256File (req.file);

        if (! hash)
            return makeError (errc::io, "cannot read the file to deliver");

        const auto secret = secret_.resolve();
        const auto timestamp = std::to_string (std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now().time_since_epoch()).count());
        const std::string name = req.file.filename().string();

        httplib::Headers headers;
        headers.emplace ("X-Rovox-Timestamp", timestamp);
        headers.emplace ("X-Rovox-Event", "delivery");

        if (cancelled && cancelled())
            return makeError (errc::cancelled, "cancelled");

        httplib::Client client (url.origin);
        client.set_connection_timeout (10, 0);
        client.set_read_timeout (120, 0);
        client.set_write_timeout (120, 0);
        httplib::Result res (nullptr, httplib::Error::Unknown);

        if (mode_ == "json")
        {
            const json body { { "event", "delivery" }, { "caption", req.caption }, { "title", req.title },
                              { "file", { { "name", name }, { "size", size }, { "sha256", *hash }, { "mime", mimeFor (req.file) } } } };
            const auto text = body.dump();

            if (! secret.empty())
                headers.emplace ("X-Rovox-Signature", "sha256=" + hmacSha256Hex (secret, timestamp + "." + text));

            res = client.Post (url.path.empty() ? "/" : url.path, headers, text, "application/json");
        }
        else
        {
            // the multipart body is assembled by the HTTP library, so the signature covers the timestamp and the file hash
            if (! secret.empty())
                headers.emplace ("X-Rovox-Signature", "sha256=" + hmacSha256Hex (secret, timestamp + "." + *hash));

            httplib::MultipartFormDataItems items {
                { "event", "delivery", "", "" },
                { "caption", req.caption, "", "" },
                { "title", req.title, "", "" },
                { "sha256", *hash, "", "" },
                { "file", readFile (req.file), name, mimeFor (req.file) },
            };
            res = client.Post (url.path.empty() ? "/" : url.path, headers, items);
        }

        if (! res || res->status / 100 != 2)
            return httpFailure (res, "webhook", secretValues());

        return json { { "http_status", res->status }, { "sha256", *hash } };
    }

private:
    std::string url_;
    SecretRef secret_;
    std::string mode_;
};

//==============================================================================
SecretRef secretOf (const toml::table& t, const std::string& base)
{
    SecretRef r;
    r.env = t[base + "_env"].value_or (std::string());
    r.file = t[base + "_file"].value_or (std::string());
    return r;
}

void rejectInlineSecrets (const toml::table& t, const std::string& where, std::initializer_list<const char*> keys, std::vector<std::string>& warnings)
{
    for (auto* k : keys)
        if (t.contains (k))
            warnings.push_back (where + ": inline '" + k + "' is ignored; secrets come from an environment variable (" + k + "_env) or a file (" + k + "_file)");
}
} // namespace

std::unique_ptr<DeliveryAdapter> makeFileAdapter (std::string id, fs::path dir)
{
    return std::make_unique<FileAdapter> (std::move (id), std::move (dir));
}

std::unique_ptr<DeliveryAdapter> makeTelegramAdapter (std::string id, SecretRef token, std::string chatId, std::string apiBase)
{
    return std::make_unique<TelegramAdapter> (std::move (id), std::move (token), std::move (chatId), std::move (apiBase));
}

std::unique_ptr<DeliveryAdapter> makeS3Adapter (std::string id, S3Config c)
{
    return std::make_unique<S3Adapter> (std::move (id), std::move (c));
}

std::unique_ptr<DeliveryAdapter> makeWebhookAdapter (std::string id, std::string url, SecretRef hmacSecret, std::string mode)
{
    return std::make_unique<WebhookAdapter> (std::move (id), std::move (url), std::move (hmacSecret), std::move (mode));
}

//==============================================================================
void DeliveryRegistry::add (std::unique_ptr<DeliveryAdapter> a)
{
    const auto id = a->id();
    adapters_[id] = std::move (a);
}

std::shared_ptr<DeliveryAdapter> DeliveryRegistry::find (const std::string& id) const
{
    auto it = adapters_.find (id);
    return it == adapters_.end() ? nullptr : it->second;
}

std::vector<std::shared_ptr<DeliveryAdapter>> DeliveryRegistry::all() const
{
    std::vector<std::shared_ptr<DeliveryAdapter>> out;

    for (auto& [id, a] : adapters_)
        out.push_back (a);

    return out;
}

DeliveryRegistry DeliveryRegistry::load (const fs::path& tomlFile, const fs::path& exportsDir)
{
    DeliveryRegistry reg;
    reg.add (makeFileAdapter ("file.exports", exportsDir));
    const auto apiBaseDefault = [] { const char* e = std::getenv ("ROVOX_TELEGRAM_API_BASE"); return std::string (e != nullptr && *e != 0 ? e : "https://api.telegram.org"); };

    std::error_code ec;

    if (fs::exists (tomlFile, ec))
    {
        try
        {
            const auto doc = toml::parse_file (tomlFile.string());

            if (auto* delivery = doc["delivery"].as_table())
            {
                for (auto&& [kind, kindNode] : *delivery)
                {
                    auto* group = kindNode.as_table();

                    if (group == nullptr)
                        continue;

                    for (auto&& [name, node] : *group)
                    {
                        auto* t = node.as_table();
                        const std::string kindStr (kind.str()), nameStr (name.str());
                        const std::string id = kindStr + "." + nameStr;

                        if (t == nullptr)
                            continue;

                        if (kindStr == "file")
                        {
                            const auto dir = (*t)["dir"].value_or (std::string());

                            if (dir.empty())    reg.warnings_.push_back (id + ": 'dir' is missing");
                            else                reg.add (makeFileAdapter (id, dir));
                        }
                        else if (kindStr == "telegram")
                        {
                            rejectInlineSecrets (*t, id, { "bot_token" }, reg.warnings_);
                            SecretRef token = secretOf (*t, "bot_token");

                            if (! token.configured())
                                token.env = "ROVOX_TELEGRAM_TOKEN";

                            std::string chat = (*t)["chat_id"].value_or (std::string());

                            if (chat.empty())
                                if (auto n = (*t)["chat_id"].value<std::int64_t>())
                                    chat = std::to_string (*n);

                            if (chat.empty())
                                if (const char* e = std::getenv ("ROVOX_TELEGRAM_CHAT_ID"))
                                    chat = e;

                            reg.add (makeTelegramAdapter (id, token, chat, (*t)["api_base"].value_or (apiBaseDefault())));
                        }
                        else if (kindStr == "s3")
                        {
                            rejectInlineSecrets (*t, id, { "access_key", "secret_key", "session_token" }, reg.warnings_);
                            S3Config c;
                            c.endpoint = (*t)["endpoint"].value_or (std::string());
                            c.bucket = (*t)["bucket"].value_or (std::string());
                            c.region = (*t)["region"].value_or (std::string ("us-east-1"));
                            c.prefix = (*t)["prefix"].value_or (std::string());
                            c.pathStyle = (*t)["path_style"].value_or (true);
                            c.presignTtlS = (int) (*t)["presign_ttl_s"].value_or (3600);
                            c.accessKey = secretOf (*t, "access_key");
                            c.secretKey = secretOf (*t, "secret_key");
                            c.sessionToken = secretOf (*t, "session_token");

                            if (c.endpoint.empty() || c.bucket.empty())     reg.warnings_.push_back (id + ": 'endpoint' and 'bucket' are required");
                            else                                            reg.add (makeS3Adapter (id, c));
                        }
                        else if (kindStr == "webhook")
                        {
                            rejectInlineSecrets (*t, id, { "hmac_secret" }, reg.warnings_);
                            const auto url = (*t)["url"].value_or (std::string());

                            if (url.empty())    reg.warnings_.push_back (id + ": 'url' is missing");
                            else                reg.add (makeWebhookAdapter (id, url, secretOf (*t, "hmac_secret"), (*t)["mode"].value_or (std::string ("multipart"))));
                        }
                        else
                        {
                            reg.warnings_.push_back (id + ": unknown adapter kind '" + kindStr + "'");
                        }
                    }
                }
            }
        }
        catch (const toml::parse_error& e)
        {
            reg.warnings_.push_back ("cannot read " + tomlFile.string() + ": " + std::string (e.description()));
        }
    }

    if (reg.find ("telegram.default") == nullptr)
    {
        const char* token = std::getenv ("ROVOX_TELEGRAM_TOKEN");
        const char* chat = std::getenv ("ROVOX_TELEGRAM_CHAT_ID");

        if (token != nullptr && *token != 0 && chat != nullptr && *chat != 0)
            reg.add (makeTelegramAdapter ("telegram.default", SecretRef { "ROVOX_TELEGRAM_TOKEN", "" }, chat, apiBaseDefault()));
    }

    return reg;
}

} // namespace rovox
