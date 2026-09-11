#include "VariationEffect.h"
#include "XgEffectTables.h"
#include <cmath>

namespace
{
    constexpr float pi = 3.14159265358979323846f;
    constexpr float twoPi = 2.0f * pi;
}

VariationEffectProcessor::VariationEffectProcessor()
{
    for (auto& buf : delayBuffer)
        buf.resize (static_cast<size_t> (maxDelaySamples), 0.0f);

    for (auto& buf : flangerBuffer)
        buf.resize (static_cast<size_t> (maxFlangerDelaySamples), 0.0f);

    for (auto& buf : rotaryBuffer)
        buf.resize (static_cast<size_t> (maxRotaryDelaySamples), 0.0f);
}

void VariationEffectProcessor::prepare (double newSampleRate, int newMaxBlockSize)
{
    sampleRate = juce::jmax (8000.0, newSampleRate);
    maxBlockSize = juce::jmax (1, newMaxBlockSize);

    for (auto& buf : delayBuffer)
    {
        buf.resize (static_cast<size_t> (maxDelaySamples), 0.0f);
        std::fill (buf.begin(), buf.end(), 0.0f);
    }
    delayWritePos = 0;
    delayDampState.fill (0.0f);

    for (auto& buf : flangerBuffer)
    {
        buf.resize (static_cast<size_t> (maxFlangerDelaySamples), 0.0f);
        std::fill (buf.begin(), buf.end(), 0.0f);
    }
    flangerWritePos = 0;
    flangerPhase = 0.0f;
    modPhase = 0.0f;

    for (auto& buf : rotaryBuffer)
    {
        buf.resize (static_cast<size_t> (maxRotaryDelaySamples), 0.0f);
        std::fill (buf.begin(), buf.end(), 0.0f);
    }
    rotaryWritePos = 0;
    rotaryPhase = 0.0f;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (maxBlockSize);
    spec.numChannels = 2;

    for (auto& ch : phaserFilterState)
        ch.fill (0.0f);
    phaserLastOutput.fill (0.0f);

    chorusProcessor.prepare (spec);
    chorusProcessor.reset();

    variationReverbProcessor.prepare (spec);
    variationReverbProcessor.reset();

    for (auto& buf : reverbInitialDelayBuffer)
        buf.assign (static_cast<size_t> (maxReverbDelayBufferSize), 0.0f);
    for (auto& buf : reverbPostDelayBuffer)
        buf.assign (static_cast<size_t> (maxReverbDelayBufferSize), 0.0f);
    reverbInitialDelayWritePos = 0;
    reverbPostDelayWritePos = 0;
    reverbErBuffer.setSize (2, maxBlockSize);
    reverbErBuffer.clear();
    reverbLateInputBuffer.setSize (2, maxBlockSize);
    reverbLateInputBuffer.clear();

    tempWetBuffer.setSize (2, maxBlockSize);
    tempWetBuffer.clear();

    reset();
}

void VariationEffectProcessor::reset()
{
    for (auto& buf : delayBuffer)
        std::fill (buf.begin(), buf.end(), 0.0f);
    delayWritePos = 0;
    delayDampState.fill (0.0f);

    for (auto& buf : reverbInitialDelayBuffer)
        std::fill (buf.begin(), buf.end(), 0.0f);
    for (auto& buf : reverbPostDelayBuffer)
        std::fill (buf.begin(), buf.end(), 0.0f);
    reverbInitialDelayWritePos = 0;
    reverbPostDelayWritePos = 0;
    reverbFbHighDampState.fill (0.0f);
    for (auto& f : reverbHpfFilters) f.reset();

    for (auto& buf : flangerBuffer)
        std::fill (buf.begin(), buf.end(), 0.0f);
    flangerWritePos = 0;
    flangerPhase = 0.0f;
    modPhase = 0.0f;

    for (auto& buf : rotaryBuffer)
        std::fill (buf.begin(), buf.end(), 0.0f);
    rotaryWritePos = 0;
    rotaryPhase = 0.0f;

    for (auto& f : distPreFilters) f.reset();
    for (auto& f : distPostFilters) f.reset();
    for (auto& f : ampSimCabFilters) f.reset();
    for (auto& f : wahFilters) f.reset();
    for (auto& f : eqLowFilters) f.reset();
    for (auto& f : eqMidFilters) f.reset();
    for (auto& f : eqHighFilters) f.reset();
    for (auto& f : postEqLow) f.reset();
    for (auto& f : postEqMid) f.reset();
    for (auto& f : postEqHigh) f.reset();
    wahLfoPhase = 0.0f;
    currentModOffset = 0.0f;
    eqFiltersActive = false;
    postEqActive = false;

    phaserPhase = 0.0f;
    for (auto& ch : phaserFilterState)
        ch.fill (0.0f);
    phaserLastOutput.fill (0.0f);
    chorusProcessor.reset();
    variationReverbProcessor.reset();
    compEnvelope.fill (0.0f);
}

void VariationEffectProcessor::setModulationInputs (float mwNorm, float bendNorm, float catNorm, float ac1Norm, float ac2Norm) noexcept
{
    const float targetMod = mwNorm * mwDepth
                          + bendNorm * bendDepth
                          + catNorm * catDepth
                          + ac1Norm * ac1Depth
                          + ac2Norm * ac2Depth;
    currentModOffset = juce::jlimit (-2.0f, 2.0f, targetMod);
}

void VariationEffectProcessor::updateDryWet (uint8_t dwVal, float defaultWetRatio)
{
    if (dwVal == 0)
    {
        wetGain = defaultWetRatio;
        dryGain = 1.0f - defaultWetRatio;
        return;
    }

    const auto val = juce::jlimit (1, 127, static_cast<int> (dwVal));
    if (val <= 64)
    {
        dryGain = 1.0f;
        wetGain = static_cast<float> (val - 1) / 63.0f;
    }
    else
    {
        wetGain = 1.0f;
        dryGain = static_cast<float> (127 - val) / 63.0f;
    }
}

void VariationEffectProcessor::updatePostEq (float lowFreq, float lowGainDb, float midFreq, float midGainDb, float midQ, float highFreq, float highGainDb) noexcept
{
    const double nyquist = sampleRate * 0.49;
    bool active = false;

    if (std::abs (lowGainDb) > 0.05f)
    {
        const auto f = juce::jlimit (20.0, nyquist, static_cast<double> (lowFreq));
        const auto coeff = juce::IIRCoefficients::makeLowShelf (sampleRate, f, 0.707, juce::Decibels::decibelsToGain (lowGainDb));
        for (auto& flt : postEqLow) flt.setCoefficients (coeff);
        active = true;
    }
    else
    {
        for (auto& flt : postEqLow) flt.makeInactive();
    }

    if (std::abs (midGainDb) > 0.05f)
    {
        const auto f = juce::jlimit (20.0, nyquist, static_cast<double> (midFreq));
        const auto q = juce::jlimit (0.1, 20.0, static_cast<double> (midQ));
        const auto coeff = juce::IIRCoefficients::makePeakFilter (sampleRate, f, q, juce::Decibels::decibelsToGain (midGainDb));
        for (auto& flt : postEqMid) flt.setCoefficients (coeff);
        active = true;
    }
    else
    {
        for (auto& flt : postEqMid) flt.makeInactive();
    }

    if (std::abs (highGainDb) > 0.05f)
    {
        const auto f = juce::jlimit (20.0, nyquist, static_cast<double> (highFreq));
        const auto coeff = juce::IIRCoefficients::makeHighShelf (sampleRate, f, 0.707, juce::Decibels::decibelsToGain (highGainDb));
        for (auto& flt : postEqHigh) flt.setCoefficients (coeff);
        active = true;
    }
    else
    {
        for (auto& flt : postEqHigh) flt.makeInactive();
    }

    postEqActive = active;
}

void VariationEffectProcessor::applyPostEq (float* wetL, float* wetR, int numSamples) noexcept
{
    if (! postEqActive) return;
    postEqLow[0].processSamples (wetL, numSamples);
    postEqLow[1].processSamples (wetR, numSamples);
    postEqMid[0].processSamples (wetL, numSamples);
    postEqMid[1].processSamples (wetR, numSamples);
    postEqHigh[0].processSamples (wetL, numSamples);
    postEqHigh[1].processSamples (wetR, numSamples);
}

void VariationEffectProcessor::updateDelayLcrParameters (const xg::VariationParameters& params)
{
    delayType = DelayType::LCR;

    float timeLMs = params.parameters14Bit[0] > 0 ? static_cast<float> (params.parameters14Bit[0]) * 0.1f : 333.3f;
    timeLMs = juce::jlimit (0.1f, 1486.0f, timeLMs);
    delayTimeLSamples = static_cast<float> (timeLMs * 0.001f * sampleRate);

    float timeRMs = params.parameters14Bit[1] > 0 ? static_cast<float> (params.parameters14Bit[1]) * 0.1f : 166.7f;
    timeRMs = juce::jlimit (0.1f, 1486.0f, timeRMs);
    delayTimeRSamples = static_cast<float> (timeRMs * 0.001f * sampleRate);

    float timeCMs = params.parameters14Bit[2] > 0 ? static_cast<float> (params.parameters14Bit[2]) * 0.1f : 500.0f;
    timeCMs = juce::jlimit (0.1f, 1486.0f, timeCMs);
    delayTimeCSamples = static_cast<float> (timeCMs * 0.001f * sampleRate);

    float fbDelayMs = params.parameters14Bit[3] > 0 ? static_cast<float> (params.parameters14Bit[3]) * 0.1f : 500.0f;
    fbDelayMs = juce::jlimit (0.1f, 1486.0f, fbDelayMs);
    delayFeedbackDelayLSamples = static_cast<float> (fbDelayMs * 0.001f * sampleRate);
    delayFeedbackDelayRSamples = delayFeedbackDelayLSamples;

    const auto fbVal = params.parameters14Bit[4] > 0 ? static_cast<int> (params.parameters14Bit[4]) : 64;
    delayFeedbackL = juce::jlimit (-0.95f, 0.95f, static_cast<float> (fbVal - 64) / 64.0f * 0.9f);
    delayFeedbackR = delayFeedbackL;

    const auto cchVal = params.parameters14Bit[5] <= 127 ? static_cast<int> (params.parameters14Bit[5]) : 100;
    delayCchLevel = static_cast<float> (cchVal) / 127.0f;

    const auto dampVal = params.parameters14Bit[6] > 0 ? static_cast<int> (params.parameters14Bit[6]) : 10;
    delayDamp = juce::jlimit (0.0f, 0.9f, 1.0f - (static_cast<float> (dampVal) / 10.0f));

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.4f);

    const float eqLowFreq = xg::tables::lookupEqFrequency (params.parameters11To16[2]);
    const float eqLowGain = xg::tables::decodeEqGain (params.parameters11To16[3]);
    const float eqHighFreq = xg::tables::lookupEqFrequency (params.parameters11To16[4]);
    const float eqHighGain = xg::tables::decodeEqGain (params.parameters11To16[5]);
    updatePostEq (eqLowFreq, eqLowGain, 1000.0f, 0.0f, 1.0f, eqHighFreq, eqHighGain);
}

void VariationEffectProcessor::updateDelayLrParameters (const xg::VariationParameters& params)
{
    delayType = DelayType::LR;

    float timeLMs = params.parameters14Bit[0] > 0 ? static_cast<float> (params.parameters14Bit[0]) * 0.1f : 250.0f;
    timeLMs = juce::jlimit (0.1f, 1486.0f, timeLMs);
    delayTimeLSamples = static_cast<float> (timeLMs * 0.001f * sampleRate);

    float timeRMs = params.parameters14Bit[1] > 0 ? static_cast<float> (params.parameters14Bit[1]) * 0.1f : 375.0f;
    timeRMs = juce::jlimit (0.1f, 1486.0f, timeRMs);
    delayTimeRSamples = static_cast<float> (timeRMs * 0.001f * sampleRate);

    float fbDelay1Ms = params.parameters14Bit[2] > 0 ? static_cast<float> (params.parameters14Bit[2]) * 0.1f : 375.0f;
    fbDelay1Ms = juce::jlimit (0.1f, 1486.0f, fbDelay1Ms);
    delayFeedbackDelayLSamples = static_cast<float> (fbDelay1Ms * 0.001f * sampleRate);

    float fbDelay2Ms = params.parameters14Bit[3] > 0 ? static_cast<float> (params.parameters14Bit[3]) * 0.1f : 375.0f;
    fbDelay2Ms = juce::jlimit (0.1f, 1486.0f, fbDelay2Ms);
    delayFeedbackDelayRSamples = static_cast<float> (fbDelay2Ms * 0.001f * sampleRate);

    const auto fbVal = params.parameters14Bit[4] > 0 ? static_cast<int> (params.parameters14Bit[4]) : 64;
    delayFeedbackL = juce::jlimit (-0.95f, 0.95f, static_cast<float> (fbVal - 64) / 64.0f * 0.9f);
    delayFeedbackR = delayFeedbackL;

    const auto dampVal = params.parameters14Bit[5] > 0 ? static_cast<int> (params.parameters14Bit[5]) : 10;
    delayDamp = juce::jlimit (0.0f, 0.9f, 1.0f - (static_cast<float> (dampVal) / 10.0f));

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.4f);

    const float eqLowFreq = xg::tables::lookupEqFrequency (params.parameters11To16[2]);
    const float eqLowGain = xg::tables::decodeEqGain (params.parameters11To16[3]);
    const float eqHighFreq = xg::tables::lookupEqFrequency (params.parameters11To16[4]);
    const float eqHighGain = xg::tables::decodeEqGain (params.parameters11To16[5]);
    updatePostEq (eqLowFreq, eqLowGain, 1000.0f, 0.0f, 1.0f, eqHighFreq, eqHighGain);
}

void VariationEffectProcessor::updateEchoParameters (const xg::VariationParameters& params)
{
    delayType = DelayType::Echo;

    float timeL1Ms = params.parameters14Bit[0] > 0 ? static_cast<float> (params.parameters14Bit[0]) * 0.1f : 170.0f;
    timeL1Ms = juce::jlimit (0.1f, 743.0f, timeL1Ms);
    delayTimeLSamples = static_cast<float> (timeL1Ms * 0.001f * sampleRate);

    const auto fbLVal = params.parameters14Bit[1] > 0 ? static_cast<int> (params.parameters14Bit[1] & 0x7F) : 64;
    delayFeedbackL = juce::jlimit (-0.95f, 0.95f, static_cast<float> (fbLVal - 64) / 64.0f * 0.9f);

    float timeR1Ms = params.parameters14Bit[2] > 0 ? static_cast<float> (params.parameters14Bit[2]) * 0.1f : 178.0f;
    timeR1Ms = juce::jlimit (0.1f, 743.0f, timeR1Ms);
    delayTimeRSamples = static_cast<float> (timeR1Ms * 0.001f * sampleRate);

    const auto fbRVal = params.parameters14Bit[3] > 0 ? static_cast<int> (params.parameters14Bit[3] & 0x7F) : 64;
    delayFeedbackR = juce::jlimit (-0.95f, 0.95f, static_cast<float> (fbRVal - 64) / 64.0f * 0.9f);

    const auto dampVal = params.parameters14Bit[4] > 0 ? static_cast<int> (params.parameters14Bit[4] & 0x7F) : 10;
    delayDamp = juce::jlimit (0.0f, 0.9f, 1.0f - (static_cast<float> (dampVal) / 10.0f));

    float timeL2Ms = params.parameters14Bit[5] > 0 ? static_cast<float> (params.parameters14Bit[5]) * 0.1f : 170.0f;
    timeL2Ms = juce::jlimit (0.1f, 743.0f, timeL2Ms);
    delayEchoL2Samples = static_cast<float> (timeL2Ms * 0.001f * sampleRate);

    float timeR2Ms = params.parameters14Bit[6] > 0 ? static_cast<float> (params.parameters14Bit[6]) * 0.1f : 178.0f;
    timeR2Ms = juce::jlimit (0.1f, 743.0f, timeR2Ms);
    delayEchoR2Samples = static_cast<float> (timeR2Ms * 0.001f * sampleRate);

    const auto lvl2Val = static_cast<int> (params.parameters14Bit[7] & 0x7F);
    delayEcho2Level = static_cast<float> (lvl2Val) / 127.0f;

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.4f);

    const float eqLowFreq = xg::tables::lookupEqFrequency (params.parameters11To16[2]);
    const float eqLowGain = xg::tables::decodeEqGain (params.parameters11To16[3]);
    const float eqHighFreq = xg::tables::lookupEqFrequency (params.parameters11To16[4]);
    const float eqHighGain = xg::tables::decodeEqGain (params.parameters11To16[5]);
    updatePostEq (eqLowFreq, eqLowGain, 1000.0f, 0.0f, 1.0f, eqHighFreq, eqHighGain);
}

void VariationEffectProcessor::updateCrossDelayParameters (const xg::VariationParameters& params)
{
    delayType = DelayType::Cross;

    float timeLRMs = params.parameters14Bit[0] > 0 ? static_cast<float> (params.parameters14Bit[0]) * 0.1f : 170.0f;
    timeLRMs = juce::jlimit (0.1f, 743.0f, timeLRMs);
    delayTimeLSamples = static_cast<float> (timeLRMs * 0.001f * sampleRate);

    float timeRLMs = params.parameters14Bit[1] > 0 ? static_cast<float> (params.parameters14Bit[1]) * 0.1f : 175.0f;
    timeRLMs = juce::jlimit (0.1f, 743.0f, timeRLMs);
    delayTimeRSamples = static_cast<float> (timeRLMs * 0.001f * sampleRate);

    const auto fbVal = params.parameters14Bit[2] > 0 ? static_cast<int> (params.parameters14Bit[2] & 0x7F) : 64;
    delayFeedbackL = juce::jlimit (-0.95f, 0.95f, static_cast<float> (fbVal - 64) / 64.0f * 0.9f);
    delayFeedbackR = delayFeedbackL;

    crossDelayInputSelect = juce::jlimit (0, 2, static_cast<int> (params.parameters14Bit[3] & 0x7F));

    const auto dampVal = params.parameters14Bit[4] > 0 ? static_cast<int> (params.parameters14Bit[4] & 0x7F) : 10;
    delayDamp = juce::jlimit (0.0f, 0.9f, 1.0f - (static_cast<float> (dampVal) / 10.0f));

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.4f);

    const float eqLowFreq = xg::tables::lookupEqFrequency (params.parameters11To16[2]);
    const float eqLowGain = xg::tables::decodeEqGain (params.parameters11To16[3]);
    const float eqHighFreq = xg::tables::lookupEqFrequency (params.parameters11To16[4]);
    const float eqHighGain = xg::tables::decodeEqGain (params.parameters11To16[5]);
    updatePostEq (eqLowFreq, eqLowGain, 1000.0f, 0.0f, 1.0f, eqHighFreq, eqHighGain);
}

void VariationEffectProcessor::updateVariationReverbParameters (const xg::VariationParameters& params)
{
    // Param 1: Reverb Time (0..69 -> Table#4: 0.3s..30.0s)
    const float revTimeSec = xg::tables::lookupReverbTime (params.parameters14Bit[0] & 0x7F);
    const float norm = std::log (juce::jmax (0.3f, revTimeSec) / 0.3f) / std::log (30.0f / 0.3f);
    const float roomSize = juce::jlimit (0.05f, 0.98f, 0.10f + 0.88f * std::pow (norm, 0.75f));

    // Param 2: Diffusion (0..10) -> maps to stereo width [0.2, 1.0]
    const auto diffVal = static_cast<float> (juce::jlimit (0, 10, static_cast<int> (params.parameters14Bit[1] & 0x7F)));
    const float width = juce::jlimit (0.2f, 1.0f, 0.2f + (diffVal / 10.0f) * 0.8f);

    // Param 3: Initial Delay (0..63 -> Table#5: 0.1ms..99.3ms)
    const float initDelayMs = xg::tables::lookupDelayTime200 (params.parameters14Bit[2] & 0x7F);
    reverbInitialDelaySamples = juce::jlimit (1.0f, static_cast<float> (maxReverbDelayBufferSize - 100),
                                              initDelayMs * 0.001f * static_cast<float> (sampleRate));

    // Param 4: HPF Cutoff Frequency (0..52 -> Table#3: 20Hz..8.0kHz, 0=Thru)
    const int hpfIdx = static_cast<int> (params.parameters14Bit[3] & 0x7F);
    if (hpfIdx > 0 && hpfIdx <= 52)
    {
        const float hpfFreq = xg::tables::lookupEqFrequency (hpfIdx);
        const auto coeffs = juce::IIRCoefficients::makeHighPass (sampleRate, juce::jlimit (20.0, sampleRate * 0.49, static_cast<double> (hpfFreq)));
        for (auto& f : reverbHpfFilters)
            f.setCoefficients (coeffs);
        reverbHpfActive = true;
    }
    else
    {
        reverbHpfActive = false;
    }

    // Param 5: LPF Cutoff Frequency (34..60 -> Table#3: 1.0kHz..20.0kHz, 60=Thru)
    const float lpfCutoffHz = xg::tables::lookupEqFrequency (params.parameters14Bit[4] & 0x7F);
    const float dampNorm = 1.0f - (std::log10 (juce::jlimit (1000.0f, 20000.0f, lpfCutoffHz) / 1000.0f) / std::log10 (20.0f));

    // Param 14: Feedback High Damp (1..10 -> 0.1 .. 1.0, default 8)
    const int highDampIdx = juce::jlimit (1, 10, static_cast<int> (params.parameters11To16[3] > 0 ? (params.parameters11To16[3] & 0x7F) : 8));
    const float highDampFactor = static_cast<float> (highDampIdx) / 10.0f;
    reverbFbHighDampCoeff = highDampFactor;

    // High Damp modifies late reverb damping: smaller High Damp means less high frequency energy in tail (higher damping)
    const float damping = juce::jlimit (0.0f, 1.0f, dampNorm * (1.25f - highDampFactor * 0.3125f));

    // Param 11: Reverb Delay (0..63 -> Table#5: 0.1ms..99.3ms)
    const float revDelayMs = xg::tables::lookupDelayTime200 (params.parameters11To16[0] & 0x7F);
    reverbPostDelaySamples = juce::jlimit (1.0f, static_cast<float> (maxReverbDelayBufferSize - 100),
                                            revDelayMs * 0.001f * static_cast<float> (sampleRate));

    // Param 12: Density (0..4, default 4)
    reverbDensity = juce::jlimit (0, 4, static_cast<int> (params.parameters11To16[1] & 0x7F));

    // Param 13: ER/Reverb Balance (1..127, 0=ER only, 64=1:1, default 50/64)
    const int erRevBal = static_cast<int> (params.parameters11To16[2] & 0x7F);
    if (erRevBal == 0)
    {
        reverbErGain = 1.0f;
        reverbLateGain = 0.0f;
    }
    else if (erRevBal <= 64)
    {
        reverbErGain = 1.0f;
        reverbLateGain = static_cast<float> (erRevBal) / 64.0f;
    }
    else
    {
        reverbErGain = static_cast<float> (127 - erRevBal) / 63.0f;
        reverbLateGain = 1.0f;
    }

    // Param 15: Feedback Level (1..127, 64=0, default 64)
    const int fbData = juce::jlimit (1, 127, static_cast<int> (params.parameters11To16[4] > 0 ? (params.parameters11To16[4] & 0x7F) : 64));
    reverbFeedbackLevel = (static_cast<float> (fbData) - 64.0f) / 63.0f * 0.7f;

    juce::dsp::Reverb::Parameters p;
    p.roomSize = roomSize;
    p.damping = damping;
    p.wetLevel = 1.0f;
    p.dryLevel = 0.0f;
    p.width = width;
    p.freezeMode = 0.0f;
    variationReverbProcessor.setParameters (p);

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.4f);
}

void VariationEffectProcessor::updateRotarySpeakerParameters (const xg::VariationParameters& params)
{
    rotarySpeedHz = xg::tables::lookupLfoFrequency (params.parameters14Bit[0] & 0x7F);
    rotaryDepth = static_cast<float> (params.parameters14Bit[1] & 0x7F) / 127.0f;

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 1.0f);

    const float eqLowFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[5] & 0x7F);
    const float eqLowGain = xg::tables::decodeEqGain (params.parameters14Bit[6] & 0x7F);
    const float eqHighFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[7] & 0x7F);
    const float eqHighGain = xg::tables::decodeEqGain (params.parameters14Bit[8] & 0x7F);
    const float eqMidFreq = xg::tables::lookupEqFrequency (params.parameters11To16[0] & 0x7F);
    const float eqMidGain = xg::tables::decodeEqGain (params.parameters11To16[1] & 0x7F);
    const float eqMidQ = juce::jmax (0.1f, static_cast<float> (params.parameters11To16[2] & 0x7F) / 10.0f);
    updatePostEq (eqLowFreq, eqLowGain, eqMidFreq, eqMidGain, eqMidQ, eqHighFreq, eqHighGain);
}

void VariationEffectProcessor::update3BandEqParameters (const xg::VariationParameters& params)
{
    const double nyquist = sampleRate * 0.49;

    const float lowGainDb = static_cast<float> (params.parameters14Bit[0] & 0x7F) - 64.0f;
    const float lowFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[5] & 0x7F);

    const float midFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[1] & 0x7F);
    const float midGainDb = static_cast<float> (params.parameters14Bit[2] & 0x7F) - 64.0f;
    const float midQ = juce::jmax (0.1f, static_cast<float> (params.parameters14Bit[3] & 0x7F) / 10.0f);

    const float highGainDb = static_cast<float> (params.parameters14Bit[4] & 0x7F) - 64.0f;
    const float highFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[6] & 0x7F);

    const auto lowCoeff = juce::IIRCoefficients::makeLowShelf (sampleRate, juce::jlimit (20.0, nyquist, static_cast<double> (lowFreq)), 0.707, juce::Decibels::decibelsToGain (lowGainDb));
    const auto midCoeff = juce::IIRCoefficients::makePeakFilter (sampleRate, juce::jlimit (20.0, nyquist, static_cast<double> (midFreq)), midQ, juce::Decibels::decibelsToGain (midGainDb));
    const auto highCoeff = juce::IIRCoefficients::makeHighShelf (sampleRate, juce::jlimit (20.0, nyquist, static_cast<double> (highFreq)), 0.707, juce::Decibels::decibelsToGain (highGainDb));

    for (auto& f : eqLowFilters) f.setCoefficients (lowCoeff);
    for (auto& f : eqMidFilters) f.setCoefficients (midCoeff);
    for (auto& f : eqHighFilters) f.setCoefficients (highCoeff);
    eqFiltersActive = true;

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 1.0f);
}

void VariationEffectProcessor::update2BandEqParameters (const xg::VariationParameters& params)
{
    const double nyquist = sampleRate * 0.49;

    const float lowFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[0] & 0x7F);
    const float lowGainDb = static_cast<float> (params.parameters14Bit[1] & 0x7F) - 64.0f;
    const float highFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[2] & 0x7F);
    const float highGainDb = static_cast<float> (params.parameters14Bit[3] & 0x7F) - 64.0f;

    const auto lowCoeff = juce::IIRCoefficients::makeLowShelf (sampleRate, juce::jlimit (20.0, nyquist, static_cast<double> (lowFreq)), 0.707, juce::Decibels::decibelsToGain (lowGainDb));
    const auto highCoeff = juce::IIRCoefficients::makeHighShelf (sampleRate, juce::jlimit (20.0, nyquist, static_cast<double> (highFreq)), 0.707, juce::Decibels::decibelsToGain (highGainDb));

    for (auto& f : eqLowFilters) f.setCoefficients (lowCoeff);
    for (auto& f : eqMidFilters) f.makeInactive();
    for (auto& f : eqHighFilters) f.setCoefficients (highCoeff);
    eqFiltersActive = true;

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 1.0f);
}

void VariationEffectProcessor::updateDistortionParameters (const xg::VariationParameters& params)
{
    if (currentTypeMsb == xg::varTypeOverdrive)
    {
        distType = DistortionType::Overdrive;
        // Overdrive: LSB 00H = Overdrive (mono/summed), 08H = Stereo Overdrive
        distSubtype = (params.typeLsb == 0x08) ? DistortionSubtype::StereoDist : DistortionSubtype::Standard;
    }
    else
    {
        distType = DistortionType::Distortion;
        // Distortion: LSB 00H = Distortion, 01H = Comp+Distortion, 08H = Stereo Distortion
        // Reserved LSBs fall back to Standard Distortion (00H)
        if (params.typeLsb == 0x01)
            distSubtype = DistortionSubtype::CompDist;
        else if (params.typeLsb == 0x08)
            distSubtype = DistortionSubtype::StereoDist;
        else
            distSubtype = DistortionSubtype::Standard;
    }

    isStereoDistortion = (distSubtype == DistortionSubtype::StereoDist);

    // Param 11: Edge (Clip Curve) (0..127)
    distortionEdge = static_cast<float> (params.parameters11To16[0] & 0x7F) / 127.0f;

    // Compressor parameters (active only for Comp+Distortion)
    if (distSubtype == DistortionSubtype::CompDist)
    {
        // Param 12: Comp Attack (Table#8: 0..19 -> 1ms .. 40ms, default 6 = 7ms)
        const auto attackIdx = static_cast<int> (params.parameters11To16[1] & 0x7F);
        compAttackMs = xg::tables::lookupCompAttackTime (attackIdx);

        // Param 13: Comp Release (Table#9: 0..15 -> 10ms .. 680ms, default 2 = 25ms)
        const auto releaseIdx = static_cast<int> (params.parameters11To16[2] & 0x7F);
        compReleaseMs = xg::tables::lookupCompReleaseTime (releaseIdx);

        // Param 14: Comp Threshold (79..121 -> -48dB .. -6dB, default 100 = -27dB)
        const auto threshData = static_cast<int> (params.parameters11To16[3] & 0x7F);
        const auto threshDb = static_cast<float> (juce::jlimit (79, 121, threshData > 0 ? threshData : 100) - 127);
        compThreshold = juce::Decibels::decibelsToGain (threshDb);

        // Param 15: Comp Ratio (Table#10: 0..7 -> 1.0 .. 20.0, default 4 = 5.0)
        const auto ratioIdx = static_cast<int> (params.parameters11To16[4] & 0x7F);
        compRatio = xg::tables::lookupCompRatio (ratioIdx);

        const float sr = static_cast<float> (sampleRate);
        compAttackCoeff = std::exp (-1.0f / (juce::jmax (0.0001f, compAttackMs * 0.001f) * sr));
        compReleaseCoeff = std::exp (-1.0f / (juce::jmax (0.0001f, compReleaseMs * 0.001f) * sr));
    }
    else
    {
        compAttackMs = 7.0f;
        compReleaseMs = 25.0f;
        compThreshold = 0.04467f;
        compRatio = 1.0f;
        compAttackCoeff = 0.0f;
        compReleaseCoeff = 0.0f;
    }
    compEnvelope.fill (0.0f);

    const auto driveVal = static_cast<float> (params.parameters14Bit[0] & 0x7F);
    if (distType == DistortionType::Overdrive)
        distortionDrive = 1.0f + (driveVal / 127.0f) * 25.0f;
    else
        distortionDrive = 1.0f + (driveVal / 127.0f) * 50.0f;

    const auto outVal = params.parameters14Bit[4] > 0 ? static_cast<float> (params.parameters14Bit[4] & 0x7F) : 64.0f;
    distortionOutputGain = (outVal / 64.0f) / std::sqrt (distortionDrive);

    const auto nyquist = sampleRate * 0.49;
    const auto preCoeff = juce::IIRCoefficients::makeHighPass (sampleRate, 80.0);
    for (auto& f : distPreFilters) f.setCoefficients (preCoeff);

    const auto lpfParam = params.parameters14Bit[3] > 0 ? static_cast<int> (params.parameters14Bit[3] & 0x7F) : 48;
    const auto lpfCutoff = juce::jlimit (1000.0, nyquist, static_cast<double> (xg::lookupEqFrequency (lpfParam)));
    const auto postCoeff = juce::IIRCoefficients::makeLowPass (sampleRate, lpfCutoff);
    for (auto& f : distPostFilters) f.setCoefficients (postCoeff);

    for (auto& f : ampSimCabFilters) f.makeInactive();
    distFiltersActive = true;

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 1.0f);

    // Param 2/3: EQ Low, Param 7/8/9: EQ Mid
    const float eqLowFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[1] & 0x7F);
    const float eqLowGain = xg::tables::decodeEqGain (params.parameters14Bit[2] & 0x7F);
    const float eqMidFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[6] & 0x7F);
    const float eqMidGain = xg::tables::decodeEqGain (params.parameters14Bit[7] & 0x7F);
    const float eqMidQ = juce::jmax (0.1f, static_cast<float> (params.parameters14Bit[8] & 0x7F) / 10.0f);
    updatePostEq (eqLowFreq, eqLowGain, eqMidFreq, eqMidGain, eqMidQ, 8000.0f, 0.0f);
}

void VariationEffectProcessor::updateAmpSimulatorParameters (const xg::VariationParameters& params)
{
    distType = DistortionType::AmpSim;
    // Amp Sim: LSB 00H = Amp Sim, 08H = Stereo Amp Sim
    distSubtype = (params.typeLsb == 0x08) ? DistortionSubtype::StereoDist : DistortionSubtype::Standard;
    isStereoDistortion = (distSubtype == DistortionSubtype::StereoDist);

    // Param 11: Edge (Clip Curve) (0..127)
    distortionEdge = static_cast<float> (params.parameters11To16[0] & 0x7F) / 127.0f;

    // Compressor is not used in Amp Simulator
    compAttackMs = 7.0f;
    compReleaseMs = 25.0f;
    compThreshold = 0.04467f;
    compRatio = 1.0f;
    compAttackCoeff = 0.0f;
    compReleaseCoeff = 0.0f;
    compEnvelope.fill (0.0f);

    // Param 1: Drive (0..127)
    const auto driveVal = static_cast<float> (params.parameters14Bit[0] & 0x7F);
    distortionDrive = 1.0f + (driveVal / 127.0f) * 35.0f;

    // Param 4: Output Level (0..127)
    const auto outVal = static_cast<float> (params.parameters14Bit[3] & 0x7F);
    distortionOutputGain = (outVal / 64.0f) / std::sqrt (distortionDrive);

    const auto nyquist = sampleRate * 0.49;

    // Pre-filter: 120Hz high-pass
    const auto preCoeff = juce::IIRCoefficients::makeHighPass (sampleRate, 120.0);
    for (auto& f : distPreFilters) f.setCoefficients (preCoeff);

    // Param 2: AMP Type (0: Off, 1: Stack, 2: Combo, 3: Tube)
    const int ampType = params.parameters14Bit[1] & 0x7F;
    if (ampType == 1) // Stack
    {
        const auto cabCoeff = juce::IIRCoefficients::makeLowPass (sampleRate, juce::jmin (nyquist, 3600.0));
        for (auto& f : ampSimCabFilters) f.setCoefficients (cabCoeff);
    }
    else if (ampType == 2) // Combo
    {
        const auto cabCoeff = juce::IIRCoefficients::makeLowPass (sampleRate, juce::jmin (nyquist, 4800.0));
        for (auto& f : ampSimCabFilters) f.setCoefficients (cabCoeff);
    }
    else if (ampType == 3) // Tube
    {
        const auto cabCoeff = juce::IIRCoefficients::makeLowPass (sampleRate, juce::jmin (nyquist, 3000.0));
        for (auto& f : ampSimCabFilters) f.setCoefficients (cabCoeff);
    }
    else // 0: Off or invalid
    {
        for (auto& f : ampSimCabFilters) f.makeInactive();
    }

    // Param 3: LPF Cutoff (Table#3: 34..60 -> 1.0k..Thru)
    const int lpfParam = static_cast<int> (params.parameters14Bit[2] & 0x7F);
    if (lpfParam >= 60)
    {
        for (auto& f : distPostFilters) f.makeInactive();
    }
    else
    {
        const float lpfCutoffHz = xg::tables::lookupEqFrequency (lpfParam);
        const auto postCoeff = juce::IIRCoefficients::makeLowPass (sampleRate, juce::jlimit (20.0, nyquist, static_cast<double> (lpfCutoffHz)));
        for (auto& f : distPostFilters) f.setCoefficients (postCoeff);
    }

    distFiltersActive = true;

    // Param 10: Dry/Wet Balance (1..127)
    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 1.0f);

    // Param 5..9 and 12..16 are reserved -> Post EQ is completely disabled for Amp Simulator
    postEqActive = false;
    for (auto& f : postEqLow) f.makeInactive();
    for (auto& f : postEqMid) f.makeInactive();
    for (auto& f : postEqHigh) f.makeInactive();
}

void VariationEffectProcessor::updateFlangerParameters (const xg::VariationParameters& params)
{
    flangerRateHz = xg::tables::lookupLfoFrequency (params.parameters14Bit[0] & 0x7F);

    const auto depthVal = static_cast<float> (params.parameters14Bit[1] & 0x7F);
    flangerDepthSec = 0.0002f + (depthVal / 127.0f) * 0.0035f;

    const auto fbVal = params.parameters14Bit[2] > 0 ? static_cast<int> (params.parameters14Bit[2] & 0x7F) : 64;
    flangerFeedback = juce::jlimit (-0.95f, 0.95f, static_cast<float> (fbVal - 64) / 64.0f * 0.9f);

    const float delayOffsetMs = xg::tables::lookupModDelayOffset (params.parameters14Bit[3] & 0x7F);
    flangerOffsetSec = delayOffsetMs * 0.001f;

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.5f);

    const float eqLowFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[5] & 0x7F);
    const float eqLowGain = xg::tables::decodeEqGain (params.parameters14Bit[6] & 0x7F);
    const float eqHighFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[7] & 0x7F);
    const float eqHighGain = xg::tables::decodeEqGain (params.parameters14Bit[8] & 0x7F);
    const float eqMidFreq = xg::tables::lookupEqFrequency (params.parameters11To16[0] & 0x7F);
    const float eqMidGain = xg::tables::decodeEqGain (params.parameters11To16[1] & 0x7F);
    const float eqMidQ = juce::jmax (0.1f, static_cast<float> (params.parameters11To16[2] & 0x7F) / 10.0f);
    updatePostEq (eqLowFreq, eqLowGain, eqMidFreq, eqMidGain, eqMidQ, eqHighFreq, eqHighGain);
}

void VariationEffectProcessor::updatePhaserParameters (const xg::VariationParameters& params)
{
    phaserRateHz = xg::tables::lookupLfoFrequency (params.parameters14Bit[0] & 0x7F);

    const auto depthVal = static_cast<float> (params.parameters14Bit[1] & 0x7F);
    phaserDepth = depthVal / 127.0f;

    phaserOffset = static_cast<float> (params.parameters14Bit[2] & 0x7F);

    const auto fbVal = params.parameters14Bit[3] > 0 ? static_cast<int> (params.parameters14Bit[3] & 0x7F) : 64;
    phaserFeedback = juce::jlimit (-0.95f, 0.95f, static_cast<float> (fbVal - 64) / 64.0f * 0.85f);

    if (currentTypeLsb == 0x08) // Phaser 2
    {
        // Param 11: Stage (3..6, default 5)
        const int s = params.parameters11To16[0] > 0 ? static_cast<int> (params.parameters11To16[0] & 0x7F) : 5;
        phaserStages = juce::jlimit (3, 6, s);

        // Param 13: LFO Phase Difference (4..124 -> -180..+180 deg, default 4 = -180 deg)
        const int phDiffVal = params.parameters11To16[2] > 0 ? static_cast<int> (params.parameters11To16[2] & 0x7F) : 4;
        phaserLfoPhaseDiff = (static_cast<float> (phDiffVal) - 64.0f) * (juce::MathConstants<float>::pi / 60.0f);
        phaserDiffusionMono = false;
    }
    else // Phaser 1
    {
        // Param 11: Stage (4..12, default 6)
        const int s = params.parameters11To16[0] > 0 ? static_cast<int> (params.parameters11To16[0] & 0x7F) : 6;
        phaserStages = juce::jlimit (4, 12, s);

        // Param 12: Diffusion (0=mono, 1=stereo, default 1)
        phaserDiffusionMono = (params.parameters11To16[1] == 0);
        phaserLfoPhaseDiff = 0.0f;
    }

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.5f);

    const float eqLowFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[5] & 0x7F);
    const float eqLowGain = xg::tables::decodeEqGain (params.parameters14Bit[6] & 0x7F);
    const float eqHighFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[7] & 0x7F);
    const float eqHighGain = xg::tables::decodeEqGain (params.parameters14Bit[8] & 0x7F);
    const float eqMidFreq = xg::tables::lookupEqFrequency (params.parameters11To16[0] & 0x7F);
    const float eqMidGain = xg::tables::decodeEqGain (params.parameters11To16[1] & 0x7F);
    const float eqMidQ = juce::jmax (0.1f, static_cast<float> (params.parameters11To16[2] & 0x7F) / 10.0f);
    updatePostEq (eqLowFreq, eqLowGain, eqMidFreq, eqMidGain, eqMidQ, eqHighFreq, eqHighGain);
}

void VariationEffectProcessor::updateTremoloAutoPanParameters (const xg::VariationParameters& params)
{
    isAutoPan = (currentTypeMsb == xg::varTypeAutoPan);

    modRateHz = xg::tables::lookupLfoFrequency (params.parameters14Bit[0] & 0x7F);

    const auto depthVal = static_cast<float> (params.parameters14Bit[1] & 0x7F);
    modDepth = juce::jlimit (0.0f, 1.0f, depthVal / 127.0f);

    if (!isAutoPan)
    {
        // Param 14: LFO Phase Difference (4..124 -> -180..+180 deg, default 64 = 0 deg)
        const int phDiffVal = params.parameters11To16[3] > 0 ? static_cast<int> (params.parameters11To16[3] & 0x7F) : 64;
        tremoloLfoPhaseDiff = (static_cast<float> (phDiffVal) - 64.0f) * (juce::MathConstants<float>::pi / 60.0f);

        // Param 15: Input Mode (0=mono, 1=stereo, default 0=mono)
        tremoloMonoInput = (params.parameters11To16[4] == 0);
    }
    else
    {
        tremoloLfoPhaseDiff = 0.0f;
        tremoloMonoInput = false;
    }

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 1.0f);

    const float eqLowFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[5] & 0x7F);
    const float eqLowGain = xg::tables::decodeEqGain (params.parameters14Bit[6] & 0x7F);
    const float eqHighFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[7] & 0x7F);
    const float eqHighGain = xg::tables::decodeEqGain (params.parameters14Bit[8] & 0x7F);
    const float eqMidFreq = xg::tables::lookupEqFrequency (params.parameters11To16[0] & 0x7F);
    const float eqMidGain = xg::tables::decodeEqGain (params.parameters11To16[1] & 0x7F);
    const float eqMidQ = juce::jmax (0.1f, static_cast<float> (params.parameters11To16[2] & 0x7F) / 10.0f);
    updatePostEq (eqLowFreq, eqLowGain, eqMidFreq, eqMidGain, eqMidQ, eqHighFreq, eqHighGain);
}

void VariationEffectProcessor::updateChorusParameters (const xg::VariationParameters& params)
{
    const float rateHz = xg::tables::lookupLfoFrequency (params.parameters14Bit[0] & 0x7F);
    chorusProcessor.setRate (juce::jmax (0.001f, rateHz));

    const auto depthVal = static_cast<float> (params.parameters14Bit[1] & 0x7F);
    baseChorusDepth = depthVal / 127.0f;
    chorusProcessor.setDepth (baseChorusDepth);

    const auto fbVal = params.parameters14Bit[2] > 0 ? static_cast<int> (params.parameters14Bit[2] & 0x7F) : 64;
    chorusProcessor.setFeedback (juce::jlimit (-0.85f, 0.85f, static_cast<float> (fbVal - 64) / 64.0f * 0.85f));

    const float delayMs = juce::jmax (0.1f, xg::tables::lookupModDelayOffset (params.parameters14Bit[3] & 0x7F));
    chorusProcessor.setCentreDelay (delayMs);

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.5f);

    const float eqLowFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[5] & 0x7F);
    const float eqLowGain = xg::tables::decodeEqGain (params.parameters14Bit[6] & 0x7F);
    const float eqHighFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[7] & 0x7F);
    const float eqHighGain = xg::tables::decodeEqGain (params.parameters14Bit[8] & 0x7F);
    const float eqMidFreq = xg::tables::lookupEqFrequency (params.parameters11To16[0] & 0x7F);
    const float eqMidGain = xg::tables::decodeEqGain (params.parameters11To16[1] & 0x7F);
    const float eqMidQ = juce::jmax (0.1f, static_cast<float> (params.parameters11To16[2] & 0x7F) / 10.0f);
    updatePostEq (eqLowFreq, eqLowGain, eqMidFreq, eqMidGain, eqMidQ, eqHighFreq, eqHighGain);
}

void VariationEffectProcessor::updateAutoWahParameters (const xg::VariationParameters& params)
{
    wahLfoRateHz = xg::tables::lookupLfoFrequency (params.parameters14Bit[0] & 0x7F);

    const auto depthVal = static_cast<float> (params.parameters14Bit[1] & 0x7F);
    wahLfoDepth = depthVal / 127.0f;

    const auto cutoffVal = params.parameters14Bit[2] > 0 ? static_cast<float> (params.parameters14Bit[2] & 0x7F) : 64.0f;
    wahManualCutoff = cutoffVal / 127.0f;

    const auto resoVal = params.parameters14Bit[3] > 0 ? static_cast<int> (params.parameters14Bit[3] & 0x7F) : 30;
    wahResonance = resoVal >= 10 ? juce::jlimit (1.0f, 10.0f, static_cast<float> (resoVal) / 10.0f)
                                 : juce::jlimit (1.0f, 10.0f, 1.0f + static_cast<float> (resoVal) * 0.2f);

    // Param 11: Drive (0..127)
    autoWahDrive = static_cast<float> (params.parameters11To16[0] & 0x7F) / 127.0f;

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 1.0f);

    const float eqLowFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[5] & 0x7F);
    const float eqLowGain = xg::tables::decodeEqGain (params.parameters14Bit[6] & 0x7F);
    const float eqHighFreq = xg::tables::lookupEqFrequency (params.parameters14Bit[7] & 0x7F);
    const float eqHighGain = xg::tables::decodeEqGain (params.parameters14Bit[8] & 0x7F);
    updatePostEq (eqLowFreq, eqLowGain, 1000.0f, 0.0f, 1.0f, eqHighFreq, eqHighGain);
}

void VariationEffectProcessor::updateParameters (const xg::VariationParameters& params)
{
    currentTypeMsb = params.typeMsb;
    currentTypeLsb = params.typeLsb;
    postEqActive = false;

    mwDepth   = static_cast<float> (static_cast<int> (params.mwControlDepth) - 64) / 64.0f;
    bendDepth = static_cast<float> (static_cast<int> (params.bendControlDepth) - 64) / 64.0f;
    catDepth  = static_cast<float> (static_cast<int> (params.catControlDepth) - 64) / 64.0f;
    ac1Depth  = static_cast<float> (static_cast<int> (params.ac1ControlDepth) - 64) / 64.0f;
    ac2Depth  = static_cast<float> (static_cast<int> (params.ac2ControlDepth) - 64) / 64.0f;

    switch (currentTypeMsb)
    {
        case xg::varTypeHall1:
        case xg::varTypeRoom1:
        case xg::varTypeStage1:
        case xg::varTypePlate:
        case xg::varTypeWhiteRoom:
        case xg::varTypeTunnel:
        case xg::varTypeCanyon:
        case xg::varTypeBasement:
            updateVariationReverbParameters (params);
            break;

        case xg::varTypeDelayLCR:
            updateDelayLcrParameters (params);
            break;

        case xg::varTypeDelayLR:
            updateDelayLrParameters (params);
            break;

        case xg::varTypeEcho:
            updateEchoParameters (params);
            break;

        case xg::varTypeCrossDelay:
            updateCrossDelayParameters (params);
            break;

        case xg::varTypeRotarySpeaker:
            updateRotarySpeakerParameters (params);
            break;

        case xg::varType3BandEq:
            update3BandEqParameters (params);
            break;

        case xg::varType2BandEq:
            update2BandEqParameters (params);
            break;

        case xg::varTypeDistortion:
        case xg::varTypeOverdrive:
            updateDistortionParameters (params);
            break;

        case xg::varTypeAmpSimulator:
            updateAmpSimulatorParameters (params);
            break;

        case xg::varTypeFlanger:
            updateFlangerParameters (params);
            break;

        case xg::varTypeAutoWah:
            updateAutoWahParameters (params);
            break;

        case xg::varTypePhaser:
            updatePhaserParameters (params);
            break;

        case xg::varTypeTremolo:
        case xg::varTypeAutoPan:
            updateTremoloAutoPanParameters (params);
            break;

        case xg::varTypeChorus:
        case xg::varTypeSymphonic:
            updateChorusParameters (params);
            break;

        case xg::varTypeThru:
        default:
            dryGain = 1.0f;
            wetGain = 0.0f;
            break;
    }
}

void VariationEffectProcessor::processDelay (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    auto* bufL = delayBuffer[0].data();
    auto* bufR = delayBuffer[1].data();

    auto* wetL = tempWetBuffer.getWritePointer (0);
    auto* wetR = tempWetBuffer.getWritePointer (1);

    const float effFeedbackL = juce::jlimit (-0.95f, 0.95f, delayFeedbackL + currentModOffset * 0.4f);
    const float effFeedbackR = juce::jlimit (-0.95f, 0.95f, delayFeedbackR + currentModOffset * 0.4f);

    auto interpolateDelay = [] (const float* buffer, float readPos) noexcept -> float
    {
        while (readPos < 0.0f) readPos += static_cast<float> (maxDelaySamples);
        const auto idx0 = static_cast<int> (readPos) % maxDelaySamples;
        const auto idx1 = (idx0 + 1) % maxDelaySamples;
        const auto frac = readPos - static_cast<float> (static_cast<int> (readPos));
        return (1.0f - frac) * buffer[idx0] + frac * buffer[idx1];
    };

    for (int i = 0; i < numSamples; ++i)
    {
        if (delayType == DelayType::LCR)
        {
            const float delL = interpolateDelay (bufL, static_cast<float> (delayWritePos) - delayTimeLSamples);
            const float delR = interpolateDelay (bufR, static_cast<float> (delayWritePos) - delayTimeRSamples);
            const float delLC = interpolateDelay (bufL, static_cast<float> (delayWritePos) - delayTimeCSamples);
            const float delRC = interpolateDelay (bufR, static_cast<float> (delayWritePos) - delayTimeCSamples);
            const float delC = 0.5f * (delLC + delRC);

            wetL[i] = delL + delayCchLevel * delC;
            wetR[i] = delR + delayCchLevel * delC;

            const float fbL = interpolateDelay (bufL, static_cast<float> (delayWritePos) - delayFeedbackDelayLSamples);
            const float fbR = interpolateDelay (bufR, static_cast<float> (delayWritePos) - delayFeedbackDelayRSamples);

            delayDampState[0] += (1.0f - delayDamp) * (fbL - delayDampState[0]);
            delayDampState[1] += (1.0f - delayDamp) * (fbR - delayDampState[1]);

            bufL[delayWritePos] = inL[i] + delayDampState[0] * effFeedbackL;
            bufR[delayWritePos] = inR[i] + delayDampState[1] * effFeedbackR;
        }
        else if (delayType == DelayType::LR)
        {
            const float delL = interpolateDelay (bufL, static_cast<float> (delayWritePos) - delayTimeLSamples);
            const float delR = interpolateDelay (bufR, static_cast<float> (delayWritePos) - delayTimeRSamples);

            wetL[i] = delL;
            wetR[i] = delR;

            const float fbL = interpolateDelay (bufL, static_cast<float> (delayWritePos) - delayFeedbackDelayLSamples);
            const float fbR = interpolateDelay (bufR, static_cast<float> (delayWritePos) - delayFeedbackDelayRSamples);

            delayDampState[0] += (1.0f - delayDamp) * (fbL - delayDampState[0]);
            delayDampState[1] += (1.0f - delayDamp) * (fbR - delayDampState[1]);

            bufL[delayWritePos] = inL[i] + delayDampState[0] * effFeedbackL;
            bufR[delayWritePos] = inR[i] + delayDampState[1] * effFeedbackR;
        }
        else if (delayType == DelayType::Echo)
        {
            const float delL1 = interpolateDelay (bufL, static_cast<float> (delayWritePos) - delayTimeLSamples);
            const float delR1 = interpolateDelay (bufR, static_cast<float> (delayWritePos) - delayTimeRSamples);
            const float delL2 = interpolateDelay (bufL, static_cast<float> (delayWritePos) - delayEchoL2Samples);
            const float delR2 = interpolateDelay (bufR, static_cast<float> (delayWritePos) - delayEchoR2Samples);

            wetL[i] = delL1 + delayEcho2Level * delL2;
            wetR[i] = delR1 + delayEcho2Level * delR2;

            delayDampState[0] += (1.0f - delayDamp) * (delL1 - delayDampState[0]);
            delayDampState[1] += (1.0f - delayDamp) * (delR1 - delayDampState[1]);

            bufL[delayWritePos] = inL[i] + delayDampState[0] * effFeedbackL;
            bufR[delayWritePos] = inR[i] + delayDampState[1] * effFeedbackR;
        }
        else // Cross
        {
            const float inSigL = (crossDelayInputSelect == 1) ? 0.0f : inL[i];
            const float inSigR = (crossDelayInputSelect == 0) ? 0.0f : inR[i];

            const float delL = interpolateDelay (bufL, static_cast<float> (delayWritePos) - delayTimeLSamples);
            const float delR = interpolateDelay (bufR, static_cast<float> (delayWritePos) - delayTimeRSamples);

            wetL[i] = delL;
            wetR[i] = delR;

            delayDampState[0] += (1.0f - delayDamp) * (delL - delayDampState[0]);
            delayDampState[1] += (1.0f - delayDamp) * (delR - delayDampState[1]);

            bufL[delayWritePos] = inSigL + delayDampState[1] * effFeedbackR;
            bufR[delayWritePos] = inSigR + delayDampState[0] * effFeedbackL;
        }

        delayWritePos = (delayWritePos + 1) % maxDelaySamples;
    }

    if (postEqActive)
        applyPostEq (wetL, wetR, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + wetL[i] * wetGain;
        outR[i] = inR[i] * dryGain + wetR[i] * wetGain;
    }
}

void VariationEffectProcessor::processVariationReverb (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    tempWetBuffer.copyFrom (0, 0, inL, numSamples);
    tempWetBuffer.copyFrom (1, 0, inR, numSamples);

    auto* sigL = tempWetBuffer.getWritePointer (0);
    auto* sigR = tempWetBuffer.getWritePointer (1);

    // 1. HPF Cutoff Frequency (Param 4)
    if (reverbHpfActive)
    {
        reverbHpfFilters[0].processSamples (sigL, numSamples);
        reverbHpfFilters[1].processSamples (sigR, numSamples);
    }

    if (reverbErBuffer.getNumSamples() < numSamples)
    {
        reverbErBuffer.setSize (2, numSamples, false, false, true);
        reverbLateInputBuffer.setSize (2, numSamples, false, false, true);
    }

    reverbErBuffer.clear();
    reverbLateInputBuffer.clear();

    auto* erL = reverbErBuffer.getWritePointer (0);
    auto* erR = reverbErBuffer.getWritePointer (1);
    auto* lateInL = reverbLateInputBuffer.getWritePointer (0);
    auto* lateInR = reverbLateInputBuffer.getWritePointer (1);

    auto* initBufL = reverbInitialDelayBuffer[0].data();
    auto* initBufR = reverbInitialDelayBuffer[1].data();
    auto* revBufL = reverbPostDelayBuffer[0].data();
    auto* revBufR = reverbPostDelayBuffer[1].data();

    const float sr = static_cast<float> (sampleRate);
    const float dtSamples = sr * 0.001f;
    const float densityScale = static_cast<float> (reverbDensity) / 4.0f;

    auto interpolateReverbDelay = [] (const float* buffer, float readPos) noexcept -> float
    {
        while (readPos < 0.0f)
            readPos += static_cast<float> (maxReverbDelayBufferSize);
        const auto idx0 = static_cast<int> (readPos) % maxReverbDelayBufferSize;
        const auto idx1 = (idx0 + 1) % maxReverbDelayBufferSize;
        const auto frac = readPos - static_cast<float> (static_cast<int> (readPos));
        return (1.0f - frac) * buffer[idx0] + frac * buffer[idx1];
    };

    for (int i = 0; i < numSamples; ++i)
    {
        // Read Initial Delay
        const float delayedInitL = interpolateReverbDelay (initBufL, static_cast<float> (reverbInitialDelayWritePos) - reverbInitialDelaySamples);
        const float delayedInitR = interpolateReverbDelay (initBufR, static_cast<float> (reverbInitialDelayWritePos) - reverbInitialDelaySamples);

        // Feedback loop with High Damp
        reverbFbHighDampState[0] += (1.0f - reverbFbHighDampCoeff) * (delayedInitL - reverbFbHighDampState[0]);
        reverbFbHighDampState[1] += (1.0f - reverbFbHighDampCoeff) * (delayedInitR - reverbFbHighDampState[1]);

        initBufL[reverbInitialDelayWritePos] = sigL[i] + reverbFbHighDampState[0] * reverbFeedbackLevel;
        initBufR[reverbInitialDelayWritePos] = sigR[i] + reverbFbHighDampState[1] * reverbFeedbackLevel;

        // Early reflections from initial delay buffer
        const float erTapL1 = interpolateReverbDelay (initBufL, static_cast<float> (reverbInitialDelayWritePos) - reverbInitialDelaySamples - 7.3f * dtSamples);
        const float erTapL2 = interpolateReverbDelay (initBufR, static_cast<float> (reverbInitialDelayWritePos) - reverbInitialDelaySamples - 22.1f * dtSamples);
        const float erTapR1 = interpolateReverbDelay (initBufR, static_cast<float> (reverbInitialDelayWritePos) - reverbInitialDelaySamples - 11.8f * dtSamples);
        const float erTapR2 = interpolateReverbDelay (initBufL, static_cast<float> (reverbInitialDelayWritePos) - reverbInitialDelaySamples - 29.5f * dtSamples);

        erL[i] = delayedInitL * 0.6f + (erTapL1 * 0.5f + erTapL2 * 0.35f) * densityScale * reverbFbHighDampCoeff;
        erR[i] = delayedInitR * 0.6f + (erTapR1 * 0.5f + erTapR2 * 0.35f) * densityScale * reverbFbHighDampCoeff;

        // Feed to Reverb Post Delay buffer
        revBufL[reverbPostDelayWritePos] = delayedInitL;
        revBufR[reverbPostDelayWritePos] = delayedInitR;

        // Read Reverb Post Delay output for Late Reverb
        lateInL[i] = interpolateReverbDelay (revBufL, static_cast<float> (reverbPostDelayWritePos) - reverbPostDelaySamples);
        lateInR[i] = interpolateReverbDelay (revBufR, static_cast<float> (reverbPostDelayWritePos) - reverbPostDelaySamples);

        reverbInitialDelayWritePos = (reverbInitialDelayWritePos + 1) % maxReverbDelayBufferSize;
        reverbPostDelayWritePos = (reverbPostDelayWritePos + 1) % maxReverbDelayBufferSize;
    }

    // Process Late Reverb
    juce::dsp::AudioBlock<float> block (reverbLateInputBuffer.getArrayOfWritePointers(), 2, 0, static_cast<size_t> (numSamples));
    juce::dsp::ProcessContextReplacing<float> context (block);
    variationReverbProcessor.process (context);

    auto* lateOutL = reverbLateInputBuffer.getWritePointer (0);
    auto* lateOutR = reverbLateInputBuffer.getWritePointer (1);

    // Combine ER and Late Reverb
    for (int i = 0; i < numSamples; ++i)
    {
        sigL[i] = erL[i] * reverbErGain + lateOutL[i] * reverbLateGain;
        sigR[i] = erR[i] * reverbErGain + lateOutR[i] * reverbLateGain;
    }

    if (postEqActive)
        applyPostEq (sigL, sigR, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + sigL[i] * wetGain;
        outR[i] = inR[i] * dryGain + sigR[i] * wetGain;
    }
}

void VariationEffectProcessor::processRotarySpeaker (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    auto* bufL = rotaryBuffer[0].data();
    auto* bufR = rotaryBuffer[1].data();

    auto* wetL = tempWetBuffer.getWritePointer (0);
    auto* wetR = tempWetBuffer.getWritePointer (1);

    const float phaseInc = static_cast<float> (twoPi * rotarySpeedHz / sampleRate);
    const float maxVibratoDelaySamples = static_cast<float> (sampleRate * 0.003);

    for (int i = 0; i < numSamples; ++i)
    {
        rotaryPhase += phaseInc;
        if (rotaryPhase >= twoPi) rotaryPhase -= twoPi;

        const float lfoL = std::sin (rotaryPhase);
        const float lfoR = std::sin (rotaryPhase + pi);

        const float delayModL = (lfoL * 0.5f + 0.5f) * maxVibratoDelaySamples * rotaryDepth + 1.0f;
        const float delayModR = (lfoR * 0.5f + 0.5f) * maxVibratoDelaySamples * rotaryDepth + 1.0f;

        float readPosL = static_cast<float> (rotaryWritePos) - delayModL;
        while (readPosL < 0.0f) readPosL += static_cast<float> (maxRotaryDelaySamples);
        const auto idxL0 = static_cast<int> (readPosL) % maxRotaryDelaySamples;
        const auto idxL1 = (idxL0 + 1) % maxRotaryDelaySamples;
        const auto fracL = readPosL - static_cast<float> (static_cast<int> (readPosL));
        const float delL = (1.0f - fracL) * bufL[idxL0] + fracL * bufL[idxL1];

        float readPosR = static_cast<float> (rotaryWritePos) - delayModR;
        while (readPosR < 0.0f) readPosR += static_cast<float> (maxRotaryDelaySamples);
        const auto idxR0 = static_cast<int> (readPosR) % maxRotaryDelaySamples;
        const auto idxR1 = (idxR0 + 1) % maxRotaryDelaySamples;
        const auto fracR = readPosR - static_cast<float> (static_cast<int> (readPosR));
        const float delR = (1.0f - fracR) * bufR[idxR0] + fracR * bufR[idxR1];

        bufL[rotaryWritePos] = inL[i];
        bufR[rotaryWritePos] = inR[i];
        rotaryWritePos = (rotaryWritePos + 1) % maxRotaryDelaySamples;

        const float ampModL = 1.0f - 0.35f * rotaryDepth * (lfoL * 0.5f + 0.5f);
        const float ampModR = 1.0f - 0.35f * rotaryDepth * (lfoR * 0.5f + 0.5f);

        wetL[i] = delL * ampModL;
        wetR[i] = delR * ampModR;
    }

    if (postEqActive)
        applyPostEq (wetL, wetR, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + wetL[i] * wetGain;
        outR[i] = inR[i] * dryGain + wetR[i] * wetGain;
    }
}

void VariationEffectProcessor::processEq (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    tempWetBuffer.copyFrom (0, 0, inL, numSamples);
    tempWetBuffer.copyFrom (1, 0, inR, numSamples);

    auto* wetL = tempWetBuffer.getWritePointer (0);
    auto* wetR = tempWetBuffer.getWritePointer (1);

    if (eqFiltersActive)
    {
        eqLowFilters[0].processSamples (wetL, numSamples);
        eqLowFilters[1].processSamples (wetR, numSamples);
        eqMidFilters[0].processSamples (wetL, numSamples);
        eqMidFilters[1].processSamples (wetR, numSamples);
        eqHighFilters[0].processSamples (wetL, numSamples);
        eqHighFilters[1].processSamples (wetR, numSamples);
    }

    if (postEqActive)
        applyPostEq (wetL, wetR, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + wetL[i] * wetGain;
        outR[i] = inR[i] * dryGain + wetR[i] * wetGain;
    }
}

void VariationEffectProcessor::processDistortion (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    tempWetBuffer.copyFrom (0, 0, inL, numSamples);
    tempWetBuffer.copyFrom (1, 0, inR, numSamples);

    auto* wetL = tempWetBuffer.getWritePointer (0);
    auto* wetR = tempWetBuffer.getWritePointer (1);

    if (distSubtype != DistortionSubtype::StereoDist)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            const float mono = 0.5f * (wetL[i] + wetR[i]);
            wetL[i] = mono;
            wetR[i] = mono;
        }
    }

    // 1. Compressor dynamics (Comp+Distortion)
    if (distSubtype == DistortionSubtype::CompDist && compRatio > 1.001f)
    {
        for (int i = 0; i < numSamples; ++i)
        {
            float* chPtrs[2] = { &wetL[i], &wetR[i] };
            for (int ch = 0; ch < 2; ++ch)
            {
                const float inAbs = std::abs (*chPtrs[ch]);
                if (inAbs > compEnvelope[static_cast<size_t> (ch)])
                    compEnvelope[static_cast<size_t> (ch)] = compAttackCoeff * compEnvelope[static_cast<size_t> (ch)] + (1.0f - compAttackCoeff) * inAbs;
                else
                    compEnvelope[static_cast<size_t> (ch)] = compReleaseCoeff * compEnvelope[static_cast<size_t> (ch)] + (1.0f - compReleaseCoeff) * inAbs;

                const float env = compEnvelope[static_cast<size_t> (ch)];
                if (env > compThreshold && compThreshold > 1e-6f)
                {
                    const float overRatio = env / compThreshold;
                    const float grLinear = std::pow (overRatio, (1.0f / compRatio) - 1.0f);
                    *chPtrs[ch] *= grLinear;
                }
            }
        }
    }

    if (distFiltersActive)
    {
        distPreFilters[0].processSamples (wetL, numSamples);
        distPreFilters[1].processSamples (wetR, numSamples);
    }

    const float modFactor = juce::jmax (0.1f, 1.0f + currentModOffset * 1.5f);
    const float effDrive = distortionDrive * modFactor;
    const float effOutGain = distortionOutputGain / std::sqrt (modFactor);

    for (int i = 0; i < numSamples; ++i)
    {
        float l = wetL[i];
        float r = wetR[i];

        l *= effDrive;
        r *= effDrive;

        if (distType == DistortionType::Overdrive)
        {
            const float kPos = 0.5f + distortionEdge * 1.0f;
            const float kNeg = 0.3f + distortionEdge * 0.6f;
            l = (l > 0.0f) ? (l / (1.0f + kPos * l)) : (l / (1.0f - kNeg * l));
            r = (r > 0.0f) ? (r / (1.0f + kPos * r)) : (r / (1.0f - kNeg * r));
        }
        else if (distType == DistortionType::AmpSim)
        {
            const float edgeScale = 0.6f + distortionEdge * 0.8f;
            l = std::tanh (l * edgeScale);
            r = std::tanh (r * edgeScale);
        }
        else
        {
            const float edgeScale = 0.8f + distortionEdge * 0.8f;
            const auto xL = juce::jlimit (-1.5f, 1.5f, l * edgeScale);
            const auto xR = juce::jlimit (-1.5f, 1.5f, r * edgeScale);
            const float cubic = 0.05f + 0.15f * distortionEdge;
            l = xL - cubic * xL * xL * xL;
            r = xR - cubic * xR * xR * xR;
        }

        wetL[i] = l * effOutGain;
        wetR[i] = r * effOutGain;
    }

    if (distFiltersActive)
    {
        distPostFilters[0].processSamples (wetL, numSamples);
        distPostFilters[1].processSamples (wetR, numSamples);

        if (distType == DistortionType::AmpSim)
        {
            ampSimCabFilters[0].processSamples (wetL, numSamples);
            ampSimCabFilters[1].processSamples (wetR, numSamples);
        }
    }

    if (postEqActive)
        applyPostEq (wetL, wetR, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + wetL[i] * wetGain;
        outR[i] = inR[i] * dryGain + wetR[i] * wetGain;
    }
}

void VariationEffectProcessor::processFlanger (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    auto* bufL = flangerBuffer[0].data();
    auto* bufR = flangerBuffer[1].data();
    const auto phaseInc = (twoPi * flangerRateHz) / static_cast<float> (sampleRate);
    const float effDepthSec = juce::jmax (0.00005f, flangerDepthSec * (1.0f + currentModOffset * 0.8f));

    auto* wetL = tempWetBuffer.getWritePointer (0);
    auto* wetR = tempWetBuffer.getWritePointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        const auto modL = 0.5f + 0.5f * std::sin (flangerPhase);
        const auto delaySecL = flangerOffsetSec + effDepthSec * modL;
        const auto delaySamplesL = delaySecL * static_cast<float> (sampleRate);

        float readPosL = static_cast<float> (flangerWritePos) - delaySamplesL;
        while (readPosL < 0.0f) readPosL += static_cast<float> (maxFlangerDelaySamples);
        const auto idxL0 = static_cast<int> (readPosL);
        const auto idxL1 = (idxL0 + 1) % maxFlangerDelaySamples;
        const auto fracL = readPosL - static_cast<float> (idxL0);
        const auto delL = (1.0f - fracL) * bufL[idxL0] + fracL * bufL[idxL1];

        const auto modR = 0.5f + 0.5f * std::sin (flangerPhase + 1.5707963f);
        const auto delaySecR = flangerOffsetSec + effDepthSec * modR;
        const auto delaySamplesR = delaySecR * static_cast<float> (sampleRate);

        float readPosR = static_cast<float> (flangerWritePos) - delaySamplesR;
        while (readPosR < 0.0f) readPosR += static_cast<float> (maxFlangerDelaySamples);
        const auto idxR0 = static_cast<int> (readPosR);
        const auto idxR1 = (idxR0 + 1) % maxFlangerDelaySamples;
        const auto fracR = readPosR - static_cast<float> (idxR0);
        const auto delR = (1.0f - fracR) * bufR[idxR0] + fracR * bufR[idxR1];

        bufL[flangerWritePos] = inL[i] + delL * flangerFeedback;
        bufR[flangerWritePos] = inR[i] + delR * flangerFeedback;

        flangerWritePos = (flangerWritePos + 1) % maxFlangerDelaySamples;
        flangerPhase += phaseInc;
        if (flangerPhase >= twoPi) flangerPhase -= twoPi;

        wetL[i] = delL;
        wetR[i] = delR;
    }

    if (postEqActive)
        applyPostEq (wetL, wetR, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + wetL[i] * wetGain;
        outR[i] = inR[i] * dryGain + wetR[i] * wetGain;
    }
}

void VariationEffectProcessor::processPhaser (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    const auto phaseInc = (twoPi * phaserRateHz) / static_cast<float> (sampleRate);
    const float effDepth = juce::jlimit (0.0f, 1.0f, phaserDepth + currentModOffset * 0.4f);
    const float effFb = juce::jlimit (-0.95f, 0.95f, phaserFeedback + currentModOffset * 0.3f);

    const float baseFreq = 300.0f + (phaserOffset / 127.0f) * 1200.0f; // 300Hz..1500Hz
    const float nyquist = static_cast<float> (sampleRate * 0.49);
    const float halfDiff = 0.5f * phaserLfoPhaseDiff;

    auto* wetL = tempWetBuffer.getWritePointer (0);
    auto* wetR = tempWetBuffer.getWritePointer (1);

    constexpr int kFreqUpdateInterval = 4;
    float aL = 0.0f, aR = 0.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        phaserPhase += phaseInc;
        if (phaserPhase >= twoPi) phaserPhase -= twoPi;

        if ((i % kFreqUpdateInterval) == 0)
        {
            const float modL = std::sin (phaserPhase - halfDiff);
            const float modR = std::sin (phaserPhase + halfDiff);

            const float fcL = juce::jlimit (40.0f, nyquist, baseFreq * std::pow (2.0f, effDepth * modL * 2.5f));
            const float fcR = juce::jlimit (40.0f, nyquist, baseFreq * std::pow (2.0f, effDepth * modR * 2.5f));

            const float tanL = std::tan (juce::MathConstants<float>::pi * fcL / static_cast<float> (sampleRate));
            aL = (tanL - 1.0f) / (tanL + 1.0f);

            const float tanR = std::tan (juce::MathConstants<float>::pi * fcR / static_cast<float> (sampleRate));
            aR = (tanR - 1.0f) / (tanR + 1.0f);
        }

        const float inChanL = phaserDiffusionMono ? 0.5f * (inL[i] + inR[i]) : inL[i];
        const float inChanR = phaserDiffusionMono ? 0.5f * (inL[i] + inR[i]) : inR[i];

        // Left Channel Allpass Chain
        float xL = inChanL + effFb * phaserLastOutput[0];
        for (int s = 0; s < phaserStages; ++s)
        {
            const float v = xL - aL * phaserFilterState[0][static_cast<size_t> (s)];
            xL = aL * v + phaserFilterState[0][static_cast<size_t> (s)];
            phaserFilterState[0][static_cast<size_t> (s)] = v;
        }
        phaserLastOutput[0] = xL;
        wetL[i] = 0.5f * (inChanL + xL);

        // Right Channel Allpass Chain
        float xR = inChanR + effFb * phaserLastOutput[1];
        for (int s = 0; s < phaserStages; ++s)
        {
            const float v = xR - aR * phaserFilterState[1][static_cast<size_t> (s)];
            xR = aR * v + phaserFilterState[1][static_cast<size_t> (s)];
            phaserFilterState[1][static_cast<size_t> (s)] = v;
        }
        phaserLastOutput[1] = xR;
        wetR[i] = 0.5f * (inChanR + xR);
    }

    if (postEqActive)
        applyPostEq (wetL, wetR, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + wetL[i] * wetGain;
        outR[i] = inR[i] * dryGain + wetR[i] * wetGain;
    }
}

void VariationEffectProcessor::processTremoloAutoPan (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    const auto phaseInc = (twoPi * modRateHz) / static_cast<float> (sampleRate);
    const float effDepth = juce::jlimit (0.0f, 1.0f, modDepth + currentModOffset * 0.5f);

    auto* wetL = tempWetBuffer.getWritePointer (0);
    auto* wetR = tempWetBuffer.getWritePointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        modPhase += phaseInc;
        if (modPhase >= twoPi) modPhase -= twoPi;

        float lGain = 1.0f;
        float rGain = 1.0f;

        if (isAutoPan)
        {
            const auto panNorm = 0.5f + 0.5f * std::sin (modPhase) * effDepth;
            lGain = std::cos (panNorm * 1.5707963f);
            rGain = std::sin (panNorm * 1.5707963f);
        }
        else
        {
            const float halfDiff = 0.5f * tremoloLfoPhaseDiff;
            lGain = 1.0f - effDepth * (0.5f + 0.5f * std::sin (modPhase - halfDiff));
            rGain = 1.0f - effDepth * (0.5f + 0.5f * std::sin (modPhase + halfDiff));
        }

        const float lInput = (!isAutoPan && tremoloMonoInput) ? 0.5f * (inL[i] + inR[i]) : inL[i];
        const float rInput = (!isAutoPan && tremoloMonoInput) ? 0.5f * (inL[i] + inR[i]) : inR[i];

        wetL[i] = lInput * lGain;
        wetR[i] = rInput * rGain;
    }

    if (postEqActive)
        applyPostEq (wetL, wetR, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + wetL[i] * wetGain;
        outR[i] = inR[i] * dryGain + wetR[i] * wetGain;
    }
}

void VariationEffectProcessor::processChorus (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    tempWetBuffer.copyFrom (0, 0, inL, numSamples);
    tempWetBuffer.copyFrom (1, 0, inR, numSamples);

    chorusProcessor.setDepth (juce::jlimit (0.0f, 1.0f, baseChorusDepth + currentModOffset * 0.4f));

    juce::dsp::AudioBlock<float> block (tempWetBuffer);
    auto subBlock = block.getSubBlock (0, static_cast<size_t> (numSamples));
    juce::dsp::ProcessContextReplacing<float> context (subBlock);
    chorusProcessor.process (context);

    auto* wetL = tempWetBuffer.getWritePointer (0);
    auto* wetR = tempWetBuffer.getWritePointer (1);

    if (postEqActive)
        applyPostEq (wetL, wetR, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + wetL[i] * wetGain;
        outR[i] = inR[i] * dryGain + wetR[i] * wetGain;
    }
}

void VariationEffectProcessor::processAutoWah (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    tempWetBuffer.copyFrom (0, 0, inL, numSamples);
    tempWetBuffer.copyFrom (1, 0, inR, numSamples);

    auto* wetL = tempWetBuffer.getWritePointer (0);
    auto* wetR = tempWetBuffer.getWritePointer (1);

    const auto phaseInc = (twoPi * wahLfoRateHz) / static_cast<float> (sampleRate);
    const auto nyquist = sampleRate * 0.49;
    const float effDepth = juce::jlimit (0.0f, 1.0f, wahLfoDepth + currentModOffset * 0.5f);

    constexpr int filterUpdateInterval = 16;

    for (int i = 0; i < numSamples; ++i)
    {
        wahLfoPhase += phaseInc;
        if (wahLfoPhase >= twoPi) wahLfoPhase -= twoPi;

        if ((i % filterUpdateInterval) == 0)
        {
            const auto lfoNorm = 0.5f + 0.5f * std::sin (wahLfoPhase);
            const auto modCutoff = juce::jlimit (0.0f, 1.0f, wahManualCutoff + effDepth * (lfoNorm - 0.5f) * 2.0f);
            const auto cutoffHz = juce::jlimit (150.0, nyquist, 200.0 * std::pow (20.0, static_cast<double> (modCutoff)));
            const auto wahCoeff = juce::IIRCoefficients::makeBandPass (sampleRate, cutoffHz, static_cast<double> (wahResonance));
            for (auto& f : wahFilters) f.setCoefficients (wahCoeff);
        }

        const auto lIn = wetL[i];
        const auto rIn = wetR[i];
        const auto lFilt = wahFilters[0].processSingleSampleRaw (lIn);
        const auto rFilt = wahFilters[1].processSingleSampleRaw (rIn);

        const float driveGain = 1.0f + autoWahDrive * 4.0f;
        float lSig = (lIn * 0.2f + lFilt * (wahResonance * 0.6f)) * driveGain;
        float rSig = (rIn * 0.2f + rFilt * (wahResonance * 0.6f)) * driveGain;

        if (autoWahDrive > 0.001f)
        {
            lSig = std::tanh (lSig) / std::sqrt (driveGain);
            rSig = std::tanh (rSig) / std::sqrt (driveGain);
        }

        wetL[i] = lSig;
        wetR[i] = rSig;
    }

    if (postEqActive)
        applyPostEq (wetL, wetR, numSamples);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + wetL[i] * wetGain;
        outR[i] = inR[i] * dryGain + wetR[i] * wetGain;
    }
}

void VariationEffectProcessor::process (const juce::AudioBuffer<float>& inBuffer,
                                        juce::AudioBuffer<float>& outBuffer,
                                        int numSamples) noexcept
{
    if (numSamples <= 0 || inBuffer.getNumChannels() < 2 || outBuffer.getNumChannels() < 2)
        return;

    if (tempWetBuffer.getNumSamples() < numSamples)
        tempWetBuffer.setSize (2, numSamples, false, false, true);

    const auto* inL = inBuffer.getReadPointer (0);
    const auto* inR = inBuffer.getReadPointer (1);
    auto* outL = outBuffer.getWritePointer (0);
    auto* outR = outBuffer.getWritePointer (1);

    switch (currentTypeMsb)
    {
        case xg::varTypeHall1:
        case xg::varTypeRoom1:
        case xg::varTypeStage1:
        case xg::varTypePlate:
        case xg::varTypeWhiteRoom:
        case xg::varTypeTunnel:
        case xg::varTypeCanyon:
        case xg::varTypeBasement:
            processVariationReverb (inL, inR, outL, outR, numSamples);
            break;

        case xg::varTypeDelayLCR:
        case xg::varTypeDelayLR:
        case xg::varTypeEcho:
        case xg::varTypeCrossDelay:
            processDelay (inL, inR, outL, outR, numSamples);
            break;

        case xg::varTypeRotarySpeaker:
            processRotarySpeaker (inL, inR, outL, outR, numSamples);
            break;

        case xg::varType3BandEq:
        case xg::varType2BandEq:
            processEq (inL, inR, outL, outR, numSamples);
            break;

        case xg::varTypeDistortion:
        case xg::varTypeOverdrive:
        case xg::varTypeAmpSimulator:
            processDistortion (inL, inR, outL, outR, numSamples);
            break;

        case xg::varTypeFlanger:
            processFlanger (inL, inR, outL, outR, numSamples);
            break;

        case xg::varTypeAutoWah:
            processAutoWah (inL, inR, outL, outR, numSamples);
            break;

        case xg::varTypePhaser:
            processPhaser (inL, inR, outL, outR, numSamples);
            break;

        case xg::varTypeTremolo:
        case xg::varTypeAutoPan:
            processTremoloAutoPan (inL, inR, outL, outR, numSamples);
            break;

        case xg::varTypeChorus:
        case xg::varTypeSymphonic:
            processChorus (inL, inR, outL, outR, numSamples);
            break;

        case xg::varTypeThru:
        default:
            outBuffer.copyFrom (0, 0, inBuffer, 0, 0, numSamples);
            outBuffer.copyFrom (1, 0, inBuffer, 1, 0, numSamples);
            break;
    }
}
