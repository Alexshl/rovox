// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "JobFiles.h"

#include "Auth.h"
#include "Hmac.h"
#include "Ids.h"
#include "Sha256.h"

#include <openssl/rand.h>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>

#include <fcntl.h>
#include <unistd.h>

namespace rovox
{

namespace fs = std::filesystem;

namespace
{
/** The 32-byte key in `file`, if it is there; a looser mode than 0600 is repaired. */
bool readKeyFile (const fs::path& file, std::string& out)
{
    std::error_code ec;

    if (! fs::is_regular_file (file, ec) || fs::file_size (file, ec) != 32)
        return false;

    std::ifstream in (file, std::ios::binary);
    std::string k (32, '\0');
    in.read (k.data(), 32);

    if (in.gcount() != 32)
        return false;

    const auto mode = fs::status (file, ec).permissions();

    if ((mode & (fs::perms::group_all | fs::perms::others_all)) != fs::perms::none)
        fs::permissions (file, fs::perms::owner_read | fs::perms::owner_write, ec);

    out = k;
    return true;
}

bool allDigits (const std::string& s)
{
    return ! s.empty() && s.size() <= 18 && std::all_of (s.begin(), s.end(), [] (char c) { return c >= '0' && c <= '9'; });
}
} // namespace

bool isValidJobFileName (const std::string& name)
{
    if (name.empty() || name.size() > 128 || name[0] == '.')
        return false;

    return std::all_of (name.begin(), name.end(), [] (char c) { return std::isalnum ((unsigned char) c) != 0 || c == '.' || c == '_' || c == '-'; });
}

JobFiles::JobFiles (fs::path dataDir) : dataDir_ (std::move (dataDir)) {}

fs::path JobFiles::outDir (const std::string& tenant, const std::string& jobId) const
{
    if (! isValidTenantId (tenant) || ! isValidId (jobId, "job"))
        return {};

    return dataDir_ / "tenants" / tenant / "jobs" / jobId / "out";
}

std::string JobFiles::key()
{
    std::lock_guard<std::mutex> lock (mutex_);

    if (! key_.empty())
        return key_;

    const auto file = dataDir_ / "auth" / "files.key";
    std::string k;

    if (readKeyFile (file, k))
        return key_ = k;

    k.assign (32, '\0');

    if (RAND_bytes (reinterpret_cast<unsigned char*> (k.data()), 32) != 1)
        throw std::runtime_error ("the system random source failed");

    // a temporary file next to the target, mode 0600 from the start, then a no-clobber link: a concurrent process's key wins
    std::error_code ec;
    fs::create_directories (file.parent_path(), ec);
    const auto tmp = file.string() + ".tmp" + newUlid();
    const int fd = ::open (tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);

    if (fd >= 0)
    {
        const bool written = ::write (fd, k.data(), k.size()) == (ssize_t) k.size();
        ::close (fd);

        if (written)
            ::link (tmp.c_str(), file.c_str());

        ::unlink (tmp.c_str());
        readKeyFile (file, k);      // adopts the winner's key; keeps ours if the file is unusable
    }

    return key_ = k;
}

std::string JobFiles::mac (const std::string& tenant, const std::string& jobId, const std::string& name, const std::string& exp)
{
    return hmacSha256Hex (key(), "GET|/files/" + tenant + "/" + jobId + "/" + name + "|" + exp + "|" + tenant);
}

Result<json> JobFiles::manifest (const std::string& tenant, const std::string& jobId) const
{
    const auto out = outDir (tenant, jobId);

    if (out.empty())
        return makeError (errc::invalidParams, "bad tenant or job id");

    std::ifstream in (out.parent_path() / "manifest.json", std::ios::binary);

    if (! in)
        return makeError (errc::notFound, "job " + jobId + " has no files");

    auto m = json::parse (in, nullptr, false);

    if (! m.is_object())
        return makeError (errc::notFound, "job " + jobId + " has no readable manifest");

    return m;
}

Result<json> JobFiles::publish (const std::string& tenant, const std::string& jobId, const std::string& name, const fs::path& source)
{
    const auto out = outDir (tenant, jobId);

    if (out.empty() || ! isValidJobFileName (name))
        return makeError (errc::invalidParams, "bad tenant, job id or file name");

    std::error_code ec;

    if (! fs::is_regular_file (source, ec))
        return makeError (errc::notFound, "the file to publish does not exist");

    fs::create_directories (out, ec);

    if (ec)
        return makeError (errc::io, "cannot create the job output directory");

    const auto target = out / name;
    const auto tmp = out / (".partial-" + newUlid());
    fs::copy_file (source, tmp, fs::copy_options::overwrite_existing, ec);

    if (ec)
        return makeError (errc::io, "cannot copy the file into the job output directory");

    const auto sha = sha256File (tmp);
    const auto bytes = fs::file_size (tmp, ec);

    if (! sha)
    {
        fs::remove (tmp, ec);
        return makeError (errc::io, "cannot read the published file");
    }

    fs::rename (tmp, target, ec);

    if (ec)
    {
        fs::remove (tmp, ec);
        return makeError (errc::io, "cannot move the file into place");
    }

    const json entry { { "name", name }, { "bytes", (std::uint64_t) bytes }, { "sha256", *sha } };

    // the manifest is written last, replacing the entry of the same name (idempotent per (job, name))
    json files = json::array();

    if (auto m = manifest (tenant, jobId); m.ok() && m.value().contains ("files") && m.value()["files"].is_array())
    {
        const auto previous = m.value()["files"];

        for (const auto& f : previous)
            if (f.value ("name", std::string()) != name)
                files.push_back (f);
    }

    files.push_back (entry);
    const auto mtmp = out.parent_path() / (".manifest-" + newUlid());

    {
        std::ofstream o (mtmp, std::ios::binary);
        o << json { { "job_id", jobId }, { "files", files } }.dump();

        if (! o)
            return makeError (errc::io, "cannot write the job manifest");
    }

    fs::rename (mtmp, out.parent_path() / "manifest.json", ec);

    if (ec)
    {
        fs::remove (mtmp, ec);
        return makeError (errc::io, "cannot write the job manifest");
    }

    return entry;
}

std::string JobFiles::signedPath (const std::string& tenant, const std::string& jobId, const std::string& name, std::int64_t expiresAt)
{
    if (! isValidTenantId (tenant) || ! isValidId (jobId, "job") || ! isValidJobFileName (name))
        return {};

    const auto exp = std::to_string (expiresAt);
    return "/files/" + tenant + "/" + jobId + "/" + name + "?exp=" + exp + "&sig=" + mac (tenant, jobId, name, exp);
}

Result<fs::path> JobFiles::resolve (const std::string& tenant, const std::string& jobId, const std::string& name, const std::string& exp, const std::string& sig, std::int64_t now)
{
    if (! isValidTenantId (tenant) || ! isValidId (jobId, "job") || ! isValidJobFileName (name) || ! allDigits (exp))
        return makeError (errc::unauthorized, "the link is invalid or has expired");

    // both checks always run (no early exit on the signature); a bad signature and an expired link answer alike
    const bool signatureOk = constantTimeEquals (sig, mac (tenant, jobId, name, exp));
    const bool fresh = std::stoll (exp) > now;

    if (! signatureOk || ! fresh)
        return makeError (errc::unauthorized, "the link is invalid or has expired");

    const auto out = outDir (tenant, jobId);
    const auto file = out / name;
    std::error_code ec;

    // a plain file directly inside out/: no symlink, no other kind of entry
    if (fs::symlink_status (file, ec).type() != fs::file_type::regular || fs::symlink_status (out, ec).type() != fs::file_type::directory)
        return makeError (errc::notFound, "no such file");

    return file;
}

} // namespace rovox
