// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// REAPER-compatible pan maths (header only, no engine dependency). Built from REAPER's public user guide (project settings "Pan mode" and
// "Pan law") and gain measurements of REAPER itself (tools/parity), not from REAPER's code.
//
// Pan law = the gain the centre gets, in dB (0 = REAPER default: the centre is not attenuated and the far side of a pan is simply faded
// out linearly, the near side stays at full level). A negative law lets the centre sit lower and the sides keep full level (constant-power
// family: gain = cos(theta)^n, n = -lawDb / 3.0103, theta = (pan + 1) * pi / 4; n = 1 is -3 dB, n = 2 is -6 dB which equals the linear -6 dB law).
//
// Pan modes (RPP `PANMODE`, REAPER's own numbers):
//   0  classic 3.x:               each channel is scaled by the mono pan gain of its side, the law applies to stereo too.
//   3  stereo balance / mono pan: the default. Stereo material is balanced (no law, the quieter side is faded, the other one is untouched);
//                                 mono material is panned with the law. For a mono source on a 0 dB law both are the same thing.
//   5  stereo pan:                the whole image is moved: left and right are panned as two mono sources whose positions follow the pan
//                                 knob (centre keeps them at -1 / +1, hard right puts both at +1 ...).
//   6  dual pan:                  left and right have their own positions (panLeft, panRight); (-1, +1) is a straight pass-through.
#pragma once

#include <algorithm>
#include <cmath>

namespace rovox::pan
{

enum class Mode { classic = 0, balance = 3, stereoPan = 5, dualPan = 6 };

inline constexpr int defaultReaperMode = 3;

/** REAPER's PANMODE number -> mode; anything unknown is the default (balance). */
inline Mode modeFromReaper (int m)
{
    switch (m)
    {
        case 0:  return Mode::classic;
        case 5:  return Mode::stereoPan;
        case 6:  return Mode::dualPan;
        default: return Mode::balance;
    }
}

/** REAPER's PANLAW is a linear gain (1 = 0 dB, 0.7071 = -3 dB). */
inline double lawDbFromReaper (double linear)
{
    return linear > 0.0 && linear < 1.0 ? 20.0 * std::log10 (linear) : 0.0;
}

/** Left / right gain of a mono source panned to `p` (-1 .. +1) under the law (in dB, <= 0). */
inline void monoGains (double p, double lawDb, double& left, double& right)
{
    p = std::clamp (p, -1.0, 1.0);

    if (lawDb > -0.01)
    {
        left  = std::min (1.0, 1.0 - p);
        right = std::min (1.0, 1.0 + p);
        return;
    }

    const double n = -lawDb / 3.010299956639812;
    const double theta = (p + 1.0) * 0.78539816339744831;      // pi / 4
    left  = std::pow (std::max (0.0, std::cos (theta)), n);
    right = std::pow (std::max (0.0, std::sin (theta)), n);
}

/** out.left = ll * in.left + lr * in.right; out.right = rl * in.left + rr * in.right */
struct Matrix { double ll = 1, lr = 0, rl = 0, rr = 1; };

/** `p` = the pan knob (for dual pan: the left position), `pRight` = right position (dual pan only), `monoSource` = the track's material is mono. */
inline Matrix matrix (Mode mode, double p, double pRight, double lawDb, bool monoSource)
{
    Matrix m;
    p = std::clamp (p, -1.0, 1.0);

    auto dual = [&] (double posL, double posR)
    {
        double gll, glr, grl, grr;
        monoGains (posL, lawDb, gll, glr);
        monoGains (posR, lawDb, grl, grr);
        m = { gll, grl, glr, grr };
    };

    switch (mode)
    {
        case Mode::classic:
        {
            double l, r;
            monoGains (p, lawDb, l, r);
            m = { l, 0, 0, r };
            break;
        }
        case Mode::balance:
        {
            double l, r;
            monoGains (p, monoSource ? lawDb : 0.0, l, r);
            m = { l, 0, 0, r };
            break;
        }
        case Mode::stereoPan:
            dual (p > 0.0 ? -1.0 + 2.0 * p : -1.0, p < 0.0 ? 1.0 + 2.0 * p : 1.0);
            break;
        case Mode::dualPan:
            dual (p, std::clamp (pRight, -1.0, 1.0));
            break;
    }

    return m;
}

} // namespace rovox::pan
