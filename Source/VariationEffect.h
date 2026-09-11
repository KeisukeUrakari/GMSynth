#pragma once

#include <JuceHeader.h>
#include "XgModel.h"
#include "XgEffectTables.h"

class VariationEffectProcessor
{
public:
    VariationEffectProcessor();
    ~VariationEffectProcessor() = default;

    void prepare (double sampleRate, int maxBlockSize);
    void reset();

    void updateParameters (const xg::VariationParameters& params);
    void setModulationInputs (float mwNorm, float bendNorm, float catNorm, float ac1Norm, float ac2Norm) noexcept;

    void process (const juce::AudioBuffer<float>& inBuffer,
                  juce::AudioBuffer<float>& outBuffer,
                  int numSamples) noexcept;

    enum class DistortionType { Overdrive, Distortion, AmpSim };
    enum class DelayType { LCR, LR, Echo, Cross };

    uint8_t getCurrentTypeMsb() const noexcept { return currentTypeMsb; }
    uint8_t getCurrentTypeLsb() const noexcept { return currentTypeLsb; }

    DistortionType getDistortionTypeForTest() const noexcept { return distType; }
    bool getIsAutoPanForTest() const noexcept { return isAutoPan; }
    float getModDepthForTest() const noexcept { return modDepth; }

    // Phase 2 test accessors
    DelayType getDelayTypeForTest() const noexcept { return delayType; }
    float getDelayCchLevelForTest() const noexcept { return delayCchLevel; }
    float getEchoFbLForTest() const noexcept { return delayFeedbackL; }
    float getEchoFbRForTest() const noexcept { return delayFeedbackR; }
    int getCrossDelayInputSelectForTest() const noexcept { return crossDelayInputSelect; }
    float getFeedbackDelayLSamplesForTest() const noexcept { return delayFeedbackDelayLSamples; }
    float getRotarySpeedHzForTest() const noexcept { return rotarySpeedHz; }
    bool isPostEqActiveForTest() const noexcept { return postEqActive; }

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

    DelayType delayType = DelayType::LCR;
    float delayTimeLSamples = 0.0f;
    float delayTimeRSamples = 0.0f;
    float delayTimeCSamples = 0.0f;
    float delayFeedbackDelayLSamples = 0.0f;
    float delayFeedbackDelayRSamples = 0.0f;
    float delayFeedbackL = 0.0f;
    float delayFeedbackR = 0.0f;
    float delayCchLevel = 0.707f;
    float delayEchoL2Samples = 0.0f;
    float delayEchoR2Samples = 0.0f;
    float delayEcho2Level = 0.0f;
    int crossDelayInputSelect = 2; // 0=L, 1=R, 2=L&R
    float delayDamp = 0.0f;
    std::array<float, 2> delayDampState { 0.0f, 0.0f };

    // --- Variation Reverb (Hall 1/2, Room 1/2/3, Stage 1/2, Plate) ---
    juce::dsp::Reverb variationReverbProcessor;

    // --- Rotary Speaker DSP ---
    static constexpr int maxRotaryDelaySamples = 2048;
    std::array<std::vector<float>, 2> rotaryBuffer;
    int rotaryWritePos = 0;
    float rotaryPhase = 0.0f;
    float rotarySpeedHz = 2.0f;
    float rotaryDepth = 0.5f;

    // --- 2-Band / 3-Band EQ DSP ---
    std::array<juce::IIRFilter, 2> eqLowFilters;
    std::array<juce::IIRFilter, 2> eqMidFilters;
    std::array<juce::IIRFilter, 2> eqHighFilters;
    bool eqFiltersActive = false;

    // --- Post-EQ for Wet Output (Params 11..16 / 6..9) ---
    std::array<juce::IIRFilter, 2> postEqLow;
    std::array<juce::IIRFilter, 2> postEqMid;
    std::array<juce::IIRFilter, 2> postEqHigh;
    bool postEqActive = false;

    // --- Distortion / Overdrive / Amp Sim DSP ---
    float distortionDrive = 1.0f;
    float distortionOutputGain = 1.0f;
    DistortionType distType = DistortionType::Overdrive;
    bool isStereoDistortion = false;

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
    float basePhaserDepth = 0.5f;
    float basePhaserFeedback = 0.0f;

    // --- Tremolo / Auto Pan DSP ---
    float modPhase = 0.0f;
    float modRateHz = 1.0f;
    float modDepth = 0.5f;
    bool isAutoPan = false;

    // --- Chorus DSP ---
    juce::dsp::Chorus<float> chorusProcessor;
    float baseChorusDepth = 0.5f;

    // --- Auto Wah DSP ---
    std::array<juce::IIRFilter, 2> wahFilters;
    float wahLfoPhase = 0.0f;
    float wahLfoRateHz = 1.0f;
    float wahLfoDepth = 0.5f;
    float wahManualCutoff = 0.5f;
    float wahResonance = 2.5f;

    // --- Controller Modulation ---
    float mwDepth = 0.0f;
    float bendDepth = 0.0f;
    float catDepth = 0.0f;
    float ac1Depth = 0.0f;
    float ac2Depth = 0.0f;
    float currentModOffset = 0.0f;

    // Temporary scratch buffer for multi-stage effects
    juce::AudioBuffer<float> tempWetBuffer;

    void processDelay (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processVariationReverb (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processRotarySpeaker (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processEq (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processDistortion (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processFlanger (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processPhaser (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processTremoloAutoPan (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processChorus (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;
    void processAutoWah (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept;

    void updateDelayLcrParameters (const xg::VariationParameters& params);
    void updateDelayLrParameters (const xg::VariationParameters& params);
    void updateEchoParameters (const xg::VariationParameters& params);
    void updateCrossDelayParameters (const xg::VariationParameters& params);
    void updateVariationReverbParameters (const xg::VariationParameters& params);
    void updateRotarySpeakerParameters (const xg::VariationParameters& params);
    void update3BandEqParameters (const xg::VariationParameters& params);
    void update2BandEqParameters (const xg::VariationParameters& params);
    void updateDistortionParameters (const xg::VariationParameters& params);
    void updateFlangerParameters (const xg::VariationParameters& params);
    void updatePhaserParameters (const xg::VariationParameters& params);
    void updateTremoloAutoPanParameters (const xg::VariationParameters& params);
    void updateChorusParameters (const xg::VariationParameters& params);
    void updateAutoWahParameters (const xg::VariationParameters& params);

    void updatePostEq (float lowFreq, float lowGainDb, float midFreq, float midGainDb, float midQ, float highFreq, float highGainDb) noexcept;
    void applyPostEq (float* wetL, float* wetR, int numSamples) noexcept;
    void updateDryWet (uint8_t dwVal, float defaultWetRatio = 0.5f);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VariationEffectProcessor)
};
