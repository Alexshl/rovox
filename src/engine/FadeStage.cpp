// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "FadeStage.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace rovox
{

namespace
{
constexpr const char* fadeStageType = "rovoxfades";

const juce::Identifier lenIn ("rovox:fadeInLen"), shapeIn ("rovox:fadeInShape"), curveIn ("rovox:fadeInCurve");
const juce::Identifier lenOut ("rovox:fadeOutLen"), shapeOut ("rovox:fadeOutShape"), curveOut ("rovox:fadeOutCurve");
const juce::Identifier stretchToneId ("rovox:stretchTone");

/** Direct form II transposed biquad (RBJ shelves), double precision. */
struct Biquad
{
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1[2] = {}, z2[2] = {};

    void shelf (double sr, double f0, double gainDb, bool high)
    {
        const double A = std::pow (10.0, gainDb / 40.0), w = 2.0 * 3.14159265358979323846 * f0 / sr, c = std::cos (w), s = std::sin (w);
        const double sq = 2.0 * std::sqrt (A) * (s / 2.0 * std::sqrt (2.0));         // slope S = 1
        double nb0, nb1, nb2, na0, na1, na2;

        if (high)
        {
            nb0 = A * ((A + 1) + (A - 1) * c + sq);  nb1 = -2 * A * ((A - 1) + (A + 1) * c);  nb2 = A * ((A + 1) + (A - 1) * c - sq);
            na0 = (A + 1) - (A - 1) * c + sq;        na1 = 2 * ((A - 1) - (A + 1) * c);       na2 = (A + 1) - (A - 1) * c - sq;
        }
        else
        {
            nb0 = A * ((A + 1) - (A - 1) * c + sq);  nb1 = 2 * A * ((A - 1) - (A + 1) * c);   nb2 = A * ((A + 1) - (A - 1) * c - sq);
            na0 = (A + 1) + (A - 1) * c + sq;        na1 = -2 * ((A - 1) + (A + 1) * c);      na2 = (A + 1) + (A - 1) * c - sq;
        }

        b0 = nb0 / na0;  b1 = nb1 / na0;  b2 = nb2 / na0;  a1 = na1 / na0;  a2 = na2 / na0;
    }

    double process (int ch, double x)
    {
        const double y = b0 * x + z1[ch];
        z1[ch] = b1 * x - a1 * y + z2[ch];
        z2[ch] = b2 * x - a2 * y;
        return y;
    }

    void reset()    { z1[0] = z1[1] = z2[0] = z2[1] = 0.0; }
};

struct Region
{
    double start = 0.0, end = 0.0, inLen = 0.0, outLen = 0.0;
    bool tone = false;
    fades::FadeCurve inCurve, outCurve;
};

using Regions = std::vector<Region>;

class RovoxItemFades : public te::Plugin
{
public:
    static inline const char* xmlTypeName = fadeStageType;

    explicit RovoxItemFades (te::PluginCreationInfo info) : te::Plugin (info), watcher_ (*this) {}

    ~RovoxItemFades() override
    {
        notifyListenersOfDeletion();
        detach();
    }

    juce::String getName() const override                   { return "Rovox Item Fades"; }
    juce::String getPluginType() override                   { return xmlTypeName; }
    juce::String getVendor() override                       { return "Rovox"; }
    juce::String getShortName (int) override                { return getName(); }
    juce::String getSelectableDescription() override        { return getName(); }
    int getNumOutputChannelsGivenInputs (int numInputChannels) override { return numInputChannels; }
    BusLayout getBusses() const override                    { return BusLayout::singleStereoInOut(); }

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        sampleRate_ = info.sampleRate;
        // REAPER stretch tone (fitted on one song at one rate): a gentle low lift and high cut, see FadeStage.h
        lowShelf_.shelf (sampleRate_, 150.0, 0.80, false);
        highShelf_.shelf (sampleRate_, 15500.0, -2.05, true);
        reset();
        detach();

        if (auto* track = getOwnerTrack())
        {
            owner_ = track->state;
            owner_.addListener (&watcher_);
        }

        rebuild();
    }

    void deinitialise() override                            { detach(); }
    void reset() override                                   { lowShelf_.reset(); highShelf_.reset(); }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (fc.destBuffer == nullptr || fc.bufferNumSamples <= 0)
            return;

        std::shared_ptr<const Regions> regions;
        {
            const juce::SpinLock::ScopedLockType lock (lock_);
            regions = regions_;
        }

        if (regions == nullptr || regions->empty())
            return;

        const double sr = sampleRate_;
        const auto n = (long long) fc.bufferNumSamples;
        const auto block = (long long) std::llround (fc.editTime.getStart().inSeconds() * sr);

        applyTone (fc, *regions, block, n);

        for (auto& r : *regions)
        {
            if (r.inLen > 0.0)
            {
                const auto from = (long long) std::llround (r.start * sr), len = (long long) std::llround (r.inLen * sr);
                apply (fc, block, n, from, len, r.inCurve, false);
            }

            if (r.outLen > 0.0)
            {
                const auto len = (long long) std::llround (r.outLen * sr), from = (long long) std::llround (r.end * sr) - len;
                apply (fc, block, n, from, len, r.outCurve, true);
            }
        }
    }

private:
    /** The stretch-tone filter inside the flagged items; it runs on the whole signal so its state is continuous across the item edges. */
    void applyTone (const te::PluginRenderContext& fc, const Regions& regions, long long block, long long n)
    {
        bool any = false;

        for (auto& r : regions)
            any = any || r.tone;

        if (! any)
            return;

        const int channels = std::min (2, fc.destBuffer->getNumChannels());

        for (long long i = 0; i < n; ++i)
        {
            bool inside = false;

            for (auto& r : regions)
                inside = inside || (r.tone && block + i >= (long long) std::llround (r.start * sampleRate_) && block + i < (long long) std::llround (r.end * sampleRate_));

            for (int c = 0; c < channels; ++c)
            {
                float& x = fc.destBuffer->getWritePointer (c, fc.bufferStartSample)[i];
                const double y = highShelf_.process (c, lowShelf_.process (c, (double) x));

                if (inside)
                    x = (float) y;
            }
        }
    }

    /** Gain F(k / L) (fade-in) or F(1 - k / L) (fade-out) for the samples k = 0 .. L-1 counted from the fade start. */
    static void apply (const te::PluginRenderContext& fc, long long block, long long n, long long from, long long len, const fades::FadeCurve& curve, bool mirror)
    {
        if (len <= 0)
            return;

        const long long i0 = std::max (0LL, from - block), i1 = std::min (n, from + len - block);

        for (long long i = i0; i < i1; ++i)
        {
            const double t = (double) (block + i - from) / (double) len;
            const float g = (float) fades::gainAt (curve, mirror ? 1.0 - t : t);

            for (int c = 0; c < fc.destBuffer->getNumChannels(); ++c)
                fc.destBuffer->getWritePointer (c, fc.bufferStartSample)[i] *= g;
        }
    }

    void detach()
    {
        if (owner_.isValid())
            owner_.removeListener (&watcher_);

        owner_ = {};
    }

    void rebuild()
    {
        auto next = std::make_shared<Regions>();

        if (auto* track = dynamic_cast<te::ClipTrack*> (getOwnerTrack()))
            for (auto* clip : track->getClips())
            {
                Region r;
                r.start = clip->getPosition().getStart().inSeconds();
                r.end = clip->getPosition().getEnd().inSeconds();
                r.inLen = (double) clip->state.getProperty (lenIn, 0.0);
                r.outLen = (double) clip->state.getProperty (lenOut, 0.0);
                r.inCurve = fades::normalise ((int) clip->state.getProperty (shapeIn, 0), (double) clip->state.getProperty (curveIn, 0.0));
                r.outCurve = fades::normalise ((int) clip->state.getProperty (shapeOut, 0), (double) clip->state.getProperty (curveOut, 0.0));

                r.tone = (bool) clip->state.getProperty (stretchToneId, false);

                if (r.inLen > 0.0 || r.outLen > 0.0 || r.tone)
                    next->push_back (r);
            }

        const juce::SpinLock::ScopedLockType lock (lock_);
        regions_ = std::move (next);
    }

    static bool concerns (const juce::Identifier& id)
    {
        return id == te::IDs::start || id == te::IDs::length || id == te::IDs::offset || id.toString().startsWith ("rovox:fade") || id == stretchToneId;
    }

    /** Listens to the track's tree; a change of a clip's position or fade, or a clip added / removed, refreshes the fade list. */
    struct Watcher : juce::ValueTree::Listener
    {
        explicit Watcher (RovoxItemFades& o) : owner (o) {}

        void valueTreePropertyChanged (juce::ValueTree& tree, const juce::Identifier& id) override
        {
            if (concerns (id) && tree.getParent() == owner.owner_)
                owner.rebuild();
        }

        void valueTreeChildAdded (juce::ValueTree& parent, juce::ValueTree&) override        { if (parent == owner.owner_) owner.rebuild(); }
        void valueTreeChildRemoved (juce::ValueTree& parent, juce::ValueTree&, int) override { if (parent == owner.owner_) owner.rebuild(); }

        RovoxItemFades& owner;
    };

    double sampleRate_ = 44100.0;
    Biquad lowShelf_, highShelf_;
    juce::ValueTree owner_;
    Watcher watcher_;
    juce::SpinLock lock_;
    std::shared_ptr<const Regions> regions_;
};

/** The stage at the head of the track's chain; creates it when missing. */
void installStage (te::AudioTrack& track)
{
    for (auto* pl : track.pluginList.getPlugins())
        if (dynamic_cast<RovoxItemFades*> (pl) != nullptr)
            return;

    if (auto plugin = track.edit.getPluginCache().createNewPlugin (fadeStageType, {}))
    {
        track.pluginList.insertPlugin (plugin, 0, nullptr);
        plugin->initialiseFully();
    }
}
} // namespace

bool isFadeStage (te::Plugin& p)
{
    return dynamic_cast<RovoxItemFades*> (&p) != nullptr;
}

int chainIndexOfVisible (te::PluginList& list, int visibleIndex)
{
    int seen = 0;

    for (int i = 0; i < list.size(); ++i)
        if (! isFadeStage (*list[i]) && seen++ == visibleIndex)
            return i;

    return list.size();
}

int visibleIndexOf (te::PluginList& list, te::Plugin& plugin)
{
    int seen = 0;

    for (auto* pl : list.getPlugins())
    {
        if (pl == &plugin)
            return seen;

        if (! isFadeStage (*pl))
            ++seen;
    }

    return -1;
}

void registerFadeStage (te::Engine& engine)
{
    engine.getPluginManager().createBuiltInType<RovoxItemFades>();
}

void setItemFade (te::Clip& clip, bool fadeIn, double lengthSeconds, const fades::FadeCurve& curve)
{
    const auto& len = fadeIn ? lenIn : lenOut;
    const auto& shape = fadeIn ? shapeIn : shapeOut;
    const auto& cur = fadeIn ? curveIn : curveOut;
    auto* audio = dynamic_cast<te::AudioClipBase*> (&clip);

    if (lengthSeconds <= 0.0)
    {
        clearItemFadeCurve (clip, fadeIn);
        return;
    }

    if (fades::isLinear (curve))
    {
        clearItemFadeCurve (clip, fadeIn);

        if (audio != nullptr)
        {
            if (fadeIn)     audio->setFadeIn (te::TimeDuration::fromSeconds (lengthSeconds));
            else            audio->setFadeOut (te::TimeDuration::fromSeconds (lengthSeconds));
        }

        return;
    }

    if (audio != nullptr)
    {
        if (fadeIn)     audio->setFadeIn (te::TimeDuration::fromSeconds (0.0));
        else            audio->setFadeOut (te::TimeDuration::fromSeconds (0.0));
    }

    clip.state.setProperty (len, lengthSeconds, nullptr);
    clip.state.setProperty (shape, curve.shape, nullptr);
    clip.state.setProperty (cur, curve.curve, nullptr);

    if (auto* track = dynamic_cast<te::AudioTrack*> (clip.getTrack()))
        installStage (*track);
}

double itemFadeSeconds (te::Clip& clip, bool fadeIn)
{
    if (const auto v = (double) clip.state.getProperty (fadeIn ? lenIn : lenOut, 0.0); v > 0.0)
        return v;

    if (auto* audio = dynamic_cast<te::AudioClipBase*> (&clip))
        return (fadeIn ? audio->getFadeIn() : audio->getFadeOut()).inSeconds();

    return 0.0;
}

void setItemStretchTone (te::Clip& clip, bool on)
{
    if (! on)
    {
        clip.state.removeProperty (stretchToneId, nullptr);
        return;
    }

    clip.state.setProperty (stretchToneId, true, nullptr);

    if (auto* track = dynamic_cast<te::AudioTrack*> (clip.getTrack()))
        installStage (*track);
}

bool itemStretchTone (te::Clip& clip)
{
    return (bool) clip.state.getProperty (stretchToneId, false);
}

void clearItemFadeCurve (te::Clip& clip, bool fadeIn)
{
    clip.state.removeProperty (fadeIn ? lenIn : lenOut, nullptr);
    clip.state.removeProperty (fadeIn ? shapeIn : shapeOut, nullptr);
    clip.state.removeProperty (fadeIn ? curveIn : curveOut, nullptr);
}

} // namespace rovox
