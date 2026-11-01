// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "UploadStore.h"

#include "Sha256.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace rovox
{

namespace fs = std::filesystem;

namespace
{
std::string randomHex (size_t bytes)
{
    std::string out;
    std::ifstream urandom ("/dev/urandom", std::ios::binary);
    static const char* hex = "0123456789abcdef";

    for (size_t i = 0; i < bytes; ++i)
    {
        unsigned char b = 0;
        urandom.read (reinterpret_cast<char*> (&b), 1);
        out += hex[b >> 4];
        out += hex[b & 15];
    }

    return out;
}

std::string readFile (const fs::path& p)
{
    std::ifstream in (p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
} // namespace

json UploadInfo::toJson() const
{
    return { { "upload_id", id }, { "filename", filename }, { "relpath", relpath }, { "batch_id", batchId }, { "length", length },
             { "offset", offset }, { "complete", complete }, { "sha256", sha256 } };
}

UploadStore::UploadStore (fs::path root, UploadLimits limits, Listener listener) : root_ (std::move (root)), limits_ (limits), listener_ (std::move (listener))
{
    std::error_code ec;
    fs::create_directories (root_, ec);
}

bool UploadStore::validId (const std::string& id)
{
    if (id.size() != 35 || id.rfind ("up_", 0) != 0)
        return false;

    return std::all_of (id.begin() + 3, id.end(), [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

fs::path UploadStore::jsonPath (const std::string& id) const         { return root_ / (id + ".json"); }
fs::path UploadStore::partPath (const std::string& id) const         { return root_ / (id + ".part"); }
fs::path UploadStore::completedPath (const std::string& id) const    { return root_ / (id + ".done"); }

Result<void> UploadStore::save (const UploadInfo& u)
{
    json j = u.toJson();
    j["created_at"] = u.createdAt;
    auto tmp = jsonPath (u.id);
    tmp += ".tmp";

    {
        std::ofstream out (tmp, std::ios::binary | std::ios::trunc);
        out << j.dump();

        if (! out)
            return makeError (errc::io, "cannot write upload state");
    }

    std::error_code ec;
    fs::rename (tmp, jsonPath (u.id), ec);
    return ec ? Result<void> (makeError (errc::io, ec.message())) : Result<void>();
}

Result<UploadInfo> UploadStore::load (const std::string& id)
{
    if (! validId (id))
        return makeError (errc::notFound, "no such upload");

    std::error_code ec;

    if (! fs::exists (jsonPath (id), ec))
        return makeError (errc::notFound, "no such upload");

    const auto j = json::parse (readFile (jsonPath (id)), nullptr, false);

    if (j.is_discarded())
        return makeError (errc::io, "upload state is corrupt");

    UploadInfo u;
    u.id = id;
    u.filename = j.value ("filename", "");
    u.relpath = j.value ("relpath", "");
    u.batchId = j.value ("batch_id", "");
    u.sha256 = j.value ("sha256", "");
    u.length = j.value ("length", (std::uint64_t) 0);
    u.complete = j.value ("complete", false);
    u.createdAt = j.value ("created_at", (std::int64_t) 0);

    // the file is the truth about the offset: a crash between write and state update must not lose or duplicate bytes
    const auto part = partPath (id);
    u.offset = u.complete ? u.length : (fs::exists (part, ec) ? (std::uint64_t) fs::file_size (part, ec) : 0);
    return u;
}

Result<UploadInfo> UploadStore::create (std::uint64_t length, const std::map<std::string, std::string>& metadata)
{
    if (length > limits_.maxFileBytes)
        return makeError (errc::invalidParams, "file is larger than the limit of " + std::to_string (limits_.maxFileBytes) + " bytes", json { { "max_bytes", limits_.maxFileBytes } });

    std::error_code ec;
    const auto space = fs::space (root_, ec);

    if (! ec && space.available < length + limits_.reserveFreeBytes)
        return makeError (errc::preconditionFailed, "not enough free disk space for this upload", json { { "available_bytes", space.available }, { "needed_bytes", length } });

    UploadInfo u;
    u.id = "up_" + randomHex (16);              // 128 bits, not guessable
    u.length = length;
    u.createdAt = std::time (nullptr);

    auto get = [&] (const char* key) { auto it = metadata.find (key); return it == metadata.end() ? std::string() : it->second; };
    u.filename = get ("filename");
    u.relpath = get ("relpath");
    u.batchId = get ("batch_id");
    u.sha256 = get ("sha256");

    {
        std::lock_guard<std::mutex> lock (mutex_);
        std::ofstream (partPath (u.id), std::ios::binary).close();

        if (auto r = save (u); ! r.ok())
            return r.error();
    }

    return u;
}

Result<UploadInfo> UploadStore::info (const std::string& id)
{
    std::lock_guard<std::mutex> lock (mutex_);
    return load (id);
}

Result<void> UploadStore::terminate (const std::string& id)
{
    std::lock_guard<std::mutex> lock (mutex_);

    if (auto u = load (id); ! u.ok())
        return u.error();

    std::error_code ec;
    fs::remove (partPath (id), ec);
    fs::remove (completedPath (id), ec);
    fs::remove (jsonPath (id), ec);
    return {};
}

UploadStore::Writer::~Writer()
{
    if (fd_ >= 0)
        ::close (fd_);

    if (store_ != nullptr)
        store_->releaseWriter (id_);
}

bool UploadStore::Writer::write (const char* data, size_t size)
{
    size_t off = 0;

    while (off < size)
    {
        const auto n = ::write (fd_, data + off, size - off);

        if (n <= 0)
            return false;

        off += (size_t) n;
        written_ += (std::uint64_t) n;
    }

    return true;
}

void UploadStore::Writer::rollback()
{
    if (fd_ >= 0 && ::ftruncate (fd_, (off_t) start_) == 0)
        written_ = 0;
}

void UploadStore::releaseWriter (const std::string& id)
{
    std::lock_guard<std::mutex> lock (mutex_);
    patching_.erase (id);
}

Result<std::unique_ptr<UploadStore::Writer>> UploadStore::openWriter (const std::string& id, std::uint64_t expectedOffset)
{
    std::lock_guard<std::mutex> lock (mutex_);
    auto u = load (id);

    if (! u.ok())
        return u.error();

    if (u.value().complete)
        return makeError (errc::preconditionFailed, "upload is already complete");

    if (patching_.count (id) || (int) patching_.size() >= limits_.maxConcurrentPatches)
        return makeError (errc::busy, "another PATCH is running on this upload (or too many uploads are active)");

    if (u.value().offset != expectedOffset)
        return makeError (errc::preconditionFailed, "Upload-Offset does not match the server offset", json { { "offset", u.value().offset } });

    auto w = std::unique_ptr<Writer> (new Writer());
    w->fd_ = ::open (partPath (id).c_str(), O_WRONLY | O_APPEND);

    if (w->fd_ < 0)
        return makeError (errc::io, "cannot open the upload file");

    w->start_ = u.value().offset;
    w->id_ = id;
    w->store_ = this;
    patching_.insert (id);
    return w;
}

Result<UploadInfo> UploadStore::finishPatch (Writer& w)
{
    UploadInfo done;
    bool completed = false;
    std::string batchDone;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        ::fsync (w.fd_);
        auto u = load (w.id_);

        if (! u.ok())
            return u.error();

        auto info = u.value();

        if (info.offset > info.length)
        {
            w.rollback();
            return makeError (errc::invalidParams, "more data than Upload-Length");
        }

        if (info.offset == info.length)
        {
            if (! info.sha256.empty())
            {
                const auto actual = sha256File (partPath (w.id_));

                if (actual != info.sha256)
                {
                    return makeError (errc::invalidParams, "the full-file SHA-256 does not match the one announced at creation",
                                      json { { "expected", info.sha256 }, { "actual", actual.value_or ("") } });
                }
            }

            std::error_code ec;
            fs::rename (partPath (w.id_), completedPath (w.id_), ec);

            if (ec)
                return makeError (errc::io, "cannot finalise the upload: " + ec.message());

            info.complete = true;
            completed = true;
        }

        if (auto r = save (info); ! r.ok())
            return r.error();

        done = info;

        if (completed && ! info.batchId.empty())
        {
            bool all = true;
            std::error_code ec;

            for (auto& e : fs::directory_iterator (root_, ec))
                if (e.path().extension() == ".json")
                {
                    const auto j = json::parse (readFile (e.path()), nullptr, false);

                    if (! j.is_discarded() && j.value ("batch_id", "") == info.batchId && ! j.value ("complete", false))
                        all = false;
                }

            if (all)
                batchDone = info.batchId;
        }
    }

    if (completed && listener_.onCompleted)
        listener_.onCompleted (done);

    if (! batchDone.empty() && listener_.onBatchCompleted)
        listener_.onBatchCompleted (batchDone);

    return done;
}

Result<UploadInfo> UploadStore::append (const std::string& id, std::uint64_t expectedOffset, const char* data, size_t size)
{
    auto w = openWriter (id, expectedOffset);

    if (! w.ok())
        return w.error();

    if (! w.value()->write (data, size))
        return makeError (errc::io, "write failed");

    return finishPatch (*w.value());
}

Result<json> UploadStore::createBatch (const json& files)
{
    if (! files.is_array() || files.empty())
        return makeError (errc::invalidParams, "files must be a non-empty array");

    const auto batchId = "bt_" + randomHex (16);
    json out = json::array();
    std::uint64_t total = 0;

    for (auto& f : files)
        total += f.value ("size", (std::uint64_t) 0);

    std::error_code ec;
    const auto space = fs::space (root_, ec);

    if (! ec && space.available < total + limits_.reserveFreeBytes)       // checked for the whole batch before anything is created
        return makeError (errc::preconditionFailed, "not enough free disk space for this batch", json { { "available_bytes", space.available }, { "needed_bytes", total } });

    for (auto& f : files)
    {
        const auto rel = f.value ("relpath", "");

        if (rel.empty() || rel.find ("..") != std::string::npos || rel[0] == '/')
            return makeError (errc::invalidParams, "invalid relpath '" + rel + "'");

        std::map<std::string, std::string> md { { "filename", fs::path (rel).filename().string() }, { "relpath", rel }, { "batch_id", batchId }, { "sha256", f.value ("sha256", "") } };
        auto u = create (f.value ("size", (std::uint64_t) 0), md);

        if (! u.ok())
            return u.error();

        out.push_back ({ { "relpath", rel }, { "upload_id", u.value().id }, { "url", "/api/uploads/" + u.value().id } });
    }

    return json { { "batch_id", batchId }, { "files", out } };
}

Result<json> UploadStore::batchStatus (const std::string& batchId)
{
    std::lock_guard<std::mutex> lock (mutex_);
    json files = json::array();
    std::uint64_t total = 0, done = 0;
    int complete = 0;
    std::error_code ec;

    for (auto& e : fs::directory_iterator (root_, ec))
        if (e.path().extension() == ".json" && e.path().filename().string().rfind ("up_", 0) == 0)
        {
            auto u = load (e.path().stem().string());

            if (u.ok() && u.value().batchId == batchId)
            {
                files.push_back (u.value().toJson());
                total += u.value().length;
                done += u.value().offset;
                complete += u.value().complete ? 1 : 0;
            }
        }

    if (files.empty())
        return makeError (errc::notFound, "no such batch");

    return json { { "batch_id", batchId }, { "files", files }, { "total_bytes", total }, { "received_bytes", done },
                  { "completed_files", complete }, { "complete", complete == (int) files.size() } };
}

int UploadStore::cleanup (std::int64_t now)
{
    std::lock_guard<std::mutex> lock (mutex_);
    int removed = 0;
    std::error_code ec;
    std::vector<std::string> ids;

    for (auto& e : fs::directory_iterator (root_, ec))
        if (e.path().extension() == ".json" && e.path().filename().string().rfind ("up_", 0) == 0)
            ids.push_back (e.path().stem().string());

    for (auto& id : ids)
    {
        auto u = load (id);

        if (u.ok() && ! u.value().complete && now - u.value().createdAt > limits_.ttl.count())
        {
            fs::remove (partPath (id), ec);
            fs::remove (jsonPath (id), ec);
            ++removed;
        }
    }

    return removed;
}

} // namespace rovox
