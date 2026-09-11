#pragma once

#include <array>
#include <cstdint>

namespace xg
{
    // Reverb / Chorus default parameter array (16 parameters: 1..16)
    using EffectDefaultParams8 = std::array<uint8_t, 16>;

    // Variation default parameters: params 1..10 are 14-bit (uint16_t), params 11..16 are 8-bit (uint8_t)
    struct VariationDefaultParams
    {
        std::array<uint16_t, 10> params14Bit {};
        std::array<uint8_t, 6>   params11To16 {};
    };

    namespace defaults
    {
        // ---------------------------------------------------------------------
        // REVERB DEFAULTS (efctparamdeflt.pdf p.39)
        // ---------------------------------------------------------------------
        inline EffectDefaultParams8 getReverbDefaults (uint8_t msb, uint8_t lsb) noexcept
        {
            switch (msb)
            {
                case 0x01: // HALL
                    if (lsb == 0x01) // HALL 2
                        return {{ 25, 10, 28,  6, 46, 0, 0, 0, 0, 40, 13, 3, 74, 7, 64, 0 }};
                    if (lsb == 0x06) // HALL M
                        return {{ 18, 10,  8, 13, 49, 0, 0, 0, 0, 40,  0, 4, 50, 8, 64, 0 }};
                    if (lsb == 0x07) // HALL L
                        return {{ 18, 10, 28,  6, 46, 0, 0, 0, 0, 40, 13, 3, 74, 7, 64, 0 }};
                    // HALL 1 (default)
                    return {{ 18, 10,  8, 13, 49, 0, 0, 0, 0, 40,  0, 4, 50, 8, 64, 0 }};

                case 0x02: // ROOM
                    if (lsb == 0x01) // ROOM 2
                        return {{ 12, 10,  5, 4, 38, 0, 0, 0, 0, 40,  0, 4, 50, 8, 64, 0 }};
                    if (lsb == 0x02) // ROOM 3
                        return {{  9, 10, 47, 5, 36, 0, 0, 0, 0, 40,  0, 4, 60, 8, 64, 0 }};
                    if (lsb == 0x05) // ROOM S
                        return {{ 11, 10,  5, 4, 38, 0, 0, 0, 0, 40,  0, 4, 50, 8, 64, 0 }};
                    if (lsb == 0x06) // ROOM M
                        return {{ 13, 10, 16, 4, 49, 0, 0, 0, 0, 40,  5, 3, 64, 8, 64, 0 }};
                    if (lsb == 0x07) // ROOM L
                        return {{ 15, 10, 47, 5, 36, 0, 0, 0, 0, 40,  0, 4, 60, 8, 64, 0 }};
                    // ROOM 1 (default)
                    return {{  5, 10, 16, 4, 49, 0, 0, 0, 0, 40,  5, 3, 64, 8, 64, 0 }};

                case 0x03: // STAGE
                    if (lsb == 0x01) // STAGE 2
                        return {{ 11, 10, 16, 7, 51, 0, 0, 0, 0, 40,  2, 2, 64, 6, 64, 0 }};
                    // STAGE 1 (default)
                    return {{ 19, 10, 16, 7, 54, 0, 0, 0, 0, 40,  0, 3, 64, 6, 64, 0 }};

                case 0x04: // PLATE
                    if (lsb == 0x07) // GM PLATE
                        return {{ 13, 10,  6, 8, 49, 0, 0, 0, 0, 40,  2, 3, 64, 5, 64, 0 }};
                    // PLATE (default)
                    return {{ 25, 10,  6, 8, 49, 0, 0, 0, 0, 40,  2, 3, 64, 5, 64, 0 }};

                case 0x10: // WHITE ROOM
                    return {{  9,  5, 11, 0, 46, 30, 50, 70,  7, 40, 34, 4, 64, 7, 64, 0 }};
                case 0x11: // TUNNEL
                    return {{ 48,  6, 19, 0, 44, 33, 52, 70, 16, 40, 20, 4, 64, 7, 64, 0 }};
                case 0x12: // CANYON
                    return {{ 59,  6, 63, 0, 45, 34, 62, 91, 13, 40, 25, 4, 64, 4, 64, 0 }};
                case 0x13: // BASEMENT
                    return {{  3,  6,  3, 0, 34, 26, 29, 59, 15, 40, 32, 4, 64, 8, 64, 0 }};

                case 0x00: // NO EFFECT
                default:
                    return {};
            }
        }

        // ---------------------------------------------------------------------
        // CHORUS DEFAULTS (efctparamdeflt.pdf p.39)
        // ---------------------------------------------------------------------
        inline EffectDefaultParams8 getChorusDefaults (uint8_t msb, uint8_t lsb) noexcept
        {
            switch (msb)
            {
                case 0x41: // CHORUS
                    if (lsb == 0x01) // CHORUS 2
                        return {{  8, 63,  64,  30, 0, 28, 62, 42, 58, 64, 46, 64, 10, 0, 0, 0 }};
                    if (lsb == 0x02) // CHORUS 3
                        return {{  4, 44,  64, 110, 0, 28, 64, 46, 66, 64, 46, 64, 10, 0, 0, 0 }};
                    if (lsb == 0x03) // GM CHORUS 1
                        return {{  9, 10,  64, 109, 0, 28, 64, 46, 64, 64, 46, 64, 10, 0, 0, 0 }};
                    if (lsb == 0x04) // GM CHORUS 2
                        return {{ 26, 34,  67, 105, 0, 28, 64, 46, 64, 64, 46, 64, 10, 0, 0, 0 }};
                    if (lsb == 0x05) // GM CHORUS 3
                        return {{  9, 34,  69, 105, 0, 28, 64, 46, 66, 64, 46, 64, 10, 0, 0, 0 }};
                    if (lsb == 0x06) // GM CHORUS 4
                        return {{ 26, 29,  75, 102, 0, 28, 64, 46, 64, 64, 46, 64, 10, 0, 0, 0 }};
                    if (lsb == 0x07) // FB CHORUS
                        return {{  6, 43, 107, 111, 0, 28, 64, 46, 64, 64, 46, 64, 10, 0, 0, 0 }};
                    if (lsb == 0x08) // CHORUS 4
                        return {{  9, 32,  69, 104, 0, 28, 64, 46, 64, 64, 46, 64, 10, 0, 1, 0 }};
                    // CHORUS 1 (default)
                    return {{  6, 54,  77, 106, 0, 28, 64, 46, 64, 64, 46, 64, 10, 0, 0, 0 }};

                case 0x42: // CELESTE
                    if (lsb == 0x01) // CELESTE 2
                        return {{ 28, 18, 90, 2, 0, 28, 62, 42, 60,  84, 40, 68, 10, 0, 0, 0 }};
                    if (lsb == 0x02) // CELESTE 3
                        return {{  4, 63, 44, 2, 0, 28, 64, 46, 68, 127, 40, 68, 10, 0, 0, 0 }};
                    if (lsb == 0x08) // CELESTE 4
                        return {{  8, 29, 64, 0, 0, 28, 64, 51, 66, 127, 40, 68, 10, 0, 1, 0 }};
                    // CELESTE 1 (default)
                    return {{ 12, 32, 64, 0, 0, 28, 64, 46, 64, 127, 40, 68, 10, 0, 0, 0 }};

                case 0x43: // FLANGER
                    if (lsb == 0x01) // FLANGER 2
                        return {{ 32,  17,  26, 2, 0, 28, 64, 46, 60,  96, 40, 64, 10, 4, 0, 0 }};
                    if (lsb == 0x07) // GM FLANGER
                        return {{  3,  21, 120, 1, 0, 28, 64, 46, 64,  96, 40, 64, 10, 4, 0, 0 }};
                    if (lsb == 0x08) // FLANGER 3
                        return {{  4, 109, 109, 2, 0, 28, 64, 46, 64, 127, 40, 64, 10, 4, 0, 0 }};
                    // FLANGER 1 (default)
                    return {{ 14,  14, 104, 2, 0, 28, 64, 46, 64,  96, 40, 64, 10, 4, 0, 0 }};

                case 0x44: // SYMPHONIC
                    return {{ 12,  25,  16, 0, 0, 28, 64, 46, 64, 127, 46, 64, 10, 0, 0, 0 }};

                case 0x00: // NO EFFECT / THRU
                default:
                    return {};
            }
        }

        // ---------------------------------------------------------------------
        // VARIATION DEFAULTS (efctparamdeflt.pdf p.39)
        // ---------------------------------------------------------------------
        inline VariationDefaultParams getVariationDefaults (uint8_t msb, uint8_t lsb) noexcept
        {
            VariationDefaultParams defs {};

            switch (msb)
            {
                case 0x01: // HALL (same as Reverb block)
                case 0x02: // ROOM
                case 0x03: // STAGE
                case 0x04: // PLATE
                case 0x10: // WHITE ROOM
                case 0x11: // TUNNEL
                case 0x12: // CANYON
                case 0x13: // BASEMENT
                {
                    const auto r = getReverbDefaults (msb, lsb);
                    for (size_t i = 0; i < 10; ++i) defs.params14Bit[i] = r[i];
                    for (size_t i = 0; i < 6; ++i)  defs.params11To16[i] = r[10 + i];
                    break;
                }

                case 0x05: // DELAY L,C,R
                    defs.params14Bit = {{ 3333, 1667, 5000, 5000, 74, 100, 10, 0, 0, 32 }};
                    defs.params11To16 = {{ 0, 60, 28, 64, 46, 64 }};
                    break;

                case 0x06: // DELAY L,R
                    defs.params14Bit = {{ 2500, 3750, 3752, 3750, 87, 10, 0, 0, 0, 32 }};
                    defs.params11To16 = {{ 0, 60, 28, 64, 46, 64 }};
                    break;

                case 0x07: // ECHO
                    defs.params14Bit = {{ 1700, 80, 1780, 80, 10, 1700, 1780, 0, 0, 40 }};
                    defs.params11To16 = {{ 0, 60, 28, 64, 46, 64 }};
                    break;

                case 0x08: // CROSS DELAY
                    defs.params14Bit = {{ 1700, 1750, 111, 1, 10, 0, 0, 0, 0, 32 }};
                    defs.params11To16 = {{ 0, 60, 28, 64, 46, 64 }};
                    break;

                case 0x41: // CHORUS (same as Chorus block)
                {
                    const auto c = getChorusDefaults (0x41, lsb);
                    for (size_t i = 0; i < 10; ++i) defs.params14Bit[i] = c[i];
                    for (size_t i = 0; i < 6; ++i)  defs.params11To16[i] = c[10 + i];
                    break;
                }

                case 0x42: // CELESTE
                {
                    const auto c = getChorusDefaults (0x42, lsb);
                    for (size_t i = 0; i < 10; ++i) defs.params14Bit[i] = c[i];
                    for (size_t i = 0; i < 6; ++i)  defs.params11To16[i] = c[10 + i];
                    break;
                }

                case 0x43: // FLANGER
                {
                    const auto c = getChorusDefaults (0x43, lsb);
                    for (size_t i = 0; i < 10; ++i) defs.params14Bit[i] = c[i];
                    for (size_t i = 0; i < 6; ++i)  defs.params11To16[i] = c[10 + i];
                    break;
                }

                case 0x44: // SYMPHONIC
                {
                    const auto c = getChorusDefaults (0x44, lsb);
                    for (size_t i = 0; i < 10; ++i) defs.params14Bit[i] = c[i];
                    for (size_t i = 0; i < 6; ++i)  defs.params11To16[i] = c[10 + i];
                    break;
                }

                case 0x45: // ROTARY SPEAKER
                    defs.params14Bit = {{ 81, 35, 0, 0, 0, 24, 60, 45, 54, 127 }};
                    defs.params11To16 = {{ 33, 52, 30, 0, 0, 0 }};
                    break;

                case 0x46: // TREMOLO
                    defs.params14Bit = {{ 83, 56, 0, 0, 0, 28, 64, 46, 64, 127 }};
                    defs.params11To16 = {{ 40, 64, 10, 64, 0, 0 }};
                    break;

                case 0x47: // AUTOPAN
                    defs.params14Bit = {{ 76, 80, 32, 5, 0, 28, 64, 46, 64, 127 }};
                    defs.params11To16 = {{ 40, 64, 10, 0, 0, 0 }};
                    break;

                case 0x48: // PHASER
                    if (lsb == 0x08) // PHASER 2
                    {
                        defs.params14Bit = {{ 8, 111, 74, 108, 0, 28, 64, 46, 64, 64 }};
                        defs.params11To16 = {{ 5, 0, 4, 0, 0, 0 }};
                    }
                    else // PHASER 1 (default)
                    {
                        defs.params14Bit = {{ 8, 111, 74, 104, 0, 28, 64, 46, 64, 64 }};
                        defs.params11To16 = {{ 6, 1, 0, 0, 0, 0 }};
                    }
                    break;

                case 0x49: // DISTORTION
                    if (lsb == 0x01) // COMP+DISTORTION
                    {
                        defs.params14Bit = {{ 40, 20, 72, 53, 48, 0, 43, 74, 10, 127 }};
                        defs.params11To16 = {{ 120, 6, 2, 100, 4, 0 }};
                    }
                    else if (lsb == 0x08) // STEREO DISTORTION
                    {
                        defs.params14Bit = {{ 18, 27, 71, 48, 84, 0, 32, 66, 10, 127 }};
                        defs.params11To16 = {{ 105, 0, 0, 0, 0, 0 }};
                    }
                    else // DISTORTION (default)
                    {
                        defs.params14Bit = {{ 40, 20, 72, 53, 48, 0, 43, 74, 10, 127 }};
                        defs.params11To16 = {{ 120, 0, 0, 0, 0, 0 }};
                    }
                    break;

                case 0x4A: // OVER DRIVE
                    if (lsb == 0x08) // STEREO OVER DRIVE
                    {
                        defs.params14Bit = {{ 10, 24, 69, 46, 105, 0, 41, 66, 10, 127 }};
                        defs.params11To16 = {{ 104, 0, 0, 0, 0, 0 }};
                    }
                    else // OVER DRIVE (default)
                    {
                        defs.params14Bit = {{ 29, 24, 68, 45, 55, 0, 41, 72, 10, 127 }};
                        defs.params11To16 = {{ 104, 0, 0, 0, 0, 0 }};
                    }
                    break;

                case 0x4B: // AMP SIMULATOR
                    if (lsb == 0x08) // STEREO AMP SIMULATOR
                    {
                        defs.params14Bit = {{ 16, 2, 46, 119, 0, 0, 0, 0, 0, 127 }};
                        defs.params11To16 = {{ 106, 0, 0, 0, 0, 0 }};
                    }
                    else // AMP SIMULATOR (default)
                    {
                        defs.params14Bit = {{ 39, 1, 48, 55, 0, 0, 0, 0, 0, 127 }};
                        defs.params11To16 = {{ 112, 0, 0, 0, 0, 0 }};
                    }
                    break;

                case 0x4C: // 3-BAND EQ
                    defs.params14Bit = {{ 70, 34, 60, 10, 70, 28, 46, 0, 0, 127 }};
                    defs.params11To16 = {{ 0, 0, 0, 0, 0, 0 }};
                    break;

                case 0x4D: // 2-BAND EQ
                    defs.params14Bit = {{ 28, 70, 46, 70, 0, 0, 0, 0, 0, 127 }};
                    defs.params11To16 = {{ 34, 64, 10, 0, 0, 0 }};
                    break;

                case 0x4E: // AUTO WAH
                    defs.params14Bit = {{ 70, 56, 39, 25, 0, 28, 66, 46, 64, 127 }};
                    defs.params11To16 = {{ 0, 0, 0, 0, 0, 0 }};
                    break;

                case 0x00: // NO EFFECT / THRU
                default:
                    break;
            }

            return defs;
        }
    } // namespace defaults
} // namespace xg
