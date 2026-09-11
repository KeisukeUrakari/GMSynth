#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include "XgEffectDefaults.h"
#include "XgEffectTables.h"

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

    // Same Note Assign definitions (Multi Part Parameter 0x0E)
    enum class SameNoteAssign : uint8_t
    {
        Single = 0,
        Multi  = 1,
        Inst   = 2
    };

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

    struct ControllerModulationDestination
    {
        int pitch = 64;       // 28H..58H (-24..+24 semitones, 40H = 0)
        int filter = 64;      // 00H..7FH (-9600..+9450 cents, 40H = 0)
        int amplitude = 64;   // 00H..7FH (-100%..+100%, 40H = 0)
        int lfoPmodDepth = 0; // 00H..7FH (0..127)
        int lfoFmodDepth = 0; // 00H..7FH (0..127)
        int lfoAmodDepth = 0; // 00H..7FH (0..127)

        void reset (int defaultPmod = 0, int defaultPitch = 64) noexcept
        {
            pitch = defaultPitch;
            filter = 64;
            amplitude = 64;
            lfoPmodDepth = defaultPmod;
            lfoFmodDepth = 0;
            lfoAmodDepth = 0;
        }
    };

    struct ControllerMatrix
    {
        ControllerModulationDestination mw;   // 1D..22: default PMOD = 10 (0x0A)
        ControllerModulationDestination bend; // 23..28: default pitch = 66 (+2 semitones, 0x42)
        int bendPitchLow = 62;                // 70: default pitch = 62 (-2 semitones, 0x3E)
        ControllerModulationDestination cat;  // 4D..52: Channel Aftertouch
        ControllerModulationDestination pat;  // 53..58: Polyphonic Aftertouch
        uint8_t ac1ControllerNo = 16;         // 59: default CC#16 (General Purpose 1)
        ControllerModulationDestination ac1;  // 5A..5F: Assignable Controller 1
        uint8_t ac2ControllerNo = 17;         // 60: default CC#17 (General Purpose 2)
        ControllerModulationDestination ac2;  // 61..66: Assignable Controller 2

        void reset() noexcept
        {
            mw.reset (10, 64);
            bend.reset (0, 66);
            bendPitchLow = 62;
            cat.reset (0, 64);
            pat.reset (0, 64);
            ac1ControllerNo = 16;
            ac1.reset (0, 64);
            ac2ControllerNo = 17;
            ac2.reset (0, 64);
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

        ControllerMatrix ctrlMatrix;

        void reset (int channelIndex = 0) noexcept
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
            rcvChannel = static_cast<uint8_t> (channelIndex & 0x7F);
            noteLimitLow = 0;
            noteLimitHigh = 127;
            dryLevel = 127;
            velocitySenseDepth = 64;
            velocitySenseOffset = 64;
            portamentoSwitch = 0;
            portamentoTime = 0;
            elementReserve = (channelIndex == 9) ? 0 : 2;

            ctrlMatrix.reset();
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
        return (depth > 64) ? static_cast<float> (depth - 64) * 2.0f : 0.0f;
    }

    inline float vibratoDelayOffsetToTimecents (int delay) noexcept
    {
        return static_cast<float> (delay - 64) * 32.0f;
    }

    // XG Format V1.35 Table#3 EQ Frequency Table (61 entries: 0 to 60)
    constexpr std::array<float, 61> eqFrequencyTable = {
        20.0f,    22.0f,    25.0f,    28.0f,    32.0f,    36.0f,    40.0f,    45.0f,    50.0f,    56.0f,
        63.0f,    70.0f,    80.0f,    90.0f,   100.0f,   110.0f,   125.0f,   140.0f,   160.0f,   180.0f,
       200.0f,   225.0f,   250.0f,   280.0f,   315.0f,   355.0f,   400.0f,   450.0f,   500.0f,   560.0f,
       630.0f,   700.0f,   800.0f,   900.0f,  1000.0f,  1100.0f,  1200.0f,  1400.0f,  1600.0f,  1800.0f,
      2000.0f,  2200.0f,  2500.0f,  2800.0f,  3200.0f,  3600.0f,  4000.0f,  4500.0f,  5000.0f,  5600.0f,
      6300.0f,  7000.0f,  8000.0f,  9000.0f, 10000.0f, 11000.0f, 12000.0f, 14000.0f, 16000.0f, 18000.0f,
     20000.0f
    };

    inline float lookupEqFrequency (int index) noexcept
    {
        if (index < 0) return eqFrequencyTable.front();
        if (static_cast<size_t> (index) >= eqFrequencyTable.size()) return eqFrequencyTable.back();
        return eqFrequencyTable[static_cast<size_t> (index)];
    }

    inline float decodePartEqGain (int data) noexcept
    {
        const int val = std::clamp (data, 0, 127);
        if (val < 64)
            return static_cast<float> (val - 64) * (12.0f / 64.0f);
        if (val > 64)
            return static_cast<float> (val - 64) * (12.0f / 63.0f);
        return 0.0f;
    }

    struct ReverbParameters
    {
        uint8_t typeMsb = 0x01; // 01H = Hall 1
        uint8_t typeLsb = 0x00;
        std::array<uint8_t, 16> parameters {}; // 1-16
        uint8_t reverbReturn = 64;             // 40H = 0 dB
        uint8_t reverbPan = 64;                // 40H = Center

        void reset() noexcept
        {
            typeMsb = 0x01;
            typeLsb = 0x00;
            parameters = defaults::getReverbDefaults (typeMsb, typeLsb);
            reverbReturn = 64;
            reverbPan = 64;
        }
    };

    struct ChorusParameters
    {
        uint8_t typeMsb = 0x41; // 41H = Chorus 1
        uint8_t typeLsb = 0x00;
        std::array<uint8_t, 16> parameters {}; // 1-16
        uint8_t chorusReturn = 64;             // 40H = 0 dB
        uint8_t chorusPan = 64;                // 40H = Center
        uint8_t sendToReverb = 0;              // 00H

        void reset() noexcept
        {
            typeMsb = 0x41;
            typeLsb = 0x00;
            parameters = defaults::getChorusDefaults (typeMsb, typeLsb);
            chorusReturn = 64;
            chorusPan = 64;
            sendToReverb = 0;
        }
    };

    constexpr uint8_t varTypeThru         = 0x00;
    constexpr uint8_t varTypeHall1        = 0x01;
    constexpr uint8_t varTypeRoom1        = 0x02;
    constexpr uint8_t varTypeStage1       = 0x03;
    constexpr uint8_t varTypePlate        = 0x04;
    constexpr uint8_t varTypeDelayLCR     = 0x05;
    constexpr uint8_t varTypeDelayLR      = 0x06;
    constexpr uint8_t varTypeEcho         = 0x07;
    constexpr uint8_t varTypeCrossDelay   = 0x08;
    constexpr uint8_t varTypeWhiteRoom    = 0x10;
    constexpr uint8_t varTypeTunnel       = 0x11;
    constexpr uint8_t varTypeCanyon       = 0x12;
    constexpr uint8_t varTypeBasement     = 0x13;
    constexpr uint8_t varTypeChorus       = 0x41;
    constexpr uint8_t varTypeFlanger      = 0x43;
    constexpr uint8_t varTypeSymphonic    = 0x44;
    constexpr uint8_t varTypeRotarySpeaker= 0x45;
    constexpr uint8_t varTypeTremolo      = 0x46;
    constexpr uint8_t varTypeAutoPan      = 0x47;
    constexpr uint8_t varTypePhaser       = 0x48;
    constexpr uint8_t varTypeDistortion   = 0x49;
    constexpr uint8_t varTypeOverdrive    = 0x4A;
    constexpr uint8_t varTypeAmpSimulator = 0x4B;
    constexpr uint8_t varType3BandEq      = 0x4C;
    constexpr uint8_t varType2BandEq      = 0x4D;
    constexpr uint8_t varTypeAutoWah      = 0x4E;

    struct VariationParameters
    {
        uint8_t typeMsb = 0x05; // 05H = Delay L,C,R
        uint8_t typeLsb = 0x00;
        std::array<uint16_t, 10> parameters14Bit {}; // Params 1..10 (14-bit)
        uint8_t varReturn = 64;                      // 40H = 0 dB
        uint8_t varPan = 64;                         // 40H = Center
        uint8_t sendToReverb = 0;                    // 00H
        uint8_t sendToChorus = 0;                    // 00H
        uint8_t connection = 0;                      // 0: Insertion, 1: System (default 0)
        uint8_t part = 127;                          // 0..15: Part 1..16, 127: OFF (default 127)
        uint8_t mwControlDepth = 64;                 // 40H = 0
        uint8_t bendControlDepth = 64;               // 40H = 0
        uint8_t catControlDepth = 64;                // 40H = 0
        uint8_t ac1ControlDepth = 64;                // 40H = 0
        uint8_t ac2ControlDepth = 64;                // 40H = 0
        std::array<uint8_t, 6> parameters11To16 {};  // Params 11..16

        void reset() noexcept
        {
            typeMsb = 0x05;
            typeLsb = 0x00;
            const auto defs = defaults::getVariationDefaults (typeMsb, typeLsb);
            parameters14Bit = defs.params14Bit;
            parameters11To16 = defs.params11To16;
            varReturn = 64;
            varPan = 64;
            sendToReverb = 0;
            sendToChorus = 0;
            connection = 0;
            part = 127;
            mwControlDepth = 64;
            bendControlDepth = 64;
            catControlDepth = 64;
            ac1ControlDepth = 64;
            ac2ControlDepth = 64;
        }
    };

    struct MultiEqParameters
    {
        uint8_t eqType = 0; // 0: Flat, 1: Jazz, 2: Pops, 3: Rock, 4: Concert
        int gain1 = 64;     // 34H..4CH (-12..+12 dB, 40H = 64 = 0 dB)
        int freq1 = 12;     // 04H..28H (default 0CH = 80 Hz)
        int q1 = 7;         // 01H..78H (default 07H = 0.7)
        int shape1 = 0;     // 0: shelving, 1: peaking (default 0)

        int gain2 = 64;     // -12..+12 dB
        int freq2 = 28;     // 0EH..36H (default 1CH = 500 Hz)
        int q2 = 7;         // default 0.7

        int gain3 = 64;     // -12..+12 dB
        int freq3 = 34;     // default 22H = 1.0 kHz
        int q3 = 7;         // default 0.7

        int gain4 = 64;     // -12..+12 dB
        int freq4 = 46;     // default 2EH = 4.0 kHz
        int q4 = 7;         // default 0.7

        int gain5 = 64;     // -12..+12 dB
        int freq5 = 52;     // default 34H = 8.0 kHz
        int q5 = 7;         // default 0.7
        int shape5 = 0;     // 0: shelving, 1: peaking (default 0)

        void reset() noexcept
        {
            eqType = 0;
            gain1 = 64; freq1 = 12; q1 = 7; shape1 = 0;
            gain2 = 64; freq2 = 28; q2 = 7;
            gain3 = 64; freq3 = 34; q3 = 7;
            gain4 = 64; freq4 = 46; q4 = 7;
            gain5 = 64; freq5 = 52; q5 = 7; shape5 = 0;
        }

        bool isFlat() const noexcept
        {
            return gain1 == 64 && gain2 == 64 && gain3 == 64 && gain4 == 64 && gain5 == 64;
        }

        void setPreset (int type) noexcept
        {
            eqType = static_cast<uint8_t> (type);
            switch (type)
            {
                case 0: // Flat
                    gain1 = 64; freq1 = 12; q1 = 7; shape1 = 0;
                    gain2 = 64; freq2 = 28; q2 = 7;
                    gain3 = 64; freq3 = 34; q3 = 7;
                    gain4 = 64; freq4 = 46; q4 = 7;
                    gain5 = 64; freq5 = 52; q5 = 7; shape5 = 0;
                    break;
                case 1: // Jazz
                    gain1 = 58; freq1 = 8;  q1 = 7; shape1 = 0;
                    gain2 = 66; freq2 = 16; q2 = 3;
                    gain3 = 68; freq3 = 33; q3 = 3;
                    gain4 = 60; freq4 = 44; q4 = 5;
                    gain5 = 58; freq5 = 50; q5 = 7; shape5 = 0;
                    break;
                case 2: // Pops
                    gain1 = 68; freq1 = 16; q1 = 7; shape1 = 0;
                    gain2 = 60; freq2 = 24; q2 = 20;
                    gain3 = 67; freq3 = 34; q3 = 7;
                    gain4 = 60; freq4 = 40; q4 = 20;
                    gain5 = 70; freq5 = 48; q5 = 7; shape5 = 0;
                    break;
                case 3: // Rock
                    gain1 = 71; freq1 = 16; q1 = 7; shape1 = 0;
                    gain2 = 68; freq2 = 20; q2 = 7;
                    gain3 = 60; freq3 = 36; q3 = 5;
                    gain4 = 68; freq4 = 41; q4 = 10;
                    gain5 = 66; freq5 = 50; q5 = 7; shape5 = 0;
                    break;
                case 4: // Concert
                    gain1 = 67; freq1 = 12; q1 = 7; shape1 = 0;
                    gain2 = 68; freq2 = 24; q2 = 7;
                    gain3 = 64; freq3 = 34; q3 = 5;
                    gain4 = 66; freq4 = 50; q4 = 7;
                    gain5 = 61; freq5 = 52; q5 = 7; shape5 = 0;
                    break;
                default:
                    break;
            }
        }
    };
}
