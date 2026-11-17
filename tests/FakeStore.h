// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// An in-memory ObjectStore for the storage and folders tests (shared by storage_test.cpp and folders_test.cpp).
#pragma once

#include "ObjectStore.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace rovox::test
{

/** An in-memory bucket. Thread-safe; can be told to fail the next puts. */
class FakeStore : public ObjectStore
{
public:
    Result<ObjectMeta> head (const std::string& key) override
    {
        if (offline)
            return makeError (errc::io, "store unreachable (test)");

        if (headDelayMs > 0)
            std::this_thread::sleep_for (std::chrono::milliseconds (headDelayMs));

        std::lock_guard<std::mutex> lock (mutex);
        auto it = objects.find (key);

        if (it == objects.end())
            return makeError (errc::notFound, "no " + key);

        return ObjectMeta { key, etagOf (key), it->second.size() };
    }

    /** Test helper: writes an object behind the store's back (as another server would), bumping its ETag. */
    void setObject (const std::string& key, const std::string& body)
    {
        std::lock_guard<std::mutex> lock (mutex);
        objects[key] = body;
        versions[key] = nextVersion++;
    }

    Result<std::string> get (const std::string& key) override
    {
        if (offline)
            return makeError (errc::io, "store unreachable (test)");

        std::lock_guard<std::mutex> lock (mutex);
        auto it = objects.find (key);

        if (it == objects.end())
            return makeError (errc::notFound, "no " + key);

        return it->second;
    }

    Result<void> getFile (const std::string& key, const std::filesystem::path& dest) override
    {
        auto body = get (key);

        if (! body.ok())
            return body.error();

        std::error_code ec;
        std::filesystem::create_directories (dest.parent_path(), ec);
        std::ofstream (dest, std::ios::binary) << body.value();
        return {};
    }

    Result<void> put (const std::string& key, const std::string& body, const std::string&) override
    {
        return doPut (key, body, nullptr);
    }

    /** Honours the preconditions like S3 (412 = putConditionFailed) unless `honourConditions` is off (then it behaves like a store that ignores them). */
    Result<void> putIf (const std::string& key, const std::string& body, const std::string&, const PutCondition& cond) override
    {
        return doPut (key, body, &cond);
    }

    /** A multipart-like upload: `fileParts` parts of `partDelayMs` each, polling `cancel` between them. */
    Result<void> putFileCancellable (const std::string& key, const std::filesystem::path& file, const std::string& type, const CancelCheck& cancel) override
    {
        for (int i = 0; i < fileParts; ++i)
        {
            if (i > 0 && cancel && cancel())
            {
                cancelledUploads++;
                return makeError (errc::cancelled, "cancelled between parts");
            }

            partsStarted++;

            if (partDelayMs > 0)
                std::this_thread::sleep_for (std::chrono::milliseconds (partDelayMs));
        }

        return putFile (key, file, type);
    }

    Result<void> putFile (const std::string& key, const std::filesystem::path& file, const std::string& type) override
    {
        std::ifstream in (file, std::ios::binary);
        std::stringstream ss;
        ss << in.rdbuf();
        return put (key, ss.str(), type);
    }

    Result<ListResult> list (const std::string& prefix, const std::string& delimiter) override
    {
        if (offline)
            return makeError (errc::io, "store unreachable (test)");

        std::lock_guard<std::mutex> lock (mutex);
        ListResult out;
        std::set<std::string> folded;

        for (auto& [k, v] : objects)
        {
            if (k.rfind (prefix, 0) != 0)
                continue;

            const auto rest = k.substr (prefix.size());

            if (! delimiter.empty() && rest.find (delimiter) != std::string::npos)
                folded.insert (prefix + rest.substr (0, rest.find (delimiter) + 1));
            else
                out.objects.push_back ({ k, etagOf (k), v.size() });
        }

        out.prefixes.assign (folded.begin(), folded.end());
        return out;
    }

    Result<void> remove (const std::string& key) override
    {
        std::lock_guard<std::mutex> lock (mutex);
        objects.erase (key);
        return {};
    }

    std::string describe() const override   { return "fake"; }

    size_t countPuts (const std::string& containing)
    {
        std::lock_guard<std::mutex> lock (mutex);
        return (size_t) std::count_if (putLog.begin(), putLog.end(), [&] (const std::string& k) { return k.find (containing) != std::string::npos; });
    }

    std::mutex mutex;
    std::map<std::string, std::string> objects;
    std::map<std::string, int> versions;
    std::vector<std::string> putLog;
    int failPuts = 0, nextVersion = 1;
    bool honourConditions = true;
    std::atomic<bool> offline { false };    // every call fails like an unreachable endpoint
    /** Called after a successful put (outside the lock): lets a test play a concurrent writer. */
    std::function<void (const std::string& key)> afterPut;
    std::string slowKey;                    // puts of keys containing this take slowPutMs
    std::atomic<int> slowPutMs { 0 }, headDelayMs { 0 }, fileParts { 1 }, partDelayMs { 0 }, cancelledUploads { 0 }, partsStarted { 0 };

private:
    std::string etagOf (const std::string& key) const      // call with the mutex held
    {
        auto it = versions.find (key);
        return "\"v" + std::to_string (it == versions.end() ? 0 : it->second) + "\"";
    }

    Result<void> doPut (const std::string& key, const std::string& body, const PutCondition* cond)
    {
        auto r = doPutLocked (key, body, cond);

        if (r.ok() && afterPut)
            afterPut (key);

        return r;
    }

    Result<void> doPutLocked (const std::string& key, const std::string& body, const PutCondition* cond)
    {
        if (offline)
            return makeError (errc::io, "store unreachable (test)");

        if (slowPutMs > 0 && ! slowKey.empty() && key.find (slowKey) != std::string::npos)
            std::this_thread::sleep_for (std::chrono::milliseconds (slowPutMs));

        std::lock_guard<std::mutex> lock (mutex);

        if (failPuts > 0)
        {
            --failPuts;
            return makeError (errc::busy, "injected failure");
        }

        if (cond != nullptr && honourConditions)
        {
            const bool exists = objects.count (key) > 0;

            if (! cond->ifNoneMatch.empty() && exists)
                return makeError (putConditionFailed, "412: " + key + " exists");

            if (! cond->ifMatch.empty() && (! exists || etagOf (key) != cond->ifMatch))
                return makeError (putConditionFailed, "412: " + key + " changed");
        }

        objects[key] = body;
        versions[key] = nextVersion++;
        putLog.push_back (key);
        return {};
    }
};

} // namespace rovox::test
