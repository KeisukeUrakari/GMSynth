#pragma once

#include "JuceHeader.h"
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace gmsynth::render
{
constexpr int sampleRate = 48000;
constexpr int blockSize = 512;

struct Tail
{
    enum class Unit { bars, seconds };
    Unit unit = Unit::bars;
    double value = 1.0;
};

struct Event
{
    int64_t sample = 0;
    juce::MidiMessage message;
};

struct Song
{
    std::vector<Event> events;
    int64_t endSample = 0;
    int64_t tailSamples = 0;
    bool releasedAtEof = false;
};

// Throws std::runtime_error for invalid/unsupported input or an unrepresentable duration.
Song readSong (const juce::File&, Tail);
Song readSong (const void*, size_t, Tail);
void applyTailFade (juce::AudioBuffer<float>&, int64_t firstSample,
                    int64_t endSample, int64_t tailSamples);

struct RenderResult
{
    int64_t totalSamples = 0;
    bool clipped = false;
};

RenderResult renderSong (const Song&, const juce::File& soundFont, const juce::File& output);
}
