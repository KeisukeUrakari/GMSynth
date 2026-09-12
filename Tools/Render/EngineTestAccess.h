#pragma once
#include "FluidSynthEngine.h"

// Tests expose copied observations, never the engine's mutable synth pointer.
struct FluidSynthEngineTestAccess
{
    struct Channel { int basic = -1, mode = -1, size = -1, applied = -1, voices = 0; float energy = 0; };
    struct Snapshot { std::array<Channel, 32> channels; int outputs = 0, groups = 0; };
    static Snapshot snapshot (FluidSynthEngine& engine, bool pending = false)
    {
        Snapshot result;
        auto* instance = engine.activeSynth;
        if (pending)
        {
            auto* change = engine.pendingChange.load();
            instance = change != nullptr ? change->synth : nullptr;
        }
        if (instance == nullptr) return result;
        result.outputs = fluid_synth_count_audio_channels (instance->synth);
        result.groups = fluid_synth_count_audio_groups (instance->synth);
        std::array<fluid_voice_t*, 1024> voices {};
        fluid_synth_get_voicelist (instance->synth, voices.data(), (int) voices.size(), -1);
        for (int i = 0; i < 32; ++i)
        {
            auto& channel = result.channels[(size_t) i];
            fluid_synth_get_basic_channel (instance->synth, i, &channel.basic, &channel.mode, &channel.size);
            if (i < 16) channel.applied = engine.appliedChannelMonoPoly[(size_t) i];
            for (auto* voice : voices)
                if (voice && fluid_voice_is_on (voice) && fluid_voice_get_channel (voice) == i) ++channel.voices;
            if (engine.multiPartBuffer.getNumSamples() > 0)
                channel.energy = engine.multiPartBuffer.getRMSLevel (2 * i, 0, engine.multiPartBuffer.getNumSamples())
                               + engine.multiPartBuffer.getRMSLevel (2 * i + 1, 0, engine.multiPartBuffer.getNumSamples());
        }
        return result;
    }
    static void failAfter (int successfulChanges) { FluidSynthEngine::channelSetFailureCountdown.store (successfulChanges); }
};
