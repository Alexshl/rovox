// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "FsObjectStore.h"

#include "Sha256.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fstream>
#include <random>
#include <set>
#include <sstream>
#include <vector>

namespace rovox
{

namespace fs = std::filesystem;

namespace
{
constexpr const char* lockName = ".rovox-lock";
constexpr const char* tmpPrefix = ".rovox-tmp-";
constexpr const char* internalPrefix = ".rovox-";
constexpr const char* sidecarSuffix = ".etag";

std::int64_t mtimeNs (const struct stat& st)
{
#ifdef __APPLE__
    return (std::int64_t) st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
#else
    return (std::int64_t) st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
#endif
}

bool endsWith (const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare (s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string quoted (const std::string& hex)     { return "\"" + hex + "\""; }

std::string unquote (std::string tag)
{
    if (tag.rfind ("W/", 0) == 0)
        tag = tag.substr (2);

    if (tag.size() >= 2 && tag.front() == '"' && tag.back() == '"')
        tag = tag.substr (1, tag.size() - 2);

    return tag;
}

Error ioError (const std::string& what, int err)
{
    return makeError (errc::io, what + ": " + std::strerror (err));
}

/** flock() on the directory's lock file for the lifetime of the object. */
class DirLock
{
public:
    DirLock (const fs::path& dir, bool exclusive)
    {
        fd_ = ::open ((dir / lockName).c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);

        if (fd_ >= 0)
            while (::flock (fd_, exclusive ? LOCK_EX : LOCK_SH) != 0 && errno == EINTR) {}
    }

    ~DirLock()
    {
        if (fd_ >= 0)
        {
            ::flock (fd_, LOCK_UN);
            ::close (fd_);
        }
    }

    DirLock (const DirLock&) = delete;
    DirLock& operator= (const DirLock&) = delete;
    bool ok() const     { return fd_ >= 0; }

private:
    int fd_ = -1;
};

void fsyncDir (const fs::path& dir)
{
    const int fd = ::open (dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (fd >= 0)
    {
        ::fsync (fd);
        ::close (fd);
    }
}

bool writeAll (int fd, const char* data, size_t size)
{
    while (size > 0)
    {
        const auto n = ::write (fd, data, size);

        if (n < 0)
        {
            if (errno == EINTR)
                continue;

            return false;
        }

        data += n;
        size -= (size_t) n;
    }

    return true;
}

/** The hash of a sidecar "<hex> <size> <mtime_ns>", if it matches the file it describes. */
std::string sidecarEtag (const fs::path& file, const struct stat& st)
{
    std::ifstream in (file.string() + sidecarSuffix);
    std::string hex;
    long long size = -1, mtime = -1;

    if (in >> hex >> size >> mtime && size == (long long) st.st_size && mtime == (long long) mtimeNs (st) && hex.size() == 64)
        return hex;

    return {};
}

Result<std::string> etagOf (const fs::path& file, const struct stat& st)
{
    auto hex = sidecarEtag (file, st);

    if (hex.empty())
    {
        auto h = sha256File (file);

        if (! h)
            return makeError (errc::io, "cannot read " + file.filename().string());

        hex = *h;
    }

    return quoted (hex);
}

void checkSegments (const std::string& s, const char* what, std::string& why, bool rejectEmpty)
{
    size_t start = 0;

    while (why.empty() && start <= s.size())
    {
        auto end = s.find ('/', start);

        if (end == std::string::npos)
            end = s.size();

        const auto segment = s.substr (start, end - start);

        if (segment.empty() && rejectEmpty)
            why = std::string (what) + " has an empty segment";
        else if (segment == "." || segment == "..")
            why = std::string (what) + " must not contain '.' or '..' segments";
        else if (segment.rfind (internalPrefix, 0) == 0)
            why = "segments starting with .rovox- are reserved";

        start = end + 1;
    }
}
} // namespace

struct FsObjectStore::Source
{
    const std::string* body = nullptr;
    fs::path file;
};

FsObjectStore::FsObjectStore (fs::path root) : root_ (std::move (root))
{
    std::error_code ec;
    fs::create_directories (root_, ec);
}

std::string FsObjectStore::describe() const
{
    return "local directory " + root_.string();
}

std::string FsObjectStore::checkKey (const std::string& key)
{
    if (key.empty())
        return "the key is empty";

    if (key.front() == '/')
        return "the key must not be absolute";

    if (key.back() == '/')
        return "the key must not end with '/'";

    if (endsWith (key, sidecarSuffix))
        return "keys ending in .etag are reserved";

    for (unsigned char c : key)
        if (c < 0x20 || c == 0x7f)
            return "the key contains a control character";

    std::string why;
    checkSegments (key, "the key", why, true);
    return why;
}

fs::path FsObjectStore::pathOf (const std::string& key) const
{
    return root_ / key;
}

#define ROVOX_FS_CHECK_KEY(key) \
    if (const auto why = checkKey (key); ! why.empty()) \
        return makeError (errc::invalidParams, "bad object key: " + why)

Result<ObjectMeta> FsObjectStore::head (const std::string& key)
{
    ROVOX_FS_CHECK_KEY (key);
    const auto path = pathOf (key);
    DirLock lock (path.parent_path(), false);        // a consistent file + sidecar pair
    struct stat st {};

    if (::stat (path.c_str(), &st) != 0 || ! S_ISREG (st.st_mode))
        return makeError (errc::notFound, "no " + key);

    auto etag = etagOf (path, st);

    if (! etag.ok())
        return etag.error();

    return ObjectMeta { key, etag.value(), (std::uint64_t) st.st_size };
}

Result<std::string> FsObjectStore::get (const std::string& key)
{
    ROVOX_FS_CHECK_KEY (key);
    std::ifstream in (pathOf (key), std::ios::binary);

    if (! in)
        return makeError (errc::notFound, "no " + key);

    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Result<void> FsObjectStore::getFile (const std::string& key, const fs::path& dest)
{
    ROVOX_FS_CHECK_KEY (key);
    const auto path = pathOf (key);
    std::error_code ec;

    if (! fs::is_regular_file (path, ec))
        return makeError (errc::notFound, "no " + key);

    fs::create_directories (dest.parent_path(), ec);
    auto part = dest;
    part += ".part";
    fs::copy_file (path, part, fs::copy_options::overwrite_existing, ec);

    if (ec)
    {
        const auto why = ec.message();
        fs::remove (part, ec);
        return makeError (errc::io, "cannot copy " + key + ": " + why);
    }

    fs::rename (part, dest, ec);

    if (ec)
    {
        const auto why = ec.message();
        fs::remove (part, ec);
        return makeError (errc::io, "cannot move the downloaded file into place: " + why);
    }

    return {};
}

Result<void> FsObjectStore::put (const std::string& key, const std::string& body, const std::string&)
{
    return store (key, Source { &body, {} }, nullptr, nullptr);
}

Result<void> FsObjectStore::putIf (const std::string& key, const std::string& body, const std::string&, const PutCondition& cond)
{
    return store (key, Source { &body, {} }, &cond, nullptr);
}

Result<void> FsObjectStore::putFile (const std::string& key, const fs::path& file, const std::string&)
{
    return store (key, Source { nullptr, file }, nullptr, nullptr);
}

Result<void> FsObjectStore::putFileCancellable (const std::string& key, const fs::path& file, const std::string&, const CancelCheck& cancel)
{
    return store (key, Source { nullptr, file }, nullptr, cancel);
}

Result<void> FsObjectStore::store (const std::string& key, const Source& source, const PutCondition* cond, const CancelCheck& cancel)
{
    ROVOX_FS_CHECK_KEY (key);
    const auto path = pathOf (key);
    const auto dir = path.parent_path();
    std::error_code ec;
    fs::create_directories (dir, ec);

    if (ec)
        return makeError (errc::io, "cannot create the directory of " + key + ": " + ec.message());

    std::random_device rd;
    const auto tmp = dir / (std::string (tmpPrefix) + std::to_string ((long) ::getpid()) + "-" + std::to_string (counter_++) + "-" + std::to_string (rd()));
    const int fd = ::open (tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);

    if (fd < 0)
        return ioError ("cannot create a temporary file for " + key, errno);

    const auto abandon = [&] (Error e)
    {
        ::close (fd);
        std::error_code ignored;
        fs::remove (tmp, ignored);
        return Result<void> (std::move (e));
    };

    Sha256 hash;

    if (source.body != nullptr)
    {
        hash.update (source.body->data(), source.body->size());

        if (! writeAll (fd, source.body->data(), source.body->size()))
            return abandon (ioError ("cannot write " + key, errno));
    }
    else
    {
        std::ifstream in (source.file, std::ios::binary);

        if (! in)
            return abandon (makeError (errc::io, "cannot read " + source.file.filename().string()));

        std::vector<char> buffer (1 << 20);

        while (in)
        {
            if (cancel && cancel())
                return abandon (makeError (errc::cancelled, "cancelled during the copy"));

            in.read (buffer.data(), (std::streamsize) buffer.size());
            const auto n = (size_t) in.gcount();

            if (n == 0)
                break;

            hash.update (buffer.data(), n);

            if (! writeAll (fd, buffer.data(), n))
                return abandon (ioError ("cannot write " + key, errno));
        }

        if (in.bad())
            return abandon (makeError (errc::io, "cannot read " + source.file.filename().string()));
    }

    if (::fsync (fd) != 0)
        return abandon (ioError ("cannot sync " + key, errno));

    struct stat st {};
    ::fstat (fd, &st);
    ::close (fd);
    const auto hex = hash.hexDigest();

    if (beforeCommit_ && beforeCommit_())
        return makeError (errc::io, "interrupted before the rename");        // the temporary file stays, invisible, as after a crash

    DirLock lock (dir, true);

    if (! lock.ok())
    {
        fs::remove (tmp, ec);
        return makeError (errc::io, "cannot lock the directory of " + key);
    }

    if (cond != nullptr && cond->any())
    {
        struct stat cur {};
        const bool exists = ::stat (path.c_str(), &cur) == 0 && S_ISREG (cur.st_mode);
        std::string currentTag;

        if (exists)
            if (auto t = etagOf (path, cur); t.ok())
                currentTag = unquote (t.value());

        if (! cond->ifNoneMatch.empty() && exists && (cond->ifNoneMatch == "*" || unquote (cond->ifNoneMatch) == currentTag))
        {
            fs::remove (tmp, ec);
            return makeError (putConditionFailed, "precondition failed: " + key + " exists");
        }

        if (! cond->ifMatch.empty() && (! exists || (cond->ifMatch != "*" && unquote (cond->ifMatch) != currentTag)))
        {
            fs::remove (tmp, ec);
            return makeError (putConditionFailed, "precondition failed: " + key + " changed");
        }
    }

    if (::rename (tmp.c_str(), path.c_str()) != 0)
    {
        const int err = errno;
        fs::remove (tmp, ec);
        return ioError ("cannot move " + key + " into place", err);
    }

    // the sidecar: written whole and renamed; if the process dies before this, the stale or missing sidecar is detected and the hash recomputed
    const auto sidecar = path.string() + sidecarSuffix;
    const auto sidecarTmp = dir / (std::string (tmpPrefix) + "etag-" + std::to_string ((long) ::getpid()) + "-" + std::to_string (counter_++));

    {
        std::ofstream out (sidecarTmp, std::ios::binary);
        out << hex << " " << (long long) st.st_size << " " << (long long) mtimeNs (st) << "\n";
    }

    if (::rename (sidecarTmp.c_str(), sidecar.c_str()) != 0)
        fs::remove (sidecarTmp, ec);

    fsyncDir (dir);
    return {};
}

Result<ListResult> FsObjectStore::list (const std::string& prefix, const std::string& delimiter)
{
    if (! prefix.empty())
    {
        std::string why;

        if (prefix.front() == '/')
            why = "it must not be absolute";
        else
            checkSegments (prefix, "the prefix", why, false);

        if (! why.empty())
            return makeError (errc::invalidParams, "bad prefix: " + why);
    }

    ListResult out;
    std::set<std::string> folded;
    std::vector<ObjectMeta> found;
    const auto slash = prefix.rfind ('/');
    const auto startDir = slash == std::string::npos ? root_ : root_ / prefix.substr (0, slash);
    std::error_code ec;

    if (! fs::is_directory (startDir, ec))
        return out;

    fs::recursive_directory_iterator it (startDir, fs::directory_options::skip_permission_denied, ec), endIt;

    for (; ! ec && it != endIt; it.increment (ec))
    {
        const auto name = it->path().filename().string();
        std::error_code typeEc;

        if (name.rfind (internalPrefix, 0) == 0)
        {
            if (it->is_directory (typeEc))
                it.disable_recursion_pending();

            continue;
        }

        if (! it->is_regular_file (typeEc) || endsWith (name, sidecarSuffix))
            continue;

        const auto key = fs::relative (it->path(), root_, typeEc).generic_string();

        if (key.rfind (prefix, 0) != 0)
            continue;

        const auto rest = key.substr (prefix.size());

        if (! delimiter.empty() && rest.find (delimiter) != std::string::npos)
        {
            folded.insert (prefix + rest.substr (0, rest.find (delimiter) + delimiter.size()));
            continue;
        }

        struct stat st {};

        if (::stat (it->path().c_str(), &st) != 0)
            continue;

        auto etag = etagOf (it->path(), st);
        found.push_back ({ key, etag.ok() ? etag.value() : std::string(), (std::uint64_t) st.st_size });
    }

    std::sort (found.begin(), found.end(), [] (const ObjectMeta& a, const ObjectMeta& b) { return a.key < b.key; });
    out.objects = std::move (found);
    out.prefixes.assign (folded.begin(), folded.end());
    return out;
}

Result<void> FsObjectStore::remove (const std::string& key)
{
    ROVOX_FS_CHECK_KEY (key);
    const auto path = pathOf (key);
    std::error_code ec;

    if (! fs::is_directory (path.parent_path(), ec))
        return {};

    DirLock lock (path.parent_path(), true);
    fs::remove (path.string() + sidecarSuffix, ec);
    ec.clear();
    fs::remove (path, ec);

    if (ec)
        return makeError (errc::io, "cannot remove " + key + ": " + ec.message());

    return {};
}

std::shared_ptr<ObjectStore> makeFsObjectStore (fs::path root)
{
    return std::make_shared<FsObjectStore> (std::move (root));
}

} // namespace rovox
