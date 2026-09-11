#include "VariationEffect.h"
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

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (maxBlockSize);
    spec.numChannels = 2;

    phaserProcessor.prepare (spec);
    phaserProcessor.reset();

    chorusProcessor.prepare (spec);
    chorusProcessor.reset();

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

    for (auto& f : distPreFilters) f.reset();
    for (auto& f : distPostFilters) f.reset();
    for (auto& f : ampSimCabFilters) f.reset();
    for (auto& f : wahFilters) f.reset();
    wahLfoPhase = 0.0f;
    currentModOffset = 0.0f;

    phaserProcessor.reset();
    chorusProcessor.reset();
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
        // Default
        wetGain = defaultWetRatio;
        dryGain = 1.0f - defaultWetRatio;
        return;
    }

    // XG Dry/Wet: 1 (D63>W) .. 64 (D=W) .. 127 (D<W63)
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

void VariationEffectProcessor::updateDelayParameters (const xg::VariationParameters& params)
{
    isCrossDelay = (currentTypeMsb == xg::varTypeCrossDelay);
    isDelayLCR   = (currentTypeMsb == xg::varTypeDelayLCR);

    // Param 1: Delay Time L (14-bit in 0.1ms units)
    float timeLMs = 250.0f;
    if (params.parameters14Bit[0] > 0)
        timeLMs = static_cast<float> (params.parameters14Bit[0]) * 0.1f;
    timeLMs = juce::jlimit (0.1f, 1486.0f, timeLMs);
    delayTimeLSamples = static_cast<float> (timeLMs * 0.001f * sampleRate);

    // Param 2: Delay Time R
    float timeRMs = timeLMs;
    if (params.parameters14Bit[1] > 0)
        timeRMs = static_cast<float> (params.parameters14Bit[1]) * 0.1f;
    timeRMs = juce::jlimit (0.1f, 1486.0f, timeRMs);
    delayTimeRSamples = static_cast<float> (timeRMs * 0.001f * sampleRate);

    // Param 3: Delay Time C (for Delay L,C,R)
    float timeCMs = (timeLMs + timeRMs) * 0.5f;
    if (params.parameters14Bit[2] > 0)
        timeCMs = static_cast<float> (params.parameters14Bit[2]) * 0.1f;
    timeCMs = juce::jlimit (0.1f, 1486.0f, timeCMs);
    delayTimeCSamples = static_cast<float> (timeCMs * 0.001f * sampleRate);

    // Param 5: Feedback Level (1..127 -> -63..+63, 64 = 0)
    const auto fbVal = params.parameters14Bit[4] > 0 ? static_cast<int> (params.parameters14Bit[4]) : 64;
    delayFeedback = juce::jlimit (-0.95f, 0.95f, static_cast<float> (fbVal - 64) / 64.0f * 0.9f);

    // Param 6: Feedback High Damp (0.1..1.0)
    const auto dampVal = params.parameters14Bit[5] > 0 ? static_cast<int> (params.parameters14Bit[5]) : 10;
    delayDamp = juce::jlimit (0.0f, 0.9f, 1.0f - (static_cast<float> (dampVal) / 10.0f));

    // Param 10: Dry / Wet
    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.4f);
}

void VariationEffectProcessor::updateDistortionParameters (const xg::VariationParameters& params)
{
    if (currentTypeMsb == xg::varTypeOverdrive)
        distType = DistortionType::Overdrive;
    else if (currentTypeMsb == xg::varTypeAmpSimulator)
        distType = DistortionType::AmpSim;
    else
        distType = DistortionType::Distortion;

    // Param 1: Drive (0..127)
    const auto driveVal = static_cast<float> (params.parameters14Bit[0] & 0x7F);
    if (distType == DistortionType::Overdrive)
        distortionDrive = 1.0f + (driveVal / 127.0f) * 25.0f;
    else if (distType == DistortionType::AmpSim)
        distortionDrive = 1.0f + (driveVal / 127.0f) * 35.0f;
    else
        distortionDrive = 1.0f + (driveVal / 127.0f) * 50.0f;

    // Param 5: Output Level
    const auto outVal = params.parameters14Bit[4] > 0 ? static_cast<float> (params.parameters14Bit[4] & 0x7F) : 64.0f;
    distortionOutputGain = (outVal / 64.0f) / std::sqrt (distortionDrive);

    // Filters
    const auto nyquist = sampleRate * 0.49;
    if (distType == DistortionType::AmpSim)
    {
        // Pre-filter: High-pass 120Hz + Mid boost at 800Hz
        const auto preCoeff = juce::IIRCoefficients::makeHighPass (sampleRate, 120.0);
        for (auto& f : distPreFilters) f.setCoefficients (preCoeff);

        // Cabinet sim post-filters: 4.5kHz low-pass
        const auto cabCoeff = juce::IIRCoefficients::makeLowPass (sampleRate, juce::jmin (nyquist, 4500.0));
        for (auto& f : ampSimCabFilters) f.setCoefficients (cabCoeff);

        const auto postCoeff = juce::IIRCoefficients::makeHighPass (sampleRate, 80.0);
        for (auto& f : distPostFilters) f.setCoefficients (postCoeff);
    }
    else
    {
        // Pre-filter: mild bass cut around 80Hz
        const auto preCoeff = juce::IIRCoefficients::makeHighPass (sampleRate, 80.0);
        for (auto& f : distPreFilters) f.setCoefficients (preCoeff);

        // Post-filter: LPF cutoff from Param 4 (0..127 -> 1kHz..16kHz)
        const auto lpfParam = params.parameters14Bit[3] > 0 ? static_cast<int> (params.parameters14Bit[3] & 0x7F) : 48;
        const auto lpfCutoff = juce::jlimit (1000.0, nyquist, static_cast<double> (xg::lookupEqFrequency (lpfParam)));
        const auto postCoeff = juce::IIRCoefficients::makeLowPass (sampleRate, lpfCutoff);
        for (auto& f : distPostFilters) f.setCoefficients (postCoeff);

        for (auto& f : ampSimCabFilters) f.makeInactive();
    }
    distFiltersActive = true;

    // Param 10: Dry / Wet (default 100% wet for distortion/amp)
    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 1.0f);
}

void VariationEffectProcessor::updateFlangerParameters (const xg::VariationParameters& params)
{
    // Param 1: LFO Freq (0..127 -> 0.05..20.0 Hz)
    const auto rateVal = static_cast<float> (params.parameters14Bit[0] & 0x7F);
    flangerRateHz = 0.05f + (rateVal / 127.0f) * 15.0f;

    // Param 2: LFO Depth (0..127 -> 0.1ms..5.0ms)
    const auto depthVal = static_cast<float> (params.parameters14Bit[1] & 0x7F);
    flangerDepthSec = 0.0002f + (depthVal / 127.0f) * 0.0035f;

    // Param 3: Feedback (-63..+63)
    const auto fbVal = params.parameters14Bit[2] > 0 ? static_cast<int> (params.parameters14Bit[2] & 0x7F) : 64;
    flangerFeedback = juce::jlimit (-0.95f, 0.95f, static_cast<float> (fbVal - 64) / 64.0f * 0.9f);

    // Param 4: Delay Offset (0.2ms..5.0ms)
    const auto offsetVal = static_cast<float> (params.parameters14Bit[3] & 0x7F);
    flangerOffsetSec = 0.0005f + (offsetVal / 127.0f) * 0.004f;

    // Param 10: Dry / Wet
    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.5f);
}

void VariationEffectProcessor::updatePhaserParameters (const xg::VariationParameters& params)
{
    // Param 1: LFO Freq
    const auto rateVal = static_cast<float> (params.parameters14Bit[0] & 0x7F);
    const auto rateHz = 0.1f + (rateVal / 127.0f) * 8.0f;
    phaserProcessor.setRate (rateHz);

    // Param 2: LFO Depth (0..1.0)
    const auto depthVal = static_cast<float> (params.parameters14Bit[1] & 0x7F);
    basePhaserDepth = depthVal / 127.0f;
    phaserProcessor.setDepth (basePhaserDepth);

    // Param 4: Feedback
    const auto fbVal = params.parameters14Bit[3] > 0 ? static_cast<int> (params.parameters14Bit[3] & 0x7F) : 64;
    basePhaserFeedback = juce::jlimit (-0.9f, 0.9f, static_cast<float> (fbVal - 64) / 64.0f * 0.85f);
    phaserProcessor.setFeedback (basePhaserFeedback);

    // Param 10: Dry / Wet
    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.5f);
}

void VariationEffectProcessor::updateTremoloAutoPanParameters (const xg::VariationParameters& params)
{
    isAutoPan = (currentTypeMsb == xg::varTypeAutoPan);

    // Param 1: LFO Freq (0..127 -> 0.1..25.0 Hz)
    const auto rateVal = static_cast<float> (params.parameters14Bit[0] & 0x7F);
    modRateHz = 0.1f + (rateVal / 127.0f) * 20.0f;

    // Param 2: LFO Depth (0..1.0)
    const auto depthVal = static_cast<float> (params.parameters14Bit[1] & 0x7F);
    modDepth = juce::jlimit (0.0f, 1.0f, depthVal / 127.0f);

    // Param 10: Dry / Wet (default 100% wet for tremolo/pan)
    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 1.0f);
}

void VariationEffectProcessor::updateChorusParameters (const xg::VariationParameters& params)
{
    const auto rateVal = static_cast<float> (params.parameters14Bit[0] & 0x7F);
    chorusProcessor.setRate (0.2f + (rateVal / 127.0f) * 5.0f);

    const auto depthVal = static_cast<float> (params.parameters14Bit[1] & 0x7F);
    baseChorusDepth = depthVal / 127.0f;
    chorusProcessor.setDepth (baseChorusDepth);

    const auto fbVal = params.parameters14Bit[2] > 0 ? static_cast<int> (params.parameters14Bit[2] & 0x7F) : 64;
    chorusProcessor.setFeedback (juce::jlimit (0.0f, 0.8f, static_cast<float> (fbVal - 64) / 64.0f * 0.7f));

    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 0.5f);
}

void VariationEffectProcessor::updateAutoWahParameters (const xg::VariationParameters& params)
{
    // Param 1: LFO Freq (0..127 -> 0.05..20.0 Hz)
    const auto rateVal = static_cast<float> (params.parameters14Bit[0] & 0x7F);
    wahLfoRateHz = 0.05f + (rateVal / 127.0f) * 20.0f;

    // Param 2: LFO Depth (0..127 -> 0.0..1.0)
    const auto depthVal = static_cast<float> (params.parameters14Bit[1] & 0x7F);
    wahLfoDepth = depthVal / 127.0f;

    // Param 3: Cutoff Frequency Offset / Manual Sweep (0..127 -> 0.0..1.0)
    const auto cutoffVal = params.parameters14Bit[2] > 0 ? static_cast<float> (params.parameters14Bit[2] & 0x7F) : 64.0f;
    wahManualCutoff = cutoffVal / 127.0f;

    // Param 4: Resonance (10..120 -> 1.0..12.0)
    const auto resoVal = params.parameters14Bit[3] > 0 ? static_cast<int> (params.parameters14Bit[3] & 0x7F) : 30;
    wahResonance = resoVal >= 10 ? juce::jlimit (1.0f, 10.0f, static_cast<float> (resoVal) / 10.0f)
                                 : juce::jlimit (1.0f, 10.0f, 1.0f + static_cast<float> (resoVal) * 0.2f);

    // Param 10: Dry / Wet (default 100% wet)
    updateDryWet (static_cast<uint8_t> (params.parameters14Bit[9] & 0x7F), 1.0f);
}

void VariationEffectProcessor::updateParameters (const xg::VariationParameters& params)
{
    currentTypeMsb = params.typeMsb;
    currentTypeLsb = params.typeLsb;

    // Controller modulation depths (-64..+63 -> -1.0 .. +0.984)
    mwDepth   = static_cast<float> (static_cast<int> (params.mwControlDepth) - 64) / 64.0f;
    bendDepth = static_cast<float> (static_cast<int> (params.bendControlDepth) - 64) / 64.0f;
    catDepth  = static_cast<float> (static_cast<int> (params.catControlDepth) - 64) / 64.0f;
    ac1Depth  = static_cast<float> (static_cast<int> (params.ac1ControlDepth) - 64) / 64.0f;
    ac2Depth  = static_cast<float> (static_cast<int> (params.ac2ControlDepth) - 64) / 64.0f;

    switch (currentTypeMsb)
    {
        case xg::varTypeDelayLCR:
        case xg::varTypeDelayLR:
        case xg::varTypeEcho:
        case xg::varTypeCrossDelay:
            updateDelayParameters (params);
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

    const float effFeedback = juce::jlimit (-0.95f, 0.95f, delayFeedback + currentModOffset * 0.4f);

    for (int i = 0; i < numSamples; ++i)
    {
        // Read Left
        float readPosL = static_cast<float> (delayWritePos) - delayTimeLSamples;
        while (readPosL < 0.0f) readPosL += static_cast<float> (maxDelaySamples);
        const auto idxL0 = static_cast<int> (readPosL);
        const auto idxL1 = (idxL0 + 1) % maxDelaySamples;
        const auto fracL = readPosL - static_cast<float> (idxL0);
        const auto delayedL = (1.0f - fracL) * bufL[idxL0] + fracL * bufL[idxL1];

        // Read Right
        float readPosR = static_cast<float> (delayWritePos) - delayTimeRSamples;
        while (readPosR < 0.0f) readPosR += static_cast<float> (maxDelaySamples);
        const auto idxR0 = static_cast<int> (readPosR);
        const auto idxR1 = (idxR0 + 1) % maxDelaySamples;
        const auto fracR = readPosR - static_cast<float> (idxR0);
        const auto delayedR = (1.0f - fracR) * bufR[idxR0] + fracR * bufR[idxR1];

        float wetL = delayedL;
        float wetR = delayedR;

        if (isDelayLCR)
        {
            float readPosC = static_cast<float> (delayWritePos) - delayTimeCSamples;
            while (readPosC < 0.0f) readPosC += static_cast<float> (maxDelaySamples);
            const auto idxC0 = static_cast<int> (readPosC);
            const auto idxC1 = (idxC0 + 1) % maxDelaySamples;
            const auto fracC = readPosC - static_cast<float> (idxC0);
            const auto delayedLC = (1.0f - fracC) * bufL[idxC0] + fracC * bufL[idxC1];
            const auto delayedRC = (1.0f - fracC) * bufR[idxC0] + fracC * bufR[idxC1];
            const auto delayedC = 0.5f * (delayedLC + delayedRC);
            wetL += 0.707f * delayedC;
            wetR += 0.707f * delayedC;
        }

        // High damp filter on feedback
        delayDampState[0] += (1.0f - delayDamp) * (delayedL - delayDampState[0]);
        delayDampState[1] += (1.0f - delayDamp) * (delayedR - delayDampState[1]);

        const auto fbL = delayDampState[0] * effFeedback;
        const auto fbR = delayDampState[1] * effFeedback;

        // Write input + feedback to circular buffer
        if (isCrossDelay)
        {
            bufL[delayWritePos] = inL[i] + fbR;
            bufR[delayWritePos] = inR[i] + fbL;
        }
        else
        {
            bufL[delayWritePos] = inL[i] + fbL;
            bufR[delayWritePos] = inR[i] + fbR;
        }

        delayWritePos = (delayWritePos + 1) % maxDelaySamples;

        outL[i] = inL[i] * dryGain + wetL * wetGain;
        outR[i] = inR[i] * dryGain + wetR * wetGain;
    }
}

void VariationEffectProcessor::processDistortion (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    tempWetBuffer.copyFrom (0, 0, inL, numSamples);
    tempWetBuffer.copyFrom (1, 0, inR, numSamples);

    auto* wetL = tempWetBuffer.getWritePointer (0);
    auto* wetR = tempWetBuffer.getWritePointer (1);

    // Pre-filters
    if (distFiltersActive)
    {
        distPreFilters[0].processSamples (wetL, numSamples);
        distPreFilters[1].processSamples (wetR, numSamples);
    }

    const float modFactor = juce::jmax (0.1f, 1.0f + currentModOffset * 1.5f);
    const float effDrive = distortionDrive * modFactor;
    const float effOutGain = distortionOutputGain / std::sqrt (modFactor);

    // Drive + Waveshape
    for (int i = 0; i < numSamples; ++i)
    {
        float l = wetL[i] * effDrive;
        float r = wetR[i] * effDrive;

        if (distType == DistortionType::Overdrive)
        {
            // Asymmetric soft saturation
            wetL[i] = (l > 0.0f) ? (l / (1.0f + l)) : (l / (1.0f - 0.6f * l));
            wetR[i] = (r > 0.0f) ? (r / (1.0f + r)) : (r / (1.0f - 0.6f * r));
        }
        else if (distType == DistortionType::AmpSim)
        {
            // Tube-like tanh saturation
            wetL[i] = std::tanh (l);
            wetR[i] = std::tanh (r);
        }
        else
        {
            // Distortion: harder diode clipping
            const auto xL = juce::jlimit (-1.5f, 1.5f, l * 1.2f);
            const auto xR = juce::jlimit (-1.5f, 1.5f, r * 1.2f);
            wetL[i] = xL - 0.15f * xL * xL * xL;
            wetR[i] = xR - 0.15f * xR * xR * xR;
        }

        wetL[i] *= effOutGain;
        wetR[i] *= effOutGain;
    }

    // Post-filters
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

    // Mix Dry and Wet
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

    for (int i = 0; i < numSamples; ++i)
    {
        // Left delay
        const auto modL = 0.5f + 0.5f * std::sin (flangerPhase);
        const auto delaySecL = flangerOffsetSec + effDepthSec * modL;
        const auto delaySamplesL = delaySecL * static_cast<float> (sampleRate);

        float readPosL = static_cast<float> (flangerWritePos) - delaySamplesL;
        while (readPosL < 0.0f) readPosL += static_cast<float> (maxFlangerDelaySamples);
        const auto idxL0 = static_cast<int> (readPosL);
        const auto idxL1 = (idxL0 + 1) % maxFlangerDelaySamples;
        const auto fracL = readPosL - static_cast<float> (idxL0);
        const auto wetL = (1.0f - fracL) * bufL[idxL0] + fracL * bufL[idxL1];

        // Right delay (90 deg phase offset)
        const auto modR = 0.5f + 0.5f * std::sin (flangerPhase + 1.5707963f);
        const auto delaySecR = flangerOffsetSec + effDepthSec * modR;
        const auto delaySamplesR = delaySecR * static_cast<float> (sampleRate);

        float readPosR = static_cast<float> (flangerWritePos) - delaySamplesR;
        while (readPosR < 0.0f) readPosR += static_cast<float> (maxFlangerDelaySamples);
        const auto idxR0 = static_cast<int> (readPosR);
        const auto idxR1 = (idxR0 + 1) % maxFlangerDelaySamples;
        const auto fracR = readPosR - static_cast<float> (idxR0);
        const auto wetR = (1.0f - fracR) * bufR[idxR0] + fracR * bufR[idxR1];

        bufL[flangerWritePos] = inL[i] + wetL * flangerFeedback;
        bufR[flangerWritePos] = inR[i] + wetR * flangerFeedback;

        flangerWritePos = (flangerWritePos + 1) % maxFlangerDelaySamples;
        flangerPhase += phaseInc;
        if (flangerPhase >= twoPi) flangerPhase -= twoPi;

        outL[i] = inL[i] * dryGain + wetL * wetGain;
        outR[i] = inR[i] * dryGain + wetR * wetGain;
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

    const auto* wetL = tempWetBuffer.getReadPointer (0);
    const auto* wetR = tempWetBuffer.getReadPointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + wetL[i] * wetGain;
        outR[i] = inR[i] * dryGain + wetR[i] * wetGain;
    }
}

void VariationEffectProcessor::processTremoloAutoPan (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    const float effRateHz = juce::jlimit (0.05f, 35.0f, modRateHz * std::pow (2.0f, currentModOffset * 1.5f));
    const float effDepth = juce::jlimit (0.0f, 1.0f, modDepth + currentModOffset * 0.3f);
    const auto phaseInc = (twoPi * effRateHz) / static_cast<float> (sampleRate);

    for (int i = 0; i < numSamples; ++i)
    {
        float gainL = 1.0f;
        float gainR = 1.0f;

        if (isAutoPan)
        {
            // 180 deg out of phase panning
            const auto s = std::sin (modPhase);
            gainL = 1.0f + effDepth * s;
            gainR = 1.0f - effDepth * s;
        }
        else
        {
            // Tremolo: in-phase volume modulation
            const auto mod = 0.5f + 0.5f * std::sin (modPhase);
            const auto g = 1.0f - effDepth * (1.0f - mod);
            gainL = g;
            gainR = g;
        }

        modPhase += phaseInc;
        if (modPhase >= twoPi) modPhase -= twoPi;

        const auto wetL = inL[i] * gainL;
        const auto wetR = inR[i] * gainR;

        outL[i] = inL[i] * dryGain + wetL * wetGain;
        outR[i] = inR[i] * dryGain + wetR * wetGain;
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

    const auto* wetL = tempWetBuffer.getReadPointer (0);
    const auto* wetR = tempWetBuffer.getReadPointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        outL[i] = inL[i] * dryGain + wetL[i] * wetGain;
        outR[i] = inR[i] * dryGain + wetR[i] * wetGain;
    }
}

void VariationEffectProcessor::processAutoWah (const float* inL, const float* inR, float* outL, float* outR, int numSamples) noexcept
{
    const auto nyquist = sampleRate * 0.49;
    const auto phaseInc = (twoPi * wahLfoRateHz) / static_cast<float> (sampleRate);
    constexpr int stepSize = 16;

    for (int offset = 0; offset < numSamples; offset += stepSize)
    {
        const int chunkLen = juce::jmin (stepSize, numSamples - offset);

        // LFO value (-1.0 .. +1.0)
        const float lfoVal = std::sin (wahLfoPhase);
        wahLfoPhase += phaseInc * static_cast<float> (chunkLen);
        if (wahLfoPhase >= twoPi)
            wahLfoPhase = std::fmod (wahLfoPhase, twoPi);

        // Modulated normalized cutoff frequency:
        // Manual Cutoff (0..1) + LFO sweep (+- wahLfoDepth * 0.45) + Controller pedal sweep (+- currentModOffset * 0.55)
        float normFreq = wahManualCutoff + (lfoVal * wahLfoDepth * 0.45f) + (currentModOffset * 0.55f);
        normFreq = juce::jlimit (0.01f, 0.99f, normFreq);

        // Exponential frequency mapping: ~180 Hz to ~4500 Hz
        const double cutoffHz = juce::jlimit (120.0, nyquist, 180.0 * std::pow (2.0, static_cast<double> (normFreq) * 4.65));

        // Resonant bandpass filter
        const auto coeff = juce::IIRCoefficients::makeBandPass (sampleRate, cutoffHz, wahResonance);
        wahFilters[0].setCoefficients (coeff);
        wahFilters[1].setCoefficients (coeff);

        const auto* srcL = inL + offset;
        const auto* srcR = inR + offset;
        auto* dstL = outL + offset;
        auto* dstR = outR + offset;

        // Bandpass gain compensation based on resonance
        const float bpGain = 1.0f + 0.6f * std::sqrt (wahResonance);

        for (int i = 0; i < chunkLen; ++i)
        {
            const float filteredL = wahFilters[0].processSingleSampleRaw (srcL[i]) * bpGain;
            const float filteredR = wahFilters[1].processSingleSampleRaw (srcR[i]) * bpGain;

            // Blend 20% direct signal for bass body warmth
            const float wetL = filteredL + 0.2f * srcL[i];
            const float wetR = filteredR + 0.2f * srcR[i];

            dstL[i] = srcL[i] * dryGain + wetL * wetGain;
            dstR[i] = srcR[i] * dryGain + wetR * wetGain;
        }
    }
}

void VariationEffectProcessor::process (const juce::AudioBuffer<float>& inBuffer,
                                        juce::AudioBuffer<float>& outBuffer,
                                        int numSamples) noexcept
{
    if (numSamples <= 0 || inBuffer.getNumChannels() < 2 || outBuffer.getNumChannels() < 2)
        return;

    const auto* inL = inBuffer.getReadPointer (0);
    const auto* inR = inBuffer.getReadPointer (1);
    auto* outL = outBuffer.getWritePointer (0);
    auto* outR = outBuffer.getWritePointer (1);

    switch (currentTypeMsb)
    {
        case xg::varTypeDelayLCR:
        case xg::varTypeDelayLR:
        case xg::varTypeEcho:
        case xg::varTypeCrossDelay:
            processDelay (inL, inR, outL, outR, numSamples);
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
