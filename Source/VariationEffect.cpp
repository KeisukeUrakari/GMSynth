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

    phaserProcessor.prepare (spec);
    phaserProcessor.reset();

    chorusProcessor.prepare (spec);
    chorusProcessor.reset();

    variationReverbProcessor.prepare (spec);
    variationReverbProcessor.reset();

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

    phaserProcessor.reset();
    chorusProcessor.reset();
    variationReverbProcessor.reset();
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
    const float revTimeSec = xg::tables::lookupReverbTime (params.parameters14Bit[0] & 0x7F);
    const float norm = std::log (juce::jmax (0.3f, revTimeSec) / 0.3f) / std::log (30.0f / 0.3f);
    const float roomSize = juce::jlimit (0.05f, 0.98f, 0.10f + 0.88f * std::pow (norm, 0.75f));

    const float lpfCutoffHz = xg::tables::lookupEqFrequency (params.parameters14Bit[4] & 0x7F);
    const float dampNorm = 1.0f - (std::log10 (juce::jlimit (1000.0f, 20000.0f, lpfCutoffHz) / 1000.0f) / std::log10 (20.0f));
    const float damping = juce::jlimit (0.0f, 1.0f, dampNorm);

    float width = 1.0f;
    if (params.parameters14Bit[1] > 0)
        width = juce::jlimit (0.2f, 1.0f, static_cast<float> (params.parameters14Bit[1] & 0x7F) / 10.0f);

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
    isStereoDistortion = (params.typeLsb != 0);

    if (currentTypeMsb == xg::varTypeOverdrive)
        distType = DistortionType::Overdrive;
    else if (currentTypeMsb == xg::varTypeAmpSimulator)
        distType = DistortionType::AmpSim;
    else
        distType = DistortionType::Distortion;

    const auto driveVal = static_cast<float> (params.parameters14Bit[0] & 0x7F);
    if (distType == DistortionType::Overdrive)
        distortionDrive = 1.0f + (driveVal / 127.0f) * 25.0f;
    else if (distType == DistortionType::AmpSim)
        distortionDrive = 1.0f + (driveVal / 127.0f) * 35.0f;
    else
        distortionDrive = 1.0f + (driveVal / 127.0f) * 50.0f;

    const auto outVal = params.parameters14Bit[4] > 0 ? static_cast<float> (params.parameters14Bit[4] & 0x7F) : 64.0f;
    distortionOutputGain = (outVal / 64.0f) / std::sqrt (distortionDrive);

    const auto nyquist = sampleRate * 0.49;
    if (distType == DistortionType::AmpSim)
    {
        const auto preCoeff = juce::IIRCoefficients::makeHighPass (sampleRate, 120.0);
        for (auto& f : distPreFilters) f.setCoefficients (preCoeff);

        const auto cabCoeff = juce::IIRCoefficients::makeLowPass (sampleRate, juce::jmin (nyquist, 4500.0));
        for (auto& f : ampSimCabFilters) f.setCoefficients (cabCoeff);

        const auto postCoeff = juce::IIRCoefficients::makeHighPass (sampleRate, 80.0);
        for (auto& f : distPostFilters) f.setCoefficients (postCoeff);
    }
    else
    {
        const auto preCoeff = juce::IIRCoefficients::makeHighPass (sampleRate, 80.0);
        for (auto& f : distPreFilters) f.setCoefficients (preCoeff);

        const auto lpfParam = params.parameters14Bit[3] > 0 ? static_cast<int> (params.parameters14Bit[3] & 0x7F) : 48;
        const auto lpfCutoff = juce::jlimit (1000.0, nyquist, static_cast<double> (xg::lookupEqFrequency (lpfParam)));
        const auto postCoeff = juce::IIRCoefficients::makeLowPass (sampleRate, lpfCutoff);
        for (auto& f : distPostFilters) f.setCoefficients (postCoeff);

        for (auto& f : ampSimCabFilters) f.makeInactive();
    }
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
    const float rateHz = xg::tables::lookupLfoFrequency (params.parameters14Bit[0] & 0x7F);
    phaserProcessor.setRate (juce::jmax (0.01f, rateHz));

    const auto depthVal = static_cast<float> (params.parameters14Bit[1] & 0x7F);
    basePhaserDepth = depthVal / 127.0f;
    phaserProcessor.setDepth (basePhaserDepth);

    const auto fbVal = params.parameters14Bit[3] > 0 ? static_cast<int> (params.parameters14Bit[3] & 0x7F) : 64;
    basePhaserFeedback = juce::jlimit (-0.9f, 0.9f, static_cast<float> (fbVal - 64) / 64.0f * 0.85f);
    phaserProcessor.setFeedback (basePhaserFeedback);

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
        case xg::varTypeAmpSimulator:
            updateDistortionParameters (params);
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

    juce::dsp::AudioBlock<float> block (tempWetBuffer.getArrayOfWritePointers(), 2, 0, static_cast<size_t> (numSamples));
    juce::dsp::ProcessContextReplacing<float> context (block);
    variationReverbProcessor.process (context);

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
        float l = wetL[i] * effDrive;
        float r = wetR[i] * effDrive;

        if (distType == DistortionType::Overdrive)
        {
            wetL[i] = (l > 0.0f) ? (l / (1.0f + l)) : (l / (1.0f - 0.6f * l));
            wetR[i] = (r > 0.0f) ? (r / (1.0f + r)) : (r / (1.0f - 0.6f * r));
        }
        else if (distType == DistortionType::AmpSim)
        {
            wetL[i] = std::tanh (l);
            wetR[i] = std::tanh (r);
        }
        else
        {
            const auto xL = juce::jlimit (-1.5f, 1.5f, l * 1.2f);
            const auto xR = juce::jlimit (-1.5f, 1.5f, r * 1.2f);
            wetL[i] = xL - 0.15f * xL * xL * xL;
            wetR[i] = xR - 0.15f * xR * xR * xR;
        }

        wetL[i] *= effOutGain;
        wetR[i] *= effOutGain;
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
    tempWetBuffer.copyFrom (0, 0, inL, numSamples);
    tempWetBuffer.copyFrom (1, 0, inR, numSamples);

    phaserProcessor.setDepth (juce::jlimit (0.0f, 1.0f, basePhaserDepth + currentModOffset * 0.4f));
    phaserProcessor.setFeedback (juce::jlimit (-0.95f, 0.95f, basePhaserFeedback + currentModOffset * 0.3f));

    juce::dsp::AudioBlock<float> block (tempWetBuffer);
    auto subBlock = block.getSubBlock (0, static_cast<size_t> (numSamples));
    juce::dsp::ProcessContextReplacing<float> context (subBlock);
    phaserProcessor.process (context);

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
            const auto trem = 1.0f - effDepth * (0.5f + 0.5f * std::sin (modPhase));
            lGain = trem;
            rGain = trem;
        }

        wetL[i] = inL[i] * lGain;
        wetR[i] = inR[i] * rGain;
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

        wetL[i] = lIn * 0.2f + lFilt * (wahResonance * 0.6f);
        wetR[i] = rIn * 0.2f + rFilt * (wahResonance * 0.6f);
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
