// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// A local HTTP server for the delivery tests: records what it receives and answers with a scripted status / body. No real network.
#pragma once

#include <httplib.h>

#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace rovox::test
{

struct Recorded
{
    std::string method, path, body;
    httplib::Headers headers;
    std::map<std::string, std::string> fields, fileNames, fileContents, fileTypes;
};

/** A local HTTP server that records what it receives and answers with a scripted status / body. */
struct Mock
{
    Mock()
    {
        auto handler = [this] (const httplib::Request& req, httplib::Response& res)
        {
            Recorded r { req.method, req.path, req.body, req.headers, {}, {}, {}, {} };

            for (auto& [name, value] : req.files)
            {
                r.fields[name] = value.content;
                r.fileNames[name] = value.filename;
                r.fileContents[name] = value.content;
                r.fileTypes[name] = value.content_type;
            }

            std::lock_guard<std::mutex> lock (mutex);
            requests.push_back (r);
            const auto index = requests.size();
            const auto& step = index <= script.size() ? script[index - 1] : defaultReply;
            res.status = step.first;
            res.set_content (step.second, "application/json");

            if (res.status / 100 == 2)
                res.set_header ("ETag", "\"abc123\"");
        };

        svr.Post (".*", handler);
        svr.Put (".*", handler);
        port = svr.bind_to_any_port ("127.0.0.1");
        thread = std::thread ([this] { svr.listen_after_bind(); });

        for (int i = 0; i < 200 && ! svr.is_running(); ++i)
            std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    ~Mock()
    {
        svr.stop();
        thread.join();
    }

    std::string url() const     { return "http://127.0.0.1:" + std::to_string (port); }

    size_t count()
    {
        std::lock_guard<std::mutex> lock (mutex);
        return requests.size();
    }

    Recorded at (size_t i)
    {
        std::lock_guard<std::mutex> lock (mutex);
        return requests.at (i);
    }

    httplib::Server svr;
    std::thread thread;
    int port = 0;
    std::mutex mutex;
    std::vector<Recorded> requests;
    std::vector<std::pair<int, std::string>> script;                                                                    // replies for request 1, 2, ...
    std::pair<int, std::string> defaultReply { 200, R"({"ok":true,"result":{"message_id":42}})" };
};


inline std::string header (const Recorded& r, const char* name)
{
    auto it = r.headers.find (name);
    return it == r.headers.end() ? std::string() : it->second;
}

inline bool contains (const std::string& haystack, const std::string& needle)
{
    return ! needle.empty() && haystack.find (needle) != std::string::npos;
}

} // namespace rovox::test
