// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "JsonSchema.h"

namespace rovox
{

namespace
{
bool matchesType (const std::string& type, const json& v)
{
    if (type == "object")   return v.is_object();
    if (type == "array")    return v.is_array();
    if (type == "string")   return v.is_string();
    if (type == "boolean")  return v.is_boolean();
    if (type == "null")     return v.is_null();
    if (type == "integer")  return v.is_number_integer();
    if (type == "number")   return v.is_number();
    return true;
}

Error fail (const std::string& path, const std::string& message)
{
    return makeError (errc::invalidParams, (path.empty() ? std::string ("/") : path) + ": " + message, json { { "path", path.empty() ? "/" : path } });
}

Result<void> check (const json& schema, const json& v, const std::string& path)
{
    if (! schema.is_object())
        return {};

    if (auto it = schema.find ("type"); it != schema.end())
    {
        bool ok = false;

        if (it->is_string())
            ok = matchesType (it->get<std::string>(), v);
        else if (it->is_array())
            for (auto& t : *it)
                ok = ok || matchesType (t.get<std::string>(), v);

        if (! ok)
            return fail (path, "expected " + it->dump());
    }

    if (auto it = schema.find ("enum"); it != schema.end() && it->is_array())
    {
        bool found = false;

        for (auto& e : *it)
            found = found || e == v;

        if (! found)
            return fail (path, "must be one of " + it->dump());
    }

    if (v.is_number())
    {
        if (auto it = schema.find ("minimum"); it != schema.end() && v.get<double>() < it->get<double>())
            return fail (path, "must be >= " + it->dump());

        if (auto it = schema.find ("maximum"); it != schema.end() && v.get<double>() > it->get<double>())
            return fail (path, "must be <= " + it->dump());
    }

    if (v.is_string())
    {
        const auto len = v.get<std::string>().size();

        if (auto it = schema.find ("minLength"); it != schema.end() && len < it->get<size_t>())
            return fail (path, "string too short");

        if (auto it = schema.find ("maxLength"); it != schema.end() && len > it->get<size_t>())
            return fail (path, "string too long");
    }

    if (v.is_array())
    {
        if (auto it = schema.find ("minItems"); it != schema.end() && v.size() < it->get<size_t>())
            return fail (path, "too few items");

        if (auto it = schema.find ("maxItems"); it != schema.end() && v.size() > it->get<size_t>())
            return fail (path, "too many items");

        if (auto it = schema.find ("items"); it != schema.end())
            for (size_t i = 0; i < v.size(); ++i)
                if (auto r = check (*it, v[i], path + "/" + std::to_string (i)); ! r.ok())
                    return r;
    }

    if (v.is_object())
    {
        if (auto it = schema.find ("required"); it != schema.end())
            for (auto& name : *it)
                if (! v.contains (name.get<std::string>()))
                    return fail (path + "/" + name.get<std::string>(), "is required");

        const auto props = schema.contains ("properties") ? schema["properties"] : json::object();

        for (auto it = v.begin(); it != v.end(); ++it)
        {
            if (auto p = props.find (it.key()); p != props.end())
            {
                if (auto r = check (*p, it.value(), path + "/" + it.key()); ! r.ok())
                    return r;
            }
            else if (schema.value ("additionalProperties", true) == false)
            {
                return fail (path + "/" + it.key(), "unknown property");
            }
        }
    }

    return {};
}
} // namespace

Result<void> validateJson (const json& schema, const json& value)
{
    return check (schema, value, "");
}

} // namespace rovox
