// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

// Item fade curves as REAPER draws them (shape 0..6 and a curve parameter -1..1). Pure functions, no engine dependency.
// A fade is a gain F(t) of the normalised position t = n / L in the fade (n = samples from the fade start, L = fade length in samples);
// a fade-in uses F(t), a fade-out F(1 - t) (an exact mirror). F = S_shape(warp(t, curve)).
#pragma once

namespace rovox::fades
{

struct FadeCurve
{
    int shape = 0;              // 0 linear-based (warped by `curve`), 5 and 6 two-part S curves; 1..4 are presets of shape 0 (see normalise)
    double curve = 0.0;         // -1 .. 1
    bool operator== (const FadeCurve&) const = default;
};

/** Shapes 1..4 are presets of shape 0 with the curve +0.5, -0.5, +1, -1 when the curve is 0. A non-zero explicit curve wins over the preset
    (the combination of a preset shape with a non-zero curve in a project file is not specified: the explicit curve is used). Out-of-range
    shapes become 0, the curve is clamped to -1 .. 1. */
FadeCurve normalise (int shape, double curve);

/** The curve warp w(t, c) for t in 0..1. */
double warp (double t, double curve);

/** Fade-in gain at t = n / L (0..1) for a normalised curve. */
double gainAt (const FadeCurve&, double t);

/** The curve of a FADEIN / FADEOUT line of a project file. Fields (0-based, after the keyword) as REAPER writes them: 0 legacy shape code, 5 legacy
    curve amount, 7 flag (0: the explicit fields below hold the curve), 8 explicit curve, 9 explicit shape (0: shape 0, 0.5: shape 5, 1: shape 6).
    Legacy codes: 0 linear, 1 curve +0.5 (REAPER's default fade), 2 curve -0.5, 3 curve +1, 4 curve -1 (the last three scaled by |field 5| when
    that is not 0), 5 shape 5, 5.1 shape 6. Missing fields count as 0, except the flag (1). */
FadeCurve fromProjectFields (double code, double amount, double flag, double explicitCurve, double explicitShape);

/** True when the curve is the plain linear ramp (shape 0, curve 0): the engine's own fade does that. */
bool isLinear (const FadeCurve&);

} // namespace rovox::fades
