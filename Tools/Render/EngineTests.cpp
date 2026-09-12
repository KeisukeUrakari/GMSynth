#include "EngineTestAccess.h"
#include <functional>
#include <iostream>

namespace
{
using Access = FluidSynthEngineTestAccess;
void check (bool ok, const char* message) { if (! ok) throw std::runtime_error (message); }
juce::MidiMessage sx (std::initializer_list<uint8_t> bytes)
{ return juce::MidiMessage::createSysExMessage (bytes.begin(), (int) bytes.size()); }
juce::MidiMessage part (int channel, int address, int value)
{ return sx ({ 0x43, 0x10, 0x4c, 8, (uint8_t) channel, (uint8_t) address, (uint8_t) value }); }
juce::MidiMessage note (int channel, int key = 69)
{ return juce::MidiMessage::noteOn (channel, key, (uint8_t) 100); }
juce::MidiMessage cc (int channel, int controller, int value)
{ return juce::MidiMessage::controllerEvent (channel, controller, value); }

struct Fixture
{
    FluidSynthEngine engine;
    juce::AudioBuffer<float> audio { 2, 512 };
    explicit Fixture (const juce::File& sf)
    {
        engine.prepare (48000, 512);
        check (engine.loadSoundFont (sf).wasOk(), "SoundFont load failed");
        block();
    }
    void block (std::initializer_list<juce::MidiMessage> messages = {})
    {
        juce::MidiBuffer midi;
        for (const auto& m : messages) midi.addEvent (m, 0);
        engine.processBlock (audio, midi, nullptr, 0);
    }
};
void layout (FluidSynthEngine& engine, bool pending = false)
{
    const auto s = Access::snapshot (engine, pending);
    check (s.groups == 32 && s.outputs == 32, "Incorrect audio groups/outputs");
    for (int i = 0; i < 32; ++i)
    {
        const auto& c = s.channels[(size_t) i];
        check (c.basic == i && c.size == 1, "Channel is not independent");
        check (c.mode == FLUID_CHANNEL_MODE_OMNIOFF_POLY, "Expected Omni Off Poly");
    }
    check (! engine.hasChannelConfigurationError(), "Unexpected configuration error");
}
}

int main (int argc, char** argv)
{
    if (argc != 2) return 2;
    const juce::File sf (juce::String::fromUTF8 (argv[1]));
    int total = 0, failures = 0;
    const auto test = [&] (const char* name, const std::function<void()>& body)
    {
        ++total;
        Access::failAfter (-1);
        try { body(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
        Access::failAfter (-1);
    };
    test ("initial layout, pending replacement and adoption", [&]
    {
        Fixture f (sf);
        layout (f.engine);
        f.block ({ cc (1, 126, 1) });
        check (f.engine.loadSoundFont (sf).wasOk(), "Replacement failed");
        auto pending = Access::snapshot (f.engine, true);
        check (pending.channels[0].mode == FLUID_CHANNEL_MODE_OMNIOFF_MONO, "Pending mode lost");
        f.block();
        check (Access::snapshot (f.engine).channels[0].mode == FLUID_CHANNEL_MODE_OMNIOFF_MONO, "Adoption lost mode");
        f.block ({ cc (1, 127, 0) });
        layout (f.engine);
    });
    test ("CC mono/poly affects only the addressed channel", [&]
    {
        Fixture f (sf);
        f.block ({ note (2), note (16) });
        f.block ({ cc (1, 126, 16), note (1, 60), note (1, 64) });
        auto s = Access::snapshot (f.engine);
        check (s.channels[0].mode == FLUID_CHANNEL_MODE_OMNIOFF_MONO && s.channels[0].voices == 1, "Mono not applied");
        check (s.channels[1].voices == 1 && s.channels[15].voices == 1, "Other channels stopped");
        f.block ({ cc (1, 127, 0), note (1, 60), note (1, 64) });
        s = Access::snapshot (f.engine);
        check (s.channels[0].voices == 2, "Poly not applied");
        check (s.channels[1].voices == 1 && s.channels[15].voices == 1, "Other channels stopped in poly change");
        layout (f.engine);
    });
    test ("XG mode changes and unchanged mode preserve held notes", [&]
    {
        Fixture f (sf);
        f.block ({ part (0, 5, 0), note (1), note (2) });
        f.block ({ part (0, 5, 0) });
        auto s = Access::snapshot (f.engine);
        check (s.channels[0].mode == FLUID_CHANNEL_MODE_OMNIOFF_MONO && s.channels[0].voices == 1, "Same mode stopped a note");
        f.block ({ part (0, 5, 1), note (1, 60), note (1, 64) });
        check (Access::snapshot (f.engine).channels[0].voices == 2, "XG poly not applied");
        f.engine.setChannelVolume (0, 90);
        f.engine.setChannelPan (0, 40);
        f.engine.setChannelProgram (0, 0);
        f.engine.setChannelBankLsb (0, 0); // forces a full state reapplication
        f.block();
        s = Access::snapshot (f.engine);
        check (s.channels[0].voices == 2 && s.channels[1].voices == 1, "State application stopped notes");
    });
    test ("GM GS XG resets restore layout before same-block notes", [&]
    {
        for (const auto& reset : { sx ({ 0x7e, 0x7f, 9, 1 }),
            sx ({ 0x41, 0x10, 0x42, 0x12, 0x40, 0, 0x7f, 0, 0x41 }),
            sx ({ 0x43, 0x10, 0x4c, 0, 0, 0x7e, 0 }),
            sx ({ 0x43, 0x10, 0x4c, 0, 0, 0x7f, 0 }) })
        {
            Fixture f (sf);
            f.block ({ cc (1, 126, 1), cc (16, 126, 1) });
            f.block ({ reset, note (1, 60), note (1, 64), note (16), note (10) });
            layout (f.engine);
            const auto s = Access::snapshot (f.engine);
            check (s.channels[0].voices == 2 && s.channels[15].voices == 1 && s.channels[9].voices == 1,
                   "Reset/next-note ordering or drum playback failed");
        }
    });
    test ("all 16 parts render into their own stereo buffers", [&]
    {
        for (int channel = 1; channel <= 16; ++channel)
        {
            Fixture f (sf);
            f.block ({ note (channel) });
            f.block();
            const auto s = Access::snapshot (f.engine);
            for (int i = 0; i < 32; ++i)
                check (i == channel - 1 ? s.channels[(size_t) i].energy > 1e-6f : s.channels[(size_t) i].energy == 0,
                       "Audio routed to the wrong part");
        }
    });
    test ("part EQ, mute and effect sends do not alter another part", [&]
    {
        Fixture reference (sf), changed (sf);
        reference.block ({ note (1), note (2, 72) });
        changed.block ({ note (1), note (2, 72) });
        changed.block ({ part (0, 0x72, 76), cc (1, 91, 127), cc (1, 93, 127) });
        reference.block();
        auto a = Access::snapshot (reference.engine), b = Access::snapshot (changed.engine);
        check (std::abs (a.channels[1].energy - b.channels[1].energy) < 1e-7f, "Another part's audio changed");
        check (std::abs (a.channels[0].energy - b.channels[0].energy) > 1e-6f, "EQ was not applied to target part");
        check (changed.engine.getPartParameters (0).reverbSend == 127
               && changed.engine.getPartParameters (1).reverbSend != 127, "Send changed another part");
        changed.engine.setChannelMuted (0, true);
        // The existing EQ filters retain a short tail after muting their input.
        for (int i = 0; i < 128; ++i) { changed.block(); reference.block(); }
        a = Access::snapshot (reference.engine); b = Access::snapshot (changed.engine);
        check (b.channels[0].energy < 1e-7f && std::abs (a.channels[1].energy - b.channels[1].energy) < 1e-7f,
               "Mute affected the wrong part");
    });
    test ("auxiliary drum audio remains isolated", [&]
    {
        Fixture f (sf);
        f.block ({ sx ({ 0x43, 0x10, 0x4c, 0, 0, 0x7e, 0 }),
                   sx ({ 0x43, 0x10, 0x4c, 0x30, 69, 0x20, 70 }), note (10), note (2) });
        f.block();
        auto s = Access::snapshot (f.engine);
        float auxEnergy = 0;
        for (int i = 16; i < 32; ++i) auxEnergy += s.channels[(size_t) i].energy;
        check (auxEnergy > 1e-6f && s.channels[1].energy > 1e-6f, "Auxiliary drum not audible");
        layout (f.engine);
        f.engine.setChannelMuted (9, true);
        for (int i = 0; i < 128; ++i) f.block();
        s = Access::snapshot (f.engine);
        for (int i = 16; i < 32; ++i) check (s.channels[(size_t) i].energy < 1e-7f, "Muted auxiliary drum still audible");
        check (s.channels[1].voices == 1, "Drum mute stopped another part");
    });
    test ("initialisation failure does not replace the playing synth", [&]
    {
        Fixture f (sf);
        f.block ({ note (1) });
        Access::failAfter (0);
        check (f.engine.loadSoundFont (sf).failed(), "Initialisation failure ignored");
        f.block();
        check (! f.engine.hasChannelConfigurationError() && Access::snapshot (f.engine).channels[0].voices == 1,
               "Failed replacement damaged the active synth");
    });
    test ("runtime failure is latched, silent, and recoverable by reload", [&]
    {
        Fixture f (sf);
        f.block ({ note (2) });
        Access::failAfter (0);
        f.block ({ cc (1, 126, 1) });
        check (f.engine.hasChannelConfigurationError(), "Failure was not latched");
        check (Access::snapshot (f.engine).channels[0].applied == 1, "Failed mode marked applied");
        check (f.audio.getMagnitude (0, 512) == 0, "Failed block is audible");
        f.block ({ note (2), sx ({ 0x7e, 0x7f, 9, 1 }) });
        check (f.audio.getMagnitude (0, 512) == 0 && f.engine.hasChannelConfigurationError(), "Failure did not remain latched");
        Access::failAfter (-1);
        check (f.engine.loadSoundFont (sf).wasOk(), "Recovery load failed");
        f.block ({ note (2) });
        check (! f.engine.hasChannelConfigurationError() && f.audio.getMagnitude (0, 512) > 0, "Reload did not recover");
    });
    std::cout << total - failures << '/' << total << " engine tests passed\n";
    return failures == 0 ? 0 : 1;
}
