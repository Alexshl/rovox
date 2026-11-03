// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "Fades.h"

#include <algorithm>
#include <cmath>

namespace rovox::fades
{

namespace
{
/** w(t, c) for c <= 0, a = -c. */
double warpNegative (double t, double a)
{
    if (a <= 0.5)
        return (1.0 - 2.0 * a) * t + 2.0 * a * t * t;

    const double b = 2.0 * a - 1.0, t2 = t * t;
    return (1.0 - b) * t2 + b * t2 * t2;
}

double shapeS (int shape, double u)
{
    if (shape == 5)
        return u < 0.5 ? 0.5 * (2.0 * u) * (2.0 * u) : 1.0 - 0.5 * (2.0 * (1.0 - u)) * (2.0 * (1.0 - u));

    if (shape == 6)
    {
        const double a = 2.0 * u, b = 2.0 * (1.0 - u);
        return u < 0.5 ? 0.5 * a * a * a * a : 1.0 - 0.5 * b * b * b * b;
    }

    return u;
}
} // namespace

FadeCurve normalise (int shape, double curve)
{
    FadeCurve f;
    f.curve = std::clamp (std::isfinite (curve) ? curve : 0.0, -1.0, 1.0);

    if (shape >= 1 && shape <= 4)
    {
        static const double preset[4] = { 0.5, -0.5, 1.0, -1.0 };

        if (f.curve == 0.0)
            f.curve = preset[shape - 1];

        f.shape = 0;
    }
    else
    {
        f.shape = shape == 5 || shape == 6 ? shape : 0;
    }

    return f;
}

FadeCurve fromProjectFields (double code, double amount, double flag, double explicitCurve, double explicitShape)
{
    if (flag < 0.5)
        return normalise (explicitShape >= 0.75 ? 6 : explicitShape >= 0.25 ? 5 : 0, explicitCurve);

    if (code >= 5.05)   return normalise (6, 0.0);
    if (code >= 4.95)   return normalise (5, 0.0);

    const int shape = (int) std::floor (code + 1.0e-6);
    const double k = amount == 0.0 ? 1.0 : std::min (1.0, std::abs (amount));

    switch (shape)
    {
        case 1:     return { 0, 0.5 };
        case 2:     return { 0, -0.5 * k };
        case 3:     return { 0, k };
        case 4:     return { 0, -k };
        default:    return {};
    }
}

double warp (double t, double c)
{
    t = std::clamp (t, 0.0, 1.0);
    c = std::clamp (c, -1.0, 1.0);
    return c <= 0.0 ? warpNegative (t, -c) : 1.0 - warpNegative (1.0 - t, c);
}

double gainAt (const FadeCurve& f, double t)
{
    return shapeS (f.shape, warp (t, f.curve));
}

bool isLinear (const FadeCurve& f)
{
    return f.shape == 0 && f.curve == 0.0;
}

} // namespace rovox::fades
