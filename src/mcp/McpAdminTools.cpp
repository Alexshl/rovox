// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// key.* and tenant.*: the operator's tools for API keys and tenants (scope `admin`, which only the local tenant can hold; every handler
// checks the tenant again). key.create and key.rotate return the full key once, in this response: it ends up in the agent's transcript,
// so give it to the tenant right away. key.list and everything else show only the key id.
#include "Mcp.h"

#include "Ids.h"
#include "KeyStore.h"
#include "Quota.h"
#include "Scopes.h"

#include <algorithm>
#include <cstdlib>
#include <set>

namespace rovox::mcp
{

namespace
{
json strProp (const std::string& description)
{
    return { { "type", "string" }, { "description", description } };
}

json adminSchema (json properties, std::vector<std::string> required = {})
{
    return { { "type", "object" }, { "properties", std::move (properties) }, { "required", json (required) }, { "additionalProperties", false } };
}

/** Admin tools are for the local operator only, whatever scopes a session carries. */
std::optional<ToolOutput> operatorOnly (ToolCall& c)
{
    if (c.session.tenant != "local")
        return ToolOutput::failure (forbiddenScopeError (Scope::admin, c.name));

    return std::nullopt;
}

fs::path configFileOf (McpServer& server)
{
    fs::path file = server.config().configFile;

    if (file.empty())
        file = std::getenv ("ROVOX_CONFIG") != nullptr ? fs::path (std::getenv ("ROVOX_CONFIG")) : server.projects().host().dataDir() / "config" / "rovox.toml";

    return file;
}

ToolOutput createdOutput (const KeyStore::Created& created)
{
    return ToolOutput::success ({ { "key", created.key }, { "record", created.record },
                                  { "note", "The key is shown only now; it cannot be read back. Give it to the tenant and do not store it elsewhere." } });
}

json tenantRecord (KeyStore& keys, const QuotaConfig& config, const std::string& tenant)
{
    int active = 0, total = 0;
    const auto now = keys.now();

    for (auto& k : keys.list())
        if (k.value ("tenant", "") == tenant)
        {
            ++total;

            if (! k.contains ("revoked_at") && ! (k.contains ("expires_at") && now >= k["expires_at"].get<std::int64_t>()))
                ++active;
        }

    json out { { "tenant", tenant }, { "keys", { { "total", total }, { "active", active } } }, { "limits", quotas().profile (tenant).toJson() } };

    if (auto it = config.tenantProfile.find (tenant); it != config.tenantProfile.end())
        out["quota_profile"] = it->second;

    out["usage"] = quotas().summary (tenant);
    return out;
}
} // namespace

void McpServer::buildAdminTools()
{
    if (config_.keyStore == nullptr)
        return;

    KeyStore& keys = *config_.keyStore;

    auto add = [this] (ToolDef t)
    {
        const auto name = t.name;
        std::string underscore = name;
        std::replace (underscore.begin(), underscore.end(), '.', '_');

        if (underscore != name)
            toolAliases_[underscore] = name;

        tools_[name] = std::move (t);
    };

    {
        ToolDef t;
        t.name = "key.create";
        t.title = "Create an API key";
        t.description = "Creates an API key for a tenant. The full key is in this response only (it lands in the transcript of whoever called): hand it over at once; it cannot be read again. "
                        "Operator only (tenant local with scope admin).";
        t.inputSchema = adminSchema ({ { "tenant", strProp ("Tenant id: 1-64 characters of a-z, 0-9, _ and -") },
                                       { "scopes", { { "type", "array" }, { "items", { { "type", "string" } } }, { "minItems", 1 },
                                                     { "description", "catalog:read, projects:read, projects:write, render, delivery, admin (admin only for tenant local); read and write are shorthands" } } },
                                       { "label", strProp ("Free text to recognise the key") },
                                       { "expires", strProp ("30d (days from now) or an ISO date / time in UTC") },
                                       { "quota_profile", strProp ("Quota profile of this key (default: the tenant's)") } }, { "tenant", "scopes" });
        t.annotations = { { "readOnlyHint", false }, { "destructiveHint", false }, { "idempotentHint", false } };
        t.handler = [&keys] (ToolCall& c) -> ToolOutput
        {
            if (auto denied = operatorOnly (c))
                return *denied;

            std::vector<std::string> names;

            for (auto& s : c.args["scopes"])
                names.push_back (s.get<std::string>());

            auto scopes = ScopeSet::parse (names);

            if (! scopes.ok())
                return ToolOutput::failure (scopes.error());

            KeyStore::CreateSpec spec;
            spec.tenant = c.args["tenant"].get<std::string>();
            spec.scopes = scopes.value();
            spec.label = c.args.value ("label", std::string());
            spec.quotaProfile = c.args.value ("quota_profile", std::string());

            if (c.args.contains ("expires"))
            {
                auto expiry = parseExpiry (c.args["expires"].get<std::string>(), keys.now());

                if (! expiry.ok())
                    return ToolOutput::failure (expiry.error());

                spec.expiresAt = expiry.value();
            }

            auto created = keys.create (spec, "mcp");

            if (! created.ok())
                return ToolOutput::failure (created.error());

            return createdOutput (created.value());
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "key.list";
        t.title = "List API keys";
        t.description = "Keys with their tenant, scopes, label, expiry, revocation and last use. Never a secret. Operator only.";
        t.inputSchema = adminSchema ({ { "tenant", strProp ("Only this tenant") } });
        t.annotations = { { "readOnlyHint", true } };
        t.handler = [&keys] (ToolCall& c) -> ToolOutput
        {
            if (auto denied = operatorOnly (c))
                return *denied;

            const auto tenant = c.args.value ("tenant", std::string());
            json out = json::array();

            for (auto& k : keys.list())
                if (tenant.empty() || k.value ("tenant", "") == tenant)
                    out.push_back (k);

            return ToolOutput::success ({ { "keys", out } });
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "key.revoke";
        t.title = "Revoke an API key";
        t.description = "Revokes a key; it stops working within a second. Operator only.";
        t.inputSchema = adminSchema ({ { "key_id", strProp ("Key id (k_...)") } }, { "key_id" });
        t.annotations = { { "readOnlyHint", false }, { "destructiveHint", true }, { "idempotentHint", true } };
        t.handler = [&keys] (ToolCall& c) -> ToolOutput
        {
            if (auto denied = operatorOnly (c))
                return *denied;

            auto r = keys.revoke (c.args["key_id"].get<std::string>(), "mcp");

            if (! r.ok())
                return ToolOutput::failure (r.error());

            return ToolOutput::success ({ { "record", r.value() } });
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "key.rotate";
        t.title = "Rotate an API key";
        t.description = "Creates a replacement with the same tenant, scopes and label; the old key is revoked at once or, with grace_s, expires after that many seconds. "
                        "The new full key is in this response only. Operator only.";
        t.inputSchema = adminSchema ({ { "key_id", strProp ("Key id (k_...)") },
                                       { "grace_s", { { "type", "integer" }, { "minimum", 0 }, { "description", "Seconds the old key keeps working (default 0)" } } } }, { "key_id" });
        t.annotations = { { "readOnlyHint", false }, { "destructiveHint", false }, { "idempotentHint", false } };
        t.handler = [&keys] (ToolCall& c) -> ToolOutput
        {
            if (auto denied = operatorOnly (c))
                return *denied;

            auto r = keys.rotate (c.args["key_id"].get<std::string>(), c.args.value ("grace_s", (std::int64_t) 0), "mcp");

            if (! r.ok())
                return ToolOutput::failure (r.error());

            return createdOutput (r.value());
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "tenant.list";
        t.title = "List tenants";
        t.description = "Tenants known from the configuration ([tenants.<id>]) and from API keys, with their key counts and quota limits. Operator only.";
        t.inputSchema = adminSchema (json::object());
        t.annotations = { { "readOnlyHint", true } };
        t.handler = [this, &keys] (ToolCall& c) -> ToolOutput
        {
            if (auto denied = operatorOnly (c))
                return *denied;

            const auto config = QuotaConfig::load (configFileOf (*this));
            std::set<std::string> ids { "local" };

            for (auto& entry : config.tenantProfile)
                ids.insert (entry.first);

            for (auto& k : keys.list())
                ids.insert (k.value ("tenant", std::string ("local")));

            json out = json::array();

            for (auto& id : ids)
            {
                auto r = tenantRecord (keys, config, id);
                r.erase ("usage");
                out.push_back (std::move (r));
            }

            return ToolOutput::success ({ { "tenants", out } });
        };
        add (std::move (t));
    }

    {
        ToolDef t;
        t.name = "tenant.get";
        t.title = "Show a tenant";
        t.description = "One tenant: its key counts, quota profile and limits, and its usage so far (as usage.summary). Operator only.";
        t.inputSchema = adminSchema ({ { "tenant", strProp ("Tenant id") } }, { "tenant" });
        t.annotations = { { "readOnlyHint", true } };
        t.handler = [this, &keys] (ToolCall& c) -> ToolOutput
        {
            if (auto denied = operatorOnly (c))
                return *denied;

            const auto tenant = c.args["tenant"].get<std::string>();

            if (! isValidTenantId (tenant))
                return ToolOutput::failure (makeError (errc::invalidParams, "invalid tenant id"));

            const auto config = QuotaConfig::load (configFileOf (*this));
            return ToolOutput::success (tenantRecord (keys, config, tenant));
        };
        add (std::move (t));
    }
}

} // namespace rovox::mcp
