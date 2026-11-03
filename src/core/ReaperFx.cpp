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

struct Sink
{
    FxTranslation out;
    double overallWet = 1.0;            // the FX-wide Wet (0..1) of the trailing Bypass / Wet / Delta block
    bool overallWetApplied = false;     // the translator folded it into its own mix parameter

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

/** ReaEQ gain knob: amplitude 2n up to n = 0.5, then 1 + 6 (n - 0.5) (-inf .. +12.04 dB). */
double reaEqGainDb (double n)
{
    const double amp = n <= 0.5 ? 2.0 * n : 1.0 + 6.0 * (n - 0.5);
    return amp <= 1.0e-7 ? minusInfDb : 20.0 * std::log10 (amp);
}

/** The value implied by the normalized parameter when it agrees with the displayed text (which is rounded) within `tolerance`, else the text's value. */
double exactOrText (double fromText, double fromNormalized, double tolerance)
{
    return std::abs (fromText - fromNormalized) <= tolerance ? fromNormalized : fromText;
}

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

        // band type: BANDTYPE<n> (ReaEQ's own code, which tells the three "Band" kinds apart) wins, the band's name serves as the fallback.
        // ReaEQ's BANDTYPE: 0 low shelf, 1 high shelf, 2 band (alt 2), 3 low pass, 4 high pass, 5 all pass, 6 notch, 7 band pass, 8 band,
        // 9 band (alt), 10 parallel band pass. The Rovox type indices: StockPlugins.cpp (RovoxEq, "type" choices).
        int type = -1, cfgType = -1;
        std::string typeNote;
        const auto has = [&] (const char* x) { return b.key.find (x) != std::string::npos; };

        if (has ("parallel"))                                   type = 9;
        else if (has ("low shelf") || has ("lowshelf"))         type = 0;
        else if (has ("high shelf") || has ("highshelf"))       type = 1;
        else if (has ("low pass") || has ("lowpass"))           type = 3;
        else if (has ("high pass") || has ("highpass"))         type = 4;
        else if (has ("notch"))                                 type = 5;
        else if (has ("band pass") || has ("bandpass"))         type = 6;
        else if (has ("all pass") || has ("allpass"))           type = 7;
        else if (has ("band") || has ("peak"))                  type = 2;

        const auto cfgKey = "BANDTYPE" + idx;

        if (auto it = fx.config.find (cfgKey); it != fx.config.end())
        {
            static const int map[] = { 0, 1, 8, 3, 4, 7, 5, 6, 2, 2, 9 };
            const int reaType = std::atoi (it->second.c_str());

            if (reaType >= 0 && reaType < 11)
                cfgType = map[reaType];

            if (cfgType >= 0)
            {
                if (type >= 0 && type != cfgType && ! (type == 2 && (cfgType == 8 || cfgType == 2)))
                    typeNote += "; " + cfgKey + "=" + it->second + " disagrees with the band name, the code is used";

                type = cfgType;
            }
        }

        if (type < 0)
            type = 2;

        static const char* const typeNames[] = { "low_shelf", "high_shelf", "peak", "low_pass", "high_pass", "notch", "band_pass", "all_pass", "peak_alt2", "parallel_band_pass" };
        s.out.translated.push_back ({ "band type (" + b.key + ")", fx.config.count (cfgKey) ? cfgKey + "=" + fx.config.at (cfgKey) : "from the band name",
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
            if (auto db = asDb (parseReaperValue (b.gain->formatted)))      s.set (*b.gain, n + "gain", exactOrText (*db, reaEqGainDb (b.gain->normalized), 0.12), "exact value of the knob");
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
                s.set (*bwp, n + "bw", exactOrText (v.value, 4.0 * bwp->normalized, 0.011), "ReaEQ bandwidth = 4 n octaves");
        }
    }

    // bands the REAPER instance does not have are off
    for (size_t i = bands.size(); i < maxBands; ++i)
        s.out.translated.push_back ({ "(no band " + std::to_string (i + 1) + ")", "", "b" + std::to_string (i + 1) + "_on", 0.0, "not present in the REAPER EQ" });

    if (outGain != nullptr)
    {
        if (auto db = asDb (parseReaperValue (outGain->formatted)))     s.set (*outGain, "out_gain", exactOrText (*db, reaEqGainDb (outGain->normalized), 0.12), "exact value of the knob");
        else                                                            s.unparsed (*outGain);
    }

    // ReaEQ's filter design (shelf slope, bandwidth alpha, band types) instead of the Rovox one
    s.out.translated.push_back ({ "(ReaEQ)", "", "reaper_compat", 1.0, "filters designed as ReaEQ does" });
}

//------------------------------------------------------------------------------ ReaComp
/** ReaComp / ReaGate / ReaDelay amplitude knobs (Dry, Wet, Threshold, Hysteresis, ...): amplitude = 2 n, in dB (-inf at 0). */
double ampKnobDb (double n)
{
    return n <= 1.0e-7 ? minusInfDb : 20.0 * std::log10 (2.0 * n);
}

/** A knob that is linear in n over 0 .. `range`: the exact value when it agrees with the displayed text (3 significant digits), else the text. */
double linearKnob (double fromText, double normalized, double range)
{
    return exactOrText (fromText, range * normalized, std::max (0.06, 0.006 * std::abs (fromText)));
}

/** A parameter without a Rovox counterpart: listed unless it is at its neutral value (detector filters open, flags off). */
void leave (Sink& s, const ReaperFxParam& p, const std::string& reason, bool neutral)
{
    if (! neutral)
        s.skip (p, reason);
}

bool zeroish (const ReaperFxParam& p)
{
    const auto v = parseReaperValue (p.formatted);

    if (v.ok && ! v.infinite)
        return std::abs (v.value) < 1.0e-9;

    const auto t = lowerOf (trim (p.formatted));
    return t == "off" || t == "disabled" || t == "normal" || t == "none";
}

bool filterOpen (const ReaperFxParam& p, bool lowpass)
{
    const auto hz = asHz (parseReaperValue (p.formatted));
    return hz && (lowpass ? *hz >= 20000.0 : *hz <= 20.0);
}

bool isMinusInf (const ReaperFxParam& p)
{
    const auto v = parseReaperValue (p.formatted);
    return v.ok && v.infinite && v.value < 0;
}

void translateComp (const ReaperFxEntry& fx, Sink& s)
{
    // units of the plain numbers ReaComp shows: threshold / knee / dry / wet in dB, attack / release / pre-comp / RMS size in ms, ratio as N:1,
    // detector filters in Hz; the exact values follow from the normalized knob positions (spec-common.md)
    for (auto& p : fx.params)
    {
        const auto k = joinWords (words (p.name));
        const auto v = parseReaperValue (p.formatted);
        const double n = p.normalized;

        if (k == "threshold" || k == "thresh")
        {
            if (auto db = asDb (v))     s.set (p, "threshold", exactOrText (*db, ampKnobDb (n), 0.12), "amplitude 2 n");
            else                        s.unparsed (p);
        }
        else if (k == "ratio")
        {
            if (v.ok && v.infinite)
                s.set (p, "ratio", 100.0, "infinite ratio");
            else if (v.ok && v.value >= 1.0 && (v.unit == ":1" || v.unit.empty()))
                s.set (p, "ratio", exactOrText (v.value, 1.0 + 99.0 * n, 0.01 * v.value));
            else
                s.unparsed (p);
        }
        else if (k == "attack")
        {
            if (auto ms = asMs (v))     s.set (p, "attack", linearKnob (*ms, n, 500.0));
            else                        s.unparsed (p);
        }
        else if (k == "release")
        {
            if (auto ms = asMs (v))     s.set (p, "release", linearKnob (*ms, n, 5000.0));
            else                        s.unparsed (p);
        }
        else if (k == "knee" || k == "knee size")
        {
            if (auto db = asDb (v))     s.set (p, "knee", linearKnob (*db, n, 24.0));
            else                        s.unparsed (p);
        }
        else if (k == "pre comp" || k == "precomp")
        {
            if (auto ms = asMs (v))     s.set (p, "lookahead", linearKnob (*ms, n, 250.0), "pre-comp");
            else                        s.unparsed (p);
        }
        else if (k == "rms size")
        {
            if (auto ms = asMs (v))     s.set (p, "rms", linearKnob (*ms, n, 1000.0));
            else                        s.unparsed (p);
        }
        else if (k == "auto make up gain")
        {
            s.set (p, "auto_makeup", asFlag (p) ? 1.0 : 0.0);
        }
        else if (k == "wet" || k == "dry")
        {
            if (auto db = asDb (v))     s.set (p, k, exactOrText (*db, ampKnobDb (n), 0.12), "amplitude 2 n");
            else                        s.unparsed (p);
        }
        else if (k == "lowpass" || k == "hipass")
        {
            if (auto hz = asHz (v))     s.set (p, k, exactOrText (*hz, 20000.0 * n, 1.0), "detector filter, 12 dB/oct");
            else                        s.unparsed (p);
        }
        else if (k == "legacy attack knee options")
        {
            if (v.ok && ! v.infinite)   s.set (p, "legacy_knee", exactOrText (v.value, n, 0.006), "0.25 is the default knee");
            else                        s.unparsed (p);
        }
        else if (k == "auto release")               leave (s, p, "auto release (program-dependent release) is not modelled", zeroish (p));
        else if (k == "signin" || k == "audin")     leave (s, p, "the external side-chain input has no equivalent in the Rovox Comp", zeroish (p));
        else if (k == "filter preview")             leave (s, p, "the detector filter preview has no equivalent in the Rovox Comp", zeroish (p));
        else if (k == "multichannel mode")          leave (s, p, "multichannel mode has no equivalent in the Rovox Comp", zeroish (p));
        else if (k == "deprecated broken anti alias")   leave (s, p, "legacy anti-alias mode has no equivalent in the Rovox Comp", zeroish (p));
        else if (k == "resvd" || k == "metering index")
            continue;       // reserved / metering slots, no setting
        else
            s.skip (p, "no equivalent in the Rovox Comp");
    }

    // ReaComp's behaviour (detector, envelope, knee, make-up, dry / wet) instead of the Rovox compressor
    s.out.translated.push_back ({ "(ReaComp)", "", "reaper_compat", 1.0, "detector, envelope and knee as ReaComp does" });
}

//------------------------------------------------------------------------------ ReaLimit
void translateLimit (const ReaperFxEntry& fx, Sink& s)
{
    for (auto& p : fx.params)
    {
        const auto w = words (p.name);
        const auto v = parseReaperValue (p.formatted);
        const double n = p.normalized;

        if (hasPrefixWord (w, "thresh"))
        {
            if (auto db = asDb (v))     s.set (p, "threshold", exactOrText (*db, -60.0 + 72.0 * n, 0.011), "-60 + 72 n");
            else                        s.unparsed (p);
        }
        else if (hasPrefixWord (w, "ceil"))
        {
            if (auto db = asDb (v))     s.set (p, "ceiling", exactOrText (*db, -24.0 + 24.0 * n, 0.011), "-24 + 24 n");
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "release"))
        {
            // the number is the rate of the slow stage in dB per second (6 - 20 log10 n); "inf" is the top of the knob: 150 dB/s (measured)
            if (v.ok && v.infinite)
                s.set (p, "release_rate", 150.0, "inf is 150 dB per second");
            else if (v.ok && v.value > 0.0)
                s.set (p, "release_rate", n > 0.0 ? exactOrText (v.value, 6.0 - 20.0 * std::log10 (n), 0.06 + 0.006 * v.value) : v.value, "dB per second");
            else
                s.unparsed (p);
        }
        else
        {
            s.skip (p, "no equivalent in the Rovox Limit");
        }
    }

    // ReaLimit's behaviour (threshold boost, sample-peak limiting, two-stage release) instead of the Rovox limiter
    s.out.translated.push_back ({ "(ReaLimit)", "", "reaper_compat", 1.0, "limiting as ReaLimit does" });
}

//------------------------------------------------------------------------------ ReaGate
void translateGate (const ReaperFxEntry& fx, Sink& s)
{
    bool sendMidi = false;

    for (auto& p : fx.params)
        if (joinWords (words (p.name)) == "send midi")
            sendMidi = ! zeroish (p);

    // plain numbers: threshold / hysteresis / dry / wet in dB, attack / release / hold / pre-open / RMS size in ms. ReaGate shows its times as whole
    // milliseconds and acts on the displayed value (a knob at 99.5 ms behaves as 100 ms, measured), so the time is the text, not the knob position.
    for (auto& p : fx.params)
    {
        const auto k = joinWords (words (p.name));
        const auto v = parseReaperValue (p.formatted);
        const double n = p.normalized;

        if (k == "threshold" || k == "thresh")
        {
            if (auto db = asDb (v))     s.set (p, "threshold", exactOrText (*db, ampKnobDb (n), 0.12), "amplitude 2 n");
            else                        s.unparsed (p);
        }
        else if (k == "attack")
        {
            if (auto ms = asMs (v))     s.set (p, "attack", *ms);
            else                        s.unparsed (p);
        }
        else if (k == "hold")
        {
            if (auto ms = asMs (v))     s.set (p, "hold", *ms);
            else                        s.unparsed (p);
        }
        else if (k == "release")
        {
            if (auto ms = asMs (v))     s.set (p, "release", *ms);
            else                        s.unparsed (p);
        }
        else if (k == "hysteresis")
        {
            if (auto db = asDb (v))     s.set (p, "rea_hysteresis", exactOrText (*db, ampKnobDb (n), 0.12), "amplitude 2 n");
            else                        s.unparsed (p);
        }
        else if (k == "dry" || k == "wet")
        {
            if (auto db = asDb (v))     s.set (p, k, exactOrText (*db, ampKnobDb (n), 0.12), "amplitude 2 n");
            else                        s.unparsed (p);
        }
        else if (k == "pre open")
        {
            if (auto ms = asMs (v))     s.set (p, "pre_open", *ms);
            else                        s.unparsed (p);
        }
        else if (k == "rms size")
        {
            if (auto ms = asMs (v))     s.set (p, "rms", *ms);
            else                        s.unparsed (p);
        }
        else if (k == "lowpass" || k == "hipass")
        {
            if (auto hz = asHz (v))     s.set (p, k, exactOrText (*hz, 20000.0 * n, 1.0), "detector filter, 12 dB/oct");
            else                        s.unparsed (p);
        }
        else if (k == "invert wet")
        {
            const auto t = lowerOf (trim (p.formatted));
            const bool on = t.rfind ("enabled", 0) == 0;
            s.set (p, "invert_wet", on ? 1.0 : 0.0, t.find ("legacy") != std::string::npos ? "legacy mode treated as the current one (not measured)" : "");
        }
        else if (k == "signin" || k == "audin")     leave (s, p, "the external side-chain input has no equivalent in the Rovox Gate", zeroish (p));
        else if (k == "noise level")                leave (s, p, "the noise level (added noise while open) is not modelled", isMinusInf (p));
        else if (k == "preview filter")             leave (s, p, "the detector filter preview has no equivalent in the Rovox Gate", zeroish (p));
        else if (k == "send midi")                  leave (s, p, "MIDI output has no equivalent in the Rovox Gate", zeroish (p));
        else if (k == "midi note" || k == "midi channel")   leave (s, p, "MIDI output has no equivalent in the Rovox Gate", ! sendMidi);
        else if (k == "multichannel mode")          leave (s, p, "multichannel mode has no equivalent in the Rovox Gate", zeroish (p));
        else if (k == "metering index")             continue;
        else
            s.skip (p, "no equivalent in the Rovox Gate");
    }

    // ReaGate's behaviour (detector, gain trajectory, pre-open) instead of the Rovox gate
    s.out.translated.push_back ({ "(ReaGate)", "", "reaper_compat", 1.0, "gain trajectory as ReaGate does" });
}

//------------------------------------------------------------------------------ ReaVerbate
/** Room size / dampening show 0..100 without a unit, width 0..1 ("1.00"); "%" in the text always means percent. */
double verbFraction (const ParsedValue& v, double normalized, bool percentWhenPlain)
{
    if (! v.ok || v.infinite)
        return std::clamp (normalized, 0.0, 1.0);

    return std::clamp (v.unit == "%" || (v.unit.empty() && percentWhenPlain) ? v.value / 100.0 : v.value, 0.0, 1.0);
}

void translateVerb (const ReaperFxEntry& fx, Sink& s)
{
    for (auto& p : fx.params)
    {
        const auto k = joinWords (words (p.name));
        const auto v = parseReaperValue (p.formatted);

        if (k == "wet")
        {
            if (auto db = asDb (v))     s.set (p, "wet", *db);
            else                        s.unparsed (p);

            s.set (p, "rea_wet", std::clamp (p.normalized, 0.0, 1.0), "knob position, amplitude 2 n");
        }
        else if (k == "dry")
        {
            if (auto db = asDb (v))     s.set (p, "dry", *db);
            else                        s.unparsed (p);

            s.set (p, "rea_dry", std::clamp (p.normalized, 0.0, 1.0), "knob position, amplitude 2 n");
        }
        else if (k == "room size" || k == "room")
        {
            s.set (p, "room_size", verbFraction (v, p.normalized, true));
            s.set (p, "rea_room", std::clamp (p.normalized, 0.0, 1.0), "knob position, feedback 0.3 + 0.68 n");
        }
        else if (k == "dampening" || k == "damping")
        {
            s.set (p, "damping", verbFraction (v, p.normalized, true));
            s.set (p, "rea_damp", std::clamp (p.normalized, 0.0, 1.0), "knob position, damping 0.4 n");
        }
        else if (k == "width" || k == "stereo width")
        {
            s.set (p, "width", verbFraction (v, p.normalized, false));
            s.set (p, "rea_width", std::clamp (p.normalized, 0.0, 1.0), "knob position, width 2 n - 1");
        }
        else if (k == "delay" || k == "initial delay" || k == "pre delay" || k == "predelay")
        {
            if (auto ms = asMs (v))     s.set (p, "pre_delay", *ms);
            else                        s.unparsed (p);

            s.set (p, "rea_delay", std::clamp (p.normalized, 0.0, 1.0), "knob position, 500 ms n, whole samples");
        }
        else if (k == "lowpass")
        {
            if (auto hz = asHz (v))     s.set (p, "high_cut", *hz, "REAPER lowpass = Rovox high cut");
            else                        s.unparsed (p);

            s.set (p, "rea_lp", std::clamp (p.normalized, 0.0, 1.0), "knob position, 20 kHz n");
        }
        else if (k == "hipass" || k == "highpass")
        {
            if (auto hz = asHz (v))     s.set (p, "low_cut", *hz, "REAPER highpass = Rovox low cut");
            else                        s.unparsed (p);

            s.set (p, "rea_hp", std::clamp (p.normalized, 0.0, 1.0), "knob position, 20 kHz n");
        }
        else
        {
            s.skip (p, "no equivalent in the Rovox Verb");
        }
    }

    // ReaVerbate's own reverberator (10 combs, 6 allpasses, per-channel input) instead of the Rovox reverb
    s.out.translated.push_back ({ "(ReaVerbate)", "", "reaper_compat", 1.0, "reverberator as ReaVerbate does" });
}

//------------------------------------------------------------------------------ ReaDelay
/** ReaDelay's amplitude knobs (Wet, Dry, Feedback, Volume): amplitude = 2 n, in dB (-inf at 0). */
double reaDelayGainDb (double n)
{
    return ampKnobDb (n);
}

void translateDelay (const ReaperFxEntry& fx, Sink& s)
{
    struct Tap { bool seen = false, hasEnabled = false; };
    std::array<Tap, 4> taps {};

    for (auto& p : fx.params)
    {
        const auto w = words (p.name);
        const auto v = parseReaperValue (p.formatted);
        const double n = p.normalized;
        int tap = 0;

        for (auto& x : w)
            if (x.size() == 1 && x[0] >= '1' && x[0] <= '9')
            {
                tap = x[0] - '0';
                break;
            }

        const bool isFeedback = hasWord (w, "feedback") || hasWord (w, "fb");
        const bool isLowpass = hasWord (w, "lowpass") || hasWord (w, "lp") || (hasWord (w, "low") && hasWord (w, "pass"));
        const bool isHighpass = hasWord (w, "highpass") || hasWord (w, "hipass") || hasWord (w, "hp") || (hasWord (w, "high") && hasWord (w, "pass"));

        if (tap == 0 && w.size() == 1 && (w[0] == "wet" || w[0] == "dry"))
        {
            if (auto db = asDb (v))     s.set (p, w[0], exactOrText (*db, reaDelayGainDb (n), 0.12), "amplitude 2 n");
            else                        s.unparsed (p);

            continue;
        }

        if (tap > 4)
        {
            s.skip (p, "the Rovox Delay has 4 taps");
            continue;
        }

        if (tap == 0)
        {
            s.skip (p, "no equivalent in the Rovox Delay");
            continue;
        }

        auto& t = taps[(size_t) tap - 1];
        t.seen = true;
        const auto id = "t" + std::to_string (tap) + "_";

        if (isFeedback)
        {
            if (auto db = asDb (v))     s.set (p, id + "fb", exactOrText (*db, reaDelayGainDb (n), 0.12), "amplitude 2 n");
            else                        s.unparsed (p);
        }
        else if (isLowpass || isHighpass)
        {
            if (auto hz = asHz (v))     s.set (p, id + (isLowpass ? "lp" : "hp"), exactOrText (*hz, 20000.0 * n, 1.0));
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "enabled") || hasWord (w, "enable") || hasWord (w, "on"))
        {
            t.hasEnabled = true;
            s.set (p, id + "on", asFlag (p) ? 1.0 : 0.0);
        }
        else if (hasWord (w, "musical"))
        {
            // in 1/8 notes (ReaDelay shows the plain number)
            if (v.ok && ! v.infinite)   s.set (p, id + "musical", exactOrText (v.value, 256.0 * n, 0.006), "1/8 notes");
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "length") || hasWord (w, "time"))
        {
            if (auto ms = asMs (v))     s.set (p, id + "time", exactOrText (*ms, 10000.0 * n, 0.06));
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "resolution"))
        {
            if (v.ok && ! v.infinite)   s.set (p, id + "bits", v.value);
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "width"))
        {
            if (v.ok && ! v.infinite)   s.set (p, id + "width", exactOrText (v.value, 2.0 * n - 1.0, 0.011));
            else                        s.unparsed (p);
        }
        else if (hasWord (w, "volume") || hasWord (w, "gain") || hasWord (w, "level"))
        {
            if (auto db = asDb (v))     s.set (p, id + "gain", exactOrText (*db, reaDelayGainDb (n), 0.12), "amplitude 2 n");
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

                s.set (p, id + "pan", exactOrText (std::clamp (pan, -1.0, 1.0), 2.0 * n - 1.0, 0.0011));
            }
            else if (text == "center" || text == "c")
            {
                s.set (p, id + "pan", 0.0);
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
        const auto id = "t" + std::to_string (i + 1) + "_";

        if (taps[i].seen && ! taps[i].hasEnabled)
            s.out.translated.push_back ({ "(tap " + std::to_string (i + 1) + ")", "", id + "on", 1.0, "tap present in the REAPER delay" });
        else if (! taps[i].seen)
            s.out.translated.push_back ({ "(no tap " + std::to_string (i + 1) + ")", "", id + "on", 0.0, "not present in the REAPER delay" });
    }

    // ReaDelay's behaviour (a loop per tap, whole-sample delay, 12 dB/oct loop filters) instead of the Rovox delay
    s.out.translated.push_back ({ "(ReaDelay)", "", "reaper_compat", 1.0, "tap loops as ReaDelay does" });
}
} // namespace

FxTranslation translateReaperFx (const std::string& id, const ReaperFxEntry& entry)
{
    Sink s;
    auto fx = entry;

    // every REAPER FX ends with the generic Bypass / Wet / Delta parameters; their "Wet" is not the effect's own "Wet" (dB)
    const ReaperFxParam* bypass = nullptr;
    const ReaperFxParam* genericWet = nullptr;
    const ReaperFxParam* delta = nullptr;
    std::vector<ReaperFxParam> generic;

    if (fx.params.size() >= 3)
    {
        const auto n = fx.params.size();

        if (lowerOf (trim (fx.params[n - 3].name)) == "bypass" && lowerOf (trim (fx.params[n - 2].name)) == "wet" && lowerOf (trim (fx.params[n - 1].name)) == "delta")
        {
            generic.assign (fx.params.end() - 3, fx.params.end());
            fx.params.resize (n - 3);
            bypass = &generic[0];
            genericWet = &generic[1];
            delta = &generic[2];
        }
    }

    if (bypass != nullptr)
    {
        const auto t = lowerOf (trim (bypass->formatted));
        s.out.bypassed = t == "bypassed" || t == "bypass" || (t != "normal" && bypass->normalized >= 0.5);
    }

    if (genericWet != nullptr)
    {
        const auto v = parseReaperValue (genericWet->formatted);
        s.overallWet = std::clamp (v.ok && ! v.infinite ? v.value / 100.0 : genericWet->normalized, 0.0, 1.0);
    }

    if (id == "eq")             translateEq (fx, s);
    else if (id == "comp")      translateComp (fx, s);
    else if (id == "limit")     translateLimit (fx, s);
    else if (id == "gate")      translateGate (fx, s);
    else if (id == "verb")      translateVerb (fx, s);
    else if (id == "delay")     translateDelay (fx, s);

    // the Rovox EQ, dynamics and Delay carry the generic Wet (linear crossfade) and Delta (processed - input, the Wet is ignored) themselves
    const bool hasMix = id == "eq" || id == "delay" || id == "comp" || id == "gate" || id == "limit" || id == "verb";
    const bool deltaOn = delta != nullptr && lowerOf (trim (delta->formatted)) != "normal" && delta->normalized >= 0.5;

    if (hasMix && genericWet != nullptr)
    {
        s.out.translated.push_back ({ genericWet->name, genericWet->formatted, "fx_wet", s.overallWet * 100.0, "linear crossfade with the input" });
        s.overallWetApplied = true;
    }

    if (hasMix && deltaOn)
        s.out.translated.push_back ({ delta->name, delta->formatted, "fx_delta", 1.0, "processed minus input" });

    if (genericWet != nullptr && s.overallWet < 1.0 - 1.0e-6 && ! s.overallWetApplied)
        s.skip (*genericWet, "the FX-wide wet mix other than 100 % has no equivalent in this Rovox effect");

    if (deltaOn && ! hasMix)
        s.skip (*delta, "delta (difference) monitoring has no equivalent in Rovox");

    return s.out;
}

std::string repairUtf8 (const std::string& in)
{
    std::string out;
    out.reserve (in.size());
    const auto n = in.size();

    for (size_t i = 0; i < n;)
    {
        const auto c = (unsigned char) in[i];
        size_t len = c < 0x80 ? 1 : (c >= 0xC2 && c <= 0xDF) ? 2 : (c >= 0xE0 && c <= 0xEF) ? 3 : (c >= 0xF0 && c <= 0xF4) ? 4 : 0;
        bool ok = len > 0 && i + len <= n;

        for (size_t k = 1; ok && k < len; ++k)
            ok = ((unsigned char) in[i + k] & 0xC0) == 0x80;

        if (ok && len > 1)
        {
            const auto c2 = (unsigned char) in[i + 1];

            if ((c == 0xE0 && c2 < 0xA0) || (c == 0xED && c2 > 0x9F) || (c == 0xF0 && c2 < 0x90) || (c == 0xF4 && c2 > 0x8F))
                ok = false;
        }

        if (ok)
        {
            out.append (in, i, len);
            i += len;
        }
        else
        {
            // not UTF-8: the byte is Latin-1 (REAPER hands out such text from some JS effects)
            out += (char) (0xC0 | (c >> 6));
            out += (char) (0x80 | (c & 0x3F));
            ++i;
        }
    }

    return out;
}

} // namespace rovox
