// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 Rovox contributors

#include "ProjectMixNode.h"

#include <cmath>

namespace rovox
{

namespace
{
class ProjectMixNode final : public tracktion::graph::Node
{
public:
    ProjectMixNode (ProjectMixerStrip& s, std::unique_ptr<tracktion::graph::Node> in) : strip_ (s), input_ (std::move (in))
    {
        setOptimisations ({ tracktion::graph::ClearBuffers::no, tracktion::graph::AllocateAudioBuffer::yes });
    }

    tracktion::graph::NodeProperties getNodeProperties() override
    {
        auto props = input_->getNodeProperties();

        if (props.nodeID != 0)
            tracktion::hash_combine (props.nodeID, static_cast<size_t> (0x524f564d4958ULL));      // "ROVMIX"

        return props;
    }

    std::vector<tracktion::graph::Node*> getDirectInputNodes() override     { return { input_.get() }; }
    bool isReadyToProcess() override                                        { return input_->hasProcessed(); }

    void prepareToPlay (const tracktion::graph::PlaybackInitialisationInfo& info) override
    {
        step_ = (float) (1.0 / (0.015 * std::max (1.0, info.sampleRate)));
        gain_ = targetGain();               // no fade-in from silence at the start of playback
    }

    void process (ProcessContext& pc) override
    {
        auto src = input_->getProcessedOutput();
        auto in = tracktion::graph::toAudioBuffer (src.audio);
        auto out = tracktion::graph::toAudioBuffer (pc.buffers.audio);
        const int n = std::min (in.getNumSamples(), out.getNumSamples());
        const int chans = std::min (in.getNumChannels(), out.getNumChannels());
        const float target = targetGain();
        const float start = gain_;

        for (int c = 0; c < chans; ++c)
        {
            float g = start;
            const float* s = in.getReadPointer (c);
            float* d = out.getWritePointer (c);

            for (int i = 0; i < n; ++i)
            {
                g = g < target ? std::min (target, g + step_) : std::max (target, g - step_);
                d[i] = s[i] * g;
            }

            if (c == chans - 1)
                gain_ = g;
        }

        pc.buffers.midi.copyFrom (src.midi);       // MIDI passes through untouched
    }

private:
    float targetGain() const
    {
        const bool silent = strip_.muted.load() || (strip_.soloCount->load() > 0 && ! strip_.solo.load());
        return silent ? 0.0f : (float) std::pow (10.0, strip_.gainDb.load() / 20.0);
    }

    ProjectMixerStrip& strip_;
    std::unique_ptr<tracktion::graph::Node> input_;
    float step_ = 0.003f, gain_ = 1.0f;
};
} // namespace

std::unique_ptr<tracktion::graph::Node> makeProjectMixNode (ProjectMixerStrip& strip, std::unique_ptr<tracktion::graph::Node> input)
{
    return std::make_unique<ProjectMixNode> (strip, std::move (input));
}

} // namespace rovox
