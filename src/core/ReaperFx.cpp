// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "ReaperFx.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>

namespace rovox
{

//==============================================================================
namespace
{
using json = nlohmann::json;

std::string lowerOf (std::string s)
{
    std::transform (s.begin(), s.end(), s.begin(), [] (unsigned char c) { return (char) std::tolower (c); });
    return s;
}

std::string trim (const std::string& s)
{
    const auto a = s.find_first_not_of (" \t\r\n");
    const auto b = s.find_last_not_of (" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr (a, b - a + 1);
}

/** Lower case words of a parameter name: "Freq-Band 1" -> ["freq", "band", "1"]. */
std::vector<std::string> words (const std::string& name)
{
    std::vector<std::string> out;
    std::string cur;

    for (unsigned char c : name)
    {
        if (std::isalnum (c))
        {
            cur += (char) std::tolower (c);
        }
        else if (! cur.empty())
        {
            out.push_back (cur);
            cur.clear();
        }
    }

    if (! cur.empty())
        out.push_back (cur);

    return out;
}

bool hasWord (const std::vector<std::string>& w, const char* word)
{
    return std::find (w.begin(), w.end(), word) != w.end();
}

bool hasPrefixWord (const std::vector<std::string>& w, const char* prefix)
{
    const std::string p (prefix);

    return std::any_of (w.begin(), w.end(), [&] (const std::string& x) { return x.rfind (p, 0) == 0; });
}

std::string joinWords (const std::vector<std::string>& w)
{
    std::string out;

    for (auto& x : w)
        out += (out.empty() ? "" : " ") + x;

    return out;
}

constexpr double minusInfDb = -150.0;

std::optional<double> asDb (const ParsedValue& p)
{
    if (! p.ok)
        return std::nullopt;

    if (p.infinite)
        return p.value < 0 ? minusInfDb : 150.0;

    if (p.unit.empty() || p.unit == "db")
        return p.value;

    return std::nullopt;
}

std::optional<double> asHz (const ParsedValue& p)
{
    if (! p.ok || p.infinite)
        return std::nullopt;

    if (p.unit.empty() || p.unit == "hz")   return p.value;
    if (p.unit == "khz" || p.unit == "k")   return p.value * 1000.0;
    return std::nullopt;
}

std::optional<double> asMs (const ParsedValue& p)
{
    if (! p.ok || p.infinite)
        return std::nullopt;

    if (p.unit.empty() || p.unit == "ms")                       return p.value;
    if (p.unit == "s" || p.unit == "sec")                       return p.value * 1000.0;
    if (p.unit == "us")                                         return p.value / 1000.0;
    return std::nullopt;
}

/** 0..1 from "50 %", else from the normalized value (REAPER's own fraction for a unit-less display). */
double asFraction (const ParsedValue& p, double normalized)
{
    if (p.ok && ! p.infinite && p.unit == "%")
        return std::clamp (p.value / 100.0, 0.0, 1.0);

    return std::clamp (normalized, 0.0, 1.0);
}

bool asFlag (const ReaperFxParam& p)
{
    const auto t = lowerOf (trim (p.formatted));

    if (t == "off" || t == "disabled" || t == "no" || t == "false" || t == "0" || t == "bypassed" || t == "inactive")
        return false;

    if (t == "on" || t == "enabled" || t == "yes" || t == "true" || t == "1" || t == "active")
        return true;

    return p.normalized >= 0.5;
}

double dbToLin (double db)      { return db <= minusInfDb + 1.0 ? 0.0 : std::pow (10.0, db / 20.0); }
double linToDb (double lin)     { return lin <= 1.0e-7 ? -100.0 : 20.0 * std::log10 (lin); }

struct Sink
{
    FxTranslation out;

    void set (const ReaperFxParam& p, const std::string& to, double value, const std::string& note = {})
    {
        out.translated.push_back ({ p.name, p.formatted, to, value, note });
    }

    void skip (const ReaperFxParam& p, const std::string& reason)
    {
        out.untranslated.push_back ({ p.name, p.formatted, reason });
    }

    void unparsed (const ReaperFxParam& p)  { skip (p, "the text could not be read as a value with a known unit"); }
};
} // namespace

//==============================================================================
ParsedValue parseReaperValue (const std::string& formatted)
{
    ParsedValue r;
    const auto t = lowerOf (trim (formatted));

    if (t.empty())
        return r;

    const bool negative = t[0] == '-';
    auto bare = (negative || t[0] == '+') ? trim (t.substr (1)) : t;

    if (bare.rfind ("inf", 0) == 0 || bare.rfind ("\xe2\x88\x9e", 0) == 0)
    {
        r.ok = true;
        r.infinite = true;
        r.value = negative ? -1.0 : 1.0;
        r.unit = trim (bare.substr (bare.rfind ("inf", 0) == 0 ? 3 : 3));

        if (r.unit.rfind (":", 0) == 0)
            r.unit = ":1";

        if (const auto sp = r.unit.find (' '); sp != std::string::npos)
            r.unit = r.unit.substr (0, sp);

        return r;
    }

    char* end = nullptr;
    const double v = std::strtod (t.c_str(), &end);

    if (end == t.c_str())
        return r;

    r.ok = true;
    r.value = v;
    auto unit = trim (std::string (end));

    // "3.0:1" is a ratio
    if (unit.rfind (":", 0) == 0)
        unit = ":1";

    // a trailing word after the unit ("100 Hz (low)") is not part of it
    if (const auto sp = unit.find (' '); sp != std::string::npos)
        unit = unit.substr (0, sp);

    if (unit == "hertz")        unit = "hz";
    if (unit == "khertz")       unit = "khz";
    if (unit == "msec")         unit = "ms";
    if (unit == "dbfs")         unit = "db";
    if (unit == "octaves" || unit == "octave")  unit = "oct";

    r.unit = unit;
    return r;
}

double bandwidthOctForQ (double q)
{
    // Q(bw) = sqrt(2^bw) / (2^bw - 1) falls with bw; bisect on [0.01, 10]
    double lo = 0.01, hi = 10.0;

    for (int i = 0; i < 80; ++i)
    {
        const double mid = 0.5 * (lo + hi), p = std::pow (2.0, mid), qq = std::sqrt (p) / (p - 1.0);

        if (qq > q)
            lo = mid;
        else
            hi = mid;
    }

    return 0.5 * (lo + hi);
}

std::string normaliseReaperGuid (const std::string& g)
{
    std::string out;

    for (unsigned char c : g)
        if (std::isxdigit (c))
            out += (char) std::tolower (c);

    return out;
}

ReaperFxEntry parseReaperFxEntry (const json& j)
{
    ReaperFxEntry e;
    e.name = j.value ("name", "");
    e.guid = j.value ("guid", "");
    e.enabled = j.value ("enabled", true);

    if (j.contains ("params") && j["params"].is_array())
        for (auto& p : j["params"])
        {
            ReaperFxParam rp;
            rp.index = p.value ("index", 0);
            rp.name = p.value ("name", "");
            rp.normalized = p.contains ("normalized") && p["normalized"].is_number() ? p["normalized"].get<double>() : 0.0;
            rp.formatted = p.contains ("formatted") && p["formatted"].is_string() ? p["formatted"].get<std::string>() : std::string();
            e.params.push_back (std::move (rp));
        }

    if (j.contains ("config") && j["config"].is_object())
        for (auto it = j["config"].begin(); it != j["config"].end(); ++it)
            e.config[it.key()] = it.value().is_string() ? it.value().get<std::string>() : it.value().dump();

    return e;
}

//==============================================================================
namespace
{
//------------------------------------------------------------------------------ ReaEQ
struct EqBandIn
{
    std::string key;
    const ReaperFxParam* freq = nullptr;
    const ReaperFxParam* gain = nullptr;
    const ReaperFxParam* bw = nullptr;
    const ReaperFxParam* q = nullptr;
    const ReaperFxParam* enable = nullptr;
};

void translateEq (const ReaperFxEntry& fx, Sink& s)
{
    std::vector<EqBandIn> bands;

    auto bandFor = [&] (const std::string& key) -> EqBandIn&
    {
        for (auto& b : bands)
            if (b.key == key)
                return b;

        bands.push_back ({});
        bands.back().key = key;
        return bands.back();
    };

    const ReaperFxParam* outGain = nullptr;

    for (auto& p : fx.params)
    {
        auto w = words (p.name);

        if ((hasWord (w, "global") || hasWord (w, "output") || hasWord (w, "master")) && hasWord (w, "gain"))
        {
            outGain = &p;
            continue;
        }

        // the kind word (freq / gain / bw / q / enabled) may stand first or last; what remains names the band
        static const char* const kinds[] = { "freq", "frequency", "gain", "bw", "bandwidth", "q", "enabled", "enable", "on" };
        std::string kind;
        std::vector<std::string> rest;

        for (auto& x : w)
        {
            if (kind.empty() && std::find (std::begin (kinds), std::end (kinds), x) != std::end (kinds))
                kind = x;
            else
                rest.push_back (x);
        }

        if (kind.empty())
        {
            s.skip (p, "unknown ReaEQ parameter");
            continue;
        }

        auto& b = bandFor (joinWords (rest));

        if (kind == "freq" || kind == "frequency")                  b.freq = &p;
        else if (kind == "gain")                                    b.gain = &p;
        else if (kind == "bw" || kind == "bandwidth")               b.bw = &p;
        else if (kind == "q")                                       b.q = &p;
        else                                                        b.enable = &p;
    }

    const size_t maxBands = 8;

    for (size_t i = 0; i < bands.size(); ++i)
    {
        auto& b = bands[i];

        if (i >= maxBands)
        {
            for (auto* p : { b.freq, b.gain, b.bw, b.q, b.enable })
                if (p != nullptr)
                    s.skip (*p, "the Rovox EQ has 8 bands");

            continue;
        }

        const auto n = "b" + std::to_string (i + 1) + "_";
        const auto idx = std::to_string (i);

        // band type: from the exporter's BANDTYPE<n> (ReaEQ named config), else from the band's name
        int type = 2;
        std::string typeNote;

        if (auto it = fx.config.find ("BANDTYPE" + idx); it != fx.config.end())
        {
            const int reaType = std::atoi (it->second.c_str());
            static const int map[] = { 0, 1, 2, 3, 4, 2, 5, 6, 2 };

            if (reaType >= 0 && reaType < 9)
                type = map[reaType];

            if (reaType == 5)
                typeNote = "; all-pass has no Rovox equivalent, the band is a flat peak";
        }
        else
        {
            const auto has = [&] (const char* x) { return b.key.find (x) != std::string::npos; };

            if (has ("low shelf") || has ("lowshelf"))              type = 0;
            else if (has ("high shelf") || has ("highshelf"))       type = 1;
            else if (has ("low pass") || has ("lowpass"))           type = 3;
            else if (has ("high pass") || has ("highpass"))         type = 4;
            else if (has ("notch"))                                 type = 5;
            else if (has ("band pass") || has ("bandpass"))         type = 6;
        }

        static const char* const typeNames[] = { "low_shelf", "high_shelf", "peak", "low_pass", "high_pass", "notch", "band_pass" };
        s.out.translated.push_back ({ "band type (" + b.key + ")", fx.config.count ("BANDTYPE" + idx) ? "BANDTYPE" + idx + "=" + fx.config.at ("BANDTYPE" + idx) : "from the band name",
                                      n + "type", (double) type, std::string (typeNames[type]) + typeNote });

        bool enabled = true;

        if (auto it = fx.config.find ("BANDENABLED" + idx); it != fx.config.end())
            enabled = std::atoi (it->second.c_str()) != 0;

        if (b.enable != nullptr)
        {
            enabled = asFlag (*b.enable);
            s.set (*b.enable, n + "on", enabled ? 1.0 : 0.0);
        }
        else
        {
            s.out.translated.push_back ({ "band enabled (" + b.key + ")", fx.config.count ("BANDENABLED" + idx) ? "BANDENABLED" + idx + "=" + fx.config.at ("BANDENABLED" + idx) : "assumed on",
                                          n + "on", enabled ? 1.0 : 0.0, "" });
        }

        if (b.freq != nullptr)
        {
            if (auto hz = asHz (parseReaperValue (b.freq->formatted)))      s.set (*b.freq, n + "freq", *hz);
            else                                                            s.unparsed (*b.freq);
        }

        if (b.gain != nullptr)
        {
            if (auto db = asDb (parseReaperValue (b.gain->formatted)))      s.set (*b.gain, n + "gain", *db);
            else                                                            s.unparsed (*b.gain);
        }

        // bandwidth: in octaves ("1.00 oct", or a bare number for a BW parameter) or as a Q
        for (auto* bwp : { b.bw, b.q })
        {
            if (bwp == nullptr)
                continue;

            const auto v = parseReaperValue (bwp->formatted);

            if (! v.ok || v.infinite || v.value <= 0.0)
            {
                s.unparsed (*bwp);
                continue;
            }

            if (bwp == b.q && v.unit != "oct")
                s.set (*bwp, n + "bw", bandwidthOctForQ (v.value), "Q " + std::to_string (v.value) + " converted to octaves");
            else
                s.set (*bwp, n + "bw", v.value);
        }
    }

    // bands the REAPER instance does not have are off
    for (size_t i = bands.size(); i < maxBands; ++i)
        s.out.translated.push_back ({ "(no band " + std::to_string (i + 1) + ")", "", "b" + std::to_string (i + 1) + "_on", 0.0, "not present in the REAPER EQ" });

    if (outGain != nullptr)
    {
        if (auto db = asDb (parseReaperValue (outGain->formatted)))     s.set (*outGain, "out_gain", *db);
        else                                                            s.unparsed (*outGain);
    }
}

//------------------------------------------------------------------------------ ReaComp
void translateComp (const ReaperFxEntry& fx, Sink& s)
{
    const ReaperFxParam *wet = nullptr, *dry = nullptr;

    for (auto& p : fx.params)
    {
        const auto w = words (p.name);
        const auto v = parseReaperValue (p.formatted);
        const auto text = joinWords (w);
        const bool lim = hasPrefixWord (w, "lim");

        if (hasPrefixWord (w, "thresh") && ! lim)
        {
            if (auto db = asDb (v))     s.set (p, "threshold", *db);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "ratio"))
        {
            if (v.ok && v.infinite)
                s.set (p, "ratio", 100.0, "infinite ratio");
            else if (v.ok && v.value >= 1.0 && (v.unit == ":1" || v.unit.empty()))
                s.set (p, "ratio", v.value);
            else
                s.unparsed (p);
        }
        else if (hasWord (w, "attack") && ! hasWord (w, "auto"))
        {
            if (auto ms = asMs (v))     s.set (p, "attack", *ms);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "release") && ! hasWord (w, "auto") && ! lim)
        {
            if (auto ms = asMs (v))     s.set (p, "release", *ms);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "knee"))
        {
            if (auto db = asDb (v))     s.set (p, "knee", *db);
            else                        s.unparsed (p);
        }
        else if (text.find ("pre comp") != std::string::npos || text.find ("precomp") != std::string::npos || hasWord (w, "lookahead"))
        {
            if (auto ms = asMs (v))     s.set (p, "lookahead", *ms, "pre-comp");
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "rms"))
        {
            if (auto ms = asMs (v))     s.set (p, "rms", *ms);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "auto") && (hasWord (w, "makeup") || (hasWord (w, "make") && hasWord (w, "up"))))
        {
            s.set (p, "auto_makeup", asFlag (p) ? 1.0 : 0.0);
        }
        else if (w.size() == 1 && w[0] == "wet")
        {
            wet = &p;
        }
        else if (w.size() == 1 && w[0] == "dry")
        {
            dry = &p;
        }
        else
        {
            s.skip (p, "no equivalent in the Rovox Comp");
        }
    }

    // ReaComp output = dry * x + wet * compressed; Rovox output = mix * makeup * compressed + (1 - mix) * x
    if (wet != nullptr || dry != nullptr)
    {
        const auto wv = wet != nullptr ? asDb (parseReaperValue (wet->formatted)) : std::optional<double> (0.0);
        const auto dv = dry != nullptr ? asDb (parseReaperValue (dry->formatted)) : std::optional<double> (minusInfDb);

        if (! wv || ! dv)
        {
            if (wet != nullptr && ! wv)     s.unparsed (*wet);
            if (dry != nullptr && ! dv)     s.unparsed (*dry);
        }
        else
        {
            const double w = dbToLin (*wv), d = dbToLin (*dv);
            const double mix = std::clamp (1.0 - d, 0.0, 1.0);
            const double makeupDb = mix > 0.0 && w > 0.0 ? linToDb (w / mix) : 0.0;
            const auto note = "wet " + std::to_string (*wv) + " dB / dry " + std::to_string (*dv) + " dB";

            if (wet != nullptr)     s.set (*wet, "makeup", makeupDb, note);
            if (dry != nullptr)     s.set (*dry, "mix", mix * 100.0, note);

            if (d >= 1.0)
                s.out.untranslated.push_back ({ "Dry", dry != nullptr ? dry->formatted : "", "dry level at or above 0 dB cannot be reproduced; the mix is 0 %" });
        }
    }
}

//------------------------------------------------------------------------------ ReaLimit
void translateLimit (const ReaperFxEntry& fx, Sink& s)
{
    const ReaperFxParam *threshold = nullptr, *ceiling = nullptr;

    for (auto& p : fx.params)
    {
        const auto w = words (p.name);

        if (hasPrefixWord (w, "thresh"))            threshold = &p;
        else if (hasPrefixWord (w, "ceil"))         ceiling = &p;
        else if (hasWord (w, "release"))
        {
            if (auto ms = asMs (parseReaperValue (p.formatted)))    s.set (p, "release", *ms);
            else                                                    s.unparsed (p);
        }
        else
        {
            s.skip (p, "no equivalent in the Rovox Limit");
        }
    }

    // ReaLimit limits at the threshold and brings the peaks up to the ceiling: Rovox input gain = ceiling - threshold
    const auto t = threshold != nullptr ? asDb (parseReaperValue (threshold->formatted)) : std::optional<double> (0.0);
    const auto c = ceiling != nullptr ? asDb (parseReaperValue (ceiling->formatted)) : std::optional<double> (0.0);

    if (threshold != nullptr && ! t)        s.unparsed (*threshold);
    if (ceiling != nullptr && ! c)          s.unparsed (*ceiling);

    if (c && ceiling != nullptr)
        s.set (*ceiling, "ceiling", *c);

    if (t && c && threshold != nullptr)
        s.set (*threshold, "input_gain", std::max (0.0, *c - *t), "threshold " + std::to_string (*t) + " dB, ceiling " + std::to_string (*c) + " dB");
}

//------------------------------------------------------------------------------ ReaGate
void translateGate (const ReaperFxEntry& fx, Sink& s)
{
    for (auto& p : fx.params)
    {
        const auto w = words (p.name);
        const auto v = parseReaperValue (p.formatted);

        if (hasPrefixWord (w, "thresh"))
        {
            if (auto db = asDb (v))     s.set (p, "threshold", *db);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "attack"))
        {
            if (auto ms = asMs (v))     s.set (p, "attack", *ms);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "hold"))
        {
            if (auto ms = asMs (v))     s.set (p, "hold", *ms);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "release"))
        {
            if (auto ms = asMs (v))     s.set (p, "release", *ms);
            else                        s.unparsed (p);
        }
        else if (hasPrefixWord (w, "hyster"))
        {
            if (auto db = asDb (v))     s.set (p, "hysteresis", *db);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "range") || hasWord (w, "floor") || (w.size() == 1 && w[0] == "dry"))
        {
            // ReaGate's dry level is what passes while the gate is closed
            if (auto db = asDb (v))     s.set (p, "range", std::max (*db, -100.0), w[0] == "dry" ? "dry level = attenuation while closed" : "");
            else                        s.unparsed (p);
        }
        else if (w.size() == 1 && w[0] == "wet")
        {
            if (auto db = asDb (v); db && std::abs (*db) < 0.05)
                continue;       // 0 dB is the Rovox behaviour

            s.skip (p, "wet level other than 0 dB has no equivalent in the Rovox Gate");
        }
        else
        {
            s.skip (p, "no equivalent in the Rovox Gate");
        }
    }
}

//------------------------------------------------------------------------------ ReaVerbate
void translateVerb (const ReaperFxEntry& fx, Sink& s)
{
    for (auto& p : fx.params)
    {
        const auto w = words (p.name);
        const auto text = joinWords (w);
        const auto v = parseReaperValue (p.formatted);

        if (w.size() == 1 && w[0] == "wet")
        {
            if (auto db = asDb (v))     s.set (p, "wet", *db);
            else                        s.unparsed (p);
        }
        else if (w.size() == 1 && w[0] == "dry")
        {
            if (auto db = asDb (v))     s.set (p, "dry", *db);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "room") && (hasWord (w, "size") || w.size() == 1))
        {
            s.set (p, "room_size", asFraction (v, p.normalized));
        }
        else if (hasPrefixWord (w, "damp"))
        {
            s.set (p, "damping", asFraction (v, p.normalized));
        }
        else if (hasWord (w, "width") || hasWord (w, "stereo"))
        {
            s.set (p, "width", asFraction (v, p.normalized));
        }
        else if (text.find ("initial delay") != std::string::npos || text.find ("pre delay") != std::string::npos || text.find ("predelay") != std::string::npos)
        {
            if (auto ms = asMs (v))     s.set (p, "pre_delay", *ms);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "lowpass") || (hasWord (w, "low") && hasWord (w, "pass")))
        {
            if (auto hz = asHz (v))     s.set (p, "high_cut", *hz, "REAPER lowpass = Rovox high cut");
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "highpass") || (hasWord (w, "high") && hasWord (w, "pass")))
        {
            if (auto hz = asHz (v))     s.set (p, "low_cut", *hz, "REAPER highpass = Rovox low cut");
            else                        s.unparsed (p);
        }
        else
        {
            s.skip (p, "no equivalent in the Rovox Verb");
        }
    }
}

//------------------------------------------------------------------------------ ReaDelay
/** A beat length from "1/8", "1/4T", "1/16 dotted", "0.5 beats". */
std::optional<double> beatsOf (const std::string& formatted)
{
    const auto t = lowerOf (trim (formatted));
    const auto slash = t.find ('/');

    if (slash != std::string::npos)
    {
        const double num = std::atof (t.substr (0, slash).c_str()), den = std::atof (t.c_str() + slash + 1);

        if (num > 0.0 && den > 0.0)
        {
            double beats = 4.0 * num / den;
            const auto tail = t.substr (slash + 1);

            if (tail.find ('t') != std::string::npos)   beats *= 2.0 / 3.0;
            if (tail.find ('d') != std::string::npos || tail.find ('.') != std::string::npos)   beats *= 1.5;
            return beats;
        }
    }

    if (t.find ("beat") != std::string::npos)
    {
        const double v = std::atof (t.c_str());

        if (v > 0.0)
            return v;
    }

    return std::nullopt;
}

void translateDelay (const ReaperFxEntry& fx, Sink& s)
{
    struct Tap { bool seen = false, hasTime = false, hasBeats = false, hasEnabled = false; };
    std::array<Tap, 4> taps {};

    for (auto& p : fx.params)
    {
        const auto w = words (p.name);
        const auto v = parseReaperValue (p.formatted);
        int tap = 0;

        for (auto& x : w)
            if (x.size() == 1 && x[0] >= '1' && x[0] <= '9')
            {
                tap = x[0] - '0';
                break;
            }

        const bool isFeedback = hasWord (w, "feedback") || hasWord (w, "fb");
        const bool isLowpass = hasWord (w, "lowpass") || hasWord (w, "lp") || (hasWord (w, "low") && hasWord (w, "pass"));
        const bool isHighpass = hasWord (w, "highpass") || hasWord (w, "hp") || (hasWord (w, "high") && hasWord (w, "pass"));

        if (tap == 0 && w.size() == 1 && (w[0] == "wet" || w[0] == "dry"))
        {
            if (auto db = asDb (v))     s.set (p, w[0], *db);
            else                        s.unparsed (p);

            continue;
        }

        if (tap > 4)
        {
            s.skip (p, "the Rovox Delay has 4 taps");
            continue;
        }

        if (isFeedback)
        {
            if (tap > 1)
            {
                s.skip (p, "the Rovox Delay feeds back tap 1 only");
                continue;
            }

            if (v.ok && ! v.infinite && v.unit == "%")
                s.set (p, "feedback", v.value);
            else if (auto db = asDb (v); db && (v.infinite || v.unit == "db"))
                s.set (p, "feedback", 100.0 * dbToLin (*db), "from dB");
            else
                s.unparsed (p);

            continue;
        }

        if (isLowpass || isHighpass)
        {
            if (tap > 1)
            {
                s.skip (p, "the Rovox Delay filters the feedback path once");
                continue;
            }

            if (auto hz = asHz (v))     s.set (p, isLowpass ? "lowpass" : "highpass", *hz);
            else                        s.unparsed (p);

            continue;
        }

        if (tap == 0)
        {
            s.skip (p, "no equivalent in the Rovox Delay");
            continue;
        }

        auto& t = taps[(size_t) tap - 1];
        t.seen = true;
        const auto n = "t" + std::to_string (tap) + "_";

        if (hasWord (w, "enabled") || hasWord (w, "enable") || hasWord (w, "on"))
        {
            t.hasEnabled = true;
            s.set (p, n + "on", asFlag (p) ? 1.0 : 0.0);
        }
        else if (hasWord (w, "length") || hasWord (w, "time") || hasWord (w, "musical"))
        {
            if (auto beats = beatsOf (p.formatted); beats && (hasWord (w, "musical") || v.unit.empty() || v.unit == "oct"))
            {
                t.hasBeats = true;
                s.set (p, n + "beats", *beats);
            }
            else if (auto ms = asMs (v))
            {
                t.hasTime = true;
                s.set (p, n + "time", *ms);
            }
            else
            {
                s.unparsed (p);
            }
        }
        else if (hasWord (w, "volume") || hasWord (w, "gain") || hasWord (w, "level"))
        {
            if (auto db = asDb (v))     s.set (p, n + "gain", *db);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "pan"))
        {
            const auto text = lowerOf (trim (p.formatted));

            if (v.ok && ! v.infinite)
            {
                // "-25%", "25% L", "30% R" or a bare -1..1
                double pan = v.unit == "%" ? v.value / 100.0 : v.value;

                if (text.find ('l') != std::string::npos && pan > 0.0)
                    pan = -pan;

                s.set (p, n + "pan", std::clamp (pan, -1.0, 1.0));
            }
            else if (text == "center" || text == "c")
            {
                s.set (p, n + "pan", 0.0);
            }
            else
            {
                s.unparsed (p);
            }
        }
        else
        {
            s.skip (p, "no equivalent in the Rovox Delay");
        }
    }

    for (size_t i = 0; i < taps.size(); ++i)
    {
        const auto n = "t" + std::to_string (i + 1) + "_";

        if (taps[i].seen && ! taps[i].hasEnabled)
            s.out.translated.push_back ({ "(tap " + std::to_string (i + 1) + ")", "", n + "on", 1.0, "tap present in the REAPER delay" });
        else if (! taps[i].seen)
            s.out.translated.push_back ({ "(no tap " + std::to_string (i + 1) + ")", "", n + "on", 0.0, "not present in the REAPER delay" });

        if (taps[i].hasBeats && ! taps[i].hasTime)
            s.out.translated.push_back ({ "(tap " + std::to_string (i + 1) + " length)", "", n + "sync", 1.0, "length is musical" });
    }
}
} // namespace

FxTranslation translateReaperFx (const std::string& id, const ReaperFxEntry& fx)
{
    Sink s;

    if (id == "eq")             translateEq (fx, s);
    else if (id == "comp")      translateComp (fx, s);
    else if (id == "limit")     translateLimit (fx, s);
    else if (id == "gate")      translateGate (fx, s);
    else if (id == "verb")      translateVerb (fx, s);
    else if (id == "delay")     translateDelay (fx, s);

    return s.out;
}

} // namespace rovox
