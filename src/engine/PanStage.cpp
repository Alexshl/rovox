// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "PanStage.h"

#include "PanLaw.h"

#include <atomic>

namespace rovox
{

namespace
{
const juce::Identifier panModeId ("rovox:panMode"), panLawId ("rovox:panLawDb"), panRightId ("rovox:panRight");
constexpr const char* panStageType = "rovoxvolume";

class RovoxVolumeAndPan : public te::VolumeAndPanPlugin
{
public:
    explicit RovoxVolumeAndPan (te::PluginCreationInfo info)
        : te::VolumeAndPanPlugin (info)
    {
        auto* um = getUndoManager();
        trackMode.referTo (state, panModeId, um, -1);
        trackLawDb.referTo (state, panLawId, um, 1.0f);
        panRight.referTo (state, panRightId, um, 1.0f);
        projectMode.referTo (edit.state, panModeId, nullptr, pan::defaultReaperMode);
        projectLawDb.referTo (edit.state, panLawId, nullptr, 0.0f);
    }

    static constexpr const char* xmlTypeName = panStageType;

    void initialise (const te::PluginInitialisationInfo& info) override
    {
        te::VolumeAndPanPlugin::initialise (info);
        monoSource_.store (tracksMaterialIsMono());
    }

    void applyToBuffer (const te::PluginRenderContext& fc) override
    {
        if (isEnabled() && fc.destBuffer != nullptr && fc.destBuffer->getNumChannels() >= 2 && fc.bufferNumSamples > 0)
        {
            const int mode = trackMode.get() >= 0 ? trackMode.get() : projectMode.get();
            const double law = trackLawDb.get() <= 0.0f ? (double) trackLawDb.get() : (double) projectLawDb.get();
            const double p = std::clamp ((double) getPan(), -1.0, 1.0);
            auto m = pan::matrix (pan::modeFromReaper (mode), p, panRight.get(), law, monoSource_.load());

            // The fader that runs after this multiplies the left channel by (1 - p) and the right one by (1 + p): divide that out per output row.
            const double dl = 1.0 - p, dr = 1.0 + p;
            const auto row = [] (double a, double b, double d, double& oa, double& ob) { if (d > 1e-6) { oa = a / d; ob = b / d; } else { oa = ob = 0.0; } };
            row (m.ll, m.lr, dl, m.ll, m.lr);
            row (m.rl, m.rr, dr, m.rl, m.rr);

            if (std::abs (m.ll - 1.0) > 1e-9 || m.lr != 0.0 || m.rl != 0.0 || std::abs (m.rr - 1.0) > 1e-9)
            {
                auto* l = fc.destBuffer->getWritePointer (0, fc.bufferStartSample);
                auto* r = fc.destBuffer->getWritePointer (1, fc.bufferStartSample);

                for (int i = 0; i < fc.bufferNumSamples; ++i)
                {
                    const float a = l[i], b = r[i];
                    l[i] = (float) (m.ll * a + m.lr * b);
                    r[i] = (float) (m.rl * a + m.rr * b);
                }
            }
        }

        te::VolumeAndPanPlugin::applyToBuffer (fc);
    }

private:
    bool tracksMaterialIsMono()
    {
        auto* ct = dynamic_cast<te::ClipTrack*> (getOwnerTrack());

        if (ct == nullptr)
            return false;

        bool any = false;

        for (auto* c : ct->getClips())
            if (auto* wc = dynamic_cast<te::WaveAudioClip*> (c))
            {
                any = true;

                if (wc->getWaveInfo().numChannels != 1)
                    return false;
            }

        return any;
    }

    juce::CachedValue<int> trackMode, projectMode;
    juce::CachedValue<float> trackLawDb, panRight, projectLawDb;
    std::atomic<bool> monoSource_ { false };
};
} // namespace

void registerPanStage (te::Engine& engine)
{
    engine.getPluginManager().createBuiltInType<RovoxVolumeAndPan>();
}

void installPanStage (te::AudioTrack& track)
{
    auto* vp = track.getVolumePlugin();

    if (vp == nullptr || dynamic_cast<RovoxVolumeAndPan*> (vp) != nullptr || vp->isMasterVolAndPan())
        return;

    auto state = vp->state.createCopy();
    state.setProperty (te::IDs::type, panStageType, nullptr);
    const int index = track.pluginList.indexOf (vp);
    vp->deleteFromParent();
    track.pluginList.insertPlugin (state, std::max (0, index));
}

void installPanStages (te::Edit& edit)
{
    for (auto* t : te::getAudioTracks (edit))
        installPanStage (*t);
}

void setProjectPan (te::Edit& edit, int reaperPanMode, double lawDb)
{
    edit.state.setProperty (panModeId, reaperPanMode, nullptr);
    edit.state.setProperty (panLawId, std::min (0.0, lawDb), nullptr);
}

int projectPanMode (te::Edit& edit)         { return (int) edit.state.getProperty (panModeId, pan::defaultReaperMode); }
double projectPanLawDb (te::Edit& edit)     { return (double) edit.state.getProperty (panLawId, 0.0); }

void setTrackPan (te::AudioTrack& track, int reaperPanMode, double lawDb)
{
    if (auto* vp = track.getVolumePlugin())
    {
        vp->state.setProperty (panModeId, reaperPanMode, nullptr);
        vp->state.setProperty (panLawId, lawDb, nullptr);
    }
}

} // namespace rovox
