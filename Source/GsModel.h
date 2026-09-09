#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace gs
{
    // GS System Exclusive constants
    constexpr uint8_t manufacturerRoland = 0x41;
    constexpr uint8_t modelIdGs          = 0x42;
    constexpr uint8_t commandDt1         = 0x12;

    // Part Mode definitions (Address 40 1x 15: Use for Rhythm Part)
    enum class PartMode : uint8_t
    {
        Normal = 0,
        Drum1  = 1,
        Drum2  = 2
    };

    inline bool isDrumMode (PartMode mode) noexcept
    {
        return mode != PartMode::Normal;
    }

    // Default parameters according to Roland GS (SC-55 / SC-88)
    constexpr int defaultVolume      = 100;
    constexpr int defaultPan         = 64;   // 40H Center
    constexpr int defaultReverbSend  = 40;   // 28H
    constexpr int defaultChorusSend  = 0;
    constexpr int defaultDelaySend   = 0;

    struct SystemParameters
    {
        std::array<uint8_t, 4> masterTuneRaw { 0, 4, 0, 0 }; // 00 04 00 00 = 1024 (center: 440.0Hz)
        float masterTuneCents = 0.0f;
        int masterVolume = 127;
        int masterKeyShift = 64;  // 28H..58H (-24..+24, 40H = 64 = 0)
        int masterPan = 64;       // 0..127, 40H = Center

        void reset() noexcept
        {
            masterTuneRaw = { 0, 4, 0, 0 };
            masterTuneCents = 0.0f;
            masterVolume = 127;
            masterKeyShift = 64;
            masterPan = 64;
        }

        void updateMasterTuneFromRaw() noexcept
        {
            const auto raw = (static_cast<int> (masterTuneRaw[0] & 0x0F) << 12)
                           | (static_cast<int> (masterTuneRaw[1] & 0x0F) << 8)
                           | (static_cast<int> (masterTuneRaw[2] & 0x0F) << 4)
                           | static_cast<int> (masterTuneRaw[3] & 0x0F);
            masterTuneCents = static_cast<float> (raw - 1024) * 0.1f;
        }
    };

    struct ReverbParameters
    {
        // Address 40 01 30..35
        uint8_t macro = 4;        // 0: Room 1, 1: Room 2, 2: Room 3, 3: Hall 1, 4: Hall 2, 5: Plate, 6: Delay, 7: Pan Delay (default 4 = Hall 2)
        uint8_t character = 4;    // 0..7
        uint8_t preLpf = 0;       // 0..7
        uint8_t level = 64;       // 0..127
        uint8_t time = 64;        // 0..127
        uint8_t delayFeedback = 0;// 0..127

        void reset() noexcept
        {
            macro = 4;
            character = 4;
            preLpf = 0;
            level = 64;
            time = 64;
            delayFeedback = 0;
        }
    };

    struct ChorusParameters
    {
        // Address 40 01 38..40
        uint8_t macro = 2;        // 0: Chorus 1, 1: Chorus 2, 2: Chorus 3, 3: Chorus 4, 4: FB Chorus, 5: Flanger, 6: Short Delay, 7: Short Delay FB (default 2 = Chorus 3)
        uint8_t preLpf = 0;       // 0..7
        uint8_t level = 64;       // 0..127
        uint8_t feedback = 8;     // 0..127
        uint8_t delay = 0;        // 0..127
        uint8_t rate = 0;         // 0: use macro default, 1..127: custom rate
        uint8_t depth = 0;        // 0: use macro default, 1..127: custom depth
        uint8_t sendToReverb = 0; // 0..127
        uint8_t sendToDelay = 0;  // 0..127

        void reset() noexcept
        {
            macro = 2;
            preLpf = 0;
            level = 64;
            feedback = 8;
            delay = 0;
            rate = 0;
            depth = 0;
            sendToReverb = 0;
            sendToDelay = 0;
        }
    };

    struct DelayParameters
    {
        // Address 40 01 50..59 (SC-88)
        uint8_t macro = 0;        // 0: Delay 1, 1: Delay 2, 2: Delay 3, 3: Delay 4, 4: Pan Delay 1, 5: Pan Delay 2, 6: Pan Delay 3, 7: Pan Delay 4, 8: Delay to Rev, 9: Pan Repeat
        uint8_t preLpf = 0;       // 0..7
        uint8_t timeCenter = 0;   // 0.1ms..1.0s
        uint8_t feedback = 0;     // 0..127
        uint8_t level = 64;       // 0..127
        uint8_t sendToReverb = 0; // 0..127

        void reset() noexcept
        {
            macro = 0;
            preLpf = 0;
            timeCenter = 0;
            feedback = 0;
            level = 64;
            sendToReverb = 0;
        }
    };

    struct PartParameters
    {
        uint8_t toneNumber = 0;      // 0..127 (PC)
        uint8_t variationBank = 0;   // 0..127 (CC#0)
        uint8_t toneMap = 0;         // 0: SC-88, 1: SC-55, 2: SC-88 (CC#32)
        PartMode partMode = PartMode::Normal;
        int keyShift = 64;           // 28H..58H (-24..+24, 40H = 64 = 0)
        std::array<uint8_t, 2> pitchOffsetFineRaw { 0x08, 0x00 }; // 08H 00H = 128 (center: 0Hz)
        float pitchOffsetFineCents = 0.0f;
        int level = defaultVolume;   // 0..127
        int pan = defaultPan;        // 0..127
        int reverbSend = defaultReverbSend;
        int chorusSend = defaultChorusSend;
        int delaySend = defaultDelaySend;
        uint8_t rcvChannel = 0;      // 0..15, 10H: OFF
        std::array<int8_t, 12> scaleTuning {}; // -64..+63 cents for C..B (default 0)

        void updatePitchOffsetFine() noexcept
        {
            // Nibblized 2 bytes: 08 00 = 128 (center: 0Hz). Range 00 00..10 00 (-12.0Hz..+12.0Hz)
            const auto raw = (static_cast<int> (pitchOffsetFineRaw[0] & 0x0F) << 4)
                           | static_cast<int> (pitchOffsetFineRaw[1] & 0x0F);
            const auto hzOffset = static_cast<float> (raw - 128) * (12.0f / 128.0f);
            if (std::abs (hzOffset) < 0.001f)
                pitchOffsetFineCents = 0.0f;
            else
                pitchOffsetFineCents = 1200.0f * std::log2 ((440.0f + hzOffset) / 440.0f);
        }

        void reset (int channel) noexcept
        {
            toneNumber = 0;
            variationBank = 0;
            toneMap = 0;
            partMode = (channel == 9) ? PartMode::Drum1 : PartMode::Normal;
            keyShift = 64;
            pitchOffsetFineRaw = { 0x08, 0x00 };
            pitchOffsetFineCents = 0.0f;
            level = defaultVolume;
            pan = defaultPan;
            reverbSend = defaultReverbSend;
            chorusSend = defaultChorusSend;
            delaySend = defaultDelaySend;
            rcvChannel = static_cast<uint8_t> (channel);
            scaleTuning.fill (0);
        }
    };

    struct DrumNoteParameters
    {
        int pitch = 64;          // 0..127 (40H = standard key)
        int level = 127;         // 0..127
        int alternateGroup = 0;  // 0: off, 1..127
        int pan = 64;            // 0: random, 1..127
        int reverbSend = defaultReverbSend;
        int chorusSend = defaultChorusSend;
        int delaySend = defaultDelaySend;
        uint8_t rcvNoteOff = 0;  // 0: off, 1: on
        uint8_t rcvNoteOn = 1;   // 0: off, 1: on
        bool modified = false;

        void reset() noexcept
        {
            pitch = 64;
            level = 127;
            alternateGroup = 0;
            pan = 64;
            reverbSend = defaultReverbSend;
            chorusSend = defaultChorusSend;
            delaySend = defaultDelaySend;
            rcvNoteOff = 0;
            rcvNoteOn = 1;
            modified = false;
        }
    };

    struct DrumSetup
    {
        std::array<DrumNoteParameters, 128> notes;

        void reset() noexcept
        {
            for (auto& note : notes)
                note.reset();
        }
    };

    // Maps Roland Part Block Address (10..1F) to zero-based MIDI Channel (0..15)
    // Block 10H is Part 10 (Channel 9), 11H is Part 1 (Channel 0), ..., 1FH is Part 16 (Channel 15)
    inline int blockToChannel (uint8_t block) noexcept
    {
        if (block == 0x10)
            return 9; // Part 10 -> Ch 10 (0-indexed 9)
        if (block >= 0x11 && block <= 0x19)
            return static_cast<int> (block - 0x11); // Part 1..9 -> Ch 1..9 (0-indexed 0..8)
        if (block >= 0x1A && block <= 0x1F)
            return static_cast<int> (block - 0x1A + 10); // Part 11..16 -> Ch 11..16 (0-indexed 10..15)
        return -1;
    }
}
