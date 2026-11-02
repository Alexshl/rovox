// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "ProjectFormat.h"

#include "Ids.h"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <fstream>
#include <functional>
#include <sstream>

#include <fcntl.h>
#include <unistd.h>

namespace rovox
{

EditSummary summarizeEdit (const fs::path& editXml)
{
    EditSummary out;
    std::error_code ec;

    if (fs::file_size (editXml, ec) > 256u * 1024u * 1024u || ec)
        return out;

    auto xml = juce::parseXML (toFile (editXml));

    if (xml == nullptr)
        return out;

    for (auto* c : xml->getChildIterator())
        if (c->hasTagName ("TRACK"))
            ++out.tracks;

    std::function<void (const juce::XmlElement&)> visit = [&] (const juce::XmlElement& e)
    {
        if (e.getTagName().containsIgnoreCase ("CLIP") && e.hasAttribute ("start") && e.hasAttribute ("length"))
            out.durationS = std::max (out.durationS, e.getDoubleAttribute ("start") + e.getDoubleAttribute ("length"));

        for (auto* c : e.getChildIterator())
            visit (*c);
    };
    visit (*xml);
    return out;
}

std::string nowIso8601()
{
    const auto t = std::time (nullptr);
    std::tm tm {};
    gmtime_r (&t, &tm);
    char buf[32];
    std::strftime (buf, sizeof (buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

json MediaEntry::toJson() const
{
    json j { { "media_id", mediaId }, { "path", path }, { "sha256", sha256 }, { "orig_name", origName },
             { "channels", channels }, { "sample_rate", sampleRate }, { "bit_depth", bitDepth }, { "frames", frames } };

    if (external)
        j["external"] = true;

    return j;
}

MediaEntry MediaEntry::fromJson (const json& j)
{
    MediaEntry m;
    m.mediaId = j.value ("media_id", "");
    m.path = j.value ("path", "");
    m.sha256 = j.value ("sha256", "");
    m.origName = j.value ("orig_name", "");
    m.channels = j.value ("channels", 0);
    m.sampleRate = j.value ("sample_rate", 0.0);
    m.bitDepth = j.value ("bit_depth", 0);
    m.frames = j.value ("frames", (std::int64_t) 0);
    m.external = j.value ("external", false);
    return m;
}

json Manifest::toJson() const
{
    json media_ = json::array();

    for (auto& m : media)
        media_.push_back (m.toJson());

    json j { { "schema", schema }, { "project_id", projectId }, { "name", name }, { "revision", revision },
             { "sample_rate", sampleRate }, { "created_at", createdAt }, { "saved_at", savedAt }, { "media", media_ },
             { "render_presets", renderPresets }, { "delivery_defaults", deliveryDefaults },
             { "tenant_id", tenantId }, { "ephemeral", ephemeral } };

    j["ttl_s"] = ttlS.has_value() ? json (*ttlS) : json (nullptr);
    return j;
}

Result<json> migrateManifest (json doc)
{
    if (! doc.is_object())
        return makeError (errc::invalidParams, "rovox.json is not an object");

    int schema = doc.value ("schema", 0);

    if (schema > currentSchema)
        return makeError (errc::unsupported, "project schema " + std::to_string (schema) + " is newer than this build supports ("
                                                 + std::to_string (currentSchema) + ")");

    if (schema < 1)
    {
        // 0 -> 1: the pre-release manifest used "id" and "rate"
        if (doc.contains ("id") && ! doc.contains ("project_id"))   { doc["project_id"] = doc["id"]; doc.erase ("id"); }
        if (doc.contains ("rate") && ! doc.contains ("sample_rate")) { doc["sample_rate"] = doc["rate"]; doc.erase ("rate"); }
        doc["schema"] = 1;
    }

    return doc;
}

void migrateEditState (juce::ValueTree&, int) {}

Result<Manifest> Manifest::fromJson (json doc)
{
    auto migrated = migrateManifest (std::move (doc));

    if (! migrated.ok())
        return migrated.error();

    const auto& j = migrated.value();
    Manifest m;

    try
    {
        m.schema = j.value ("schema", currentSchema);
        m.projectId = j.at ("project_id").get<std::string>();
        m.name = j.value ("name", "Untitled");
        m.revision = j.value ("revision", (std::uint64_t) 0);
        m.sampleRate = j.value ("sample_rate", 44100.0);
        m.createdAt = j.value ("created_at", "");
        m.savedAt = j.value ("saved_at", "");
        m.renderPresets = j.value ("render_presets", json::object());
        m.deliveryDefaults = j.value ("delivery_defaults", json::object());
        m.tenantId = j.value ("tenant_id", "local");
        m.ephemeral = j.value ("ephemeral", false);

        if (j.contains ("ttl_s") && j["ttl_s"].is_number_integer())
            m.ttlS = j["ttl_s"].get<int>();

        if (j.contains ("media"))
            for (auto& e : j["media"])
                m.media.push_back (MediaEntry::fromJson (e));
    }
    catch (const std::exception& e)
    {
        return makeError (errc::invalidParams, std::string ("invalid rovox.json: ") + e.what());
    }

    if (! isValidTenantId (m.tenantId))
        return makeError (errc::invalidParams, "invalid tenant_id in rovox.json");

    return m;
}

const MediaEntry* Manifest::findMedia (const std::string& sha) const
{
    for (auto& m : media)
        if (m.sha256 == sha)
            return &m;

    return nullptr;
}

//==============================================================================
void fsyncDirectory (const fs::path& dir)
{
    const int fd = ::open (dir.c_str(), O_RDONLY | O_DIRECTORY);

    if (fd >= 0)
    {
        ::fsync (fd);
        ::close (fd);
    }
}

namespace
{
Result<void> writeTemp (const fs::path& tmp, const std::string& content, bool doFsync)
{
    const int fd = ::open (tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);

    if (fd < 0)
        return makeError (errc::io, "cannot write " + tmp.string());

    size_t off = 0;

    while (off < content.size())
    {
        const auto n = ::write (fd, content.data() + off, content.size() - off);

        if (n <= 0)
        {
            ::close (fd);
            return makeError (errc::io, "short write to " + tmp.string());
        }

        off += (size_t) n;
    }

    if (doFsync)
        ::fsync (fd);

    ::close (fd);
    return {};
}

std::string readFile (const fs::path& p)
{
    std::ifstream in (p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

std::string editXml (const juce::ValueTree& state, std::uint64_t revision)
{
    auto copy = state.createCopy();
    copy.setProperty (prop::revision, (juce::int64) revision, nullptr);
    copy.setProperty (prop::schema, currentSchema, nullptr);
    std::unique_ptr<juce::XmlElement> xml (copy.createXml());
    return xml->toString().toStdString();
}

juce::ValueTree parseEdit (const fs::path& file)
{
    std::unique_ptr<juce::XmlElement> xml (juce::XmlDocument::parse (toFile (file)));
    return xml != nullptr ? juce::ValueTree::fromXml (*xml) : juce::ValueTree();
}
} // namespace

Result<void> writeFileAtomic (const fs::path& path, const std::string& content, bool doFsync)
{
    auto tmp = path;
    tmp += ".tmp";

    if (auto r = writeTemp (tmp, content, doFsync); ! r.ok())
        return r;

    std::error_code ec;
    fs::rename (tmp, path, ec);
    return ec ? Result<void> (makeError (errc::io, "rename failed: " + ec.message())) : Result<void>();
}

Result<void> writeProject (const fs::path& dir, const juce::ValueTree& editState, const Manifest& manifestIn, const SaveOptions& opt)
{
    std::error_code ec;
    fs::create_directories (dir, ec);

    auto manifest = manifestIn;
    manifest.savedAt = nowIso8601();

    if (manifest.createdAt.empty())
        manifest.createdAt = manifest.savedAt;

    const auto editFile = dir / editFileName, manifestFile = dir / manifestFileName;
    auto editTmp = editFile, manifestTmp = manifestFile;
    editTmp += ".tmp";
    manifestTmp += ".tmp";

    if (auto r = writeTemp (editTmp, editXml (editState, manifest.revision), opt.fsync); ! r.ok())
        return r;

    if (auto r = writeTemp (manifestTmp, manifest.toJson().dump (2) + "\n", opt.fsync); ! r.ok())
        return r;

    if (opt.fault == SaveFault::afterTempFiles)
        return makeError (errc::io, "fault injected: after temp files");

    fs::rename (editTmp, editFile, ec);

    if (ec)
        return makeError (errc::io, "rename edit.xml failed: " + ec.message());

    if (opt.fault == SaveFault::betweenRenames)
        return makeError (errc::io, "fault injected: between renames");

    fs::rename (manifestTmp, manifestFile, ec);

    if (ec)
        return makeError (errc::io, "rename rovox.json failed: " + ec.message());

    if (opt.fsync)
        fsyncDirectory (dir);

    return {};
}

std::vector<std::uint64_t> listAutosaves (const fs::path& dir)
{
    std::vector<std::uint64_t> out;
    std::error_code ec;

    for (auto& e : fs::directory_iterator (dir / "autosave", ec))
    {
        const auto name = e.path().filename().string();

        if (e.is_directory() && name.size() > 4 && name.substr (name.size() - 4) == ".rvx")
        {
            std::uint64_t rev = 0;
            const auto stem = name.substr (0, name.size() - 4);

            if (std::from_chars (stem.data(), stem.data() + stem.size(), rev).ec == std::errc())
                out.push_back (rev);
        }
    }

    std::sort (out.begin(), out.end());
    return out;
}

Result<LoadedProject> readProject (const fs::path& dir)
{
    const auto manifestFile = dir / manifestFileName, editFile = dir / editFileName;

    if (! fs::exists (manifestFile))
        return makeError (errc::notFound, "not a Rovox project (no rovox.json): " + dir.string());

    json doc = json::parse (readFile (manifestFile), nullptr, false);

    if (doc.is_discarded())
        return makeError (errc::invalidParams, "rovox.json is not valid JSON: " + manifestFile.string());

    auto manifest = Manifest::fromJson (std::move (doc));

    if (! manifest.ok())
        return manifest.error();

    LoadedProject out;
    out.manifest = manifest.value();
    out.editState = fs::exists (editFile) ? parseEdit (editFile) : juce::ValueTree();

    if (! out.editState.isValid() || ! out.editState.hasType (te::IDs::EDIT))
    {
        // edit.xml unreadable: fall back to the newest autosave that parses
        const auto autosaves = listAutosaves (dir);

        for (auto it = autosaves.rbegin(); it != autosaves.rend(); ++it)
            if (auto r = readAutosave (dir, *it); r.ok())
            {
                r.value().warnings.push_back ("edit.xml is unreadable; restored autosave revision " + std::to_string (*it));
                return r;
            }

        return makeError (errc::io, "edit.xml is missing or unreadable and there is no usable autosave: " + dir.string());
    }

    const std::uint64_t editRevision = (std::uint64_t) (juce::int64) out.editState.getProperty (prop::revision, (juce::int64) out.manifest.revision);

    if (editRevision > out.manifest.revision)
    {
        out.warnings.push_back ("rovox.json was behind edit.xml (interrupted save); recovered revision " + std::to_string (editRevision));
        out.manifest.revision = editRevision;

        // media saved after the last manifest write: rebuild index entries from files present in media/
        std::error_code ec;

        for (auto& e : fs::directory_iterator (dir / "media", ec))
        {
            const auto sha = e.path().stem().string();

            if (out.manifest.findMedia (sha) == nullptr)
            {
                MediaEntry m;
                m.mediaId = newId ("med");
                m.sha256 = sha;
                m.path = "media/" + e.path().filename().string();
                out.manifest.media.push_back (std::move (m));
            }
        }
    }
    else if (editRevision < out.manifest.revision)
    {
        out.warnings.push_back ("edit.xml (revision " + std::to_string (editRevision) + ") is older than rovox.json (revision "
                                + std::to_string (out.manifest.revision) + ")");
        out.manifest.revision = editRevision;
    }

    migrateEditState (out.editState, (int) out.editState.getProperty (prop::schema, 1));

    if (const auto autosaves = listAutosaves (dir); ! autosaves.empty() && autosaves.back() > out.manifest.revision)
        out.newerAutosave = autosaves.back();

    return out;
}

Result<void> writeAutosave (const fs::path& dir, const juce::ValueTree& editState, const Manifest& manifest)
{
    const auto target = dir / "autosave" / (std::to_string (manifest.revision) + ".rvx");

    if (auto r = writeProject (target, editState, manifest, { .fsync = false }); ! r.ok())
        return r;

    auto all = listAutosaves (dir);

    while ((int) all.size() > autosaveKeep)
    {
        std::error_code ec;
        fs::remove_all (dir / "autosave" / (std::to_string (all.front()) + ".rvx"), ec);
        all.erase (all.begin());
    }

    return {};
}

Result<LoadedProject> readAutosave (const fs::path& dir, std::uint64_t revision)
{
    return readProject (dir / "autosave" / (std::to_string (revision) + ".rvx"));
}

} // namespace rovox
