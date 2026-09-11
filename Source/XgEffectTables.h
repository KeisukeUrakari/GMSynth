#pragma once

#include <array>
#include <algorithm>
#include <cstdint>

namespace xg
{
namespace tables
{
    // Table#1: LFO Frequency (0..127 -> 0.00Hz .. 39.7Hz)
    inline constexpr std::array<float, 128> table1_LfoFrequency = {
        0.0000f, 0.0400f, 0.0800f, 0.1300f, 0.1700f, 0.2100f, 0.2500f, 0.2900f,
        0.3400f, 0.3800f, 0.4200f, 0.4600f, 0.5100f, 0.5500f, 0.5900f, 0.6300f,
        0.6700f, 0.7200f, 0.7600f, 0.8000f, 0.8400f, 0.8800f, 0.9300f, 0.9700f,
        1.0100f, 1.0500f, 1.0900f, 1.1400f, 1.1800f, 1.2200f, 1.2600f, 1.3000f,
        1.3500f, 1.3900f, 1.4300f, 1.4700f, 1.5100f, 1.5600f, 1.6000f, 1.6400f,
        1.6800f, 1.7200f, 1.7700f, 1.8100f, 1.8500f, 1.8900f, 1.9400f, 1.9800f,
        2.0200f, 2.0600f, 2.1000f, 2.1500f, 2.1900f, 2.2300f, 2.2700f, 2.3100f,
        2.3600f, 2.4000f, 2.4400f, 2.4800f, 2.5200f, 2.5700f, 2.6100f, 2.6500f,
        2.6900f, 2.7800f, 2.8600f, 2.9400f, 3.0300f, 3.1100f, 3.2000f, 3.2800f,
        3.3700f, 3.4500f, 3.5300f, 3.6200f, 3.7000f, 3.8700f, 4.0400f, 4.2100f,
        4.3700f, 4.5400f, 4.7100f, 4.8800f, 5.0500f, 5.2200f, 5.3800f, 5.5500f,
        5.7200f, 6.0600f, 6.3900f, 6.7300f, 7.0700f, 7.4000f, 7.7400f, 8.0800f,
        8.4100f, 8.7500f, 9.0800f, 9.4200f, 9.7600f, 10.1000f, 10.8000f, 11.4000f,
        12.1000f, 12.8000f, 13.5000f, 14.1000f, 14.8000f, 15.5000f, 16.2000f, 16.8000f,
        17.5000f, 18.2000f, 19.5000f, 20.9000f, 22.2000f, 23.6000f, 24.9000f, 26.2000f,
        27.6000f, 28.9000f, 30.3000f, 31.6000f, 33.0000f, 34.3000f, 37.0000f, 39.7000f,
    };

    // Table#2: Modulation Delay Offset (0..127 -> 0.0ms .. 50.0ms)
    inline constexpr std::array<float, 128> table2_ModDelayOffset = {
        0.0000f, 0.1000f, 0.2000f, 0.3000f, 0.4000f, 0.5000f, 0.6000f, 0.7000f,
        0.8000f, 0.9000f, 1.0000f, 1.1000f, 1.2000f, 1.3000f, 1.4000f, 1.5000f,
        1.6000f, 1.7000f, 1.8000f, 1.9000f, 2.0000f, 2.1000f, 2.2000f, 2.3000f,
        2.4000f, 2.5000f, 2.6000f, 2.7000f, 2.8000f, 2.9000f, 3.0000f, 3.1000f,
        3.2000f, 3.3000f, 3.4000f, 3.5000f, 3.6000f, 3.7000f, 3.8000f, 3.9000f,
        4.0000f, 4.1000f, 4.2000f, 4.3000f, 4.4000f, 4.5000f, 4.6000f, 4.7000f,
        4.8000f, 4.9000f, 5.0000f, 5.1000f, 5.2000f, 5.3000f, 5.4000f, 5.5000f,
        5.6000f, 5.7000f, 5.8000f, 5.9000f, 6.0000f, 6.1000f, 6.2000f, 6.3000f,
        6.4000f, 6.5000f, 6.6000f, 6.7000f, 6.8000f, 6.9000f, 7.0000f, 7.1000f,
        7.2000f, 7.3000f, 7.4000f, 7.5000f, 7.6000f, 7.7000f, 7.8000f, 7.9000f,
        8.0000f, 8.1000f, 8.2000f, 8.3000f, 8.4000f, 8.5000f, 8.6000f, 8.7000f,
        8.8000f, 8.9000f, 9.0000f, 9.1000f, 9.2000f, 9.3000f, 9.4000f, 9.5000f,
        9.6000f, 9.7000f, 9.8000f, 9.9000f, 10.0000f, 11.1000f, 12.2000f, 13.3000f,
        14.4000f, 15.5000f, 17.1000f, 18.6000f, 20.2000f, 21.8000f, 23.3000f, 24.9000f,
        26.5000f, 28.0000f, 29.6000f, 31.2000f, 32.8000f, 34.3000f, 35.9000f, 37.5000f,
        39.0000f, 40.6000f, 42.2000f, 43.7000f, 45.3000f, 46.9000f, 48.4000f, 50.0000f,
    };

    // Table#3: EQ / Filter Frequency (0..60 -> 20Hz .. 20.0kHz, 0=THRU/20Hz, 60=THRU/20kHz)
    inline constexpr std::array<float, 61> table3_EqFrequency = {
        20.0000f, 22.0000f, 25.0000f, 28.0000f, 32.0000f, 36.0000f, 40.0000f, 45.0000f,
        50.0000f, 56.0000f, 63.0000f, 70.0000f, 80.0000f, 90.0000f, 100.0000f, 110.0000f,
        125.0000f, 140.0000f, 160.0000f, 180.0000f, 200.0000f, 225.0000f, 250.0000f, 280.0000f,
        315.0000f, 355.0000f, 400.0000f, 450.0000f, 500.0000f, 560.0000f, 630.0000f, 700.0000f,
        800.0000f, 900.0000f, 1000.0000f, 1100.0000f, 1200.0000f, 1400.0000f, 1600.0000f, 1800.0000f,
        2000.0000f, 2200.0000f, 2500.0000f, 2800.0000f, 3200.0000f, 3600.0000f, 4000.0000f, 4500.0000f,
        5000.0000f, 5600.0000f, 6300.0000f, 7000.0000f, 8000.0000f, 9000.0000f, 10000.0000f, 11000.0000f,
        12000.0000f, 14000.0000f, 16000.0000f, 18000.0000f, 20000.0000f,
    };

    // Table#4: Reverb Time (0..69 -> 0.3s .. 30.0s)
    inline constexpr std::array<float, 70> table4_ReverbTime = {
        0.3000f, 0.4000f, 0.5000f, 0.6000f, 0.7000f, 0.8000f, 0.9000f, 1.0000f,
        1.1000f, 1.2000f, 1.3000f, 1.4000f, 1.5000f, 1.6000f, 1.7000f, 1.8000f,
        1.9000f, 2.0000f, 2.1000f, 2.2000f, 2.3000f, 2.4000f, 2.5000f, 2.6000f,
        2.7000f, 2.8000f, 2.9000f, 3.0000f, 3.1000f, 3.2000f, 3.3000f, 3.4000f,
        3.5000f, 3.6000f, 3.7000f, 3.8000f, 3.9000f, 4.0000f, 4.1000f, 4.2000f,
        4.3000f, 4.4000f, 4.5000f, 4.6000f, 4.7000f, 4.8000f, 4.9000f, 5.0000f,
        5.5000f, 6.0000f, 6.5000f, 7.0000f, 7.5000f, 8.0000f, 8.5000f, 9.0000f,
        9.5000f, 10.0000f, 11.0000f, 12.0000f, 13.0000f, 14.0000f, 15.0000f, 16.0000f,
        17.0000f, 18.0000f, 19.0000f, 20.0000f, 25.0000f, 30.0000f,
    };

    // Table#5: Delay Time (0..127 -> 0.1ms .. 200.0ms)
    inline constexpr std::array<float, 128> table5_DelayTime200 = {
        0.1000f, 1.7000f, 3.2000f, 4.8000f, 6.4000f, 8.0000f, 9.5000f, 11.1000f,
        12.7000f, 14.3000f, 15.8000f, 17.4000f, 19.0000f, 20.6000f, 22.1000f, 23.7000f,
        25.3000f, 26.9000f, 28.4000f, 30.0000f, 31.6000f, 33.2000f, 34.7000f, 36.3000f,
        37.9000f, 39.5000f, 41.0000f, 42.6000f, 44.2000f, 45.7000f, 47.3000f, 48.9000f,
        50.5000f, 52.0000f, 53.6000f, 55.2000f, 56.8000f, 58.3000f, 59.9000f, 61.5000f,
        63.1000f, 64.6000f, 66.2000f, 67.8000f, 69.4000f, 70.9000f, 72.5000f, 74.1000f,
        75.7000f, 77.2000f, 78.8000f, 80.4000f, 81.9000f, 83.5000f, 85.1000f, 86.7000f,
        88.2000f, 89.8000f, 91.4000f, 93.0000f, 94.5000f, 96.1000f, 97.7000f, 99.3000f,
        100.8000f, 102.4000f, 104.0000f, 105.6000f, 107.1000f, 108.7000f, 110.3000f, 111.9000f,
        113.4000f, 115.0000f, 116.6000f, 118.2000f, 119.7000f, 121.3000f, 122.9000f, 124.4000f,
        126.0000f, 127.6000f, 129.2000f, 130.7000f, 132.3000f, 133.9000f, 135.5000f, 137.0000f,
        138.6000f, 140.2000f, 141.8000f, 143.3000f, 144.9000f, 146.5000f, 148.1000f, 149.6000f,
        151.2000f, 152.8000f, 154.4000f, 155.9000f, 157.5000f, 159.1000f, 160.6000f, 162.2000f,
        163.8000f, 165.4000f, 166.9000f, 168.5000f, 170.1000f, 171.7000f, 173.2000f, 174.8000f,
        176.4000f, 178.0000f, 179.5000f, 181.1000f, 182.7000f, 184.3000f, 185.8000f, 187.4000f,
        189.0000f, 190.6000f, 192.1000f, 193.7000f, 195.3000f, 196.9000f, 198.4000f, 200.0000f,
    };

    // Table#6: Room Size (0..44 -> 0.1 .. 7.0)
    inline constexpr std::array<float, 45> table6_RoomSize = {
        0.1000f, 0.3000f, 0.4000f, 0.6000f, 0.7000f, 0.9000f, 1.0000f, 1.2000f,
        1.4000f, 1.5000f, 1.7000f, 1.8000f, 2.0000f, 2.1000f, 2.3000f, 2.5000f,
        2.6000f, 2.8000f, 2.9000f, 3.1000f, 3.2000f, 3.4000f, 3.5000f, 3.7000f,
        3.9000f, 4.0000f, 4.2000f, 4.3000f, 4.5000f, 4.6000f, 4.8000f, 5.0000f,
        5.1000f, 5.3000f, 5.4000f, 5.6000f, 5.7000f, 5.9000f, 6.1000f, 6.2000f,
        6.4000f, 6.5000f, 6.7000f, 6.8000f, 7.0000f,
    };

    // Table#7: Delay Time (0..127 -> 0.1ms .. 400.0ms)
    inline constexpr std::array<float, 128> table7_DelayTime400 = {
        0.1000f, 3.2000f, 6.4000f, 9.5000f, 12.7000f, 15.8000f, 19.0000f, 22.1000f,
        25.3000f, 28.4000f, 31.6000f, 34.7000f, 37.9000f, 41.0000f, 44.2000f, 47.3000f,
        50.5000f, 53.6000f, 56.8000f, 59.9000f, 63.1000f, 66.2000f, 69.4000f, 72.5000f,
        75.7000f, 78.8000f, 82.0000f, 85.1000f, 88.3000f, 91.4000f, 94.6000f, 97.7000f,
        100.9000f, 104.0000f, 107.2000f, 110.3000f, 113.5000f, 116.6000f, 119.8000f, 122.9000f,
        126.1000f, 129.2000f, 132.4000f, 135.5000f, 138.6000f, 141.8000f, 144.9000f, 148.1000f,
        151.2000f, 154.4000f, 157.5000f, 160.7000f, 163.8000f, 167.0000f, 170.1000f, 173.3000f,
        176.4000f, 179.6000f, 182.7000f, 185.9000f, 189.0000f, 192.2000f, 195.3000f, 198.5000f,
        201.6000f, 204.8000f, 207.9000f, 211.1000f, 214.2000f, 217.4000f, 220.5000f, 223.7000f,
        226.8000f, 230.0000f, 233.1000f, 236.3000f, 239.4000f, 242.6000f, 245.7000f, 248.9000f,
        252.0000f, 255.2000f, 258.3000f, 261.5000f, 264.6000f, 267.7000f, 270.9000f, 274.0000f,
        277.2000f, 280.3000f, 283.5000f, 286.6000f, 289.8000f, 292.9000f, 296.1000f, 299.2000f,
        302.4000f, 305.5000f, 308.7000f, 311.8000f, 315.0000f, 318.1000f, 321.3000f, 324.4000f,
        327.6000f, 330.7000f, 333.9000f, 337.0000f, 340.2000f, 343.3000f, 346.5000f, 349.6000f,
        352.8000f, 355.9000f, 359.1000f, 362.2000f, 365.4000f, 368.5000f, 371.7000f, 374.8000f,
        378.0000f, 381.1000f, 384.3000f, 387.4000f, 390.6000f, 393.7000f, 396.9000f, 400.0000f,
    };

    // Table#8: Compressor Attack Time (0..19 -> 1ms .. 40ms)
    inline constexpr std::array<float, 20> table8_CompAttackTime = {
        1.0000f, 2.0000f, 3.0000f, 4.0000f, 5.0000f, 6.0000f, 7.0000f, 8.0000f,
        9.0000f, 10.0000f, 12.0000f, 14.0000f, 16.0000f, 18.0000f, 20.0000f, 23.0000f,
        26.0000f, 30.0000f, 35.0000f, 40.0000f,
    };

    // Table#9: Compressor Release Time (0..15 -> 10ms .. 680ms)
    inline constexpr std::array<float, 16> table9_CompReleaseTime = {
        10.0000f, 15.0000f, 25.0000f, 35.0000f, 45.0000f, 55.0000f, 65.0000f, 75.0000f,
        85.0000f, 100.0000f, 115.0000f, 140.0000f, 170.0000f, 230.0000f, 340.0000f, 680.0000f,
    };

    // Table#10: Compressor Ratio (0..7 -> 1.0 .. 20.0)
    inline constexpr std::array<float, 8> table10_CompRatio = {
        1.0000f, 1.5000f, 2.0000f, 3.0000f, 5.0000f, 7.0000f, 10.0000f, 20.0000f,
    };

    // Table#11: Reverb Width / Depth / Height (0..104 -> 0.5m .. 30.2m)
    inline constexpr std::array<float, 105> table11_ReverbDimension = {
        0.5000f, 0.8000f, 1.0000f, 1.3000f, 1.5000f, 1.8000f, 2.0000f, 2.3000f,
        2.6000f, 2.8000f, 3.1000f, 3.3000f, 3.6000f, 3.9000f, 4.1000f, 4.4000f,
        4.6000f, 4.9000f, 5.2000f, 5.4000f, 5.7000f, 5.9000f, 6.2000f, 6.5000f,
        6.7000f, 7.0000f, 7.2000f, 7.5000f, 7.8000f, 8.0000f, 8.3000f, 8.6000f,
        8.8000f, 9.1000f, 9.4000f, 9.6000f, 9.9000f, 10.2000f, 10.4000f, 10.7000f,
        11.0000f, 11.2000f, 11.5000f, 11.8000f, 12.1000f, 12.3000f, 12.6000f, 12.9000f,
        13.1000f, 13.4000f, 13.7000f, 14.0000f, 14.2000f, 14.5000f, 14.8000f, 15.1000f,
        15.4000f, 15.6000f, 15.9000f, 16.2000f, 16.5000f, 16.8000f, 17.1000f, 17.3000f,
        17.6000f, 17.9000f, 18.2000f, 18.5000f, 18.8000f, 19.1000f, 19.4000f, 19.7000f,
        20.0000f, 20.2000f, 20.5000f, 20.8000f, 21.1000f, 21.4000f, 21.7000f, 22.0000f,
        22.4000f, 22.7000f, 23.0000f, 23.3000f, 23.6000f, 23.9000f, 24.2000f, 24.5000f,
        24.9000f, 25.2000f, 25.5000f, 25.8000f, 26.1000f, 26.5000f, 26.8000f, 27.1000f,
        27.5000f, 27.8000f, 28.1000f, 28.5000f, 28.8000f, 29.2000f, 29.5000f, 29.9000f,
        30.2000f,
    };

    inline float lookupLfoFrequency (int data) noexcept
    {
        const int idx = std::clamp (data, 0, static_cast<int> (table1_LfoFrequency.size() - 1));
        return table1_LfoFrequency[static_cast<size_t> (idx)];
    }

    inline float lookupModDelayOffset (int data) noexcept
    {
        const int idx = std::clamp (data, 0, static_cast<int> (table2_ModDelayOffset.size() - 1));
        return table2_ModDelayOffset[static_cast<size_t> (idx)];
    }

    inline float lookupEqFrequency (int data) noexcept
    {
        const int idx = std::clamp (data, 0, static_cast<int> (table3_EqFrequency.size() - 1));
        return table3_EqFrequency[static_cast<size_t> (idx)];
    }

    inline float lookupReverbTime (int data) noexcept
    {
        const int idx = std::clamp (data, 0, static_cast<int> (table4_ReverbTime.size() - 1));
        return table4_ReverbTime[static_cast<size_t> (idx)];
    }

    inline float lookupDelayTime200 (int data) noexcept
    {
        const int idx = std::clamp (data, 0, static_cast<int> (table5_DelayTime200.size() - 1));
        return table5_DelayTime200[static_cast<size_t> (idx)];
    }

    inline float lookupRoomSize (int data) noexcept
    {
        const int idx = std::clamp (data, 0, static_cast<int> (table6_RoomSize.size() - 1));
        return table6_RoomSize[static_cast<size_t> (idx)];
    }

    inline float lookupDelayTime400 (int data) noexcept
    {
        const int idx = std::clamp (data, 0, static_cast<int> (table7_DelayTime400.size() - 1));
        return table7_DelayTime400[static_cast<size_t> (idx)];
    }

    inline float lookupCompAttackTime (int data) noexcept
    {
        const int idx = std::clamp (data, 0, static_cast<int> (table8_CompAttackTime.size() - 1));
        return table8_CompAttackTime[static_cast<size_t> (idx)];
    }

    inline float lookupCompReleaseTime (int data) noexcept
    {
        const int idx = std::clamp (data, 0, static_cast<int> (table9_CompReleaseTime.size() - 1));
        return table9_CompReleaseTime[static_cast<size_t> (idx)];
    }

    inline float lookupCompRatio (int data) noexcept
    {
        const int idx = std::clamp (data, 0, static_cast<int> (table10_CompRatio.size() - 1));
        return table10_CompRatio[static_cast<size_t> (idx)];
    }

    inline float lookupReverbDimension (int data) noexcept
    {
        const int idx = std::clamp (data, 0, static_cast<int> (table11_ReverbDimension.size() - 1));
        return table11_ReverbDimension[static_cast<size_t> (idx)];
    }

    inline float decodeEqGain (int data) noexcept
    {
        if (data >= 52 && data <= 76)
            return static_cast<float> (data - 64);
        return 0.0f;
    }

} // namespace tables
} // namespace xg
