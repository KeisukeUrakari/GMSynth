#pragma once

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
}
