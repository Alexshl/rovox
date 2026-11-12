// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// E7-03: review.request / resolve / wait against a local mock receiver: delivery, the answer, expiry, rollback of the agent's steps,
// tenant ownership and the rule that secrets never show up in results, events or the record files.
#include "Delivery.h"
#include "DeliveryService.h"
#include "JobManager.h"
#include "MockServer.h"
#include "Render.h"
#include "Review.h"
#include "TestFramework.h"
#include "TestSupport.h"

#include <atomic>
#include <fstream>
#include <sstream>
#include <thread>

#include <unistd.h>

using namespace rovox;
using test::Mock;

namespace
{
/** A project with one rendered file, a delivery service with a mock adapter and a Review whose commands run on the message thread. */
struct ReviewFixture
{
    ReviewFixture()
        : jobs { fs::path() },
          service (env.pm, jobs, env.executor, test::makeTempDir ("rcfg") / "none.toml"),
          reviews (env.pm, jobs, service, [this] (const std::string& project, const std::string& command, const json& params) { return env.run (project, command, params); })
    {
        service.setRetryBackoffMs (20);
        id = env.newProject ("review");
        const auto dir = test::makeTempDir ("rsine");
        test::writeWav (dir / "s.wav", { test::sine (440.0, -18.0, 2.0, 44100.0) }, 44100.0, 24);
        auto plan = env.ok (id, "import.plan", { { "paths", { (dir / "s.wav").string() } } });
        env.ok (id, "import.apply", { { "plan_id", plan["plan_id"] } });
        RenderRequest req;
        req.name = "mix";
        req.format.container = "mp3";
        req.format.bitrateKbps = 128;
        req.tailS = 0;
        REQUIRE (renderProject (env.project (id), req).ok());
        service.registry().add (makeWebhookAdapter ("webhook.test", mock.url() + "/hook", SecretRef { "ROVOX_TEST_REVIEW_SECRET", "" }));
    }

    ~ReviewFixture() { jobs.shutdown(); }

    Review::RequestArgs args (const std::string& message = "Drums +3 dB in the chorus")
    {
        Review::RequestArgs a;
        a.projectId = id;
        a.adapter = "webhook.test";
        a.message = message;
        a.source = { { "file", "renders/mix.mp3" } };
        return a;
    }

    json request (const Review::RequestArgs& a)
    {
        auto r = reviews.request (a);
        REQUIRE (r.ok());
        return r.value();
    }

    json waitDelivered (const std::string& reviewId)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);

        for (;;)
        {
            test::pumpMessages (10);
            auto r = reviews.get ("local", reviewId);
            REQUIRE (r.ok());

            if (r.value()["delivery"]["status"] != "sending")
                return r.value();

            REQUIRE (std::chrono::steady_clock::now() < deadline);
        }
    }

    size_t undoSteps() { return env.ok (id, "history.list")["undo"].size(); }

    test::Env env;
    Mock mock;
    JobManager jobs;
    DeliveryService service;
    Review reviews;
    std::string id;
};

std::string readAll (const fs::path& dir)
{
    std::string all;
    std::error_code ec;

    for (fs::recursive_directory_iterator it (dir, ec), end; ! ec && it != end; it.increment (ec))
        if (it->is_regular_file())
        {
            std::ifstream in (it->path());
            std::stringstream ss;
            ss << in.rdbuf();
            all += ss.str();
        }

    return all;
}
} // namespace

ROVOX_TEST (review, request_delivers_the_clip_and_approval_answers_the_wait)
{
    ReviewFixture f;
    auto sub = f.env.hub.subscribe ({ .projectId = f.id });
    const auto rv = f.request (f.args());
    const auto reviewId = rv["review_id"].get<std::string>();
    CHECK (rv["status"] == "pending");
    CHECK (rv["created"] == true);
    CHECK (rv["tenant_id"] == "local");
    CHECK (rv["checkpoint"] == false);

    const auto delivered = f.waitDelivered (reviewId);
    CHECK (delivered["delivery"]["status"] == "sent");
    REQUIRE (f.mock.count() == 1);
    CHECK (f.mock.at (0).fields["caption"] == "Drums +3 dB in the chorus");

    // nobody answered yet: the long poll ends with the review still pending
    auto pending = f.reviews.wait ("local", reviewId, 0.1);
    REQUIRE (pending.ok());
    CHECK (pending.value()["status"] == "pending");

    // a waiter that is already blocked wakes up when the answer arrives
    std::atomic<bool> done { false };
    json answered;
    std::thread waiter ([&]
    {
        auto r = f.reviews.wait ("local", reviewId, 20);
        answered = r.ok() ? r.value() : json (nullptr);
        done = true;
    });

    test::pumpMessages (100);
    auto resolved = f.reviews.resolve ("local", reviewId, "approved", "sounds good", nullptr);
    REQUIRE (resolved.ok());
    CHECK (resolved.value()["status"] == "approved");
    CHECK (resolved.value()["changed"] == true);

    for (int i = 0; i < 500 && ! done; ++i)
        test::pumpMessages (10);

    waiter.join();
    REQUIRE (answered.is_object());
    CHECK (answered["status"] == "approved");
    CHECK (answered["comment"] == "sounds good");

    // the same verdict again is harmless, another one is refused
    auto again = f.reviews.resolve ("local", reviewId, "approved", "", nullptr);
    REQUIRE (again.ok());
    CHECK (again.value()["changed"] == false);
    auto other = f.reviews.resolve ("local", reviewId, "rejected", "", nullptr);
    CHECK (! other.ok());
    CHECK (other.error().code == errc::preconditionFailed);

    // review.requested and review.resolved reach the event hub
    std::vector<std::string> types;
    Event ev;

    while (sub->pop (ev, std::chrono::milliseconds (200)))
        if (ev.body.value ("type", "").rfind ("review.", 0) == 0)
            types.push_back (ev.body["type"].get<std::string>());

    CHECK (std::count (types.begin(), types.end(), "review.requested") == 1);
    CHECK (std::count (types.begin(), types.end(), "review.resolved") == 1);
}

ROVOX_TEST (review, changes_requested_carries_the_comment)
{
    ReviewFixture f;
    const auto reviewId = f.request (f.args())["review_id"].get<std::string>();
    f.waitDelivered (reviewId);
    auto r = f.reviews.resolve ("local", reviewId, "changes_requested", "less kick, more air", nullptr);
    REQUIRE (r.ok());
    CHECK (r.value()["status"] == "changes_requested");
    CHECK (r.value()["comment"] == "less kick, more air");
    CHECK (! r.value().contains ("rollback"));
    CHECK (f.reviews.list ("local", f.id, "changes_requested").value()["reviews"].size() == 1);
    CHECK (f.reviews.list ("local", f.id, "pending").value()["reviews"].empty());

    // an unknown verdict and an unknown id are refused
    auto bad = f.reviews.resolve ("local", reviewId, "maybe", "", nullptr);
    CHECK (! bad.ok());
    CHECK (bad.error().code == errc::invalidParams);
    CHECK (f.reviews.resolve ("local", "rv_00000000000000000000000000", "approved", "", nullptr).error().code == errc::notFound);
}

ROVOX_TEST (review, a_pending_review_expires)
{
    ReviewFixture f;
    auto a = f.args();
    a.ttlS = 1;
    const auto reviewId = f.request (a)["review_id"].get<std::string>();
    f.waitDelivered (reviewId);
    std::this_thread::sleep_for (std::chrono::milliseconds (1300));

    auto waited = f.reviews.wait ("local", reviewId, 0);
    REQUIRE (waited.ok());
    CHECK (waited.value()["status"] == "expired");
    auto late = f.reviews.resolve ("local", reviewId, "approved", "", nullptr);
    CHECK (! late.ok());
    CHECK (late.error().code == errc::preconditionFailed);

    // the sweep finds the ones nobody has read
    auto b = f.args();
    b.ttlS = 1;
    const auto second = f.request (b)["review_id"].get<std::string>();
    std::this_thread::sleep_for (std::chrono::milliseconds (1300));
    CHECK (f.reviews.sweepExpired() == 1);
    CHECK (f.reviews.get ("local", second).value()["status"] == "expired");

    auto invalid = f.args();
    invalid.ttlS = 0;
    CHECK (f.reviews.request (invalid).error().code == errc::invalidParams);
}

ROVOX_TEST (review, rejection_rolls_back_the_agents_steps_to_the_checkpoint)
{
    ReviewFixture f;
    const auto trackId = f.env.ok (f.id, "track.create", { { "name", "Drums" } })["track_id"].get<std::string>();
    f.env.ok (f.id, "agent.accept");
    const auto before = f.undoSteps();

    // the agent changes the volume, asks, the person says "revert"
    REQUIRE (f.env.run (f.id, "track.update", { { "track_id", trackId }, { "volume_db", -3.0 } }).ok);
    CHECK (f.undoSteps() == before + 1);
    auto a = f.args();
    a.checkpoint = true;
    const auto rv = f.request (a);
    CHECK (rv["checkpoint_revision"].is_number());
    const auto reviewId = rv["review_id"].get<std::string>();
    f.waitDelivered (reviewId);

    auto r = f.reviews.resolve ("local", reviewId, "rejected", "worse, revert", nullptr);
    REQUIRE (r.ok());
    CHECK (r.value()["rollback"]["performed"] == true);
    CHECK (r.value()["rollback"]["undone_steps"] == 1);
    CHECK (f.undoSteps() == before);
}

ROVOX_TEST (review, rejection_only_suggests_the_undo_when_a_person_edited_in_between)
{
    ReviewFixture f;
    const auto trackId = f.env.ok (f.id, "track.create", { { "name", "Drums" } })["track_id"].get<std::string>();
    f.env.ok (f.id, "agent.accept");
    const auto before = f.undoSteps();

    REQUIRE (f.env.run (f.id, "track.update", { { "track_id", trackId }, { "volume_db", -3.0 } }).ok);
    auto a = f.args();
    a.checkpoint = true;
    const auto reviewId = f.request (a)["review_id"].get<std::string>();
    f.waitDelivered (reviewId);
    REQUIRE (f.env.runAs ({ Origin::Kind::ui, "ws_1", "alex@browser" }, f.id, "track.update", { { "track_id", trackId }, { "pan", 0.2 } }).ok);

    auto r = f.reviews.resolve ("local", reviewId, "rejected", "", nullptr);
    REQUIRE (r.ok());
    CHECK (r.value()["rollback"]["performed"] == false);
    CHECK (r.value()["rollback"]["suggested_undo_steps"] == 2);
    CHECK (f.undoSteps() == before + 2);
}

ROVOX_TEST (review, approval_accepts_the_agents_changes)
{
    ReviewFixture f;
    const auto trackId = f.env.ok (f.id, "track.create", { { "name", "Drums" } })["track_id"].get<std::string>();
    REQUIRE (f.env.run (f.id, "track.update", { { "track_id", trackId }, { "volume_db", -3.0 } }).ok);
    const auto reviewId = f.request (f.args())["review_id"].get<std::string>();
    f.waitDelivered (reviewId);
    CHECK (f.env.project (f.id).acceptedRevision().load() == 0);

    auto r = f.reviews.resolve ("local", reviewId, "approved", "", nullptr);
    REQUIRE (r.ok());
    CHECK (r.value()["accepted_revision"].is_number());
    CHECK (f.env.project (f.id).acceptedRevision().load() > 0);
}

ROVOX_TEST (review, another_tenant_can_neither_read_nor_answer_nor_request)
{
    ReviewFixture f;
    const auto reviewId = f.request (f.args())["review_id"].get<std::string>();
    f.waitDelivered (reviewId);

    CHECK (f.reviews.get ("other", reviewId).error().code == errc::notFound);
    CHECK (f.reviews.wait ("other", reviewId, 0).error().code == errc::notFound);
    CHECK (f.reviews.resolve ("other", reviewId, "approved", "", nullptr).error().code == errc::notFound);
    CHECK (f.reviews.list ("other", "", "").value()["reviews"].empty());
    CHECK (f.reviews.get ("../local", reviewId).error().code == errc::notFound);

    auto a = f.args();
    a.tenant = "other";
    CHECK (! f.reviews.request (a).ok());

    // the owner is not affected
    CHECK (f.reviews.get ("local", reviewId).value()["status"] == "pending");
    CHECK (f.reviews.list ("local", "", "").value()["reviews"].size() == 1);
}

ROVOX_TEST (review, a_project_on_hold_queues_the_delivery_and_the_review_stays_pending)
{
    ReviewFixture f;
    f.env.ok (f.id, "project.update", { { "delivery_hold", true } });
    const auto rv = f.request (f.args());
    CHECK (rv["status"] == "pending");
    CHECK (rv["delivery"]["status"] == "held");
    CHECK (f.mock.count() == 0);

    auto released = f.service.release ("local", f.id, "", nullptr);
    REQUIRE (released.ok());
    REQUIRE (released.value().size() == 1);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);
    json now;

    do
    {
        test::pumpMessages (10);
        now = f.reviews.get ("local", rv["review_id"]).value()["delivery"];
        REQUIRE (std::chrono::steady_clock::now() < deadline);
    } while (now["status"] != "sent");

    CHECK (f.mock.count() == 1);
}

ROVOX_TEST (review, secrets_never_appear_in_results_events_or_records)
{
    ::setenv ("ROVOX_TEST_REVIEW_SECRET", "s3cr3t-hmac-value", 1);
    ReviewFixture f;
    auto sub = f.env.hub.subscribe ({ .projectId = f.id });
    std::string seen;
    const auto rv = f.request (f.args());
    const auto reviewId = rv["review_id"].get<std::string>();
    seen += rv.dump();
    seen += f.waitDelivered (reviewId).dump();
    REQUIRE (f.mock.count() == 1);
    CHECK (! f.mock.at (0).headers.empty());            // the receiver did get a signed request
    seen += f.reviews.resolve ("local", reviewId, "approved", "ok", nullptr).value().dump();
    seen += f.reviews.list ("local", "", "").value().dump();

    Event ev;

    while (sub->pop (ev, std::chrono::milliseconds (200)))
        seen += ev.body.dump();

    seen += readAll (f.env.pm.host().dataDir() / "reviews");
    CHECK (seen.find ("s3cr3t-hmac-value") == std::string::npos);
    CHECK (seen.find ("abs_path") == std::string::npos);
    CHECK (seen.find ("ROVOX_TEST_REVIEW_SECRET") == std::string::npos);
    ::unsetenv ("ROVOX_TEST_REVIEW_SECRET");
}

ROVOX_TEST (review, a_client_request_id_makes_a_repeat_harmless)
{
    ReviewFixture f;
    auto a = f.args();
    a.clientRequestId = "same-ask";
    const auto first = f.request (a);
    const auto second = f.request (a);
    CHECK (second["review_id"] == first["review_id"]);
    CHECK (second["created"] == false);
    f.waitDelivered (first["review_id"]);
    CHECK (f.mock.count() == 1);
}
