#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace xg
{
    // Bank MSB constants according to XG Format V1.35
    constexpr int bankMsbNormal = 0;       // Melody (Normal Voice)
    constexpr int bankMsbSfxVoice = 64;    // SFX Voice (40H)
    constexpr int bankMsbSfxKit = 126;     // SFX Kit (7EH)
    constexpr int bankMsbDrumKit = 127;    // Drum Kit (7FH)

    // Part Mode definitions (Multi Part Parameter 0x07)
    enum class PartMode : uint8_t
    {
        Normal = 0,
        Drum = 1,
        Drums1 = 2,
        Drums2 = 3,
        Drums3 = 4,
        Drums4 = 5
    };

    inline bool isDrumMode (PartMode mode) noexcept
    {
        return mode != PartMode::Normal;
    }

    // Default values according to XG Format V1.35
    constexpr int defaultVolume = 100;     // 64H
    constexpr int defaultPan = 64;         // 40H (Center)
    constexpr int defaultExpression = 127; // 7FH
    constexpr int defaultReverbSend = 40;  // 28H
    constexpr int defaultChorusSend = 0;   // 00H
    constexpr int defaultVariationSend = 0;// 00H
    constexpr int defaultPitchBendSensitivity = 2; // Semitones

    constexpr int defaultFilterCutoff = 64;      // 40H (Relative 0)
    constexpr int defaultFilterResonance = 64;   // 40H (Relative 0)
    constexpr int defaultEgAttack = 64;          // 40H (Relative 0)
    constexpr int defaultEgDecay = 64;           // 40H (Relative 0)
    constexpr int defaultEgRelease = 64;         // 40H (Relative 0)
    constexpr int defaultVibratoRate = 64;       // 40H (Relative 0)
    constexpr int defaultVibratoDepth = 64;      // 40H (Relative 0)
    constexpr int defaultVibratoDelay = 64;      // 40H (Relative 0)
    constexpr int defaultEqBass = 64;            // 40H (Relative 0)
    constexpr int defaultEqTreble = 64;          // 40H (Relative 0)
    constexpr int defaultEqBassFreq = 12;        // 0CH
    constexpr int defaultEqTrebleFreq = 54;      // 36H

    struct SystemParameters
    {
        std::array<uint8_t, 4> masterTuneRaw { 0, 4, 0, 0 }; // 00 04 00 00 = 1024 (center)
        float masterTuneCents = 0.0f;                         // -102.4 to +102.3 cents
        int masterVolume = 127;                               // 0..127 (default 127)
        int masterAttenuator = 0;                             // 0..127
        int transpose = 64;                                   // 28H..58H (-24..+24 semitones, 40H = 64 = 0)

        void reset() noexcept
        {
            masterTuneRaw = { 0, 4, 0, 0 };
            masterTuneCents = 0.0f;
            masterVolume = 127;
            masterAttenuator = 0;
            transpose = 64;
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

    enum class ParameterSelection : uint8_t
    {
        None = 0,
        Rpn,
        Nrpn
    };

    struct NrpnState
    {
        uint8_t msb = 127;
        uint8_t lsb = 127;
        ParameterSelection activeSelection = ParameterSelection::None;

        void reset() noexcept
        {
            msb = 127;
            lsb = 127;
            activeSelection = ParameterSelection::None;
        }
    };

    struct PartParameters
    {
        int filterCutoff = defaultFilterCutoff;
        int filterResonance = defaultFilterResonance;
        int egAttack = defaultEgAttack;
        int egDecay = defaultEgDecay;
        int egRelease = defaultEgRelease;
        int vibratoRate = defaultVibratoRate;
        int vibratoDepth = defaultVibratoDepth;
        int vibratoDelay = defaultVibratoDelay;
        int pitchBendSensitivity = defaultPitchBendSensitivity;
        int reverbSend = defaultReverbSend;
        int chorusSend = defaultChorusSend;
        int variationSend = defaultVariationSend;
        int eqBass = defaultEqBass;
        int eqTreble = defaultEqTreble;
        int eqBassFreq = defaultEqBassFreq;
        int eqTrebleFreq = defaultEqTrebleFreq;

        int noteShift = 64;            // 28H..58H (-24..+24 semitones, 40H = 64 = 0)
        uint8_t detuneMsb = 0x08;      // 08H
        uint8_t detuneLsb = 0x00;      // 00H -> 80H = 128 (center = 0 Hz)
        float detuneCents = 0.0f;      // Detune in cents
        uint8_t monoPolyMode = 1;      // 0: Mono, 1: Poly (default 1)
        uint8_t sameNoteAssign = 1;    // 0: Single, 1: Multi, 2: Inst (default 1)
        uint8_t rcvChannel = 0;        // 0..15, 7FH: OFF
        int noteLimitLow = 0;          // 0..127
        int noteLimitHigh = 127;       // 0..127
        int dryLevel = 127;            // 0..127
        int velocitySenseDepth = 64;   // 0..127 (default 40H)
        int velocitySenseOffset = 64;  // 0..127 (default 40H)
        uint8_t portamentoSwitch = 0;  // 0: Off, 1: On
        int portamentoTime = 0;        // 0..127
        int elementReserve = 2;        // 0..32

        void reset() noexcept
        {
            filterCutoff = defaultFilterCutoff;
            filterResonance = defaultFilterResonance;
            egAttack = defaultEgAttack;
            egDecay = defaultEgDecay;
            egRelease = defaultEgRelease;
            vibratoRate = defaultVibratoRate;
            vibratoDepth = defaultVibratoDepth;
            vibratoDelay = defaultVibratoDelay;
            pitchBendSensitivity = defaultPitchBendSensitivity;
            reverbSend = defaultReverbSend;
            chorusSend = defaultChorusSend;
            variationSend = defaultVariationSend;
            eqBass = defaultEqBass;
            eqTreble = defaultEqTreble;
            eqBassFreq = defaultEqBassFreq;
            eqTrebleFreq = defaultEqTrebleFreq;

            noteShift = 64;
            detuneMsb = 0x08;
            detuneLsb = 0x00;
            detuneCents = 0.0f;
            monoPolyMode = 1;
            sameNoteAssign = 1;
            rcvChannel = 0;
            noteLimitLow = 0;
            noteLimitHigh = 127;
            dryLevel = 127;
            velocitySenseDepth = 64;
            velocitySenseOffset = 64;
            portamentoSwitch = 0;
            portamentoTime = 0;
            elementReserve = 2;
        }

        void updateDetuneCents() noexcept
        {
            const auto raw = ((detuneMsb & 0x0F) << 4) | (detuneLsb & 0x0F);
            const auto detuneHz = static_cast<float> (raw - 128) * 0.1f;
            if (detuneHz == 0.0f)
            {
                detuneCents = 0.0f;
            }
            else
            {
                const auto f = 440.0f + detuneHz;
                detuneCents = 1200.0f * std::log2 (f / 440.0f);
            }
        }
    };

    struct DrumNoteParameters
    {
        int pitchCoarse = 64;    // 40H (-64..0..+63 semitones)
        int pitchFine = 64;      // 40H (-64..0..+63 cents)
        int level = 127;         // 0..127
        int alternateGroup = 0;  // 0: off, 1..127
        int pan = 64;            // 40H Center, 0: random, 1..127
        int reverbSend = 40;     // 28H
        int chorusSend = 0;      // 00H
        int variationSend = 0;   // 00H
        int keyAssign = 0;       // 0: single, 1: multi
        int rcvNoteOff = 0;      // 0: off, 1: on
        int rcvNoteOn = 1;       // 0: off, 1: on
        int filterCutoff = 64;   // 40H
        int filterResonance = 64;// 40H
        int egAttack = 64;       // 40H
        int egDecay1 = 64;       // 40H
        int egDecay2 = 64;       // 40H
        int eqBass = 64;         // 40H
        int eqTreble = 64;       // 40H
        bool modified = false;

        void reset() noexcept
        {
            pitchCoarse = 64;
            pitchFine = 64;
            level = 127;
            alternateGroup = 0;
            pan = 64;
            reverbSend = 40;
            chorusSend = 0;
            variationSend = 0;
            keyAssign = 0;
            rcvNoteOff = 0;
            rcvNoteOn = 1;
            filterCutoff = 64;
            filterResonance = 64;
            egAttack = 64;
            egDecay1 = 64;
            egDecay2 = 64;
            eqBass = 64;
            eqTreble = 64;
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

    // Generator scaling helpers (relative offset from 64 to SF2 generator units)
    inline float cutoffOffsetToCents (int cutoff) noexcept
    {
        return static_cast<float> (cutoff - 64) * 64.0f;
    }

    inline float resonanceOffsetToCentibels (int reso) noexcept
    {
        return static_cast<float> (reso - 64) * 3.0f;
    }

    inline float attackOffsetToTimecents (int attack) noexcept
    {
        return static_cast<float> (attack - 64) * 32.0f;
    }

    inline float decayOffsetToTimecents (int decay) noexcept
    {
        return static_cast<float> (decay - 64) * 32.0f;
    }

    inline float releaseOffsetToTimecents (int release) noexcept
    {
        return static_cast<float> (release - 64) * 32.0f;
    }

    inline float vibratoRateOffsetToCents (int rate) noexcept
    {
        return static_cast<float> (rate - 64) * 32.0f;
    }

    inline float vibratoDepthOffsetToCents (int depth) noexcept
    {
        return static_cast<float> (depth - 64) * 4.0f;
    }

    inline float vibratoDelayOffsetToTimecents (int delay) noexcept
    {
        return static_cast<float> (delay - 64) * 32.0f;
    }
}
