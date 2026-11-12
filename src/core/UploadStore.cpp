// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "UploadStore.h"

#include "Sha256.h"

#include <algorithm>
#include <cctype>
#include <tuple>
#include <vector>
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
    j["completed_at"] = u.completedAt;
    j["placed"] = u.placed;
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
    u.completedAt = j.value ("completed_at", (std::int64_t) 0);
    u.placed = j.value ("placed", "");

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
    const auto remaining = length_ > start_ + written_ ? length_ - (start_ + written_) : 0;

    if ((std::uint64_t) size > remaining)
    {
        overflow_ = true;           // nothing of this chunk reaches the disk
        return false;
    }

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
    w->length_ = u.value().length;
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
    bool mismatch = false;
    std::string actualHash;

    ::fsync (w.fd_);                // outside mutex_: patching_ keeps this upload exclusive, other uploads must stay responsive
    std::string expectedHash;
    bool needHash = false;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto u = load (w.id_);

        if (! u.ok())
            return u.error();

        if (u.value().offset > u.value().length)
        {
            w.rollback();
            return makeError (errc::invalidParams, "more data than Upload-Length");
        }

        needHash = u.value().offset == u.value().length && ! u.value().sha256.empty();
        expectedHash = u.value().sha256;
    }

    if (needHash)
    {
        const auto actual = sha256File (partPath (w.id_));
        actualHash = actual.value_or ("");
        mismatch = actual != expectedHash;
    }

    {
        std::lock_guard<std::mutex> lock (mutex_);
        auto u = load (w.id_);

        if (! u.ok())
            return u.error();

        auto info = u.value();

        if (info.offset == info.length)
        {
            if (mismatch)
            {
                return makeError (errc::invalidParams, "the full-file SHA-256 does not match the one announced at creation",
                                  json { { "expected", expectedHash }, { "actual", actualHash } });
            }

            std::error_code ec;
            fs::rename (partPath (w.id_), completedPath (w.id_), ec);

            if (ec)
                return makeError (errc::io, "cannot finalise the upload: " + ec.message());

            info.complete = true;
            info.completedAt = std::time (nullptr);
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
    bool placedAll = true;
    std::error_code ec;

    for (auto& e : fs::directory_iterator (root_, ec))
        if (e.path().extension() == ".json" && e.path().filename().string().rfind ("up_", 0) == 0)
        {
            auto u = load (e.path().stem().string());

            if (u.ok() && u.value().batchId == batchId)
            {
                placedAll = placedAll && ! u.value().placed.empty();
                files.push_back (u.value().toJson());
                total += u.value().length;
                done += u.value().offset;
                complete += u.value().complete ? 1 : 0;
            }
        }

    if (files.empty())
        return makeError (errc::notFound, "no such batch");

    json out { { "batch_id", batchId }, { "files", files }, { "total_bytes", total }, { "received_bytes", done },
               { "completed_files", complete }, { "complete", complete == (int) files.size() } };

    if (complete == (int) files.size() && placedAll)
        out["materialised_path"] = materialisedDir (batchId).string();

    return out;
}

namespace
{
/** Cuts at a UTF-8 boundary so the name stays valid text. */
std::string cutUtf8 (std::string s, size_t maxBytes)
{
    if (s.size() <= maxBytes)
        return s;

    size_t n = maxBytes;

    while (n > 0 && ((unsigned char) s[n] & 0xC0) == 0x80)
        --n;

    s.resize (n);
    return s;
}

std::string sanitiseSegment (const std::string& in)
{
    std::string s;

    for (unsigned char c : in)
        if (c >= 0x20 && c != 0x7f)
            s += (char) c;

    const auto b = s.find_first_not_of (' ');

    if (b == std::string::npos)
        return {};

    s = s.substr (b);

    while (! s.empty() && (s.back() == ' ' || s.back() == '.'))
        s.pop_back();

    if (s.size() > 255)
    {
        const auto ext = fs::path (s).extension().string();
        s = ext.size() <= 16 ? cutUtf8 (s.substr (0, s.size() - ext.size()), 255 - ext.size()) + ext : cutUtf8 (s, 255);
    }

    return s;
}

/** Segments of a client-supplied relative path with everything dangerous dropped: "..", ".", empty parts, control characters, leading slashes. */
std::vector<std::string> sanitisePath (const std::string& rel)
{
    std::vector<std::string> out;
    std::string cur;

    auto flush = [&]
    {
        auto seg = sanitiseSegment (cur);
        cur.clear();

        if (! seg.empty() && seg != "." && seg != "..")
            out.push_back (std::move (seg));
    };

    for (char c : rel)
    {
        if (c == '/' || c == '\\')
            flush();
        else
            cur += c;
    }

    flush();

    if (out.size() > 16)
        out.erase (out.begin(), out.end() - 16);       // keep the deepest part: the file name matters most

    return out;
}

std::string lowerAscii (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

bool validBatchId (const std::string& id)
{
    return id.size() == 35 && id.rfind ("bt_", 0) == 0
        && std::all_of (id.begin() + 3, id.end(), [] (char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
} // namespace

fs::path UploadStore::materialisedDir (const std::string& id) const
{
    return root_ / id;
}

std::vector<UploadInfo> UploadStore::membersOf (const std::string& id)      // call with mutex_ held
{
    std::vector<UploadInfo> out;

    if (id.rfind ("bt_", 0) == 0)
    {
        std::error_code ec;

        for (auto& e : fs::directory_iterator (root_, ec))
            if (e.path().extension() == ".json" && e.path().filename().string().rfind ("up_", 0) == 0)
                if (auto u = load (e.path().stem().string()); u.ok() && u.value().batchId == id)
                    out.push_back (u.value());

        std::sort (out.begin(), out.end(), [] (const UploadInfo& a, const UploadInfo& b) { return std::tie (a.relpath, a.id) < std::tie (b.relpath, b.id); });
    }
    else if (auto u = load (id); u.ok())
    {
        out.push_back (u.value());
    }

    return out;
}

Result<json> UploadStore::finish (const std::string& id)
{
    if (! validId (id) && ! validBatchId (id))
        return makeError (errc::notFound, "no such upload or batch");

    std::vector<UploadInfo> members;

    {
        std::lock_guard<std::mutex> lock (mutex_);
        members = membersOf (id);

        if (members.empty())
            return makeError (errc::notFound, "no such upload or batch");

        for (auto& m : members)
            if (! m.complete)
                return makeError (errc::preconditionFailed, "upload " + m.id + " is not complete yet", json { { "upload_id", m.id }, { "offset", m.offset }, { "length", m.length } });

        const auto dir = materialisedDir (id);
        std::error_code ec;
        std::set<std::string> used;

        for (auto& m : members)
            if (! m.placed.empty())
                used.insert (lowerAscii (m.placed));

        for (auto& m : members)
        {
            if (! m.placed.empty())
            {
                if (! fs::exists (dir / fs::path (m.placed), ec))
                    return makeError (errc::preconditionFailed, "the materialised files were removed (after an import or by expiry)");

                continue;
            }

            auto segs = id.rfind ("bt_", 0) == 0 ? sanitisePath (m.relpath.empty() ? m.filename : m.relpath) : sanitisePath (m.filename);

            if (segs.empty())
                segs = { "file" };

            auto build = [] (const std::vector<std::string>& s) { std::string r; for (auto& x : s) r += (r.empty() ? "" : "/") + x; return r; };
            auto rel = build (segs);

            for (int n = 2; used.count (lowerAscii (rel)) != 0 || fs::exists (dir / fs::path (rel), ec); ++n)
            {
                auto s2 = segs;
                const auto name = fs::path (s2.back());
                const auto ext = name.extension().string();
                const auto stem = ext.empty() ? name.string() : name.string().substr (0, name.string().size() - ext.size());
                s2.back() = cutUtf8 (stem, 255 - ext.size() - 8) + " (" + std::to_string (n) + ")" + ext;
                rel = build (s2);
            }

            const auto dest = dir / fs::path (rel);
            fs::create_directories (dest.parent_path(), ec);

            if (ec)
                return makeError (errc::io, "cannot create " + dest.parent_path().string() + ": " + ec.message());

            fs::rename (completedPath (m.id), dest, ec);

            if (ec)
                return makeError (errc::io, "cannot materialise the upload: " + ec.message());

            m.placed = rel;
            used.insert (lowerAscii (rel));

            if (auto r = save (m); ! r.ok())
                return r.error();
        }
    }

    // hashes are computed outside the lock (multi-GB files) and cached in the state file
    const auto dir = materialisedDir (id);
    json files = json::array();
    std::vector<std::string> rels;

    for (auto& m : members)
    {
        const auto file = dir / fs::path (m.placed);
        std::error_code ec;
        auto hash = m.sha256;

        if (hash.empty())
        {
            hash = sha256File (file).value_or ("");

            if (! hash.empty())
            {
                std::lock_guard<std::mutex> lock (mutex_);

                if (auto u = load (m.id); u.ok())
                {
                    auto upd = u.value();
                    upd.sha256 = hash;
                    save (upd);
                }
            }
        }

        files.push_back ({ { "relpath", m.placed }, { "bytes", (std::uint64_t) fs::file_size (file, ec) }, { "sha256", hash } });
        rels.push_back (m.placed);
    }

    // a batch that is one folder (ZOOM0001.zprj/...) is addressed by that folder, so import.plan sees a project, not a parent of folders
    fs::path path = dir;
    std::string top;
    bool oneTop = ! rels.empty();

    for (auto& r : rels)
    {
        const auto slash = r.find ('/');

        if (slash == std::string::npos || (! top.empty() && r.substr (0, slash) != top))
        {
            oneTop = false;
            break;
        }

        top = r.substr (0, slash);
    }

    if (oneTop)
        path = dir / fs::path (top);

    std::string detected = "audio";
    std::error_code ec;
    std::vector<std::string> rpps;

    for (auto& r : rels)
        if (lowerAscii (fs::path (r).extension().string()) == ".rpp")
            rpps.push_back (r);

    if (lowerAscii (path.extension().string()) == ".zprj" || fs::exists (path / "R20PRJDATA.r20prj", ec))
    {
        detected = "zoom_r20";
    }
    else if (! rpps.empty())
    {
        detected = "rpp";

        if (rpps.size() == 1)
            path = dir / fs::path (rpps.front());     // import.rpp takes the .RPP file; media beside it is found by relative path
    }

    return json { { "path", path.string() }, { "files", files }, { "detected", detected } };
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

    std::vector<UploadInfo> infos;
    std::map<std::string, std::int64_t> newest;         // materialised directory -> latest completion in it

    for (auto& id : ids)
        if (auto u = load (id); u.ok())
        {
            infos.push_back (u.value());
            auto& n = newest[u.value().batchId.empty() ? id : u.value().batchId];
            n = std::max (n, u.value().complete ? (u.value().completedAt != 0 ? u.value().completedAt : u.value().createdAt) : now);     // an unfinished member keeps the batch alive
        }

    std::set<std::string> dirsGone;

    for (auto& u : infos)
    {
        if (! u.complete)
        {
            if (now - u.createdAt > limits_.ttl.count())
            {
                fs::remove (partPath (u.id), ec);
                fs::remove (jsonPath (u.id), ec);
                ++removed;
            }

            continue;
        }

        const auto key = u.batchId.empty() ? u.id : u.batchId;

        if (now - newest[key] > limits_.ttl.count())
        {
            if (dirsGone.insert (key).second)
                fs::remove_all (materialisedDir (key), ec);

            fs::remove (completedPath (u.id), ec);
            fs::remove (jsonPath (u.id), ec);
            ++removed;
        }
    }

    return removed;
}

} // namespace rovox
