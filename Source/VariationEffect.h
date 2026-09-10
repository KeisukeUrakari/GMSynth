#pragma once

#include <JuceHeader.h>
#include "XgModel.h"

class VariationEffectProcessor
{
public:
    VariationEffectProcessor();
    ~VariationEffectProcessor() = default;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    void updateParameters (const xg::VariationParameters& params);

    void process (const juce::AudioBuffer<float>& inBuffer,
                  juce::AudioBuffer<float>& outBuffer,
                  int numSamples) noexcept;

    uint8_t getCurrentTypeMsb() const noexcept { return currentTypeMsb; }
    uint8_t getCurrentTypeLsb() const noexcept { return currentTypeLsb; }

private:
    double sampleRate = 44100.0;
    int maxBlockSize = 512;

    uint8_t currentTypeMsb = 0x00; // 00H = Thru
    uint8_t currentTypeLsb = 0x00;

    // Dry / Wet crossfade
    float dryGain = 1.0f;
    float wetGain = 0.0f;

    // --- Delay DSP ---
    static constexpr int maxDelaySamples = 192000; // ~4s at 48kHz, ~2s at 96kHz
    std::array<std::vector<float>, 2> delayBuffer;
    int delayWritePos = 0;

    float delayTimeLSamples = 0.0f;
    float delayTimeRSamples = 0.0f;
    float delayTimeCSamples = 0.0f;
    float delayFeedback = 0.0f;
    float delayDamp = 0.0f;
    std::array<float, 2> delayDampState { 0.0f, 0.0f };
    bool isCrossDelay = false;
    bool isDelayLCR = false;

    // --- Distortion / Overdrive / Amp Sim DSP ---
    float distortionDrive = 1.0f;
    float distortionOutputGain = 1.0f;
    enum class DistortionType { Overdrive, Distortion, AmpSim };
    DistortionType distType = DistortionType::Overdrive;

    std::array<juce::IIRFilter, 2> distPreFilters;
    std::array<juce::IIRFilter, 2> distPostFilters;
    std::array<juce::IIRFilter, 2> ampSimCabFilters;
    bool distFiltersActive = false;

    // --- Flanger DSP ---
    static constexpr int maxFlangerDelaySamples = 4096;
    std::array<std::vector<float>, 2> flangerBuffer;
    int flangerWritePos = 0;
    float flangerPhase = 0.0f;
    float flangerRateHz = 0.2f;
    float flangerDepthSec = 0.002f;
    float flangerOffsetSec = 0.001f;
    float flangerFeedback = 0.0f;

    // --- Phaser DSP ---
    juce::dsp::Phaser<float> phaserProcessor;

    // --- Tremolo / Auto Pan DSP ---
    float modPhase = 0.0f;
    float modRateHz = 1.0f;
    float modDepth = 0.5f;
    bool isAutoPan = false;

    // --- Chorus DSP ---
    juce::dsp::Chorus<float> chorusProcessor;

    // Temporary scratch buffer for multi-stage effects
    juce::AudioBuffer<float> tempWetBuffer;

    void processDelay (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processDistortion (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processFlanger (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processPhaser (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processTremoloAutoPan (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processChorus (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;

    void updateDelayParameters (const xg::VariationParameters& params);
    void updateDistortionParameters (const xg::VariationParameters& params);
    void updateFlangerParameters (const xg::VariationParameters& params);
    void updatePhaserParameters (const xg::VariationParameters& params);
    void updateTremoloAutoPanParameters (const xg::VariationParameters& params);
    void updateChorusParameters (const xg::VariationParameters& params);
    void updateDryWet (uint8_t dwVal, float defaultWetRatio = 0.5f);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VariationEffectProcessor)
};
