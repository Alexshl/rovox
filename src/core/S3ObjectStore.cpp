// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "ObjectStore.h"

#include "Hmac.h"
#include "Sha256.h"

#include <httplib.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <thread>

namespace rovox
{

namespace fs = std::filesystem;

namespace
{
struct Endpoint
{
    std::string scheme, host, basePath, hostHeader;
    int port = 0;
    bool valid = false;
};

Endpoint parseEndpoint (std::string url)
{
    Endpoint e;

    while (! url.empty() && url.back() == '/')
        url.pop_back();

    const auto schemeEnd = url.find ("://");

    if (schemeEnd == std::string::npos)
        return e;

    e.scheme = url.substr (0, schemeEnd);

    if (e.scheme != "http" && e.scheme != "https")
        return e;

    const auto hostStart = schemeEnd + 3;
    const auto pathStart = url.find ('/', hostStart);
    auto hostPort = url.substr (hostStart, pathStart == std::string::npos ? std::string::npos : pathStart - hostStart);
    e.basePath = pathStart == std::string::npos ? "" : url.substr (pathStart);
    e.port = e.scheme == "https" ? 443 : 80;

    if (const auto colon = hostPort.rfind (':'); colon != std::string::npos && hostPort.find (']') == std::string::npos)
    {
        e.port = std::atoi (hostPort.c_str() + colon + 1);
        hostPort = hostPort.substr (0, colon);
    }

    e.host = hostPort;
    e.hostHeader = e.host + ((e.scheme == "https" && e.port == 443) || (e.scheme == "http" && e.port == 80) ? "" : ":" + std::to_string (e.port));
    e.valid = ! e.host.empty();
    return e;
}

struct Reply
{
    int status = 0;                 // 0 = no HTTP answer (network error)
    std::string body, netError;
    httplib::Headers headers;

    std::string header (const char* name) const
    {
        auto it = headers.find (name);
        return it == headers.end() ? std::string() : it->second;
    }
};

struct Req
{
    std::string method, key;
    std::map<std::string, std::string> query;
    std::string contentType, payloadHash;
    const std::string* body = nullptr;
    fs::path uploadFile;            // PUT streamed from here ...
    std::uint64_t uploadLength = 0;
    fs::path downloadTo;            // GET streamed to here (a .part file the caller renames)
};

Req makeReq (const char* method, const std::string& key)
{
    Req r;
    r.method = method;
    r.key = key;
    return r;
}

std::string xmlUnescape (std::string s)
{
    static const std::pair<const char*, const char*> table[] = { { "&lt;", "<" }, { "&gt;", ">" }, { "&quot;", "\"" }, { "&apos;", "'" }, { "&amp;", "&" } };

    for (auto& [from, to] : table)
        for (size_t pos = 0; (pos = s.find (from, pos)) != std::string::npos;)
        {
            s.replace (pos, std::strlen (from), to);
            pos += std::strlen (to);
        }

    return s;
}

std::string xmlEscape (const std::string& s)
{
    std::string out;

    for (char c : s)
        switch (c)
        {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            default: out += c;
        }

    return out;
}

/** Text of the first <tag>...</tag> at or after `from`; `next` is set past the closing tag. */
bool xmlTag (const std::string& xml, const std::string& tag, size_t from, std::string& out, size_t* next = nullptr)
{
    const auto open = xml.find ("<" + tag + ">", from);

    if (open == std::string::npos)
        return false;

    const auto start = open + tag.size() + 2;
    const auto close = xml.find ("</" + tag + ">", start);

    if (close == std::string::npos)
        return false;

    out = xmlUnescape (xml.substr (start, close - start));

    if (next != nullptr)
        *next = close + tag.size() + 3;

    return true;
}

std::int64_t nowSeconds()
{
    return (std::int64_t) std::chrono::duration_cast<std::chrono::seconds> (std::chrono::system_clock::now().time_since_epoch()).count();
}

//==============================================================================
class S3ObjectStore : public ObjectStore
{
public:
    explicit S3ObjectStore (S3StoreConfig c) : c_ (std::move (c)), ep_ (parseEndpoint (c_.endpoint)) {}

    std::string describe() const override   { return "s3://" + c_.bucket + " at " + c_.endpoint; }

    Result<ObjectMeta> head (const std::string& key) override
    {
        auto r = call (makeReq ("HEAD", key), "HEAD " + key);

        if (! r.ok())
            return r.error();

        ObjectMeta m { key, r.value().header ("ETag"), 0 };
        m.size = std::strtoull (r.value().header ("Content-Length").c_str(), nullptr, 10);
        return m;
    }

    Result<std::string> get (const std::string& key) override
    {
        auto r = call (makeReq ("GET", key), "GET " + key);

        if (! r.ok())
            return r.error();

        return std::move (r.value().body);
    }

    Result<void> getFile (const std::string& key, const fs::path& dest) override
    {
        std::error_code ec;
        fs::create_directories (dest.parent_path(), ec);
        auto part = dest;
        part += ".part";
        auto req = makeReq ("GET", key);
        req.downloadTo = part;
        auto r = call (req, "GET " + key);

        if (! r.ok())
        {
            fs::remove (part, ec);
            return r.error();
        }

        fs::rename (part, dest, ec);

        if (ec)
            return makeError (errc::io, "cannot move the downloaded file into place: " + ec.message());

        return {};
    }

    Result<void> put (const std::string& key, const std::string& body, const std::string& contentType) override
    {
        auto req = makeReq ("PUT", key);
        req.contentType = contentType;
        req.payloadHash = sha256Hex (body);
        req.body = &body;
        auto r = call (req, "PUT " + key);
        return r.ok() ? Result<void>() : Result<void> (r.error());
    }

    Result<void> putFile (const std::string& key, const fs::path& file, const std::string& contentType) override
    {
        std::error_code ec;
        const auto size = (std::uint64_t) fs::file_size (file, ec);

        if (ec)
            return makeError (errc::notFound, "cannot read " + file.filename().string() + ": " + ec.message());

        if (size > c_.multipartThreshold)
            return putMultipart (key, file, size, contentType);

        const auto hash = sha256File (file);

        if (! hash)
            return makeError (errc::io, "cannot read " + file.filename().string());

        auto req = makeReq ("PUT", key);
        req.contentType = contentType;
        req.payloadHash = *hash;
        req.uploadFile = file;
        req.uploadLength = size;
        auto r = call (req, "PUT " + key);
        return r.ok() ? Result<void>() : Result<void> (r.error());
    }

    Result<ListResult> list (const std::string& prefix, const std::string& delimiter) override
    {
        ListResult out;
        std::string token;

        for (int page = 0; page < 100000; ++page)
        {
            auto req = makeReq ("GET", "");
            req.query["list-type"] = "2";
            req.query["prefix"] = prefix;

            if (! delimiter.empty())
                req.query["delimiter"] = delimiter;

            if (! token.empty())
                req.query["continuation-token"] = token;

            auto r = call (req, "LIST " + prefix);

            if (! r.ok())
                return r.error();

            const auto& xml = r.value().body;

            for (size_t pos = 0;;)
            {
                const auto open = xml.find ("<Contents>", pos);

                if (open == std::string::npos)
                    break;

                const auto close = xml.find ("</Contents>", open);
                const auto block = xml.substr (open, close == std::string::npos ? std::string::npos : close - open);
                ObjectMeta m;
                std::string size;
                xmlTag (block, "Key", 0, m.key);
                xmlTag (block, "ETag", 0, m.etag);

                if (xmlTag (block, "Size", 0, size))
                    m.size = std::strtoull (size.c_str(), nullptr, 10);

                out.objects.push_back (std::move (m));
                pos = close == std::string::npos ? xml.size() : close + 11;
            }

            for (size_t pos = 0;;)
            {
                const auto open = xml.find ("<CommonPrefixes>", pos);

                if (open == std::string::npos)
                    break;

                std::string p;

                if (xmlTag (xml, "Prefix", open, p))
                    out.prefixes.push_back (p);

                pos = open + 16;
            }

            std::string truncated, next;

            if (! xmlTag (xml, "IsTruncated", 0, truncated) || truncated != "true" || ! xmlTag (xml, "NextContinuationToken", 0, next))
                break;

            token = next;
        }

        return out;
    }

    Result<void> remove (const std::string& key) override
    {
        auto r = call (makeReq ("DELETE", key), "DELETE " + key);

        if (! r.ok() && r.error().code == errc::notFound)
            return {};

        return r.ok() ? Result<void>() : Result<void> (r.error());
    }

private:
    std::string connectHost() const     { return c_.pathStyle ? ep_.hostHeader : c_.bucket + "." + ep_.hostHeader; }

    std::string pathFor (const std::string& key) const
    {
        if (c_.pathStyle)
            return ep_.basePath + "/" + c_.bucket + (key.empty() ? "" : "/" + key);

        return ep_.basePath + "/" + key;
    }

    static std::string queryString (const std::map<std::string, std::string>& q)
    {
        std::vector<std::pair<std::string, std::string>> pairs;

        for (auto& [k, v] : q)
            pairs.emplace_back (sigv4::uriEncode (k, false), sigv4::uriEncode (v, false));

        std::sort (pairs.begin(), pairs.end());
        std::string s;

        for (auto& [k, v] : pairs)
            s += (s.empty() ? "" : "&") + k + "=" + v;

        return s;
    }

    std::string scrub (std::string text) const
    {
        for (const auto& secret : { c_.secretKey, c_.sessionToken })
            if (secret.size() >= 4)
                for (size_t pos = 0; (pos = text.find (secret, pos)) != std::string::npos;)
                {
                    text.replace (pos, secret.size(), "***");
                    pos += 3;
                }

        return text;
    }

    Reply once (const Req& req)
    {
        Reply out;

        if (! ep_.valid)
        {
            out.netError = "invalid S3 endpoint URL";
            return out;
        }

        sigv4::Credentials cred { c_.accessKey, c_.secretKey, c_.sessionToken, c_.region, "s3" };
        sigv4::Request sr;
        sr.method = req.method;
        sr.host = connectHost();
        sr.path = pathFor (req.key);
        sr.query = req.query;
        sr.payloadHash = req.payloadHash.empty() ? sha256Hex ("") : req.payloadHash;

        if (! req.contentType.empty())
            sr.headers["content-type"] = req.contentType;

        std::map<std::string, std::string> signedHeaders;
        const auto authorization = sigv4::authorize (cred, sr, nowSeconds(), signedHeaders);
        httplib::Headers headers;

        for (auto& [k, v] : signedHeaders)
            if (k != "host" && k != "content-type")
                headers.emplace (k, v);

        headers.emplace ("Authorization", authorization);
        const auto q = queryString (req.query);
        const auto target = sigv4::uriEncode (sr.path, true) + (q.empty() ? "" : "?" + q);

        httplib::Client client (ep_.scheme + "://" + sr.host);
        client.set_connection_timeout (c_.connectTimeoutS, 0);
        client.set_read_timeout (c_.ioTimeoutS, 0);
        client.set_write_timeout (c_.ioTimeoutS, 0);
        auto res = [&]() -> httplib::Result
        {
            if (req.method == "GET" && ! req.downloadTo.empty())
            {
                auto sink = std::make_shared<std::ofstream> (req.downloadTo, std::ios::binary | std::ios::trunc);
                auto good = std::make_shared<bool> (true);
                return client.Get (target, headers, [good] (const httplib::Response& r) { *good = r.status / 100 == 2; return true; },
                                   [sink, good] (const char* data, size_t n)
                                   {
                                       if (*good)
                                           sink->write (data, (std::streamsize) n);

                                       return sink->good();
                                   });
            }

            if (req.method == "HEAD")
                return client.Head (target, headers);

            if (req.method == "DELETE")
                return client.Delete (target, headers);

            if (req.method == "PUT" && ! req.uploadFile.empty())
            {
                auto in = std::make_shared<std::ifstream> (req.uploadFile, std::ios::binary);
                return client.Put (target, headers, (size_t) req.uploadLength,
                                   [in] (size_t offset, size_t length, httplib::DataSink& sink)
                                   {
                                       std::string buf (std::min<size_t> (length, 1u << 20), '\0');
                                       in->clear();
                                       in->seekg ((std::streamoff) offset);
                                       in->read (buf.data(), (std::streamsize) buf.size());
                                       sink.write (buf.data(), (size_t) in->gcount());
                                       return in->gcount() > 0;
                                   },
                                   req.contentType);
            }

            if (req.method == "PUT")
                return client.Put (target, headers, req.body != nullptr ? *req.body : std::string(), req.contentType);

            if (req.method == "POST")
                return client.Post (target, headers, req.body != nullptr ? *req.body : std::string(), req.contentType.empty() ? "application/xml" : req.contentType);

            return client.Get (target, headers);
        }();

        if (! res)
        {
            out.netError = httplib::to_string (res.error());
            return out;
        }

        out.status = res->status;
        out.body = std::move (res->body);
        out.headers = res->headers;
        return out;
    }

    Error failure (const Reply& r, const std::string& what) const
    {
        if (r.status == 0)
            return makeError (errc::busy, scrub (what + ": no answer from the object store (" + r.netError + ")"));

        std::string code, message;
        xmlTag (r.body, "Code", 0, code);
        xmlTag (r.body, "Message", 0, message);
        const auto detail = " (HTTP " + std::to_string (r.status) + (code.empty() ? "" : ", " + code) + (message.empty() ? "" : ": " + message.substr (0, 200)) + ")";
        json details { { "http_status", r.status } };

        if (code == "NoSuchBucket")
            return makeError (errc::preconditionFailed, scrub (what + ": the bucket does not exist (check S3_BUCKET and S3_ENDPOINT)" + detail), details);

        if (r.status == 404)
            return makeError (errc::notFound, scrub (what + ": no such object" + detail), details);

        if (r.status == 401 || r.status == 403)
            return makeError (errc::preconditionFailed, scrub (what + ": the object store refused the credentials or the bucket policy" + detail), details);

        return makeError (r.status >= 500 || r.status == 429 ? errc::busy : errc::io, scrub (what + ": the object store answered" + detail), details);
    }

    /** One request with retries (network errors, 5xx, 429) and exponential backoff. 2xx is success; everything else is an Error. */
    Result<Reply> call (const Req& req, const std::string& what)
    {
        static thread_local std::mt19937 rng { std::random_device{}() };
        Reply last;

        for (int attempt = 0; attempt < std::max (1, c_.maxAttempts); ++attempt)
        {
            if (attempt > 0)
            {
                const int base = c_.backoffBaseMs << std::min (attempt - 1, 6);
                std::this_thread::sleep_for (std::chrono::milliseconds (base + (int) (rng() % (unsigned) std::max (1, base / 2))));
            }

            last = once (req);

            if (last.status / 100 == 2)
                return last;

            if (last.status != 0 && last.status < 500 && last.status != 429)
                break;
        }

        return failure (last, what);
    }

    Result<void> putMultipart (const std::string& key, const fs::path& file, std::uint64_t size, const std::string& contentType)
    {
        auto startReq = makeReq ("POST", key);
        startReq.query["uploads"] = "";
        auto started = call (startReq, "start multipart upload of " + key);

        if (! started.ok())
            return started.error();

        std::string uploadId;

        if (! xmlTag (started.value().body, "UploadId", 0, uploadId))
            return makeError (errc::io, "the object store did not return an upload id for " + key);

        auto abort = [&]
        {
            auto del = makeReq ("DELETE", key);
            del.query["uploadId"] = uploadId;
            call (del, "abort multipart upload of " + key);
        };

        std::ifstream in (file, std::ios::binary);
        std::string completeXml = "<CompleteMultipartUpload>";
        const auto partSize = std::max<std::uint64_t> (c_.partSize, 1);

        for (std::uint64_t offset = 0, n = 1; offset < size; offset += partSize, ++n)
        {
            std::string buf ((size_t) std::min (partSize, size - offset), '\0');
            in.seekg ((std::streamoff) offset);
            in.read (buf.data(), (std::streamsize) buf.size());

            if ((size_t) in.gcount() != buf.size())
            {
                abort();
                return makeError (errc::io, "cannot read " + file.filename().string());
            }

            auto partReq = makeReq ("PUT", key);
            partReq.query["partNumber"] = std::to_string (n);
            partReq.query["uploadId"] = uploadId;
            partReq.contentType = contentType;
            partReq.payloadHash = sha256Hex (buf);
            partReq.body = &buf;
            auto part = call (partReq, "upload part " + std::to_string (n) + " of " + key);

            if (! part.ok())
            {
                abort();
                return part.error();
            }

            completeXml += "<Part><PartNumber>" + std::to_string (n) + "</PartNumber><ETag>" + xmlEscape (part.value().header ("ETag")) + "</ETag></Part>";
        }

        completeXml += "</CompleteMultipartUpload>";
        auto doneReq = makeReq ("POST", key);
        doneReq.query["uploadId"] = uploadId;
        doneReq.contentType = "application/xml";
        doneReq.payloadHash = sha256Hex (completeXml);
        doneReq.body = &completeXml;
        auto done = call (doneReq, "complete multipart upload of " + key);

        if (! done.ok())
        {
            abort();
            return done.error();
        }

        // S3 can answer 200 with an <Error> body when the completion fails
        if (done.value().body.find ("<Error>") != std::string::npos)
        {
            abort();
            return makeError (errc::io, "the object store could not complete the multipart upload of " + key);
        }

        return {};
    }

    S3StoreConfig c_;
    Endpoint ep_;
};
} // namespace

std::shared_ptr<ObjectStore> makeS3ObjectStore (S3StoreConfig c)
{
    return std::make_shared<S3ObjectStore> (std::move (c));
}

} // namespace rovox
