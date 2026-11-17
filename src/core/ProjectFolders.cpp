// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "ProjectFolders.h"

#include "Ids.h"

#include <algorithm>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>

#include <unistd.h>

namespace rovox
{

namespace
{
std::string isoNow()
{
    const auto t = std::time (nullptr);
    std::tm tm {};
    gmtime_r (&t, &tm);
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

bool isTop (const std::string& parentId)
{
    return parentId.empty() || parentId == ProjectFolders::rootId;
}

/** UTF-8 to code points; false on malformed input. */
bool decodeUtf8 (const std::string& s, std::vector<char32_t>& out)
{
    out.clear();

    for (size_t i = 0; i < s.size();)
    {
        const auto c = (unsigned char) s[i];
        int extra = 0;
        char32_t cp = 0;

        if (c < 0x80)                   { cp = c; }
        else if ((c & 0xe0) == 0xc0)    { cp = c & 0x1f; extra = 1; }
        else if ((c & 0xf0) == 0xe0)    { cp = c & 0x0f; extra = 2; }
        else if ((c & 0xf8) == 0xf0)    { cp = c & 0x07; extra = 3; }
        else                            return false;

        if (extra > 0 && i + (size_t) extra >= s.size())
            return false;

        for (int k = 1; k <= extra; ++k)
        {
            const auto cc = (unsigned char) s[i + (size_t) k];

            if ((cc & 0xc0) != 0x80)
                return false;

            cp = (cp << 6) | (cc & 0x3f);
        }

        // overlong forms, surrogates and values beyond Unicode
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff))
            return false;

        out.push_back (cp);
        i += (size_t) extra + 1;
    }

    return true;
}

void encodeUtf8 (char32_t cp, std::string& out)
{
    if (cp < 0x80)
    {
        out += (char) cp;
    }
    else if (cp < 0x800)
    {
        out += (char) (0xc0 | (cp >> 6));
        out += (char) (0x80 | (cp & 0x3f));
    }
    else if (cp < 0x10000)
    {
        out += (char) (0xe0 | (cp >> 12));
        out += (char) (0x80 | ((cp >> 6) & 0x3f));
        out += (char) (0x80 | (cp & 0x3f));
    }
    else
    {
        out += (char) (0xf0 | (cp >> 18));
        out += (char) (0x80 | ((cp >> 12) & 0x3f));
        out += (char) (0x80 | ((cp >> 6) & 0x3f));
        out += (char) (0x80 | (cp & 0x3f));
    }
}

char32_t foldCodePoint (char32_t c)
{
    if (c >= 'A' && c <= 'Z')                                   return c + 32;
    if (c >= 0xc0 && c <= 0xde && c != 0xd7)                    return c + 32;      // Latin-1
    if (c >= 0x391 && c <= 0x3a9 && c != 0x3a2)                 return c + 32;      // Greek
    if (c >= 0x400 && c <= 0x40f)                               return c + 80;      // Cyrillic Ѐ..Џ
    if (c >= 0x410 && c <= 0x42f)                               return c + 32;      // Cyrillic А..Я
    if (((c >= 0x460 && c <= 0x481) || (c >= 0x48a && c <= 0x4bf)) && c % 2 == 0)   return c + 1;   // Ѡ.., Ґ, Ғ ...
    if (c >= 0x100 && c <= 0x17f && c % 2 == 0 && c != 0x130)   return c + 1;       // most of Latin Extended-A

    return c;
}

const Folder& testsFolder()
{
    static const Folder f { ProjectFolders::testsId, ProjectFolders::testsName, "", "", "", true };
    return f;
}

bool siblingNameTaken (const FoldersDoc& doc, const std::string& parentId, const std::string& name, const std::string& exceptId)
{
    const auto key = ProjectFolders::foldCase (name);

    for (auto& f : doc.folders)
        if (f.id != exceptId && f.parentId == parentId && ProjectFolders::foldCase (f.name) == key)
            return true;

    return false;
}

Error duplicateName (const std::string& name)
{
    return makeError (errc::invalidParams, "a folder named '" + name + "' already exists here", json { { "path", "/name" } });
}

Error folderNotFound (const std::string& id, const char* param = "folder_id")
{
    return makeError (errc::notFound, "no folder " + id, json { { param, id } });
}

Error testsIsFixed()
{
    return makeError (errc::preconditionFailed, "the system folder 'tests' cannot be renamed, moved or deleted", json { { "folder_id", ProjectFolders::testsId } });
}

/** Height of the subtree below `id` (1 = no subfolders). */
int heightOf (const FoldersDoc& doc, const std::string& id, int guard = 0)
{
    if (guard > ProjectFolders::maxFolders)
        return 1;

    int h = 1;

    for (auto& f : doc.folders)
        if (f.parentId == id)
            h = std::max (h, 1 + heightOf (doc, f.id, guard + 1));

    return h;
}
} // namespace

//==============================================================================
json FoldersDoc::toJson() const
{
    json list = json::array();

    for (auto& f : folders)
        list.push_back ({ { "folder_id", f.id }, { "name", f.name }, { "parent_id", f.parentId.empty() ? json (nullptr) : json (f.parentId) },
                          { "created_at", f.createdAt }, { "updated_at", f.updatedAt } });

    json map = json::object();

    for (auto& [p, f] : projects)
        map[p] = f;

    return { { "schema", 1 }, { "version", version }, { "updated_at", updatedAt }, { "folders", list }, { "projects", map } };
}

FoldersDoc FoldersDoc::fromJson (const json& j)
{
    FoldersDoc doc;

    if (! j.is_object())
        return doc;

    if (j.contains ("version") && j["version"].is_number_unsigned())
        doc.version = j["version"].get<std::uint64_t>();

    if (j.contains ("updated_at") && j["updated_at"].is_string())
        doc.updatedAt = j["updated_at"].get<std::string>();

    std::set<std::string> ids;

    if (j.contains ("folders") && j["folders"].is_array())
        for (auto& f : j["folders"])
        {
            if (! f.is_object() || ! f.contains ("folder_id") || ! f["folder_id"].is_string() || ! f.contains ("name") || ! f["name"].is_string())
                continue;

            Folder folder;
            folder.id = f["folder_id"].get<std::string>();

            if (! isValidId (folder.id, "fld") || ids.count (folder.id) != 0)
                continue;                               // a stored `tests` (or anything else that is not a folder id) is ignored

            folder.name = f["name"].get<std::string>();
            folder.parentId = f.contains ("parent_id") && f["parent_id"].is_string() ? f["parent_id"].get<std::string>() : "";
            folder.createdAt = f.value ("created_at", "");
            folder.updatedAt = f.value ("updated_at", "");

            if (folder.parentId == ProjectFolders::rootId)
                folder.parentId.clear();

            ids.insert (folder.id);
            doc.folders.push_back (std::move (folder));
        }

    // a parent that does not exist (or a cycle in a hand-edited file) puts the folder at the top level
    for (auto& f : doc.folders)
        if (! f.parentId.empty() && ids.count (f.parentId) == 0)
            f.parentId.clear();

    for (auto& f : doc.folders)
    {
        std::string at = f.parentId;

        for (int steps = 0; ! at.empty(); ++steps)
        {
            if (at == f.id || steps > ProjectFolders::maxFolders)
            {
                f.parentId.clear();
                break;
            }

            auto it = std::find_if (doc.folders.begin(), doc.folders.end(), [&] (const Folder& o) { return o.id == at; });
            at = it == doc.folders.end() ? "" : it->parentId;
        }
    }

    if (j.contains ("projects") && j["projects"].is_object())
        for (auto& [p, f] : j["projects"].items())
            if (isValidId (p, "p") && f.is_string())
                doc.projects[p] = f.get<std::string>();

    return doc;
}

const Folder* FoldersDoc::find (const std::string& folderId) const
{
    if (folderId == ProjectFolders::testsId)
        return &testsFolder();

    for (auto& f : folders)
        if (f.id == folderId)
            return &f;

    return nullptr;
}

std::string FoldersDoc::folderOf (const std::string& projectId) const
{
    auto it = projects.find (projectId);

    if (it == projects.end() || find (it->second) == nullptr)
        return "";

    return it->second;
}

std::set<std::string> FoldersDoc::subtree (const std::string& folderId) const
{
    std::set<std::string> out { folderId };

    for (bool grew = true; grew;)
    {
        grew = false;

        for (auto& f : folders)
            if (! f.parentId.empty() && out.count (f.parentId) != 0 && out.insert (f.id).second)
                grew = true;
    }

    return out;
}

int FoldersDoc::depthOf (const std::string& folderId) const
{
    int depth = 0;
    std::string at = folderId;

    while (! at.empty() && depth <= ProjectFolders::maxFolders)
    {
        auto* f = find (at);

        if (f == nullptr)
            break;

        ++depth;
        at = f->parentId;
    }

    return depth;
}

//==============================================================================
ProjectFolders::ProjectFolders (LocalPath p) : localPath_ (std::move (p)) {}

void ProjectFolders::setStore (std::shared_ptr<ObjectStore> store, StorageKeys keys, std::function<bool()> conditional)
{
    std::lock_guard<std::recursive_mutex> lock (mutex_);
    store_ = std::move (store);
    keys_ = std::move (keys);
    conditional_ = std::move (conditional);
    cache_.clear();
}

std::string ProjectFolders::key (const std::string& tenant) const
{
    return keys_.folders (tenant);
}

Result<std::string> ProjectFolders::normaliseName (const std::string& raw)
{
    auto isSpace = [] (char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; };
    size_t b = 0, e = raw.size();

    while (b < e && isSpace (raw[b]))       ++b;
    while (e > b && isSpace (raw[e - 1]))   --e;

    const auto name = raw.substr (b, e - b);
    std::vector<char32_t> cps;

    if (! decodeUtf8 (name, cps))
        return makeError (errc::invalidParams, "folder name is not valid UTF-8", json { { "path", "/name" } });

    if (cps.empty() || cps.size() > (size_t) maxNameLength)
        return makeError (errc::invalidParams, "folder name must be 1 to " + std::to_string (maxNameLength) + " characters", json { { "path", "/name" } });

    for (auto c : cps)
        if (c == '/' || c < 0x20 || (c >= 0x7f && c <= 0x9f))
            return makeError (errc::invalidParams, "folder name must not contain '/' or control characters", json { { "path", "/name" } });

    return name;
}

std::string ProjectFolders::foldCase (const std::string& s)
{
    std::vector<char32_t> cps;

    if (! decodeUtf8 (s, cps))
        return s;

    std::string out;

    for (auto c : cps)
        encodeUtf8 (foldCodePoint (c), out);

    return out;
}

//==============================================================================
FoldersDoc ProjectFolders::readLocal (const std::string& tenant) const
{
    std::ifstream in (localPath_ (tenant), std::ios::binary);

    if (! in)
        return {};

    std::stringstream ss;
    ss << in.rdbuf();
    return FoldersDoc::fromJson (json::parse (ss.str(), nullptr, false));
}

Result<void> ProjectFolders::writeLocal (const std::string& tenant, const std::string& body) const
{
    const auto file = localPath_ (tenant);
    std::error_code ec;
    std::filesystem::create_directories (file.parent_path(), ec);
    const auto tmp = file.string() + ".tmp-" + std::to_string ((long) ::getpid());

    {
        std::ofstream out (tmp, std::ios::binary | std::ios::trunc);
        out << body;
        out.flush();

        if (! out)
            return makeError (errc::io, "cannot write " + tmp);
    }

    if (::rename (tmp.c_str(), file.c_str()) != 0)
    {
        std::filesystem::remove (tmp, ec);
        return makeError (errc::io, "cannot replace " + file.string());
    }

    return {};
}

FoldersDoc ProjectFolders::read (const std::string& tenant)
{
    std::lock_guard<std::recursive_mutex> lock (mutex_);

    if (store_ == nullptr)
        return readLocal (tenant);

    const auto now = std::chrono::steady_clock::now();
    auto cached = cache_.find (tenant);

    if (cached != cache_.end() && now - cached->second.fetched < cacheTtl_)
        return cached->second.doc;

    auto head = store_->head (key (tenant));

    if (head.ok())
    {
        if (cached != cache_.end() && ! cached->second.etag.empty() && cached->second.etag == head.value().etag)
        {
            cached->second.fetched = now;
            return cached->second.doc;
        }

        if (auto body = store_->get (key (tenant)); body.ok())
        {
            auto doc = FoldersDoc::fromJson (json::parse (body.value(), nullptr, false));
            cache_[tenant] = { doc, head.value().etag, now };
            writeLocal (tenant, body.value());
            return doc;
        }
    }
    else if (head.error().code == errc::notFound)
    {
        cache_[tenant] = { FoldersDoc {}, "", now };
        return {};
    }

    // the bucket cannot be reached: the last copy this server saw (not marked fresh, so the next read asks the bucket again)
    if (cached != cache_.end())
        return cached->second.doc;

    return readLocal (tenant);
}

//==============================================================================
void ProjectFolders::finish (const std::string& tenant, FoldersDoc& doc, std::uint64_t previousVersion)
{
    doc.version = previousVersion + 1;
    doc.updatedAt = isoNow();

    std::optional<KnownProjects> known;

    if (known_)
        known = known_ (tenant);

    for (auto it = doc.projects.begin(); it != doc.projects.end();)
    {
        const bool folderGone = doc.find (it->second) == nullptr;
        const bool projectGone = known && known->complete && known->ids.count (it->first) == 0;
        it = (folderGone || projectGone) ? doc.projects.erase (it) : std::next (it);
    }
}

Result<FoldersDoc> ProjectFolders::updateLocal (const std::string& tenant, const Mutate& mutate)
{
    auto doc = readLocal (tenant);
    const auto previous = doc.version;

    if (auto r = mutate (doc); ! r.ok())
        return r.error();

    finish (tenant, doc, previous);

    if (auto w = writeLocal (tenant, doc.toJson().dump()); ! w.ok())
        return w.error();

    return doc;
}

Result<FoldersDoc> ProjectFolders::updateRemote (const std::string& tenant, const Mutate& mutate)
{
    const auto k = key (tenant);
    const bool conditional = conditional_ ? conditional_() : false;
    auto unreachable = [] (const Error& e)
    {
        return makeError (errc::io, "folders cannot be changed while the object store is unreachable: " + e.message);
    };

    for (int attempt = 0; attempt < maxAttempts; ++attempt)
    {
        // always fresh: head for the ETag, then the body (if it changed in between, the conditional put fails and this goes round again)
        FoldersDoc current;
        std::string etag;
        bool exists = false;

        if (auto head = store_->head (k); head.ok())
        {
            auto body = store_->get (k);

            if (! body.ok())
            {
                if (body.error().code == errc::notFound)
                    continue;

                return unreachable (body.error());
            }

            exists = true;
            etag = head.value().etag;
            current = FoldersDoc::fromJson (json::parse (body.value(), nullptr, false));
        }
        else if (head.error().code != errc::notFound)
        {
            return unreachable (head.error());
        }

        auto next = current;

        if (auto r = mutate (next); ! r.ok())
            return r.error();

        finish (tenant, next, current.version);
        const auto body = next.toJson().dump();

        if (conditional)
        {
            PutCondition cond;

            if (exists && ! etag.empty())
                cond.ifMatch = etag;
            else
                cond.ifNoneMatch = "*";

            auto put = store_->putIf (k, body, "application/json", cond);

            if (! put.ok() && put.error().code == putConditionFailed)
                continue;                               // somebody else wrote in between: read again and re-apply

            if (! put.ok())
                return unreachable (put.error());
        }
        else
        {
            // best effort for stores that ignore preconditions: write, read back, and go round again if another writer's document is there now
            if (auto put = store_->put (k, body, "application/json"); ! put.ok())
                return unreachable (put.error());

            auto back = store_->get (k);

            if (! back.ok())
                return unreachable (back.error());

            const auto readBack = FoldersDoc::fromJson (json::parse (back.value(), nullptr, false));

            if (readBack.version != next.version || back.value() != body)
                continue;
        }

        cache_[tenant] = { next, "", std::chrono::steady_clock::now() };
        writeLocal (tenant, body);
        return next;
    }

    cache_.erase (tenant);
    return makeError (errc::busy, "the folders were changed concurrently " + std::to_string (maxAttempts) + " times in a row; try again");
}

Result<FoldersDoc> ProjectFolders::update (const std::string& tenant, const Mutate& mutate)
{
    std::lock_guard<std::recursive_mutex> lock (mutex_);
    return store_ == nullptr ? updateLocal (tenant, mutate) : updateRemote (tenant, mutate);
}

//==============================================================================
Result<ProjectFolders::Change> ProjectFolders::create (const std::string& tenant, const std::string& rawName, const std::string& rawParent)
{
    auto name = normaliseName (rawName);

    if (! name.ok())
        return name.error();

    const auto parent = isTop (rawParent) ? std::string() : rawParent;

    if (parent == testsId)
        return makeError (errc::invalidParams, "the system folder 'tests' cannot have subfolders", json { { "path", "/parent_id" } });

    Folder created;
    created.id = newId ("fld");
    created.name = name.value();
    created.parentId = parent;
    created.createdAt = created.updatedAt = isoNow();

    auto doc = update (tenant, [&] (FoldersDoc& d) -> Result<void>
    {
        if (! parent.empty() && d.find (parent) == nullptr)
            return folderNotFound (parent, "parent_id");

        if (! parent.empty() && d.depthOf (parent) + 1 > maxDepth)
            return makeError (errc::invalidParams, "folders can be nested at most " + std::to_string (maxDepth) + " levels deep", json { { "path", "/parent_id" } });

        if ((int) d.folders.size() >= maxFolders)
            return makeError (errc::quotaExceeded, "at most " + std::to_string (maxFolders) + " folders per tenant", json { { "limit", maxFolders } });

        if (siblingNameTaken (d, parent, created.name, ""))
            return duplicateName (created.name);

        d.folders.push_back (created);
        return {};
    });

    if (! doc.ok())
        return doc.error();

    return Change { doc.value().version, created };
}

Result<ProjectFolders::Change> ProjectFolders::rename (const std::string& tenant, const std::string& folderId, const std::string& rawName)
{
    if (folderId == testsId)
        return testsIsFixed();

    auto name = normaliseName (rawName);

    if (! name.ok())
        return name.error();

    Folder renamed;
    auto doc = update (tenant, [&] (FoldersDoc& d) -> Result<void>
    {
        auto it = std::find_if (d.folders.begin(), d.folders.end(), [&] (const Folder& f) { return f.id == folderId; });

        if (it == d.folders.end())
            return folderNotFound (folderId);

        if (siblingNameTaken (d, it->parentId, name.value(), folderId))
            return duplicateName (name.value());

        it->name = name.value();
        it->updatedAt = isoNow();
        renamed = *it;
        return {};
    });

    if (! doc.ok())
        return doc.error();

    return Change { doc.value().version, renamed };
}

Result<ProjectFolders::Change> ProjectFolders::move (const std::string& tenant, const std::string& folderId, const std::string& rawParent)
{
    if (folderId == testsId)
        return testsIsFixed();

    const auto parent = isTop (rawParent) ? std::string() : rawParent;

    if (parent == testsId)
        return makeError (errc::invalidParams, "the system folder 'tests' cannot have subfolders", json { { "path", "/parent_id" } });

    Folder moved;
    auto doc = update (tenant, [&] (FoldersDoc& d) -> Result<void>
    {
        auto it = std::find_if (d.folders.begin(), d.folders.end(), [&] (const Folder& f) { return f.id == folderId; });

        if (it == d.folders.end())
            return folderNotFound (folderId);

        if (! parent.empty() && d.find (parent) == nullptr)
            return folderNotFound (parent, "parent_id");

        if (! parent.empty() && d.subtree (folderId).count (parent) != 0)
            return makeError (errc::invalidParams, "a folder cannot be moved into itself or one of its subfolders", json { { "path", "/parent_id" } });

        if ((parent.empty() ? 0 : d.depthOf (parent)) + heightOf (d, folderId) > maxDepth)
            return makeError (errc::invalidParams, "folders can be nested at most " + std::to_string (maxDepth) + " levels deep", json { { "path", "/parent_id" } });

        if (siblingNameTaken (d, parent, it->name, folderId))
            return duplicateName (it->name);

        it->parentId = parent;
        it->updatedAt = isoNow();
        moved = *it;
        return {};
    });

    if (! doc.ok())
        return doc.error();

    return Change { doc.value().version, moved };
}

Result<ProjectFolders::Change> ProjectFolders::remove (const std::string& tenant, const std::string& folderId)
{
    if (folderId == testsId)
        return testsIsFixed();

    Folder removed;
    auto doc = update (tenant, [&] (FoldersDoc& d) -> Result<void>
    {
        auto it = std::find_if (d.folders.begin(), d.folders.end(), [&] (const Folder& f) { return f.id == folderId; });

        if (it == d.folders.end())
            return folderNotFound (folderId);

        const auto subfolders = std::count_if (d.folders.begin(), d.folders.end(), [&] (const Folder& f) { return f.parentId == folderId; });
        std::optional<KnownProjects> known;

        if (known_)
            known = known_ (tenant);

        int projects = 0;

        for (auto& [p, f] : d.projects)
            if (f == folderId && (! known || known->ids.count (p) != 0))
                ++projects;

        if (subfolders > 0 || projects > 0)
            return makeError (errc::preconditionFailed, "the folder is not empty", json { { "folder_id", folderId }, { "projects", projects }, { "folders", subfolders } });

        removed = *it;
        d.folders.erase (it);
        return {};
    });

    if (! doc.ok())
        return doc.error();

    return Change { doc.value().version, removed };
}

Result<ProjectFolders::Change> ProjectFolders::assign (const std::string& tenant, const std::vector<std::string>& projectIds, const std::string& rawFolder)
{
    const auto folder = isTop (rawFolder) ? std::string() : rawFolder;
    Folder target;

    auto doc = update (tenant, [&] (FoldersDoc& d) -> Result<void>
    {
        if (! folder.empty())
        {
            auto* f = d.find (folder);

            if (f == nullptr)
                return folderNotFound (folder);

            target = *f;
        }

        for (auto& p : projectIds)
        {
            if (folder.empty())
                d.projects.erase (p);
            else
                d.projects[p] = folder;
        }

        return {};
    });

    if (! doc.ok())
        return doc.error();

    return Change { doc.value().version, target };
}

void ProjectFolders::forget (const std::string& tenant, const std::string& projectId)
{
    std::lock_guard<std::recursive_mutex> lock (mutex_);

    if (read (tenant).projects.count (projectId) == 0)
        return;

    auto r = update (tenant, [&] (FoldersDoc& d) -> Result<void>
    {
        d.projects.erase (projectId);
        return {};
    });

    if (! r.ok())
        std::cerr << "rovox: warning: could not drop the folder assignment of deleted project " << projectId << ": " << r.error().message << "\n";
}

//==============================================================================
json ProjectFolders::folderJson (const Folder& f, const FoldersDoc& doc, const std::set<std::string>& existing)
{
    int projects = 0, folders = 0;

    for (auto& [p, id] : doc.projects)
        if (id == f.id && existing.count (p) != 0)
            ++projects;

    for (auto& o : doc.folders)
        if (o.parentId == f.id)
            ++folders;

    auto orNull = [] (const std::string& s) { return s.empty() ? json (nullptr) : json (s); };
    return { { "folder_id", f.id }, { "name", f.name }, { "parent_id", orNull (f.parentId) }, { "created_at", orNull (f.createdAt) },
             { "updated_at", orNull (f.updatedAt) }, { "system", f.system }, { "project_count", projects }, { "folder_count", folders } };
}

json ProjectFolders::foldersJson (const FoldersDoc& doc, const std::set<std::string>& existing)
{
    std::vector<const Folder*> sorted;

    for (auto& f : doc.folders)
        sorted.push_back (&f);

    std::stable_sort (sorted.begin(), sorted.end(), [] (const Folder* a, const Folder* b)
    {
        const auto ka = foldCase (a->name), kb = foldCase (b->name);
        return ka != kb ? ka < kb : a->id < b->id;
    });

    json out = json::array();

    for (auto* f : sorted)
        out.push_back (folderJson (*f, doc, existing));

    out.push_back (folderJson (testsFolder(), doc, existing));
    return out;
}

} // namespace rovox
