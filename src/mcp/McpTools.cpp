// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// The tool set (task 210). A tool is either a command of the CommandBus (the schema is the command's own schema plus
// project_id / if_revision / undo_label / dry_run) or a job tool (render, preview, analysis, import planning), or a
// read tool over the event buffers. There is no second model of the project here.
#include "Mcp.h"

#include "JsonSchema.h"
#include "McpExtras.h"
#include "Sha1.h"

#include <algorithm>
#include <fstream>

namespace rovox::mcp
{

namespace
{
json str (const std::string& description = {})
{
    json j { { "type", "string" } };

    if (! description.empty())
        j["description"] = description;

    return j;
}

json integer (const std::string& description = {}, int minimum = 0)
{
    json j { { "type", "integer" }, { "minimum", minimum } };

    if (! description.empty())
        j["description"] = description;

    return j;
}

json boolean (const std::string& description = {})
{
    json j { { "type", "boolean" } };

    if (! description.empty())
        j["description"] = description;

    return j;
}

json number (const std::string& description = {})
{
    json j { { "type", "number" } };

    if (! description.empty())
        j["description"] = description;

    return j;
}

json strings()      { return { { "type", "array" }, { "items", { { "type", "string" } } } }; }

json timePos()
{
    return { { "type", "object" }, { "description", "Time: {\"s\": 12.5} | {\"bar\": 18, \"beat\": 1, \"tick\": 0} (1-based, 960 ticks per quarter) | {\"qn\": 68.0} | {\"marker\": \"mrk_...\"}" } };
}

json schema (json properties, std::vector<std::string> required = {}, bool additional = false)
{
    return { { "type", "object" }, { "properties", std::move (properties) }, { "required", json (required) }, { "additionalProperties", additional } };
}

json projectIdProp()    { return str ("Project id (p_...). Every project tool needs it; there is no active project."); }

json rangeSchema()
{
    return { { "type", "object" }, { "description", "{start, end} or {start, length}: start/end are times (see track tools), length is {s}|{bars}|{qn}. Default: the whole project (preview: the first 30 s)." },
             { "properties", { { "start", timePos() }, { "end", timePos() }, { "length", { { "type", "object" } } } } }, { "additionalProperties", false } };
}

std::string canonicalName (const std::string& n)    { return n; }

ToolOutput fromCommand (const CommandResult& r, const std::string& projectId, bool projectScoped)
{
    if (! r.ok)
        return ToolOutput::failure (r.error.value_or (makeError (errc::internal, "command failed")), projectScoped ? std::optional<std::uint64_t> (r.revision) : std::nullopt, projectId);

    return ToolOutput::success (r.data, projectScoped ? std::optional<std::uint64_t> (r.revision) : std::nullopt, projectId);
}

/** Splits the envelope arguments (project_id, if_revision, undo_label, dry_run) from the command parameters. */
CommandContext splitEnvelope (McpServer& server, Session& session, json& params, bool keepProjectId)
{
    const std::string projectId = params.value ("project_id", "");
    auto ctx = server.contextFor (session, projectId);

    if (params.contains ("if_revision") && params["if_revision"].is_number_integer() && params["if_revision"].get<std::int64_t>() >= 0)
        ctx.ifRevision = params["if_revision"].get<std::uint64_t>();

    ctx.undoLabel = params.value ("undo_label", "");
    ctx.dryRun = params.value ("dry_run", false);
    ctx.coalesce = params.value ("coalesce", false);        // internal: recipe steps continue one undo step; not part of any tool schema
    params.erase ("coalesce");
    params.erase ("if_revision");
    params.erase ("undo_label");
    params.erase ("dry_run");

    if (! keepProjectId)
        params.erase ("project_id");

    return ctx;
}

struct CommandToolSpec
{
    const char* command;
    const char* title;
    const char* description;
    bool readOnly;
};

const CommandToolSpec commandTools[] = {
    { "project.list", "List projects", "Lists the open projects of this server with id, name, revision, dirty flag and track count.", true },
    { "project.new", "New project", "Creates and opens an empty project. Returns project_id. ephemeral + ttl_s make it self-deleting.", false },
    { "project.open", "Open project", "Opens a project by .rvx directory, project id or name. Returns project_id.", false },
    { "project.close", "Close project", "Closes a project. save=true (default) saves it first.", false },
    { "project.delete", "Delete project", "Closes and deletes a project from disk. Refused while a job runs on it.", false },
    { "project.set_ttl", "Set project TTL", "Makes a project ephemeral: it is deleted ttl_s seconds after creation.", false },
    { "project.get", "Get project", "One-shot snapshot: tracks (with volume_db, pan), master, items, markers, tempo, fx chains. include narrows it down.", true },
    { "project.update", "Update project", "Renames the project (undoable).", false },
    { "project.save", "Save project", "Writes the project to disk now (autosave runs every 60 s anyway).", false },
    { "project.activity", "Project activity", "The last commands run on the project with origin, params, duration and the revision they produced.", true },
    { "undo", "Undo", "Undoes the last step of the project, whoever made it (undo is linear). Returns the label of the undone step.", false },
    { "redo", "Redo", "Redoes the last undone step.", false },
    { "history.list", "History", "The undo and redo stacks with revision, label and origin of each step.", true },
    { "track.list", "List tracks", "Lists tracks with track_id, name, volume_db, pan, mute, solo, items and plugins, plus the master track.", true },
    { "track.create", "Create track", "Creates an audio track at the end. Returns track_id.", false },
    { "track.update", "Update tracks", "Sets name, volume_db, pan (-1..1), mute, solo, color of one track (track_id=\"master\" for the master fader) or of many at once with updates[]; one undo step.", false },
    { "track.delete", "Delete track", "Deletes a track and its items.", false },
    { "track.move", "Move track", "Moves a track to to_index among the audio tracks.", false },
    { "item.list", "List items", "Lists items (clips) with item_id, track_id, start_s, length_s, bar.beat position. Filter by track_id or a time window.", true },
    { "item.update", "Update item", "Changes name, mute, start (start_s or a time), length (length_s or a duration), trim_start_s / trim_end_s (audio stays in place), fade_in_s, fade_out_s and gain_db of an item.", false },
    { "item.split", "Split item", "Splits an item at a time. Returns left_id (the original id) and right_id; both stay valid.", false },
    { "item.delete", "Delete item", "Deletes an item.", false },
    { "transport.get", "Get transport", "Playback state of a project: state (playing|stopped), position_s, loop {enabled,start_s,end_s}, epoch, audio mode. The audible output needs the server to run with --audio browser.", true },
    { "transport.set", "Set transport", "Starts or stops playback (state), seeks (position_s or a time) and sets the loop. Playback is heard in the web UI; it is not an undo step and does not change the revision.", false },
    { "mixer.get_meters", "Get meters", "Peak level in dBFS per track and master since the previous call. Meters only move while the project plays (transport.set state=playing).", true },
    { "agent.accept", "Accept agent changes", "Marks the project's changes up to a revision (default: now) as seen by a person; the web UI stops highlighting them. Not an undo step.", false },
    { "marker.list", "List markers", "Lists markers with marker_id, name, time_s and bar.beat position.", true },
    { "marker.upsert", "Upsert markers", "Creates markers (no marker_id) or moves/renames existing ones (marker_id) in one undo step.", false },
    { "marker.delete", "Delete marker", "Deletes a marker.", false },
    { "tempo.get", "Get tempo", "The tempo points and the time signature.", true },
    { "tempo.set", "Set tempo", "Replaces the tempo points; the first is the tempo at the start. Position later points with bar or qn.", false },
    { "time.convert", "Convert time", "Converts a time ({s} | {bar,beat,tick} | {qn} | {marker}) to seconds, quarter notes and bar.beat.tick under the project's tempo map.", true },
    { "fx.list", "List plugins", "The plugin chain of a track (or target=\"master\"), in order. The volume fader shows up as is_fader.", true },
    { "fx.add", "Add plugin", "Adds a built-in effect (builtin: equaliser, compressor, reverb, delay, chorus, phaser, lowpass) or an external plugin (plugin: {plugin_uid} from plugin.catalog) to a track or the master, before the fader. Instruments go on the track whose MIDI items they should play. The plugin is tested in a separate process first.", false },
    { "fx.remove", "Remove plugin", "Removes a plugin by plugin_id.", false },
    { "fx.params", "Plugin parameters", "Lists the parameters of a plugin: ident, name, value in the parameter's own unit, display text, min, max.", true },
    { "fx.update", "Update plugin", "Sets enabled and/or params {ident_or_name: number | \"text such as -6 dB\"} of a plugin in one undo step.", false },
    { "project.catalog", "Project catalog", "Every project of this server, open or only on disk: id, name, tracks, modified time, open flag. project.open takes the project_id.", true },
    { "plugin.catalog", "Plugin catalog", "Installed plugins the agent can load: plugin_uid, name, vendor, kind (instrument|effect), io, status (ok|blacklisted), tags and, with include_presets, factory presets. Filter by kind/query/tag/format. Use the plugin_uid with fx.add.", true },
    { "plugin.rescan", "Rescan plugins", "Scans the plugin directories again (in a child process). Unchanged directories return the cached catalog at once; force=true rescans anyway.", false },
    { "plugin.probe", "Probe plugin", "Loads one plugin in a separate process, pushes audio through it and, for an instrument, plays notes: reports latency, parameter count and whether it makes sound. A plugin that crashes is blacklisted; the server is not affected.", false },
    { "plugin.tag", "Tag plugin", "Sets free-form tags (genre, role) on a catalog entry so plugin.catalog can find it by tag.", false },
    { "plugin.unblacklist", "Unblacklist plugin", "Removes a plugin from the blacklist so that it can be probed and loaded again.", false },
    { "fx.presets", "Plugin presets", "Factory and user presets of a loaded external plugin: preset_id, name, source.", true },
    { "fx.preset_load", "Load plugin preset", "Loads a preset into a plugin on the track (undoable).", false },
    { "fx.preset_save", "Save plugin preset", "Stores the plugin's current state as a user preset (name, tags) under the data directory.", false },
    { "fx.move", "Move plugin", "Moves a plugin to to_index inside its chain (track or master).", false },
    { "item.create_midi", "Create MIDI item", "Creates a MIDI item on a track at a time, optionally with notes {pitch (60 or \"C4\"), start_qn, length_qn, velocity}; note times are quarter notes from the item start. The track's instrument plugin plays it.", false },
    { "item.duplicate", "Duplicate item", "Copies an item (audio or MIDI) after the original, or at a time / on another track; count repeats it.", false },
    { "midi.get", "Get MIDI notes", "The notes of a MIDI item with note_id, pitch, start_qn, length_qn, velocity.", true },
    { "midi.write", "Write MIDI notes", "Replaces (mode=replace, default) or appends (mode=append) the notes of a MIDI item; the item grows to fit unless extend=false.", false },
    { "midi.edit", "Edit MIDI notes", "Adds, updates and deletes notes of a MIDI item by note_id in one undo step.", false },
    { "midi.transpose", "Transpose MIDI notes", "Shifts the pitches of an item (or the given note_ids) by semitones.", false },
    { "midi.quantize", "Quantize MIDI notes", "Snaps note starts (and optionally lengths) to a grid in quarter notes; strength 0..1 moves partway.", false },
    { "midi.import_file", "Import MIDI file", "Reads a standard MIDI file on the server into a new MIDI item on a track.", false },
    { "routing.list", "List sends", "Aux sends (send_id, from track, to_track_id, gain_db, pre_fader) and the return tracks of the project.", true },
    { "routing.send_create", "Create send", "Sends a track to a bus track (reverb, parallel compression). The bus track gets a return automatically; put the effect on that track with fx.add.", false },
    { "routing.send_update", "Update send", "Changes gain_db, mute, pre_fader or the destination of a send.", false },
    { "routing.send_delete", "Delete send", "Removes a send (and the return when nothing else feeds it).", false },
    { "track.duplicate", "Duplicate track", "Copies a track with its plugins and items (sends are not copied).", false },
    { "template.list", "List templates", "Project templates (built-in and saved): id, name, description, tags.", true },
    { "template.get", "Get template", "The full template document: tracks, instruments, effects, sends, master chain.", true },
    { "template.save", "Save template", "Saves the structure of a project (tracks, instruments, effects, sends, master chain, tempo) as a template under the data directory.", false },
    { "template.apply", "Apply template", "Creates the template's tracks, instruments, effects, sends and master chain in a project as ONE undo step. Fails with PRECONDITION_FAILED listing the missing plugins when it needs instruments that are not installed. bindings {role: {notes | midi_file}} puts MIDI on tracks.", false },
    { "template.delete", "Delete template", "Deletes a saved template.", false },
    { "import.rpp", "Import REAPER project", "Imports a REAPER .RPP file into the project: tracks, items, markers, tempo. Unsupported FX are reported, not translated.", false },
    { "import.apply", "Apply import plan", "Creates the tracks and items of a plan made by import.plan. rename maps source keys (REC01) or planned names to track names.", false },
};
} // namespace

//==============================================================================
void McpServer::buildTools()
{
    auto add = [this] (ToolDef t)
    {
        auto name = t.name;
        std::string underscore = name;
        std::replace (underscore.begin(), underscore.end(), '.', '_');

        if (underscore != name)
            toolAliases_[underscore] = name;

        tools_[name] = std::move (t);
    };

    // ---- command-backed tools ------------------------------------------------------------------------------------
    for (auto& spec : commandTools)
    {
        auto* cmd = bus_.registry().find (spec.command);

        if (cmd == nullptr)
            continue;

        ToolDef t;
        t.name = spec.command;
        t.title = spec.title;
        t.description = spec.description;
        t.inputSchema = cmd->paramsSchema();
        auto& props = t.inputSchema["properties"];
        auto required = t.inputSchema.value ("required", json::array());
        const bool global = cmd->isGlobal();
        const bool hasProjectId = props.contains ("project_id");

        if (! global && ! hasProjectId)
        {
            props["project_id"] = projectIdProp();
            required.push_back ("project_id");
        }

        if (cmd->mutates() && ! global)
        {
            props["if_revision"] = integer ("Optimistic concurrency: fails with REVISION_CONFLICT if the project is at another revision.");
            props["undo_label"] = str ("Name of the undo step.");
            props["dry_run"] = boolean ("Validate only; change nothing.");
        }

        if (std::string (spec.command) == "track.update")
        {
            props["updates"] = { { "type", "array" }, { "minItems", 1 }, { "maxItems", 200 }, { "description", "Many tracks in one undo step: [{track_id, name?, volume_db?, pan?, mute?, solo?, color?}]. Use instead of the single-track fields." },
                                 { "items", cmd->paramsSchema() } };
            required = json::array ({ "project_id" });
        }

        t.inputSchema["required"] = required;
        t.annotations = { { "readOnlyHint", spec.readOnly }, { "destructiveHint", ! spec.readOnly && (std::string (spec.command).find ("delete") != std::string::npos || std::string (spec.command) == "undo") },
                          { "idempotentHint", spec.readOnly }, { "openWorldHint", false } };

        const std::string commandName = spec.command;
        const bool keepProjectId = global && hasProjectId;
        t.handler = [commandName, global, keepProjectId] (ToolCall& c) -> ToolOutput
        {
            json params = c.args;
            auto ctx = splitEnvelope (c.server, c.session, params, keepProjectId);

            if (commandName == "track.update" && params.contains ("updates"))
            {
                std::vector<Op> ops;

                for (auto& u : params["updates"])
                    ops.push_back ({ "track.update", u });

                auto r = c.server.runBatch (std::move (ops), ctx, c.rc.cancelled.get());

                if (r.ok)
                {
                    json updated = json::array();

                    for (auto& u : params["updates"])
                        updated.push_back (u["track_id"]);

                    r.data = { { "updated", updated } };
                }

                return fromCommand (r, ctx.projectId, true);
            }

            auto r = c.server.runCommand (commandName, params, ctx, c.rc.cancelled.get());

            if (r.ok && (commandName == "project.new" || commandName == "project.open" || commandName == "project.close" || commandName == "project.delete"))
                c.server.announceProjectsChanged();

            return fromCommand (r, ctx.projectId, ! global);
        };

        add (std::move (t));
    }

    // ---- batch ---------------------------------------------------------------------------------------------------
    {
        ToolDef t;
        t.name = "batch";
        t.title = "Batch";
        t.description = "Runs several project tools in one atomic call: one undo step, one revision, all-or-nothing. ops: [{tool, arguments}] where tool is a project tool "
                        "(track.update, item.split, fx.add, marker.upsert ...); project_id is taken from the batch. undo and redo cannot be batched.";
        t.inputSchema = schema ({ { "project_id", projectIdProp() },
                                  { "ops", { { "type", "array" }, { "minItems", 1 }, { "maxItems", 500 },
                                             { "items", schema ({ { "tool", str() }, { "arguments", { { "type", "object" } } } }, { "tool" }) } } },
                                  { "if_revision", integer() }, { "undo_label", str() }, { "dry_run", boolean() } }, { "project_id", "ops" });
        t.annotations = { { "readOnlyHint", false }, { "destructiveHint", true } };
        t.handler = [] (ToolCall& c) -> ToolOutput
        {
            json params = c.args;
            auto ctx = splitEnvelope (c.server, c.session, params, false);
            std::vector<Op> ops;

            for (auto& op : params["ops"])
            {
                auto* tool = c.server.findTool (op["tool"].get<std::string>());
                auto* cmd = tool == nullptr ? nullptr : c.server.bus().registry().find (tool->name);

                if (cmd == nullptr || cmd->isGlobal() || cmd->manualUndo())
                    return ToolOutput::failure (makeError (errc::invalidParams, op["tool"].get<std::string>() + " cannot be part of a batch (project command tools only)",
                                                          json { { "path", "/ops/" + std::to_string (ops.size()) + "/tool" } }), std::nullopt, ctx.projectId);

                json args = op.value ("arguments", json::object());
                args.erase ("project_id");

                if (tool->name == "track.update" && args.contains ("updates"))
                {
                    for (auto& u : args["updates"])
                        ops.push_back ({ "track.update", u });

                    continue;
                }

                ops.push_back ({ tool->name, args });
            }

            return fromCommand (c.server.runBatch (std::move (ops), ctx, c.rc.cancelled.get()), ctx.projectId, true);
        };

        add (std::move (t));
    }

    // ---- jobs ----------------------------------------------------------------------------------------------------
    auto waitForJob = [] (ToolCall& c, const std::string& jobId, bool created, double waitS, const std::string& projectId, bool failWhenFailed) -> ToolOutput
    {
        auto& jobs = c.server.jobs();
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds ((int) (waitS * 1000));
        std::optional<JobInfo> info;
        double lastProgress = -1;

        for (;;)
        {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds> (deadline - std::chrono::steady_clock::now());
            info = jobs.get (jobId, std::max (std::chrono::milliseconds (0), std::min (left, std::chrono::milliseconds (250))), c.rc.cancelled.get());

            if (! info)
                return ToolOutput::failure (makeError (errc::notFound, "no job " + jobId), std::nullopt, projectId);

            if (! c.rc.progressToken.is_null() && info->progress > lastProgress && c.rc.emit)
            {
                lastProgress = info->progress;
                c.rc.emit ({ { "jsonrpc", "2.0" }, { "method", "notifications/progress" },
                             { "params", { { "progressToken", c.rc.progressToken }, { "progress", (int) std::lround (info->progress * 100.0) }, { "total", 100 }, { "message", info->stage } } } });
            }

            if (isFinished (info->state) || left.count() <= 0 || c.rc.cancelled->load() || c.server.stoppingFlag().load())
                break;
        }

        auto data = info->toJson();
        data["created"] = created;
        const auto rev = c.server.projects().revisionOf (info->projectId);

        if (failWhenFailed && info->state == JobState::failed && info->error)
        {
            auto e = *info->error;
            json details = e.details.is_object() ? e.details : json::object();
            details["job_id"] = jobId;
            e.details = details;
            return ToolOutput::failure (e, rev, projectId);
        }

        if (failWhenFailed && info->state == JobState::cancelled)
            return ToolOutput::failure (makeError (errc::cancelled, "the job was cancelled", json { { "job_id", jobId } }), rev, projectId);

        auto out = ToolOutput::success (data, rev, projectId);
        return out;
    };

    auto jobWait = [] (const json& args, double defaultWait, double maxWait) { return std::clamp (args.value ("wait_s", defaultWait), 0.0, maxWait); };

    auto jobContext = [] (ToolCall& c, const std::string& projectId)
    {
        return JobContextInfo { c.session.tenant, projectId, c.session.origin() };
    };

    auto renderSchema = [] (bool preflight)
    {
        json p {
            { "project_id", projectIdProp() },
            { "name", str ("File name without extension, default \"mix\". Written to the project's renders/ directory, overwritten atomically.") },
            { "scope", { { "type", "string" }, { "enum", { "master", "tracks", "stems" } }, { "description", "master = the mix; tracks = the listed tracks summed; stems = one file per listed track" } } },
            { "track_ids", strings() }, { "stems_through_master", boolean() },
            { "range", rangeSchema() },
            { "format", schema ({ { "container", { { "type", "string" }, { "enum", { "wav", "flac", "mp3" } } } }, { "bit_depth", { { "type", "integer" }, { "enum", { 16, 24, 32 } } } },
                                  { "bitrate_kbps", integer ("mp3 only", 32) }, { "sample_rate", number ("Hz; default the project's") }, { "channels", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 2 } } } }) },
            { "tail_s", number ("Reverb/delay tail after the range end, cut at the first silent block. Default 2.") },
            { "target_lufs", number ("Loudness target in LUFS. With normalize (default true) a gain is applied, never past true_peak_ceiling_dbtp; the result reports before/after.") },
            { "normalize", boolean ("Apply gain to reach target_lufs (default true). false + check=true only verifies.") },
            { "tolerance_lu", number() }, { "true_peak_ceiling_dbtp", number() },
            { "limiter", boolean ("Master true-peak brickwall limiter (lookahead, 4x oversampled) on the rendered mix. Default true when target_lufs is set, otherwise false. With normalize, gain and limiter iterate to reach target_lufs; the report gives the max gain reduction and a heavy-limiting warning above 6 dB.") },
            { "ceiling_dbtp", number ("True-peak ceiling of the limiter in dBTP (default -1.0); same as true_peak_ceiling_dbtp.") },
            { "check", boolean ("true: preflight and post-render findings (offline media, unloaded plugin, silent instrument or output, clipping) FAIL the render and no file is written. false (default): they are reported as warnings.") },
            { "fail_on", strings() }, { "warn_on", strings() }, { "seed", str() }, { "stall_timeout_s", number() },
            { "client_request_id", str ("Idempotency: the same value returns the same job.") },
            { "wait_s", { { "type", "number" }, { "minimum", 0 }, { "maximum", 60 }, { "description", "Wait up to this long for the job and return its result in the same call." } } }
        };

        (void) preflight;
        return schema (p, { "project_id" });
    };

    {
        ToolDef t;
        t.name = "render.preflight";
        t.title = "Render preflight";
        t.description = "Runs the render checks without writing a file: offline media, plugins that failed to load, instruments that produce no sound, empty tracks. Returns a job; wait_s waits for it.";
        t.inputSchema = renderSchema (true);
        t.annotations = { { "readOnlyHint", true } };
        t.mayBlock = true;
        t.defaultWaitS = 30;
        t.handler = [waitForJob, jobWait, jobContext] (ToolCall& c) -> ToolOutput
        {
            auto started = c.server.jobService().startRender (jobContext (c, c.args["project_id"]), c.args, true);

            if (! started.ok())
                return ToolOutput::failure (started.error(), std::nullopt, c.args["project_id"]);

            return waitForJob (c, started.value().jobId, started.value().created, jobWait (c.args, 30, 60), c.args["project_id"], false);
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "render.start";
        t.title = "Render";
        t.description = "Starts an offline render as a job (progress, cancel). With check=true it fails instead of warning; target_lufs normalizes the loudness (reported before/after). "
                        "Pass wait_s (up to 60) to get the finished result in this call; otherwise poll job.get with wait_s. The file lands in the project's renders/ directory.";
        t.inputSchema = renderSchema (false);
        t.annotations = { { "readOnlyHint", false }, { "destructiveHint", false } };
        t.mayBlock = true;
        t.handler = [waitForJob, jobWait, jobContext] (ToolCall& c) -> ToolOutput
        {
            auto started = c.server.jobService().startRender (jobContext (c, c.args["project_id"]), c.args, false);

            if (! started.ok())
                return ToolOutput::failure (started.error(), std::nullopt, c.args["project_id"]);

            return waitForJob (c, started.value().jobId, started.value().created, jobWait (c.args, 0, 60), c.args["project_id"], true);
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "render.preview";
        t.title = "Render preview";
        t.description = "Short clip of a range plus numeric analysis in one call: loudness (LUFS, true peak, RMS, crest), spectrum bands, per-bar loudness and optionally per-track presence. "
                        "At most 300 s. Clips live in a ring cache (last 10, 24 h) and do not touch renders/. wait_s (default 30) waits for the result.";
        t.inputSchema = schema ({ { "project_id", projectIdProp() }, { "range", rangeSchema() }, { "track_ids", strings() },
                                  { "format", { { "type", "string" }, { "enum", { "mp3", "wav", "flac" } }, { "description", "default mp3 (128 kbps)" } } },
                                  { "analysis", { { "type", "array" }, { "items", { { "type", "string" }, { "enum", { "loudness", "spectrum_bands", "per_bar", "tracks_presence" } } } },
                                                  { "description", "default [loudness, spectrum_bands, per_bar]; tracks_presence renders every track as a stem (slower)" } } },
                                  { "embed_audio", boolean ("Also return the clip as MCP audio content (at most 2.5 MB).") },
                                  { "client_request_id", str() }, { "wait_s", { { "type", "number" }, { "minimum", 0 }, { "maximum", 60 } } } }, { "project_id" });
        t.annotations = { { "readOnlyHint", true } };
        t.mayBlock = true;
        t.defaultWaitS = 30;
        t.handler = [waitForJob, jobWait, jobContext] (ToolCall& c) -> ToolOutput
        {
            const std::string projectId = c.args["project_id"];
            auto started = c.server.jobService().startPreview (jobContext (c, projectId), c.args);

            if (! started.ok())
                return ToolOutput::failure (started.error(), std::nullopt, projectId);

            auto out = waitForJob (c, started.value().jobId, started.value().created, jobWait (c.args, 30, 60), projectId, true);

            if (out.ok && c.args.value ("embed_audio", false) && out.data.value ("state", "") == "succeeded" && out.data["result"].contains ("clip"))
            {
                const auto path = out.data["result"]["clip"].value ("abs_path", "");
                std::error_code ec;

                if (! path.empty() && fs::file_size (path, ec) <= 2500u * 1000u)
                {
                    std::ifstream in (path, std::ios::binary);
                    std::string bytes ((std::istreambuf_iterator<char> (in)), std::istreambuf_iterator<char>());
                    const auto container = out.data["result"]["clip"].value ("container", "mp3");
                    out.extraContent.push_back ({ { "type", "audio" }, { "data", base64Encode (reinterpret_cast<const std::uint8_t*> (bytes.data()), bytes.size()) },
                                                  { "mimeType", container == "mp3" ? "audio/mpeg" : (container == "flac" ? "audio/flac" : "audio/wav") } });
                }
            }

            return out;
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "analyze.audio";
        t.title = "Analyze audio";
        t.description = "Numeric analysis of the master or one track over a range without keeping a clip: loudness, spectrum bands, per-bar loudness (master also tracks_presence). Runs as a job; wait_s (default 30) waits.";
        t.inputSchema = schema ({ { "project_id", projectIdProp() }, { "target", str ("\"master\" (default) or a track_id") }, { "range", rangeSchema() },
                                  { "kinds", { { "type", "array" }, { "items", { { "type", "string" }, { "enum", { "loudness", "spectrum_bands", "per_bar", "tracks_presence" } } } } } },
                                  { "client_request_id", str() }, { "wait_s", { { "type", "number" }, { "minimum", 0 }, { "maximum", 60 } } } }, { "project_id" });
        t.annotations = { { "readOnlyHint", true } };
        t.mayBlock = true;
        t.defaultWaitS = 30;
        t.handler = [waitForJob, jobWait, jobContext] (ToolCall& c) -> ToolOutput
        {
            const std::string projectId = c.args["project_id"];
            auto started = c.server.jobService().startAnalysis (jobContext (c, projectId), c.args);

            if (! started.ok())
                return ToolOutput::failure (started.error(), std::nullopt, projectId);

            return waitForJob (c, started.value().jobId, started.value().created, jobWait (c.args, 30, 60), projectId, true);
        };
        add (std::move (t));
    }

    if (auto* cmd = bus_.registry().find ("import.plan"))
    {
        ToolDef t;
        t.name = "import.plan";
        t.title = "Plan import";
        t.description = "Probes, hashes and stages audio files, a folder or a Zoom R20 .zprj folder (paths on the server) and returns a plan with named tracks, clips and warnings. "
                        "Runs as a job (large projects take seconds); wait_s (default 30) waits. Then call import.apply with the plan_id (in result.plan_id).";
        t.inputSchema = cmd->paramsSchema();
        t.inputSchema["properties"]["project_id"] = projectIdProp();
        t.inputSchema["properties"]["wait_s"] = { { "type", "number" }, { "minimum", 0 }, { "maximum", 60 } };
        t.inputSchema["properties"]["client_request_id"] = str();
        t.inputSchema["required"] = json::array ({ "project_id", "paths" });
        t.annotations = { { "readOnlyHint", false } };
        t.mayBlock = true;
        t.defaultWaitS = 30;
        t.handler = [waitForJob, jobWait, jobContext] (ToolCall& c) -> ToolOutput
        {
            const std::string projectId = c.args["project_id"];
            json params = c.args;
            params.erase ("project_id");
            params.erase ("wait_s");
            auto started = c.server.jobService().startImportPlan (jobContext (c, projectId), params);

            if (! started.ok())
                return ToolOutput::failure (started.error(), std::nullopt, projectId);

            return waitForJob (c, started.value().jobId, started.value().created, jobWait (c.args, 30, 60), projectId, true);
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "job.get";
        t.title = "Get job";
        t.description = "State, progress, stage and (when finished) result or error of a job. wait_s (0..30) long-polls: returns as soon as the job is finished or the time is up.";
        t.inputSchema = schema ({ { "job_id", str() }, { "project_id", projectIdProp() }, { "wait_s", { { "type", "number" }, { "minimum", 0 }, { "maximum", 30 } } } }, { "job_id" });
        t.annotations = { { "readOnlyHint", true } };
        t.mayBlock = true;
        t.handler = [waitForJob, jobWait] (ToolCall& c) -> ToolOutput
        {
            auto info = c.server.jobs().get (c.args["job_id"]);

            if (! info || info->tenant != c.session.tenant || (c.args.contains ("project_id") && info->projectId != c.args["project_id"]))
                return ToolOutput::failure (makeError (errc::notFound, "no job " + c.args["job_id"].get<std::string>()));

            return waitForJob (c, info->id, false, jobWait (c.args, 0, 30), info->projectId, false);
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "job.cancel";
        t.title = "Cancel job";
        t.description = "Cancels a queued or running job. A running render stops within about a second, its temporary files are removed and no target file is written or changed.";
        t.inputSchema = schema ({ { "job_id", str() }, { "project_id", projectIdProp() } }, { "job_id" });
        t.annotations = { { "readOnlyHint", false }, { "destructiveHint", false }, { "idempotentHint", true } };
        t.handler = [] (ToolCall& c) -> ToolOutput
        {
            const std::string id = c.args["job_id"];
            auto info = c.server.jobs().get (id);

            if (! info || info->tenant != c.session.tenant || (c.args.contains ("project_id") && info->projectId != c.args["project_id"]))
                return ToolOutput::failure (makeError (errc::notFound, "no job " + id));

            const bool cancelling = c.server.jobs().cancel (id);
            info = c.server.jobs().get (id, std::chrono::milliseconds (cancelling ? 1500 : 0));
            auto data = info->toJson();
            data["cancel_requested"] = cancelling;
            return ToolOutput::success (data, std::nullopt, info->projectId);
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "job.list";
        t.title = "List jobs";
        t.description = "Jobs of this server, newest first. Filter by project_id, state (queued|running|succeeded|failed|cancelled) or kind (render|preview|analysis|import_plan|preflight).";
        t.inputSchema = schema ({ { "project_id", projectIdProp() }, { "state", { { "type", "string" }, { "enum", { "queued", "running", "succeeded", "failed", "cancelled" } } } },
                                  { "kind", str() }, { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 200 } } } });
        t.annotations = { { "readOnlyHint", true } };
        t.handler = [] (ToolCall& c) -> ToolOutput
        {
            JobManager::Filter f;
            f.tenant = c.session.tenant;
            f.project = c.args.value ("project_id", "");
            f.kind = c.args.value ("kind", "");

            if (c.args.contains ("state"))
                for (auto st : { JobState::queued, JobState::running, JobState::succeeded, JobState::failed, JobState::cancelled })
                    if (c.args["state"] == toString (st))
                        f.state = st;

            json list = json::array();
            const size_t limit = (size_t) c.args.value ("limit", 50);

            for (auto& j : c.server.jobs().list (f))
            {
                if (list.size() >= limit)
                    break;

                auto summary = j.toJson();
                summary.erase ("result");
                list.push_back (summary);
            }

            return ToolOutput::success ({ { "jobs", list } });
        };
        add (std::move (t));
    }

    // ---- change tracking -----------------------------------------------------------------------------------------
    {
        ToolDef t;
        t.name = "project.diff";
        t.title = "Project diff";
        t.description = "What changed since a revision: the commands (with origin and undo label) and the entities they touched. RESYNC_REQUIRED means the buffer is too old: call project.get. "
                        "Every project tool response already carries external_changes for edits made by others since your last call.";
        t.inputSchema = schema ({ { "project_id", projectIdProp() }, { "since_revision", integer() } }, { "project_id", "since_revision" });
        t.annotations = { { "readOnlyHint", true } };
        t.handler = [] (ToolCall& c) -> ToolOutput
        {
            const std::string projectId = c.args["project_id"];
            auto d = c.server.diffSince (projectId, c.args["since_revision"].get<std::uint64_t>());

            if (! d.ok())
                return ToolOutput::failure (d.error(), c.server.projects().revisionOf (projectId), projectId);

            return ToolOutput::success (d.value(), c.server.projects().revisionOf (projectId), projectId);
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "events.poll";
        t.title = "Poll events";
        t.description = "Events (project.changed with origin, revision and entities; job.updated; upload events) with event_id greater than since_event_id. wait_s (0..30) long-polls. "
                        "Start with since_event_id 0 and pass next_event_id back. resync_required=true means events were dropped: re-read with project.get.";
        t.inputSchema = schema ({ { "project_id", projectIdProp() }, { "since_event_id", integer() }, { "limit", { { "type", "integer" }, { "minimum", 1 }, { "maximum", 500 } } },
                                  { "wait_s", { { "type", "number" }, { "minimum", 0 }, { "maximum", 30 } } } });
        t.annotations = { { "readOnlyHint", true } };
        t.mayBlock = true;
        t.handler = [] (ToolCall& c) -> ToolOutput
        {
            const auto wait = std::chrono::milliseconds ((int) (std::clamp (c.args.value ("wait_s", 0.0), 0.0, 30.0) * 1000));
            return ToolOutput::success (c.server.pollEvents (c.session.tenant, c.args.value ("project_id", ""), c.args.value ("since_event_id", (std::uint64_t) 0),
                                                              (size_t) c.args.value ("limit", 100), wait, c.rc.cancelled.get()));
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "events.subscribe";
        t.title = "Subscribe to events";
        t.description = "Asks the server to push notifications/rovox/project_changed (others' edits) and notifications/resources/updated for rovox://project/{id}/summary to this session. "
                        "project_id \"*\" = all projects. Clients that do not show notifications to the model should use events.poll or external_changes instead.";
        t.inputSchema = schema ({ { "project_id", str ("A project id or \"*\"") }, { "subscribe", boolean() } }, { "project_id" });
        t.annotations = { { "readOnlyHint", false }, { "idempotentHint", true } };
        t.handler = [] (ToolCall& c) -> ToolOutput
        {
            const std::string project = c.args["project_id"];
            const bool on = c.args.value ("subscribe", true);
            std::lock_guard<std::mutex> lock (c.session.stateMutex);
            const auto uri = "rovox://project/" + project + "/summary";

            if (on)
            {
                c.session.eventProjects.insert (project);

                if (project != "*")
                    c.session.subscribedUris.insert (uri);
            }
            else
            {
                c.session.eventProjects.erase (project);
                c.session.subscribedUris.erase (uri);
            }

            return ToolOutput::success ({ { "subscribed", on }, { "project_id", project } });
        };
        add (std::move (t));
    }

    if (config_.uploadBatchStatus)
    {
        ToolDef t;
        t.name = "upload.status";
        t.title = "Upload status";
        t.description = "Progress of an upload batch started over the HTTP upload API: per file offset, size and whether it is complete.";
        t.inputSchema = schema ({ { "batch_id", str() } }, { "batch_id" });
        t.annotations = { { "readOnlyHint", true } };
        t.handler = [fn = config_.uploadBatchStatus] (ToolCall& c) -> ToolOutput
        {
            auto r = fn (c.args["batch_id"]);

            if (! r.ok())
                return ToolOutput::failure (r.error());

            return ToolOutput::success (r.value());
        };
        add (std::move (t));
    }

    // ---- delivery (task 510) -------------------------------------------------------------------------------------
    {
        ToolDef t;
        t.name = "delivery.list_adapters";
        t.title = "List delivery adapters";
        t.description = "Where renders can be sent: id (such as telegram.me, s3.backup, webhook.n8n, file.exports), kind and a short description. Never shows tokens, chat ids or bucket credentials.";
        t.inputSchema = schema (json::object());
        t.annotations = { { "readOnlyHint", true } };
        t.handler = [] (ToolCall& c) -> ToolOutput { return ToolOutput::success (c.server.delivery().listAdapters()); };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "delivery.send";
        t.title = "Deliver a render";
        t.description = "Sends a finished render or preview (source {job_id} of a render.start / render.preview job, or {file: \"renders/mix.mp3\"}) with an adapter from delivery.list_adapters. "
                        "If the project is on hold (project.update delivery_hold) the file is queued and the status is HELD until delivery.release. client_request_id makes a repeat harmless; transient failures are retried.";
        t.inputSchema = schema ({ { "project_id", projectIdProp() }, { "source", schema ({ { "job_id", str() }, { "file", str ("path inside the project, e.g. renders/mix.mp3") } }) },
                                  { "adapter", str() }, { "caption", str() }, { "title", str() }, { "options", { { "type", "object" } } },
                                  { "force", boolean ("send although the project is on hold") }, { "client_request_id", str() },
                                  { "wait_s", { { "type", "number" }, { "minimum", 0 }, { "maximum", 120 } } } }, { "project_id", "source", "adapter" });
        t.annotations = { { "readOnlyHint", false }, { "destructiveHint", false }, { "openWorldHint", true } };
        t.mayBlock = true;
        t.handler = [waitForJob, jobWait] (ToolCall& c) -> ToolOutput
        {
            const std::string projectId = c.args["project_id"];
            DeliverySendArgs a;
            a.tenant = c.session.tenant;
            a.projectId = projectId;
            a.adapter = c.args["adapter"];
            a.caption = c.args.value ("caption", "");
            a.title = c.args.value ("title", "");
            a.clientRequestId = c.args.value ("client_request_id", "");
            a.source = c.args["source"];
            a.options = c.args.value ("options", json::object());
            a.force = c.args.value ("force", false);
            a.origin = c.session.origin().toJson();
            auto started = c.server.delivery().send (a);

            if (! started.ok())
                return ToolOutput::failure (started.error(), std::nullopt, projectId);

            if (! started.value().held.is_null())
            {
                auto d = started.value().held;
                return ToolOutput::success ({ { "delivery_id", d["delivery_id"] }, { "status", "HELD" }, { "adapter", d["adapter"] }, { "file", d["file"] },
                                              { "note", "the project is on hold: the file is queued; delivery.release sends it" } }, std::nullopt, projectId);
            }

            if (! started.value().existing.is_null())
            {
                auto d = started.value().existing;
                return ToolOutput::success ({ { "delivery_id", d["delivery_id"] }, { "status", d["status"] }, { "adapter", d["adapter"] }, { "receipt", d.value ("receipt", json (nullptr)) }, { "created", false } }, std::nullopt, projectId);
            }

            auto out = waitForJob (c, started.value().jobId, true, jobWait (c.args, 60, 120), projectId, true);

            if (out.ok && out.data.value ("state", "") == "succeeded")
            {
                const auto& d = out.data["result"];
                out.data = { { "delivery_id", d["delivery_id"] }, { "status", "sent" }, { "adapter", d["adapter"] }, { "receipt", d.value ("receipt", json (nullptr)) },
                             { "attempts", d.value ("attempts", 1) }, { "job_id", started.value().jobId }, { "created", true } };
            }
            else if (out.ok)
            {
                out.data["status"] = "sending";
            }

            return out;
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "delivery.release";
        t.title = "Release held deliveries";
        t.description = "Sends the deliveries that were queued while the project was on hold (one delivery_id, or all). Each is sent exactly once. Turn the hold off with project.update delivery_hold=false.";
        t.inputSchema = schema ({ { "project_id", projectIdProp() }, { "delivery_id", str() }, { "wait_s", { { "type", "number" }, { "minimum", 0 }, { "maximum", 120 } } } }, { "project_id" });
        t.annotations = { { "readOnlyHint", false }, { "openWorldHint", true } };
        t.mayBlock = true;
        t.handler = [waitForJob, jobWait] (ToolCall& c) -> ToolOutput
        {
            const std::string projectId = c.args["project_id"];
            auto started = c.server.delivery().release (c.session.tenant, projectId, c.args.value ("delivery_id", ""), c.session.origin().toJson());

            if (! started.ok())
                return ToolOutput::failure (started.error(), std::nullopt, projectId);

            json released = json::array();

            for (auto& s : started.value())
            {
                auto out = waitForJob (c, s.jobId, true, jobWait (c.args, 60, 120), projectId, false);

                if (! out.ok)
                    return out;

                if (out.data.value ("state", "") == "succeeded")
                {
                    const auto& d = out.data["result"];
                    released.push_back ({ { "delivery_id", d["delivery_id"] }, { "status", "sent" }, { "adapter", d["adapter"] }, { "receipt", d.value ("receipt", json (nullptr)) } });
                }
                else
                {
                    released.push_back ({ { "job_id", s.jobId }, { "status", out.data.value ("state", "") }, { "error", out.data.value ("error", json (nullptr)) } });
                }
            }

            return ToolOutput::success ({ { "released", released } }, std::nullopt, projectId);
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "delivery.list";
        t.title = "List deliveries";
        t.description = "The delivery journal of a project: delivery_id, adapter, file, status (held|pending|sending|sent|failed), receipt or error.";
        t.inputSchema = schema ({ { "project_id", projectIdProp() }, { "status", str() } }, { "project_id" });
        t.annotations = { { "readOnlyHint", true } };
        t.handler = [] (ToolCall& c) -> ToolOutput
        {
            auto r = c.server.delivery().list (c.session.tenant, c.args["project_id"], c.args.value ("status", ""));

            if (! r.ok())
                return ToolOutput::failure (r.error(), std::nullopt, c.args["project_id"]);

            return ToolOutput::success (r.value(), std::nullopt, c.args["project_id"]);
        };
        add (std::move (t));
    }

    // ---- recipes (task 600, MVP) ---------------------------------------------------------------------------------
    {
        ToolDef t;
        t.name = "recipe.list";
        t.title = "List recipes";
        t.description = "Named multi-step procedures (built-in and saved): recipe_id, description and the parameters each takes.";
        t.inputSchema = schema (json::object());
        t.annotations = { { "readOnlyHint", true } };
        t.handler = [] (ToolCall& c) -> ToolOutput { return ToolOutput::success ({ { "recipes", listRecipes (c.server.projects().host().dataDir()) } }); };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "recipe.get";
        t.title = "Get recipe";
        t.description = "The full recipe: parameter schema, defaults and steps ([{tool, arguments}] with ${params.x} and ${steps.N.field} references).";
        t.inputSchema = schema ({ { "recipe_id", str() } }, { "recipe_id" });
        t.annotations = { { "readOnlyHint", true } };
        t.handler = [] (ToolCall& c) -> ToolOutput
        {
            auto r = loadRecipe (c.server.projects().host().dataDir(), c.args["recipe_id"]);

            if (! r)
                return ToolOutput::failure (makeError (errc::notFound, "no recipe " + c.args["recipe_id"].get<std::string>()));

            return ToolOutput::success ({ { "recipe", *r } });
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "recipe.save";
        t.title = "Save recipe";
        t.description = "Stores a recipe: {id, name, description, params (JSON schema), defaults, steps: [{tool, arguments}]}. Steps may use any tool except recipe.* and batch.";
        t.inputSchema = schema ({ { "recipe", { { "type", "object" } } } }, { "recipe" });
        t.annotations = { { "readOnlyHint", false } };
        t.handler = [] (ToolCall& c) -> ToolOutput
        {
            auto r = saveRecipe (c.server.projects().host().dataDir(), c.args["recipe"]);

            if (! r.ok())
                return ToolOutput::failure (r.error());

            return ToolOutput::success ({ { "recipe_id", r.value()["id"] } });
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "recipe.delete";
        t.title = "Delete recipe";
        t.description = "Deletes a saved recipe.";
        t.inputSchema = schema ({ { "recipe_id", str() } }, { "recipe_id" });
        t.annotations = { { "readOnlyHint", false }, { "destructiveHint", true } };
        t.handler = [] (ToolCall& c) -> ToolOutput
        {
            auto r = deleteRecipe (c.server.projects().host().dataDir(), c.args["recipe_id"]);
            return r.ok() ? ToolOutput::success ({ { "recipe_id", c.args["recipe_id"] } }) : ToolOutput::failure (r.error());
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "recipe.run";
        t.title = "Run recipe";
        t.description = "Runs a recipe step by step with the given params (project_id is filled into project tools). Stops at the first failing step and reports it. The project edits of one run are ONE undo step.";
        t.inputSchema = schema ({ { "recipe_id", str() }, { "project_id", projectIdProp() }, { "params", { { "type", "object" } } } }, { "recipe_id" });
        t.annotations = { { "readOnlyHint", false }, { "openWorldHint", true } };
        t.mayBlock = true;
        t.handler = [] (ToolCall& c) -> ToolOutput
        {
            auto r = loadRecipe (c.server.projects().host().dataDir(), c.args["recipe_id"]);

            if (! r)
                return ToolOutput::failure (makeError (errc::notFound, "no recipe " + c.args["recipe_id"].get<std::string>()));

            return runRecipe (c, *r, c.args.value ("params", json::object()), c.args.value ("project_id", ""));
        };
        add (std::move (t));
    }

    // ---- compatibility with the old mcp__reaper__ bridge (task 210, "shim") -------------------------------------------
    if (config_.reaperCompat)
    {
        using Mapper = std::function<std::pair<std::string, json> (const json&)>;
        auto rename = [] (json a, std::initializer_list<std::pair<const char*, const char*>> keys)
        {
            for (auto& [from, to] : keys)
                if (a.contains (from))
                {
                    a[to] = a[from];
                    a.erase (from);
                }

            return a;
        };

        // batch of one command per array element
        auto each = [] (const json& a, const std::string& arrayKey, const std::string& tool, const std::string& itemKey) -> std::pair<std::string, json>
        {
            json ops = json::array();

            for (auto& v : a.value (arrayKey, json::array()))
                ops.push_back ({ { "tool", tool }, { "arguments", itemKey.empty() ? v : json { { itemKey, v } } } });

            json out { { "project_id", a.value ("project_id", "") }, { "ops", ops } };
            return { "batch", out };
        };

        std::vector<std::pair<std::string, Mapper>> shim {
            { "list_projects", [] (const json& a) { return std::make_pair (std::string ("project.list"), a); } },
            { "new_project", [] (const json& a) { return std::make_pair (std::string ("project.new"), a); } },
            { "open_project", [rename] (const json& a) { return std::make_pair (std::string ("project.open"), rename (a, { { "path", "target" } })); } },
            { "close_project", [] (const json& a) { return std::make_pair (std::string ("project.close"), a); } },
            { "save_project", [] (const json& a) { return std::make_pair (std::string ("project.save"), a); } },
            { "get_project", [] (const json& a) { return std::make_pair (std::string ("project.get"), a); } },
            { "get_undo_history", [] (const json& a) { return std::make_pair (std::string ("history.list"), a); } },
            { "list_tracks", [] (const json& a) { return std::make_pair (std::string ("track.list"), a); } },
            { "create_tracks", [each] (const json& a) { return each (a, "tracks", "track.create", ""); } },
            { "update_tracks", [] (const json& a) { return std::make_pair (std::string ("track.update"), a); } },
            { "delete_tracks", [each] (const json& a) { return each (a, "track_ids", "track.delete", "track_id"); } },
            { "list_items", [] (const json& a) { return std::make_pair (std::string ("item.list"), a); } },
            { "split_items", [] (const json& a)
              {
                  json ops = json::array();

                  for (auto& id : a.value ("item_ids", json::array()))
                      ops.push_back ({ { "tool", "item.split" }, { "arguments", { { "item_id", id }, { "at_s", a.value ("at_s", 0.0) } } } });

                  return std::make_pair (std::string ("batch"), json { { "project_id", a.value ("project_id", "") }, { "ops", ops } });
              } },
            { "delete_items", [each] (const json& a) { return each (a, "item_ids", "item.delete", "item_id"); } },
            { "update_items", [rename] (const json& a)
              {
                  json ops = json::array();

                  for (auto& u : a.value ("updates", json::array()))
                      ops.push_back ({ { "tool", "item.update" }, { "arguments", rename (u, { { "position_s", "start_s" } }) } });

                  return std::make_pair (std::string ("batch"), json { { "project_id", a.value ("project_id", "") }, { "ops", ops } });
              } },
            { "list_markers", [] (const json& a) { return std::make_pair (std::string ("marker.list"), a); } },
            { "upsert_markers", [] (const json& a)
              {
                  json markers = json::array();

                  for (auto m : a.value ("markers", json::array()))
                  {
                      if (m.contains ("id"))          { m["marker_id"] = m["id"]; m.erase ("id"); }
                      if (m.contains ("position_s"))  { m["time_s"] = m["position_s"]; m.erase ("position_s"); }
                      m.erase ("kind");
                      m.erase ("end_s");
                      m.erase ("color");
                      markers.push_back (m);
                  }

                  return std::make_pair (std::string ("marker.upsert"), json { { "project_id", a.value ("project_id", "") }, { "markers", markers } });
              } },
            { "delete_markers", [each] (const json& a) { return each (a, "ids", "marker.delete", "marker_id"); } },
            { "get_tempo_map", [] (const json& a) { return std::make_pair (std::string ("tempo.get"), a); } },
            { "set_tempo_map", [] (const json& a) { return std::make_pair (std::string ("tempo.set"), a); } },
            { "time_convert", [] (const json& a) { return std::make_pair (std::string ("time.convert"), a); } },
            { "list_fx", [] (const json& a)
              {
                  json t = a.value ("target", json::object());
                  return std::make_pair (std::string ("fx.list"), json { { "project_id", a.value ("project_id", "") }, { "target", t.is_string() ? t : json (t.value ("track_id", "master")) } });
              } },
            { "add_fx", [] (const json& a)
              {
                  json t = a.value ("target", json::object());
                  json out { { "project_id", a.value ("project_id", "") }, { "track_id", t.is_string() ? t : json (t.value ("track_id", "master")) } };
                  auto name = a.value ("name_or_id", a.value ("builtin", std::string()));
                  std::transform (name.begin(), name.end(), name.begin(), [] (unsigned char ch) { return (char) std::tolower (ch); });

                  for (auto* b : { "equaliser", "compressor", "reverb", "delay", "chorus", "phaser", "lowpass" })
                      if (name.find (b) != std::string::npos || (std::string (b) == "equaliser" && name.find ("eq") != std::string::npos))
                          out["builtin"] = b;

                  return std::make_pair (std::string ("fx.add"), out);
              } },
            { "remove_fx", [rename] (const json& a) { return std::make_pair (std::string ("fx.remove"), rename (a, { { "fx_id", "plugin_id" } })); } },
            { "update_fx", [rename] (const json& a) { return std::make_pair (std::string ("fx.update"), rename (a, { { "fx_id", "plugin_id" } })); } },
            { "get_fx_params", [rename] (const json& a) { return std::make_pair (std::string ("fx.params"), rename (a, { { "fx_id", "plugin_id" } })); } },
            { "render", [] (const json& a) { return std::make_pair (std::string ("render.start"), a); } },
            { "get_render_job", [] (const json& a) { return std::make_pair (std::string ("job.get"), a); } },
            { "cancel_render", [] (const json& a) { return std::make_pair (std::string ("job.cancel"), a); } },
        };

        for (auto& [alias, mapper] : shim)
        {
            if (tools_.count (alias) || toolAliases_.count (alias))
                continue;

            ToolDef t;
            t.name = alias;
            t.title = alias + " (REAPER bridge name)";
            t.description = "Compatibility name of the old mcp__reaper__ bridge; same arguments as the Rovox tool it maps to, ids are Rovox ids.";
            t.inputSchema = { { "type", "object" }, { "additionalProperties", true } };
            t.mayBlock = true;
            t.handler = [this, mapper] (ToolCall& c) -> ToolOutput
            {
                auto [target, args] = mapper (c.args);
                const auto* tool = findTool (target);

                if (tool == nullptr)
                    return ToolOutput::failure (makeError (errc::notImplemented, "no Rovox tool for " + target));

                if (auto v = validateJson (tool->inputSchema, args); ! v.ok())
                    return ToolOutput::failure (v.error());

                ToolCall inner { c.server, c.session, c.rc, args, tool->name };
                return tool->handler (inner);
            };
            tools_[alias] = std::move (t);
        }
    }

    (void) canonicalName;
}

} // namespace rovox::mcp
