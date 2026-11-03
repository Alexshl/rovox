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

struct Region
{
    double start = 0.0, end = 0.0, inLen = 0.0, outLen = 0.0;
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
        detach();

        if (auto* track = getOwnerTrack())
        {
            owner_ = track->state;
            owner_.addListener (&watcher_);
        }

        rebuild();
    }

    void deinitialise() override                            { detach(); }
    void reset() override                                   {}

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

                if (r.inLen > 0.0 || r.outLen > 0.0)
                    next->push_back (r);
            }

        const juce::SpinLock::ScopedLockType lock (lock_);
        regions_ = std::move (next);
    }

    static bool concerns (const juce::Identifier& id)
    {
        return id == te::IDs::start || id == te::IDs::length || id == te::IDs::offset || id.toString().startsWith ("rovox:fade");
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

void clearItemFadeCurve (te::Clip& clip, bool fadeIn)
{
    clip.state.removeProperty (fadeIn ? lenIn : lenOut, nullptr);
    clip.state.removeProperty (fadeIn ? shapeIn : shapeOut, nullptr);
    clip.state.removeProperty (fadeIn ? curveIn : curveOut, nullptr);
}

} // namespace rovox
