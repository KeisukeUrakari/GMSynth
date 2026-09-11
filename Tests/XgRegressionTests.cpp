#include <iostream>
#include <cassert>
#include <cmath>
#include <vector>
#include <string>

#include "../JuceLibraryCode/JuceHeader.h"
#include "../Source/XgModel.h"
#include "../Source/XgEffectDefaults.h"
#include "../Source/VariationEffect.h"
#include "../Source/FluidSynthEngine.h"

namespace
{
    int gPassedTests = 0;
    int gFailedTests = 0;

    void logTestResult (const std::string& testName, bool passed, const std::string& detail = "")
    {
        if (passed)
        {
            std::cout << "  [PASS] " << testName << std::endl;
            ++gPassedTests;
        }
        else
        {
            std::cerr << "  [FAIL] " << testName;
            if (! detail.empty())
                std::cerr << " (" << detail << ")";
            std::cerr << std::endl;
            ++gFailedTests;
        }
    }

    bool isNear (float a, float b, float tolerance = 0.001f)
    {
        return std::abs (a - b) <= tolerance;
    }
}

// =============================================================================
// TEST-E01: Variation Effect Type MSB Constants and DSP Selection
// =============================================================================
void testE01_VariationTypeMsbConstants()
{
    std::cout << "\n=== TEST-E01: Variation Type MSB Constants and DSP Selection ===" << std::endl;

    // Verify all MSB constants match XG Format V1.35 specifications (efctmap.pdf p.20)
    logTestResult ("varTypeChorus is 0x41", xg::varTypeChorus == 0x41);
    logTestResult ("varTypeFlanger is 0x43", xg::varTypeFlanger == 0x43);
    logTestResult ("varTypeSymphonic is 0x44", xg::varTypeSymphonic == 0x44);
    logTestResult ("varTypeTremolo is 0x46", xg::varTypeTremolo == 0x46);
    logTestResult ("varTypeAutoPan is 0x47", xg::varTypeAutoPan == 0x47);
    logTestResult ("varTypePhaser is 0x48", xg::varTypePhaser == 0x48);
    logTestResult ("varTypeDistortion is 0x49", xg::varTypeDistortion == 0x49);
    logTestResult ("varTypeOverdrive is 0x4A", xg::varTypeOverdrive == 0x4A);
    logTestResult ("varTypeAmpSimulator is 0x4B", xg::varTypeAmpSimulator == 0x4B);
    logTestResult ("varTypeAutoWah is 0x4E", xg::varTypeAutoWah == 0x4E);

    // Verify DSP selection in VariationEffectProcessor
    VariationEffectProcessor processor;
    processor.prepare (44100.0, 512);

    xg::VariationParameters params;
    params.reset();

    // 1. Distortion (0x49) -> distortion Drive is configured for Distortion (up to 50x)
    params.typeMsb = xg::varTypeDistortion;
    params.parameters14Bit[0] = 127; // Max Drive
    processor.updateParameters (params);
    // Overdrive max drive: 26.0f, AmpSim: 36.0f, Distortion: 51.0f
    // If it incorrectly selected AmpSim, it would be 36.0f.
    logTestResult ("Distortion (0x49) does not select AmpSim",
                   processor.getDistortionTypeForTest() == VariationEffectProcessor::DistortionType::Distortion);

    // 2. Overdrive (0x4A) -> Overdrive
    params.typeMsb = xg::varTypeOverdrive;
    processor.updateParameters (params);
    logTestResult ("Overdrive (0x4A) selects Overdrive",
                   processor.getDistortionTypeForTest() == VariationEffectProcessor::DistortionType::Overdrive);

    // 3. Amp Simulator (0x4B) -> AmpSim
    params.typeMsb = xg::varTypeAmpSimulator;
    processor.updateParameters (params);
    logTestResult ("Amp Simulator (0x4B) selects AmpSim",
                   processor.getDistortionTypeForTest() == VariationEffectProcessor::DistortionType::AmpSim);

    // 4. Auto Pan (0x47) -> isAutoPan
    params.typeMsb = xg::varTypeAutoPan;
    processor.updateParameters (params);
    logTestResult ("Auto Pan (0x47) sets isAutoPan = true", processor.getIsAutoPanForTest() == true);

    // 5. Tremolo (0x46) -> not isAutoPan
    params.typeMsb = xg::varTypeTremolo;
    processor.updateParameters (params);
    logTestResult ("Tremolo (0x46) sets isAutoPan = false", processor.getIsAutoPanForTest() == false);

    // DoD: all ten corrected type numbers must select a non-bypass DSP path.
    struct TypeCase { uint8_t msb; const char* name; };
    const std::array<TypeCase, 10> typeCases {{
        { xg::varTypeChorus, "Chorus" },
        { xg::varTypeFlanger, "Flanger" },
        { xg::varTypeSymphonic, "Symphonic" },
        { xg::varTypeTremolo, "Tremolo" },
        { xg::varTypeAutoPan, "Auto Pan" },
        { xg::varTypePhaser, "Phaser" },
        { xg::varTypeDistortion, "Distortion" },
        { xg::varTypeOverdrive, "Overdrive" },
        { xg::varTypeAmpSimulator, "Amp Simulator" },
        { xg::varTypeAutoWah, "Auto Wah" }
    }};

    juce::AudioBuffer<float> input (2, 4096);
    for (int i = 0; i < input.getNumSamples(); ++i)
    {
        const auto sample = 0.25f * std::sin (2.0f * 3.14159265f * 440.0f * static_cast<float> (i) / 44100.0f);
        input.setSample (0, i, sample);
        input.setSample (1, i, sample);
    }

    for (const auto& typeCase : typeCases)
    {
        VariationEffectProcessor typeProcessor;
        typeProcessor.prepare (44100.0, input.getNumSamples());
        xg::VariationParameters typeParams;
        typeParams.reset();
        typeParams.typeMsb = typeCase.msb;
        const auto defaults = xg::defaults::getVariationDefaults (typeParams.typeMsb, 0x00);
        typeParams.parameters14Bit = defaults.params14Bit;
        typeParams.parameters11To16 = defaults.params11To16;
        typeProcessor.updateParameters (typeParams);

        juce::AudioBuffer<float> output (2, input.getNumSamples());
        typeProcessor.process (input, output, input.getNumSamples());
        float difference = 0.0f;
        for (int ch = 0; ch < 2; ++ch)
            for (int i = 0; i < input.getNumSamples(); ++i)
                difference += std::abs (output.getSample (ch, i) - input.getSample (ch, i));

        logTestResult (std::string (typeCase.name) + " selects an active DSP path",
                       typeProcessor.getCurrentTypeMsb() == typeCase.msb && difference > 0.01f);
    }
}

// =============================================================================
// TEST-E03: Effect Default Tables and Type Change Initialization
// =============================================================================
void testE03_EffectDefaultTables()
{
    std::cout << "\n=== TEST-E03: Effect Default Tables and Type Change ===" << std::endl;

    // Hall 1 (0x01, 0x00) defaults: 18, 10, 8, 13, 49
    const auto hall1 = xg::defaults::getReverbDefaults (0x01, 0x00);
    logTestResult ("Hall 1 Param 1..5 are 18, 10, 8, 13, 49",
                   hall1[0] == 18 && hall1[1] == 10 && hall1[2] == 8 && hall1[3] == 13 && hall1[4] == 49);

    // Hall 2 (0x01, 0x01) defaults: 25, 10, 28, 6, 46
    const auto hall2 = xg::defaults::getReverbDefaults (0x01, 0x01);
    logTestResult ("Hall 2 Param 1..5 are 25, 10, 28, 6, 46",
                   hall2[0] == 25 && hall2[1] == 10 && hall2[2] == 28 && hall2[3] == 6 && hall2[4] == 46);

    // ReverbParameters::reset() loads Hall 1 defaults
    xg::ReverbParameters reverb;
    reverb.reset();
    logTestResult ("ReverbParameters::reset loads Hall 1 defaults",
                   reverb.parameters[0] == 18 && reverb.parameters[1] == 10 && reverb.parameters[2] == 8);

    // Chorus 1 (0x41, 0x00) defaults: 6, 54, 77, 106, 0
    const auto chorus1 = xg::defaults::getChorusDefaults (0x41, 0x00);
    logTestResult ("Chorus 1 Param 1..4 are 6, 54, 77, 106",
                   chorus1[0] == 6 && chorus1[1] == 54 && chorus1[2] == 77 && chorus1[3] == 106);

    // Variation Delay LCR (0x05) defaults: 3333, 1667, 5000, 5000, 74, 100, 10, 0, 0, 32
    const auto delayLCR = xg::defaults::getVariationDefaults (0x05, 0x00);
    logTestResult ("Delay LCR Param 1..5 are 3333, 1667, 5000, 5000, 74",
                   delayLCR.params14Bit[0] == 3333 && delayLCR.params14Bit[1] == 1667
                   && delayLCR.params14Bit[2] == 5000 && delayLCR.params14Bit[3] == 5000
                   && delayLCR.params14Bit[4] == 74);

    // Engine Type Change test via SysEx
    FluidSynthEngine engine;

    // Send XG System On: F0 43 10 4C 00 00 7E 00 F7
    const uint8_t xgSystemOn[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    engine.handleSysExForTest (xgSystemOn, sizeof (xgSystemOn));

    // Reverb should initially be Hall 1 defaults
    logTestResult ("After XG System On, Reverb has Hall 1 initial params",
                   engine.getReverbParametersForTest().parameters[0] == 18
                   && engine.getReverbParametersForTest().parameters[1] == 10);

    // Send Reverb Type change to Hall 2: F0 43 10 4C 02 01 00 01 01 F7
    const uint8_t setHall2[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x00, 0x01, 0x01, 0xF7 };
    engine.handleSysExForTest (setHall2, sizeof (setHall2));

    logTestResult ("After Type change to Hall 2, Reverb parameters re-initialized to Hall 2",
                   engine.getReverbParametersForTest().parameters[0] == 25
                   && engine.getReverbParametersForTest().parameters[2] == 28);
}

// =============================================================================
// TEST-E04: Proper Handling of Valid Zero Values
// =============================================================================
void testE04_ZeroValueHandling()
{
    std::cout << "\n=== TEST-E04: Valid Zero Values Handling ===" << std::endl;

    FluidSynthEngine engine;
    const uint8_t xgSystemOn[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    engine.handleSysExForTest (xgSystemOn, sizeof (xgSystemOn));

    // Initially Chorus Depth is default (CHORUS 1: 54 / 127.0f)
    logTestResult ("Initial Chorus depth > 0", engine.getChorusProcessorDepthForTest() > 0.0f);

    // Send Chorus Depth = 0 SysEx: F0 43 10 4C 02 01 23 00 F7 (Addr 02 01 23 = Chorus Param 2)
    const uint8_t setChorusDepthZero[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x23, 0x00, 0xF7 };
    engine.handleSysExForTest (setChorusDepthZero, sizeof (setChorusDepthZero));

    logTestResult ("Chorus Depth=0 sets DSP depth to exactly 0.0f (modulation stopped)",
                   engine.getChorusProcessorDepthForTest() == 0.0f);

    // Set Chorus Depth to 64 -> modulation restored
    const uint8_t setChorusDepth64[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x23, 0x40, 0xF7 };
    engine.handleSysExForTest (setChorusDepth64, sizeof (setChorusDepth64));

    logTestResult ("Chorus Depth=64 restores DSP depth to ~0.5f",
                   isNear (engine.getChorusProcessorDepthForTest(), 64.0f / 127.0f, 0.01f));

    // Test Tremolo in VariationEffectProcessor: Depth = 0
    VariationEffectProcessor varProc;
    varProc.prepare (44100.0, 512);
    xg::VariationParameters varParams;
    varParams.reset();
    varParams.typeMsb = xg::varTypeTremolo;
    varParams.parameters14Bit[1] = 0; // LFO Depth = 0
    varProc.updateParameters (varParams);

    logTestResult ("Tremolo LFO Depth=0 results in modDepth = 0.0f",
                   varProc.getModDepthForTest() == 0.0f);

    varParams.parameters14Bit[1] = 64;
    varProc.updateParameters (varParams);
    logTestResult ("Tremolo LFO Depth=64 results in modDepth ~ 0.5f",
                   isNear (varProc.getModDepthForTest(), 64.0f / 127.0f, 0.01f));

    // --- Audio Waveform Modulation Verification (DoD verification) ---
    // 1. Tremolo Audio Modulation Stop / Resume
    constexpr int numModSamples = 4096;
    constexpr int warmupSamples = 4096;

    juce::AudioBuffer<float> tremWarmup (2, warmupSamples);
    juce::AudioBuffer<float> tremWarmupOut (2, warmupSamples);
    for (int i = 0; i < warmupSamples; ++i)
    {
        const float s = std::sin (2.0f * 3.14159265f * 441.0f * static_cast<float> (i) / 44100.0f);
        tremWarmup.setSample (0, i, s);
        tremWarmup.setSample (1, i, s);
    }

    juce::AudioBuffer<float> tremIn (2, numModSamples);
    for (int i = 0; i < numModSamples; ++i)
    {
        const float s = std::sin (2.0f * 3.14159265f * 441.0f * static_cast<float> (i + warmupSamples) / 44100.0f);
        tremIn.setSample (0, i, s);
        tremIn.setSample (1, i, s);
    }
    juce::AudioBuffer<float> tremOut (2, numModSamples);

    varParams.reset();
    varParams.typeMsb = xg::varTypeTremolo;
    varParams.parameters14Bit[0] = 64;  // LFO Rate ≈ 2.69 Hz
    varParams.parameters14Bit[9] = 127; // 100% Wet
    varParams.parameters14Bit[1] = 0;   // Depth = 0 (stopped)
    varProc.prepare (44100.0, numModSamples);
    varProc.updateParameters (varParams);

    varProc.process (tremWarmup, tremWarmupOut, warmupSamples); // warmup filter and delay state
    varProc.process (tremIn, tremOut, numModSamples);

    // 200 samples = exactly 2 cycles of 441Hz at 44100Hz
    constexpr int subBlockSize = 200;
    const int numSubBlocks = numModSamples / subBlockSize;
    float minRmsDepth0 = 1000.0f;
    float maxRmsDepth0 = 0.0f;
    for (int b = 0; b < numSubBlocks; ++b)
    {
        const float rms = tremOut.getRMSLevel (0, b * subBlockSize, subBlockSize);
        minRmsDepth0 = std::min (minRmsDepth0, rms);
        maxRmsDepth0 = std::max (maxRmsDepth0, rms);
    }
    logTestResult ("Tremolo audio with Depth=0 has zero amplitude modulation (rms variation < 0.005f)",
                   (maxRmsDepth0 - minRmsDepth0) < 0.005f);

    // Now resume modulation with Depth = 127
    varParams.parameters14Bit[1] = 127;
    varProc.updateParameters (varParams);
    varProc.process (tremIn, tremOut, numModSamples);

    float minRmsDepth127 = 1000.0f;
    float maxRmsDepth127 = 0.0f;
    for (int b = 0; b < numSubBlocks; ++b)
    {
        const float rms = tremOut.getRMSLevel (0, b * subBlockSize, subBlockSize);
        minRmsDepth127 = std::min (minRmsDepth127, rms);
        maxRmsDepth127 = std::max (maxRmsDepth127, rms);
    }
    logTestResult ("Tremolo audio with Depth=127 exhibits active amplitude modulation (rms variation > 0.3f)",
                   (maxRmsDepth127 - minRmsDepth127) > 0.3f);

    // 2. Chorus Audio Modulation Stop / Resume
    // Use 441Hz (exactly 100 samples per period at 44100Hz) and warmup to bypass initial delay filling
    juce::AudioBuffer<float> choWarmup (2, warmupSamples);
    juce::AudioBuffer<float> choWarmupOut (2, warmupSamples);
    for (int i = 0; i < warmupSamples; ++i)
    {
        const float s = std::sin (2.0f * 3.14159265f * 441.0f * static_cast<float> (i) / 44100.0f);
        choWarmup.setSample (0, i, s);
        choWarmup.setSample (1, i, s);
    }

    juce::AudioBuffer<float> choIn (2, numModSamples);
    for (int i = 0; i < numModSamples; ++i)
    {
        const float s = std::sin (2.0f * 3.14159265f * 441.0f * static_cast<float> (i + warmupSamples) / 44100.0f);
        choIn.setSample (0, i, s);
        choIn.setSample (1, i, s);
    }
    juce::AudioBuffer<float> choOut (2, numModSamples);

    xg::VariationParameters choParams;
    choParams.reset();
    choParams.typeMsb = xg::varTypeChorus;
    choParams.parameters14Bit[0] = 64;  // Rate ≈ 2.69 Hz
    choParams.parameters14Bit[9] = 127; // 100% Wet
    choParams.parameters14Bit[1] = 0;   // Depth = 0 (stopped)
    varProc.updateParameters (choParams);
    varProc.process (choWarmup, choWarmupOut, warmupSamples); // warmup delay line
    varProc.process (choIn, choOut, numModSamples);

    // 200 samples = exactly 2 integer cycles of 441Hz sine wave
    constexpr int choBlockSize = 200;
    const int numChoBlocks = numModSamples / choBlockSize;
    float minRmsChoDepth0 = 1000.0f;
    float maxRmsChoDepth0 = 0.0f;
    for (int b = 0; b < numChoBlocks; ++b)
    {
        const float rms = choOut.getRMSLevel (0, b * choBlockSize, choBlockSize);
        minRmsChoDepth0 = std::min (minRmsChoDepth0, rms);
        maxRmsChoDepth0 = std::max (maxRmsChoDepth0, rms);
    }
    logTestResult ("Chorus audio with Depth=0 has constant output envelope (modulation stopped)",
                   (maxRmsChoDepth0 - minRmsChoDepth0) < 0.005f);

    // Resume Chorus modulation with Depth = 127
    choParams.parameters14Bit[1] = 127;
    varProc.updateParameters (choParams);
    varProc.process (choIn, choOut, numModSamples);

    float minRmsChoDepth127 = 1000.0f;
    float maxRmsChoDepth127 = 0.0f;
    for (int b = 0; b < numChoBlocks; ++b)
    {
        const float rms = choOut.getRMSLevel (0, b * choBlockSize, choBlockSize);
        minRmsChoDepth127 = std::min (minRmsChoDepth127, rms);
        maxRmsChoDepth127 = std::max (maxRmsChoDepth127, rms);
    }
    logTestResult ("Chorus audio with Depth=127 resumes modulation and alters envelope",
                   (maxRmsChoDepth127 - minRmsChoDepth127) > 0.01f);

    // 3. Out-of-range zero value handling (e.g. Feedback where 0 is outside 1..127 range)
    // Specification: Chorus Feedback 1..127 (-63..+63, 64=0). Value 0 is out-of-range.
    const uint8_t setChorusFbPositive[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x24, 0x60, 0xF7 };
    engine.handleSysExForTest (setChorusFbPositive, sizeof (setChorusFbPositive));
    const auto feedbackBeforeInvalidWrite = engine.getChorusParametersForTest().parameters[2];
    const uint8_t setChorusFbZero[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x24, 0x00, 0xF7 };
    engine.handleSysExForTest (setChorusFbZero, sizeof (setChorusFbZero));
    logTestResult ("Out-of-range zero for Chorus Feedback is rejected and preserves the prior value",
                   engine.getChorusParametersForTest().parameters[2] == feedbackBeforeInvalidWrite);

    // Reverb Time uses Table#4, where data 0 is valid and means 0.3 seconds.
    const uint8_t setRevTimeZero[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x02, 0x00, 0xF7 };
    engine.handleSysExForTest (setRevTimeZero, sizeof (setRevTimeZero));
    logTestResult ("Valid Reverb Time=0 is stored and maps to the 0.3s minimum",
                   engine.getReverbParametersForTest().parameters[0] == 0
                   && isNear (engine.getReverbProcessorRoomSizeForTest(), 0.10f, 0.02f));
}

// =============================================================================
// TEST-E05: Inter-Effect Send Level Conversion
// =============================================================================
void testE05_EffectSendConversion()
{
    std::cout << "\n=== TEST-E05: Inter-Effect Send Level Conversion ===" << std::endl;

    // Send conversion formula:
    // val == 0: 0.0f
    // val == 64: 1.0f (0 dB)
    // val == 127: ~1.99526f (+6 dB)
    // 1..64: val / 64.0f
    // 65..127: 1.0 + (val - 64)/63 * (10^(6/20) - 1.0)
    logTestResult ("Send 0 yields gain 0.0f (-inf dB)", FluidSynthEngine::testConvertEffectSendLevel (0) == 0.0f);
    logTestResult ("Send 64 yields gain 1.0f (0 dB)", FluidSynthEngine::testConvertEffectSendLevel (64) == 1.0f);
    logTestResult ("Send 127 yields gain ~1.99526f (+6 dB)",
                   isNear (FluidSynthEngine::testConvertEffectSendLevel (127), 1.99526f, 0.001f));

    // Mid points
    logTestResult ("Send 32 yields gain 0.5f (-6 dB)",
                   isNear (FluidSynthEngine::testConvertEffectSendLevel (32), 0.5f, 0.001f));
    const auto gain96 = FluidSynthEngine::testConvertEffectSendLevel (96);
    const auto expected96 = 1.0f + (32.0f / 63.0f) * (1.9952623f - 1.0f);
    logTestResult ("Send 96 yields expected interpolated gain", isNear (gain96, expected96, 0.001f));

    // --- Audio Rendering & Route Measurement (DoD verification) ---
    FluidSynthEngine engine;
    engine.prepare (44100.0, 512);

    const uint8_t xgSystemOn[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    engine.handleSysExForTest (xgSystemOn, sizeof (xgSystemOn));
    engine.setEffectBypassForTest (true);

    // Mute Part 1 Dry Level (Addr 08 00 11 = 0) and zero other sends initially
    const uint8_t setDry0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x11, 0x00, 0xF7 };
    engine.handleSysExForTest (setDry0, sizeof (setDry0));
    const uint8_t setPartRev0_init[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x13, 0x00, 0xF7 };
    engine.handleSysExForTest (setPartRev0_init, sizeof (setPartRev0_init));
    const uint8_t setPartCho0_init[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x12, 0x00, 0xF7 };
    engine.handleSysExForTest (setPartCho0_init, sizeof (setPartCho0_init));
    const uint8_t setPartVar0_init[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x14, 0x00, 0xF7 };
    engine.handleSysExForTest (setPartVar0_init, sizeof (setPartVar0_init));

    // Prepare 1.0 amplitude 1000Hz test sine signal
    juce::AudioBuffer<float> inSignal (2, 512);
    for (int i = 0; i < 512; ++i)
    {
        const float s = std::sin (2.0f * 3.14159265f * 1000.0f * static_cast<float> (i) / 44100.0f);
        inSignal.setSample (0, i, s);
        inSignal.setSample (1, i, s);
    }
    juce::AudioBuffer<float> outBuf (2, 512);

    // 1. Route 1: Variation -> Chorus
    // Set Variation to System connection (02 01 5A = 01)
    const uint8_t setVarSystem[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x5A, 0x01, 0xF7 };
    engine.handleSysExForTest (setVarSystem, sizeof (setVarSystem));
    // Part 1 VarSend = 127 (Part -> Var gain = 1.0, Addr 08 00 14 = 7F)
    const uint8_t setPartVarSend127[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x14, 0x7F, 0xF7 };
    engine.handleSysExForTest (setPartVarSend127, sizeof (setPartVarSend127));
    // Var Return = 0 (do not sum Var directly to master)
    const uint8_t setVarReturn0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x56, 0x00, 0xF7 };
    engine.handleSysExForTest (setVarReturn0, sizeof (setVarReturn0));
    // Var Send to Reverb = 0
    const uint8_t setVarToRev0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x58, 0x00, 0xF7 };
    engine.handleSysExForTest (setVarToRev0, sizeof (setVarToRev0));
    // Chorus Return = 64 (0 dB)
    const uint8_t setChoReturn64[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x2C, 0x40, 0xF7 };
    engine.handleSysExForTest (setChoReturn64, sizeof (setChoReturn64));
    // Chorus Send to Reverb = 0
    const uint8_t setChoToRev0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x2E, 0x00, 0xF7 };
    engine.handleSysExForTest (setChoToRev0, sizeof (setChoToRev0));
    // Reverb Return = 0
    const uint8_t setRevReturn0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x0C, 0x00, 0xF7 };
    engine.handleSysExForTest (setRevReturn0, sizeof (setRevReturn0));

    // Var -> Chorus Send = 0 (Addr 02 01 59 = 00)
    const uint8_t setVarToCho0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x59, 0x00, 0xF7 };
    engine.handleSysExForTest (setVarToCho0, sizeof (setVarToCho0));
    engine.renderBlockWithInjectedPartForTest (outBuf, 0, inSignal);
    logTestResult ("Audio route Var->Chorus: Send 0 produces zero output",
                   outBuf.getMagnitude (0, 512) == 0.0f);

    // Var -> Chorus Send = 64 (Addr 02 01 59 = 40)
    const uint8_t setVarToCho64[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x59, 0x40, 0xF7 };
    engine.handleSysExForTest (setVarToCho64, sizeof (setVarToCho64));
    engine.renderBlockWithInjectedPartForTest (outBuf, 0, inSignal);
    const float varToChoGain64 = outBuf.getMagnitude (0, 512);
    logTestResult ("Audio route Var->Chorus: Send 64 produces gain ~1.0f (0 dB)",
                   isNear (varToChoGain64, 1.0f, 0.02f));

    // Var -> Chorus Send = 127 (Addr 02 01 59 = 7F)
    const uint8_t setVarToCho127[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x59, 0x7F, 0xF7 };
    engine.handleSysExForTest (setVarToCho127, sizeof (setVarToCho127));
    engine.renderBlockWithInjectedPartForTest (outBuf, 0, inSignal);
    const float varToChoGain127 = outBuf.getMagnitude (0, 512);
    logTestResult ("Audio route Var->Chorus: Send 127 produces gain ~1.995f (+6 dB)",
                   isNear (varToChoGain127 / varToChoGain64, 1.99526f, 0.02f));

    // 2. Route 2: Chorus -> Reverb
    // Part 1 VarSend = 0, Part 1 ChoSend = 127 (Part -> Cho gain = 1.0)
    const uint8_t setPartVarSend0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x14, 0x00, 0xF7 };
    engine.handleSysExForTest (setPartVarSend0, sizeof (setPartVarSend0));
    const uint8_t setPartChoSend127[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x12, 0x7F, 0xF7 };
    engine.handleSysExForTest (setPartChoSend127, sizeof (setPartChoSend127));
    // Chorus Return = 0 (do not sum Chorus directly to master)
    const uint8_t setChoReturn0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x2C, 0x00, 0xF7 };
    engine.handleSysExForTest (setChoReturn0, sizeof (setChoReturn0));
    // Reverb Return = 64 (0 dB)
    const uint8_t setRevReturn64[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x0C, 0x40, 0xF7 };
    engine.handleSysExForTest (setRevReturn64, sizeof (setRevReturn64));

    // Cho -> Reverb Send = 0 (Addr 02 01 2E = 00)
    const uint8_t setChoToRev0_test[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x2E, 0x00, 0xF7 };
    engine.handleSysExForTest (setChoToRev0_test, sizeof (setChoToRev0_test));
    engine.renderBlockWithInjectedPartForTest (outBuf, 0, inSignal);
    logTestResult ("Audio route Chorus->Reverb: Send 0 produces zero output",
                   outBuf.getMagnitude (0, 512) == 0.0f);

    // Cho -> Reverb Send = 64 (Addr 02 01 2E = 40)
    const uint8_t setChoToRev64[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x2E, 0x40, 0xF7 };
    engine.handleSysExForTest (setChoToRev64, sizeof (setChoToRev64));
    engine.renderBlockWithInjectedPartForTest (outBuf, 0, inSignal);
    const float choToRevGain64 = outBuf.getMagnitude (0, 512);
    logTestResult ("Audio route Chorus->Reverb: Send 64 produces gain ~1.0f (0 dB)",
                   isNear (choToRevGain64, 1.0f, 0.02f));

    // Cho -> Reverb Send = 127 (Addr 02 01 2E = 7F)
    const uint8_t setChoToRev127[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x2E, 0x7F, 0xF7 };
    engine.handleSysExForTest (setChoToRev127, sizeof (setChoToRev127));
    engine.renderBlockWithInjectedPartForTest (outBuf, 0, inSignal);
    const float choToRevGain127 = outBuf.getMagnitude (0, 512);
    logTestResult ("Audio route Chorus->Reverb: Send 127 produces gain ~1.995f (+6 dB)",
                   isNear (choToRevGain127 / choToRevGain64, 1.99526f, 0.02f));

    // 3. Route 3: Variation -> Reverb
    // Part 1 ChoSend = 0, Part 1 VarSend = 127 (Part -> Var gain = 1.0)
    const uint8_t setPartChoSend0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x12, 0x00, 0xF7 };
    engine.handleSysExForTest (setPartChoSend0, sizeof (setPartChoSend0));
    engine.handleSysExForTest (setPartVarSend127, sizeof (setPartVarSend127));
    // Var -> Chorus Send = 0
    engine.handleSysExForTest (setVarToCho0, sizeof (setVarToCho0));

    // Var -> Reverb Send = 0 (Addr 02 01 58 = 00)
    const uint8_t setVarToRev0_test[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x58, 0x00, 0xF7 };
    engine.handleSysExForTest (setVarToRev0_test, sizeof (setVarToRev0_test));
    engine.renderBlockWithInjectedPartForTest (outBuf, 0, inSignal);
    logTestResult ("Audio route Var->Reverb: Send 0 produces zero output",
                   outBuf.getMagnitude (0, 512) == 0.0f);

    // Var -> Reverb Send = 64 (Addr 02 01 58 = 40)
    const uint8_t setVarToRev64[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x58, 0x40, 0xF7 };
    engine.handleSysExForTest (setVarToRev64, sizeof (setVarToRev64));
    engine.renderBlockWithInjectedPartForTest (outBuf, 0, inSignal);
    const float varToRevGain64 = outBuf.getMagnitude (0, 512);
    logTestResult ("Audio route Var->Reverb: Send 64 produces gain ~1.0f (0 dB)",
                   isNear (varToRevGain64, 1.0f, 0.02f));

    // Var -> Reverb Send = 127 (Addr 02 01 58 = 7F)
    const uint8_t setVarToRev127[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x58, 0x7F, 0xF7 };
    engine.handleSysExForTest (setVarToRev127, sizeof (setVarToRev127));
    engine.renderBlockWithInjectedPartForTest (outBuf, 0, inSignal);
    const float varToRevGain127 = outBuf.getMagnitude (0, 512);
    logTestResult ("Audio route Var->Reverb: Send 127 produces gain ~1.995f (+6 dB)",
                   isNear (varToRevGain127 / varToRevGain64, 1.99526f, 0.02f));

    // 4. Verification that Part Send is NOT modified (value / 127.0f, NOT +6 dB)
    engine.handleSysExForTest (setPartVarSend0, sizeof (setPartVarSend0));
    engine.handleSysExForTest (setVarToRev0_test, sizeof (setVarToRev0_test));

    // Part 1 RevSend = 64 (Addr 08 00 13 = 40) -> 64 / 127.0f ≈ 0.5039f
    const uint8_t setPartRevSend64[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x13, 0x40, 0xF7 };
    engine.handleSysExForTest (setPartRevSend64, sizeof (setPartRevSend64));
    engine.renderBlockWithInjectedPartForTest (outBuf, 0, inSignal);
    const float partRevGain64 = outBuf.getMagnitude (0, 512);
    logTestResult ("Part Send=64 preserves standard value/127 gain (~0.504f), NOT +0 dB (1.0f)",
                   isNear (partRevGain64, 64.0f / 127.0f, 0.02f));

    // Part 1 RevSend = 127 (Addr 08 00 13 = 7F) -> 127 / 127.0f = 1.0f
    const uint8_t setPartRevSend127[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x13, 0x7F, 0xF7 };
    engine.handleSysExForTest (setPartRevSend127, sizeof (setPartRevSend127));
    engine.renderBlockWithInjectedPartForTest (outBuf, 0, inSignal);
    const float partRevGain127 = outBuf.getMagnitude (0, 512);
    logTestResult ("Part Send=127 produces gain ~1.0f (0 dB), NOT +6 dB (~2.0f)",
                   isNear (partRevGain127, 1.0f, 0.02f));
}

// =============================================================================
// TEST-E09: Multi EQ Preset Values and Filter Update
// =============================================================================
void testE09_MultiEqPresets()
{
    std::cout << "\n=== TEST-E09: Multi EQ Preset Values and Shape ===" << std::endl;

    xg::MultiEqParameters eq;

    // 1. Flat (0) (efctparamdeflt.pdf p.40)
    eq.setPreset (0);
    logTestResult ("Flat preset: All gains are 64", eq.isFlat());
    logTestResult ("Flat frequencies: 12, 28, 34, 46, 52",
                   eq.freq1 == 12 && eq.freq2 == 28 && eq.freq3 == 34 && eq.freq4 == 46 && eq.freq5 == 52);
    logTestResult ("Flat Q: 7, 7, 7, 7, 7",
                   eq.q1 == 7 && eq.q2 == 7 && eq.q3 == 7 && eq.q4 == 7 && eq.q5 == 7);
    logTestResult ("Flat Shapes: Shelving (0) for Band 1 & 5",
                   eq.shape1 == 0 && eq.shape5 == 0);

    // 2. Jazz (1): Gain {58, 66, 68, 60, 58}, Freq {8, 16, 33, 44, 50}, Q {7, 3, 3, 5, 7}, Shapes {0, 0}
    eq.setPreset (1);
    logTestResult ("Jazz preset gains: 58, 66, 68, 60, 58",
                   eq.gain1 == 58 && eq.gain2 == 66 && eq.gain3 == 68 && eq.gain4 == 60 && eq.gain5 == 58);
    logTestResult ("Jazz preset frequencies: 8, 16, 33, 44, 50",
                   eq.freq1 == 8 && eq.freq2 == 16 && eq.freq3 == 33 && eq.freq4 == 44 && eq.freq5 == 50);
    logTestResult ("Jazz preset Q: 7, 3, 3, 5, 7",
                   eq.q1 == 7 && eq.q2 == 3 && eq.q3 == 3 && eq.q4 == 5 && eq.q5 == 7);
    logTestResult ("Jazz preset Shapes: Shelving (0) for Band 1 & 5",
                   eq.shape1 == 0 && eq.shape5 == 0);

    // 3. Pops (2): Gain {68, 60, 67, 60, 70}, Freq {16, 24, 34, 40, 48}, Q {7, 20, 7, 20, 7}, Shapes {0, 0}
    eq.setPreset (2);
    logTestResult ("Pops preset gains: 68, 60, 67, 60, 70",
                   eq.gain1 == 68 && eq.gain2 == 60 && eq.gain3 == 67 && eq.gain4 == 60 && eq.gain5 == 70);
    logTestResult ("Pops preset frequencies: 16, 24, 34, 40, 48",
                   eq.freq1 == 16 && eq.freq2 == 24 && eq.freq3 == 34 && eq.freq4 == 40 && eq.freq5 == 48);
    logTestResult ("Pops preset Q: 7, 20, 7, 20, 7",
                   eq.q1 == 7 && eq.q2 == 20 && eq.q3 == 7 && eq.q4 == 20 && eq.q5 == 7);
    logTestResult ("Pops preset Shapes: Shelving (0) for Band 1 & 5",
                   eq.shape1 == 0 && eq.shape5 == 0);

    // 4. Rock (3): Gain {71, 68, 60, 68, 66}, Freq {16, 20, 36, 41, 50}, Q {7, 7, 5, 10, 7}, Shapes {0, 0}
    eq.setPreset (3);
    logTestResult ("Rock preset gains: 71, 68, 60, 68, 66",
                   eq.gain1 == 71 && eq.gain2 == 68 && eq.gain3 == 60 && eq.gain4 == 68 && eq.gain5 == 66);
    logTestResult ("Rock preset frequencies: 16, 20, 36, 41, 50",
                   eq.freq1 == 16 && eq.freq2 == 20 && eq.freq3 == 36 && eq.freq4 == 41 && eq.freq5 == 50);
    logTestResult ("Rock preset Q: 7, 7, 5, 10, 7",
                   eq.q1 == 7 && eq.q2 == 7 && eq.q3 == 5 && eq.q4 == 10 && eq.q5 == 7);
    logTestResult ("Rock preset Shapes: Shelving (0) for Band 1 & 5",
                   eq.shape1 == 0 && eq.shape5 == 0);

    // 5. Concert (4): Gain {67, 68, 64, 66, 61}, Freq {12, 24, 34, 50, 52}, Q {7, 7, 5, 7, 7}, Shapes {0, 0}
    eq.setPreset (4);
    logTestResult ("Concert preset gains: 67, 68, 64, 66, 61",
                   eq.gain1 == 67 && eq.gain2 == 68 && eq.gain3 == 64 && eq.gain4 == 66 && eq.gain5 == 61);
    logTestResult ("Concert preset frequencies: 12, 24, 34, 50, 52",
                   eq.freq1 == 12 && eq.freq2 == 24 && eq.freq3 == 34 && eq.freq4 == 50 && eq.freq5 == 52);
    logTestResult ("Concert preset Q: 7, 7, 5, 7, 7",
                   eq.q1 == 7 && eq.q2 == 7 && eq.q3 == 5 && eq.q4 == 7 && eq.q5 == 7);
    logTestResult ("Concert preset Shapes: Shelving (0) for Band 1 & 5",
                   eq.shape1 == 0 && eq.shape5 == 0);

    // Resetting back to Flat after custom edit restores all 17 parameters completely
    eq.gain1 = 80; eq.freq1 = 40; eq.q1 = 25; eq.shape1 = 1;
    eq.gain2 = 80; eq.freq2 = 40; eq.q2 = 25;
    eq.gain3 = 80; eq.freq3 = 40; eq.q3 = 25;
    eq.gain4 = 80; eq.freq4 = 40; eq.q4 = 25;
    eq.gain5 = 80; eq.freq5 = 40; eq.q5 = 25; eq.shape5 = 1;

    eq.setPreset (0);
    logTestResult ("Resetting to Flat cleans up custom edits across all 17 parameters",
                   eq.gain1 == 64 && eq.freq1 == 12 && eq.q1 == 7 && eq.shape1 == 0
                   && eq.gain2 == 64 && eq.freq2 == 28 && eq.q2 == 7
                   && eq.gain3 == 64 && eq.freq3 == 34 && eq.q3 == 7
                   && eq.gain4 == 64 && eq.freq4 == 46 && eq.q4 == 7
                   && eq.gain5 == 64 && eq.freq5 == 52 && eq.q5 == 7 && eq.shape5 == 0);

    // --- DSP Frequency Response Audio Measurement (DoD verification) ---
    FluidSynthEngine eqEngine;
    eqEngine.prepare (44100.0, 512);

    const uint8_t eqXgOn[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    eqEngine.handleSysExForTest (eqXgOn, sizeof (eqXgOn));
    eqEngine.setEffectBypassForTest (false); // ensure Multi EQ is active

    // Part 1 Dry Level = 127, all sends = 0
    const uint8_t setDry127[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x11, 0x7F, 0xF7 };
    eqEngine.handleSysExForTest (setDry127, sizeof (setDry127));
    const uint8_t setCho0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x12, 0x00, 0xF7 };
    eqEngine.handleSysExForTest (setCho0, sizeof (setCho0));
    const uint8_t setRev0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x13, 0x00, 0xF7 };
    eqEngine.handleSysExForTest (setRev0, sizeof (setRev0));
    const uint8_t setVar0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x14, 0x00, 0xF7 };
    eqEngine.handleSysExForTest (setVar0, sizeof (setVar0));

    auto makeSineSignal = [] (float freqHz, int numSamples)
    {
        juce::AudioBuffer<float> buf (2, numSamples);
        for (int i = 0; i < numSamples; ++i)
        {
            const float s = std::sin (2.0f * 3.14159265f * freqHz * static_cast<float> (i) / 44100.0f);
            buf.setSample (0, i, s);
            buf.setSample (1, i, s);
        }
        return buf;
    };

    const auto lowSine = makeSineSignal (50.0f, 512);
    const auto midSine = makeSineSignal (1000.0f, 512);
    const auto highSine = makeSineSignal (8000.0f, 512);
    juce::AudioBuffer<float> eqOut (2, 512);

    // 1. Flat (Baseline RMS)
    const uint8_t setEqFlat[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x40, 0x00, 0x00, 0xF7 };
    eqEngine.handleSysExForTest (setEqFlat, sizeof (setEqFlat));

    eqEngine.renderBlockWithInjectedPartForTest (eqOut, 0, lowSine);
    const float flatLowRms = eqOut.getRMSLevel (0, 0, 512);

    eqEngine.renderBlockWithInjectedPartForTest (eqOut, 0, midSine);
    const float flatMidRms = eqOut.getRMSLevel (0, 0, 512);

    eqEngine.renderBlockWithInjectedPartForTest (eqOut, 0, highSine);
    const float flatHighRms = eqOut.getRMSLevel (0, 0, 512);

    // 2. Jazz preset (Addr 02 40 00 = 01): Band 1 = -6dB (cut), Band 3 = +4dB (boost), Band 5 = -6dB (cut)
    const uint8_t setEqJazz[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x40, 0x00, 0x01, 0xF7 };
    eqEngine.handleSysExForTest (setEqJazz, sizeof (setEqJazz));

    eqEngine.renderBlockWithInjectedPartForTest (eqOut, 0, lowSine);
    const float jazzLowRms = eqOut.getRMSLevel (0, 0, 512);
    logTestResult ("Multi EQ DSP Jazz preset attenuates Low band (50Hz) relative to Flat",
                   jazzLowRms < flatLowRms * 0.85f);

    eqEngine.renderBlockWithInjectedPartForTest (eqOut, 0, midSine);
    const float jazzMidRms = eqOut.getRMSLevel (0, 0, 512);
    logTestResult ("Multi EQ DSP Jazz preset boosts Mid band (1kHz) relative to Flat",
                   jazzMidRms > flatMidRms * 1.15f);

    eqEngine.renderBlockWithInjectedPartForTest (eqOut, 0, highSine);
    const float jazzHighRms = eqOut.getRMSLevel (0, 0, 512);
    logTestResult ("Multi EQ DSP Jazz preset attenuates High band (8kHz) relative to Flat",
                   jazzHighRms < flatHighRms * 0.85f);

    // 3. Rock preset (Addr 02 40 00 = 03): Band 1 = +7dB (boost), Band 3 = -4dB (cut)
    const uint8_t setEqRock[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x40, 0x00, 0x03, 0xF7 };
    eqEngine.handleSysExForTest (setEqRock, sizeof (setEqRock));

    eqEngine.renderBlockWithInjectedPartForTest (eqOut, 0, lowSine);
    const float rockLowRms = eqOut.getRMSLevel (0, 0, 512);
    logTestResult ("Multi EQ DSP Rock preset boosts Low band (80Hz) relative to Flat",
                   rockLowRms > flatLowRms * 1.25f);

    eqEngine.renderBlockWithInjectedPartForTest (eqOut, 0, midSine);
    const float rockMidRms = eqOut.getRMSLevel (0, 0, 512);
    logTestResult ("Multi EQ DSP Rock preset attenuates Mid band (1kHz) relative to Flat",
                   rockMidRms < flatMidRms * 0.85f);
}

// =============================================================================
// TEST-E10: Multi Part Reset Values and Part 10 Drums1
// =============================================================================
void testE10_MultiPartResetValues()
{
    std::cout << "\n=== TEST-E10: Multi Part Reset Values ===" << std::endl;

    FluidSynthEngine engine;
    const uint8_t xgSystemOn[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    engine.handleSysExForTest (xgSystemOn, sizeof (xgSystemOn));

    // Part 10 (ch 9) check
    logTestResult ("Part 10 Part Mode is Drums1 (0x02)",
                   engine.getChannelPartModeForTest (9) == static_cast<uint8_t> (xg::PartMode::Drums1));
    logTestResult ("Part 10 Element Reserve is 0",
                   engine.getPartParametersForTest (9).elementReserve == 0);

    // Other Parts check
    bool otherModesNormal = true;
    bool otherReservesTwo = true;
    bool rcvChannelsMatch = true;

    for (int ch = 0; ch < 16; ++ch)
    {
        if (ch != 9)
        {
            if (engine.getChannelPartModeForTest (ch) != static_cast<uint8_t> (xg::PartMode::Normal))
                otherModesNormal = false;
            if (engine.getPartParametersForTest (ch).elementReserve != 2)
                otherReservesTwo = false;
        }
        if (engine.getPartParametersForTest (ch).rcvChannel != static_cast<uint8_t> (ch))
            rcvChannelsMatch = false;
    }

    logTestResult ("Parts 1..9, 11..16 Part Mode is Normal (0x00)", otherModesNormal);
    logTestResult ("Parts 1..9, 11..16 Element Reserve is 2", otherReservesTwo);
    logTestResult ("All Parts 1..16 Rcv Channel matches channel index (0..15)", rcvChannelsMatch);

    // The shared reset path must retain the mode-specific GM and GS defaults.
    FluidSynthEngine gmEngine;
    gmEngine.setEngineMode (FluidSynthEngine::EngineMode::GM);
    const uint8_t gmSystemOn[] = { 0xF0, 0x7E, 0x7F, 0x09, 0x01, 0xF7 };
    gmEngine.handleSysExForTest (gmSystemOn, sizeof (gmSystemOn));
    logTestResult ("GM reset retains GM mode and GM channel-volume default",
                   gmEngine.getActiveMode() == FluidSynthEngine::ActiveMode::GM
                   && gmEngine.getChannelState (0).volume == 127);

    FluidSynthEngine gsEngine;
    gsEngine.setEngineMode (FluidSynthEngine::EngineMode::GS);
    const uint8_t gsReset[] = { 0xF0, 0x41, 0x10, 0x42, 0x12, 0x40, 0x00, 0x7F, 0x00, 0x41, 0xF7 };
    gsEngine.handleSysExForTest (gsReset, sizeof (gsReset));
    logTestResult ("GS reset retains GS mode and GS channel-volume default",
                   gsEngine.getActiveMode() == FluidSynthEngine::ActiveMode::GS
                   && gsEngine.getChannelState (0).volume == 100);
    logTestResult ("GS reset retains the GS default drum part on Part 10",
                   gsEngine.getChannelState (9).gsPartMode == gs::PartMode::Drum1);
}

// =============================================================================
// TEST-E08: Part Mode Drums3/4 Ignored and Setup Mapping Cleaned Up
// =============================================================================
void testE08_PartModeDrums34Ignored()
{
    std::cout << "\n=== TEST-E08: Part Mode Drums3/4 Handling ===" << std::endl;

    FluidSynthEngine engine;
    const uint8_t xgSystemOn[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    engine.handleSysExForTest (xgSystemOn, sizeof (xgSystemOn));

    // Initially Part 1 (ch 0) is Normal
    logTestResult ("Part 1 initially Normal",
                   engine.getChannelPartModeForTest (0) == static_cast<uint8_t> (xg::PartMode::Normal));

    // Send Part Mode 04 (Drums3) SysEx: F0 43 10 4C 08 00 07 04 F7
    const uint8_t setDrums3[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x07, 0x04, 0xF7 };
    engine.handleSysExForTest (setDrums3, sizeof (setDrums3));

    logTestResult ("Part Mode 04 (Drums3) is ignored (mode unchanged)",
                   engine.getChannelPartModeForTest (0) == static_cast<uint8_t> (xg::PartMode::Normal));

    // Send Part Mode 05 (Drums4) SysEx: F0 43 10 4C 08 00 07 05 F7
    const uint8_t setDrums4[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x07, 0x05, 0xF7 };
    engine.handleSysExForTest (setDrums4, sizeof (setDrums4));

    logTestResult ("Part Mode 05 (Drums4) is ignored (mode unchanged)",
                   engine.getChannelPartModeForTest (0) == static_cast<uint8_t> (xg::PartMode::Normal));

    // Send supported Part Mode 02 (Drums1): F0 43 10 4C 08 00 07 02 F7
    const uint8_t setDrums1[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x07, 0x02, 0xF7 };
    engine.handleSysExForTest (setDrums1, sizeof (setDrums1));

    logTestResult ("Supported Part Mode 02 (Drums1) successfully sets Drums1",
                   engine.getChannelPartModeForTest (0) == static_cast<uint8_t> (xg::PartMode::Drums1));
}

// =============================================================================
// Main Test Runner
// =============================================================================

// =============================================================================
// TEST-E02: Delay Family Parameter Specialization (Delay LCR, LR, Echo, Cross)
// =============================================================================
void testE02_DelayFamilySpecialization()
{
    std::cout << "\n=== TEST-E02: Delay Family Parameter Specialization ===" << std::endl;

    VariationEffectProcessor proc;
    proc.prepare (44100.0, 2048);

    xg::VariationParameters p;

    // 1. Delay L,C,R (MSB 0x05)
    p.reset();
    p.typeMsb = xg::varTypeDelayLCR;
    p.parameters14Bit[0] = 3000; // Lch Delay = 300.0ms
    p.parameters14Bit[1] = 2000; // Rch Delay = 200.0ms
    p.parameters14Bit[2] = 4000; // Cch Delay = 400.0ms
    p.parameters14Bit[3] = 4500; // Feedback Delay = 450.0ms
    p.parameters14Bit[4] = 80;   // Feedback Level
    p.parameters14Bit[5] = 100;  // Cch Level (Param 6)
    p.parameters14Bit[6] = 8;    // High Damp (Param 7)
    proc.updateParameters (p);

    logTestResult ("Delay LCR selects DelayType::LCR", proc.getDelayTypeForTest() == VariationEffectProcessor::DelayType::LCR);
    logTestResult ("Delay LCR Param 6 (Cch Level) decodes correctly", isNear (proc.getDelayCchLevelForTest(), 100.0f / 127.0f, 0.01f));
    logTestResult ("Delay LCR Param 4 (Feedback Delay) decodes to 450ms",
                   isNear (proc.getFeedbackDelayLSamplesForTest(), 450.0f * 0.001f * 44100.0f, 1.0f));

    // 2. Delay L,R (MSB 0x06)
    p.reset();
    p.typeMsb = xg::varTypeDelayLR;
    proc.updateParameters (p);
    logTestResult ("Delay LR selects DelayType::LR", proc.getDelayTypeForTest() == VariationEffectProcessor::DelayType::LR);

    // 3. Echo (MSB 0x07)
    p.reset();
    p.typeMsb = xg::varTypeEcho;
    p.parameters14Bit[0] = 1500; // Lch Delay 1 = 150ms
    p.parameters14Bit[1] = 80;   // Lch Feedback Level (Param 2)
    p.parameters14Bit[2] = 1800; // Rch Delay 1 = 180ms
    p.parameters14Bit[3] = 96;   // Rch Feedback Level (Param 4)
    proc.updateParameters (p);

    logTestResult ("Echo selects DelayType::Echo", proc.getDelayTypeForTest() == VariationEffectProcessor::DelayType::Echo);
    // Param 2 must be interpreted as Lch Feedback Level (NOT Rch Delay)
    logTestResult ("Echo Param 2 is Lch Feedback Level (not Rch Delay)",
                   isNear (proc.getEchoFbLForTest(), (80.0f - 64.0f) / 64.0f * 0.9f, 0.01f));
    logTestResult ("Echo Param 4 is Rch Feedback Level",
                   isNear (proc.getEchoFbRForTest(), (96.0f - 64.0f) / 64.0f * 0.9f, 0.01f));

    // 4. Cross Delay (MSB 0x08)
    p.reset();
    p.typeMsb = xg::varTypeCrossDelay;
    p.parameters14Bit[3] = 1; // Input Select = 1 (R only)
    proc.updateParameters (p);
    logTestResult ("Cross Delay selects DelayType::Cross", proc.getDelayTypeForTest() == VariationEffectProcessor::DelayType::Cross);
    logTestResult ("Cross Delay Param 4 (Input Select) decodes to R only (1)", proc.getCrossDelayInputSelectForTest() == 1);

    // Test impulse routing with Delay LCR
    p.reset();
    p.typeMsb = xg::varTypeDelayLCR;
    p.parameters14Bit[0] = 100; // Lch = 10.0ms -> ~441 samples
    p.parameters14Bit[1] = 200; // Rch = 20.0ms -> ~882 samples
    p.parameters14Bit[2] = 300; // Cch = 30.0ms -> ~1323 samples
    p.parameters14Bit[5] = 127; // Max Cch level
    p.parameters14Bit[9] = 127; // 100% wet
    proc.updateParameters (p);

    juce::AudioBuffer<float> inBuf (2, 2048);
    juce::AudioBuffer<float> outBuf (2, 2048);
    inBuf.clear();
    outBuf.clear();
    inBuf.setSample (0, 0, 1.0f); // impulse on left channel only

    proc.process (inBuf, outBuf, 2048);

    // Left channel should have echo at ~441 and center echo at ~1323
    const float* leftOut = outBuf.getReadPointer (0);
    const float* rightOut = outBuf.getReadPointer (1);

    logTestResult ("Delay LCR output Left has direct tap echo", std::abs (leftOut[441]) > 0.5f);
    // Right channel should receive center echo at ~1323 even with impulse only on left
    logTestResult ("Delay LCR output Right receives center tap echo", std::abs (rightOut[1323]) > 0.1f);
}

// =============================================================================
// TEST-S01: Physical Mapping for Reverb and Chorus (Table#1, Table#2, Table#4)
// =============================================================================
void testS01_PhysicalMappingReverbChorus()
{
    std::cout << "\n=== TEST-S01: Physical Mapping for Reverb and Chorus ===" << std::endl;

    FluidSynthEngine engine;
    engine.prepare (44100.0, 512);

    // 1. Chorus Table#1 (LFO Frequency: 0..127 -> 0.00Hz .. 39.7Hz)
    // F0 43 10 4C 02 01 22 <rate> F7 (Param 1 = LFO Frequency, Address Low 0x22)
    const uint8_t setRate0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x22, 0, 0xF7 };
    engine.handleSysExForTest (setRate0, sizeof (setRate0));
    logTestResult ("Chorus Table#1 data 0 -> 0.00 Hz", isNear (engine.getChorusProcessorRateForTest(), 0.00f, 0.01f));

    const uint8_t setRate32[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x22, 32, 0xF7 };
    engine.handleSysExForTest (setRate32, sizeof (setRate32));
    logTestResult ("Chorus Table#1 data 32 -> 1.35 Hz", isNear (engine.getChorusProcessorRateForTest(), 1.35f, 0.01f));

    const uint8_t setRate64[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x22, 64, 0xF7 };
    engine.handleSysExForTest (setRate64, sizeof (setRate64));
    logTestResult ("Chorus Table#1 data 64 -> 2.69 Hz", isNear (engine.getChorusProcessorRateForTest(), 2.69f, 0.01f));

    const uint8_t setRate127[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x22, 127, 0xF7 };
    engine.handleSysExForTest (setRate127, sizeof (setRate127));
    logTestResult ("Chorus Table#1 data 127 -> 39.7 Hz", isNear (engine.getChorusProcessorRateForTest(), 39.70f, 0.01f));

    // 2. Chorus Table#2 (Delay Offset: 0..127 -> 0.0ms .. 50.0ms)
    // F0 43 10 4C 02 01 25 <delay> F7 (Param 4 = Delay Offset, Address Low 0x25)
    const uint8_t setDelay0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x25, 0, 0xF7 };
    engine.handleSysExForTest (setDelay0, sizeof (setDelay0));
    logTestResult ("Chorus Table#2 data 0 -> 0.1 ms (clamped min)", isNear (engine.getChorusProcessorDelayForTest(), 0.1f, 0.01f));

    const uint8_t setDelay64[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x25, 64, 0xF7 };
    engine.handleSysExForTest (setDelay64, sizeof (setDelay64));
    logTestResult ("Chorus Table#2 data 64 -> 6.4 ms", isNear (engine.getChorusProcessorDelayForTest(), 6.4f, 0.01f));

    const uint8_t setDelay127[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x25, 127, 0xF7 };
    engine.handleSysExForTest (setDelay127, sizeof (setDelay127));
    logTestResult ("Chorus Table#2 data 127 -> 50.0 ms", isNear (engine.getChorusProcessorDelayForTest(), 50.0f, 0.01f));

    // 3. Reverb Table#4 (Reverb Time: 0..69 -> 0.3s .. 30.0s)
    // F0 43 10 4C 02 01 02 <time> F7 (Param 1 = Reverb Time, Address Low 0x02)
    const uint8_t setRevTime0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x02, 0, 0xF7 };
    engine.handleSysExForTest (setRevTime0, sizeof (setRevTime0));
    const float roomSize0 = engine.getReverbProcessorRoomSizeForTest();

    const uint8_t setRevTime18[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x02, 18, 0xF7 };
    engine.handleSysExForTest (setRevTime18, sizeof (setRevTime18));
    const float roomSize18 = engine.getReverbProcessorRoomSizeForTest();

    const uint8_t setRevTime64[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x02, 64, 0xF7 };
    engine.handleSysExForTest (setRevTime64, sizeof (setRevTime64));
    const float roomSize64 = engine.getReverbProcessorRoomSizeForTest();

    const uint8_t setRevTime69[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x02, 69, 0xF7 };
    engine.handleSysExForTest (setRevTime69, sizeof (setRevTime69));
    const float roomSize69 = engine.getReverbProcessorRoomSizeForTest();

    logTestResult ("Reverb Time data 0 (0.3s) maps to roomSize ~0.10", isNear (roomSize0, 0.10f, 0.02f));
    logTestResult ("Reverb Time data 18 (2.1s Hall1) maps to roomSize ~0.56", isNear (roomSize18, 0.56f, 0.03f));
    logTestResult ("Reverb Time data 64 (17.0s) maps to roomSize ~0.90", isNear (roomSize64, 0.90f, 0.03f));
    logTestResult ("Reverb Time data 69 (30.0s) maps to roomSize ~0.98", isNear (roomSize69, 0.98f, 0.02f));
    logTestResult ("Reverb Time roomSize is strictly monotonic",
                   roomSize0 < roomSize18 && roomSize18 < roomSize64 && roomSize64 < roomSize69);

    // 4. Reverb LPF Cutoff Damping (Table#3: 1kHz..20kHz -> damping 1.0..0.0)
    // F0 43 10 4C 02 01 06 <lpf> F7 (Param 5 = LPF Cutoff, Address Low 0x06)
    const uint8_t setLpf60[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x06, 60, 0xF7 }; // 20kHz (Thru)
    engine.handleSysExForTest (setLpf60, sizeof (setLpf60));
    logTestResult ("Reverb LPF 20kHz (Thru) gives damping 0.0f", isNear (engine.getReverbProcessorDampingForTest(), 0.0f, 0.01f));

    const uint8_t setLpf34[] = { 0xF0, 0x43, 0x10, 0x4C, 0x02, 0x01, 0x06, 34, 0xF7 }; // 1.0kHz
    engine.handleSysExForTest (setLpf34, sizeof (setLpf34));
    logTestResult ("Reverb LPF 1kHz gives damping 1.0f", isNear (engine.getReverbProcessorDampingForTest(), 1.0f, 0.01f));
}

// =============================================================================
// TEST-U02: ESSENTIAL Variation Effect Types (Reverbs, Rotary, 2/3-Band EQ)
// =============================================================================
void testU02_EssentialVariationTypes()
{
    std::cout << "\n=== TEST-U02: ESSENTIAL Variation Effect Types ===" << std::endl;

    VariationEffectProcessor proc;
    proc.prepare (44100.0, 2048);

    xg::VariationParameters p;

    // 1. Variation Reverb (Hall 1: MSB 0x01)
    p.reset();
    p.typeMsb = xg::varTypeHall1;
    p.parameters14Bit[0] = 18;  // Reverb Time = 2.1s (Table#4)
    p.parameters14Bit[4] = 60;  // LPF = Thru
    p.parameters14Bit[9] = 127; // 100% wet
    proc.updateParameters (p);

    juce::AudioBuffer<float> inBuf (2, 2048);
    juce::AudioBuffer<float> outBuf (2, 2048);
    inBuf.clear();
    outBuf.clear();
    inBuf.setSample (0, 0, 1.0f); // single impulse
    proc.process (inBuf, outBuf, 2048);

    // Variation reverb should produce a reverberant wet tail after comb delay (> 1100 samples)
    float tailEnergy = 0.0f;
    for (int i = 1100; i < 2048; ++i)
        tailEnergy += std::abs (outBuf.getSample (0, i)) + std::abs (outBuf.getSample (1, i));

    logTestResult ("Variation Reverb (Hall 1) produces wet reverb tail", tailEnergy > 0.01f);

    // 2. Rotary Speaker (MSB 0x45)
    p.reset();
    p.typeMsb = xg::varTypeRotarySpeaker;
    p.parameters14Bit[0] = 81; // 4.54 Hz (Table#1 default)
    p.parameters14Bit[1] = 64; // Depth
    p.parameters14Bit[9] = 127; // 100% wet
    proc.updateParameters (p);

    logTestResult ("Rotary Speaker speed decodes to 4.54 Hz from Table#1",
                   isNear (proc.getRotarySpeedHzForTest(), 4.54f, 0.01f));

    inBuf.clear();
    outBuf.clear();
    for (int i = 0; i < 2048; ++i)
    {
        const float val = std::sin (2.0f * 3.14159f * 440.0f * i / 44100.0f);
        inBuf.setSample (0, i, val);
        inBuf.setSample (1, i, val);
    }
    proc.process (inBuf, outBuf, 2048);

    // Rotary creates stereo difference between L and R channels due to complementary phase/amp modulation
    float stereoDiff = 0.0f;
    for (int i = 200; i < 2048; ++i)
        stereoDiff += std::abs (outBuf.getSample (0, i) - outBuf.getSample (1, i));

    logTestResult ("Rotary Speaker introduces stereo modulation difference", stereoDiff > 1.0f);

    // 3. 3-Band EQ (MSB 0x4C)
    p.reset();
    p.typeMsb = xg::varType3BandEq;
    p.parameters14Bit[0] = 76; // Low Gain = +12 dB
    p.parameters14Bit[1] = 34; // Mid Freq = 1.0 kHz
    p.parameters14Bit[2] = 64; // Mid Gain = 0 dB
    p.parameters14Bit[3] = 10; // Mid Q = 1.0
    p.parameters14Bit[4] = 64; // High Gain = 0 dB
    p.parameters14Bit[5] = 28; // Low Freq = 500 Hz
    p.parameters14Bit[6] = 46; // High Freq = 4.0 kHz
    p.parameters14Bit[9] = 127; // 100% wet
    proc.updateParameters (p);

    // Feed 100Hz low frequency sine wave
    inBuf.clear();
    outBuf.clear();
    for (int i = 0; i < 512; ++i)
    {
        const float val = std::sin (2.0f * 3.14159f * 100.0f * i / 44100.0f);
        inBuf.setSample (0, i, val);
        inBuf.setSample (1, i, val);
    }
    proc.process (inBuf, outBuf, 512);

    // Output low frequency amplitude should be boosted (greater than 1.0)
    float maxAmp = 0.0f;
    for (int i = 100; i < 512; ++i)
        maxAmp = std::max (maxAmp, std::abs (outBuf.getSample (0, i)));

    logTestResult ("3-Band EQ boosts low frequency by ~+12dB", maxAmp > 2.0f);

    // 4. 2-Band EQ (MSB 0x4D)
    p.reset();
    p.typeMsb = xg::varType2BandEq;
    p.parameters14Bit[0] = 28; // Low Freq = 500 Hz
    p.parameters14Bit[1] = 76; // Low Gain = +12 dB
    p.parameters14Bit[2] = 46; // High Freq = 4.0 kHz
    p.parameters14Bit[3] = 64; // High Gain = 0 dB
    p.parameters14Bit[9] = 127;
    proc.updateParameters (p);
    proc.process (inBuf, outBuf, 512);

    maxAmp = 0.0f;
    for (int i = 100; i < 512; ++i)
        maxAmp = std::max (maxAmp, std::abs (outBuf.getSample (0, i)));

    logTestResult ("2-Band EQ functions correctly and boosts low freq", maxAmp > 2.0f);
}

// =============================================================================
// TEST-S02 / TEST-U03: Variation Subtypes and Parameters 11-16 (Post-EQ)
// =============================================================================
void testS02_U03_SubtypesAndParameters11To16()
{
    std::cout << "\n=== TEST-S02 / TEST-U03: Variation Subtypes and Params 11-16 ===" << std::endl;

    VariationEffectProcessor proc;
    proc.prepare (44100.0, 512);

    xg::VariationParameters p;

    // 1. Post-EQ activation via Param 11-13 (Mid EQ)
    p.reset();
    p.typeMsb = xg::varTypeFlanger;
    p.parameters11To16[0] = 40; // Mid Freq = 2.0 kHz
    p.parameters11To16[1] = 76; // Mid Gain = +12 dB (non-zero)
    p.parameters11To16[2] = 10; // Mid Q = 1.0
    proc.updateParameters (p);

    logTestResult ("Non-zero Mid EQ on Param 11-13 activates Post-EQ", proc.isPostEqActiveForTest() == true);

    // 2. Post-EQ inactive when gains are 64 (0 dB)
    p.parameters11To16[1] = 64; // Mid Gain = 0 dB
    p.parameters14Bit[6] = 64;  // Low Gain = 0 dB
    p.parameters14Bit[8] = 64;  // High Gain = 0 dB
    proc.updateParameters (p);
    logTestResult ("Zero-gain EQ leaves Post-EQ inactive", proc.isPostEqActiveForTest() == false);

    // 3. Post-EQ activation on Delay LCR via Param 13-16 (Low/High EQ)
    p.reset();
    p.typeMsb = xg::varTypeDelayLCR;
    p.parameters11To16[3] = 74; // Low Gain = +10 dB
    proc.updateParameters (p);
    logTestResult ("Delay LCR Param 14 (Low Gain) activates Post-EQ", proc.isPostEqActiveForTest() == true);

    // 4. Subtype LSB differences (Distortion vs Stereo Distortion)
    p.reset();
    p.typeMsb = xg::varTypeDistortion;
    p.typeLsb = 0x00; // Mono Distortion
    proc.updateParameters (p);
    logTestResult ("Distortion LSB 0x00 stored as currentTypeLsb", proc.getCurrentTypeLsb() == 0x00);

    p.typeLsb = 0x02; // Stereo Distortion
    proc.updateParameters (p);
    logTestResult ("Distortion LSB 0x02 stored as currentTypeLsb", proc.getCurrentTypeLsb() == 0x02);
}

// =============================================================================
// TEST-E07: Tone Fallback Strategy and Silent Voice Processing (TASK-301)
// =============================================================================
void testE07_ToneFallbackAndSilentVoice()
{
    std::cout << "\n=== TEST-E07: Tone Fallback Strategy and Silent Voice Processing ===" << std::endl;

    FluidSynthEngine engine;
    engine.prepare (44100.0, 512);

    // Initialize XG System On
    const uint8_t xgOn[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    engine.handleSysExForTest (xgOn, sizeof (xgOn));

    // Load test soundfont
    const juce::File sfFile ("/Volumes/proj/gmsynth/GMSynth/external/fluidsynth/sf2/VintageDreamsWaves-v2.sf2");
    if (sfFile.existsAsFile())
    {
        engine.loadSoundFont (sfFile);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        engine.processBlock (buffer, midi, nullptr, 0);
    }

    // 1. SFX Bank (MSB 0x40 = 64) unmapped voice -> Silent Voice
    // Send Bank MSB 64, LSB 0, PC 127 to Channel 1 (Part 0)
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 0, 64), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 32, 0), 1);
    midi.addEvent (juce::MidiMessage::programChange (1, 127), 2);
    engine.processBlock (buf, midi, nullptr, 0);

    logTestResult ("SFX Bank unmapped voice sets silent voice", engine.isChannelSilentVoiceForTest (0) == true);

    // 2. Note-on during silent voice does not trigger sound/active voice
    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Silent voice does not produce active voices", engine.getPartActiveVoiceCountForTest (0) == 0);

    // 3. Valid Program Change clears silent voice and restores playability
    midi.clear();
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 0, 0), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (1, 32, 0), 1);
    midi.addEvent (juce::MidiMessage::programChange (1, 0), 2);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Valid program clears silent voice", engine.isChannelSilentVoiceForTest (0) == false);

    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Normal voice produces active voice after recovery", engine.getPartActiveVoiceCountForTest (0) > 0);

    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOff (1, 60, (juce::uint8) 0), 0);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Voice terminates after note off", engine.getPartActiveVoiceCountForTest (0) == 0);

    // 4. Non-zero MSB (e.g. MSB 32) unmapped voice -> Silent Voice
    midi.clear();
    midi.addEvent (juce::MidiMessage::controllerEvent (2, 0, 32), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (2, 32, 0), 1);
    midi.addEvent (juce::MidiMessage::programChange (2, 0), 2);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Non-zero MSB unmapped voice sets silent voice", engine.isChannelSilentVoiceForTest (1) == true);

    // 5. Proxy Bank (MSB 0x60 = 96) falls back to Normal Bank 0
    midi.clear();
    midi.addEvent (juce::MidiMessage::controllerEvent (3, 0, 0x60), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (3, 32, 0), 1);
    midi.addEvent (juce::MidiMessage::programChange (3, 0), 2);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Proxy bank falls back to Bank 0 and remains playable", engine.isChannelSilentVoiceForTest (2) == false);

    // 6. Normal Bank (MSB 0x00) unsupported LSB retains last valid melodic LSB
    midi.clear();
    midi.addEvent (juce::MidiMessage::controllerEvent (4, 0, 0), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (4, 32, 0), 1);
    midi.addEvent (juce::MidiMessage::programChange (4, 0), 2);
    engine.processBlock (buf, midi, nullptr, 0);
    assert (engine.getLastValidMelodicLsbForTest (3) == 0);

    // Send unsupported LSB bank (e.g. 99)
    midi.clear();
    midi.addEvent (juce::MidiMessage::controllerEvent (4, 0, 0), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (4, 32, 99), 1);
    midi.addEvent (juce::MidiMessage::programChange (4, 0), 2);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Unsupported LSB bank retains last valid melodic LSB", engine.getLastValidMelodicLsbForTest (3) == 0);
    logTestResult ("Unsupported LSB voice remains playable via fallback", engine.isChannelSilentVoiceForTest (3) == false);

    // 7. Drum Kit (MSB 0x7F) unmapped kit maintains drum playability without overwrite
    midi.clear();
    midi.addEvent (juce::MidiMessage::controllerEvent (10, 0, 127), 0);
    midi.addEvent (juce::MidiMessage::controllerEvent (10, 32, 0), 1);
    midi.addEvent (juce::MidiMessage::programChange (10, 127), 2);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Unsupported drum kit maintains drum playability", engine.isChannelSilentVoiceForTest (9) == false);
}

// =============================================================================
// TEST-E06: Multi Part Routing, Same Note Assign, Element Reserve (TASK-302)
// =============================================================================
void testE06_MultiPartRoutingSameNoteAssignElementReserve()
{
    std::cout << "\n=== TEST-E06: Multi Part Routing, Same Note Assign, Element Reserve ===" << std::endl;

    FluidSynthEngine engine;
    engine.prepare (44100.0, 512);

    // Initialize XG System On
    const uint8_t xgOn[] = { 0xF0, 0x43, 0x10, 0x4C, 0x00, 0x00, 0x7E, 0x00, 0xF7 };
    engine.handleSysExForTest (xgOn, sizeof (xgOn));

    // Load test soundfont
    const juce::File sfFile ("/Volumes/proj/gmsynth/GMSynth/external/fluidsynth/sf2/VintageDreamsWaves-v2.sf2");
    if (sfFile.existsAsFile())
    {
        engine.loadSoundFont (sfFile);
        juce::AudioBuffer<float> buffer (2, 512);
        juce::MidiBuffer midi;
        engine.processBlock (buffer, midi, nullptr, 0);
    }

    // 1. Multi Part Receive Channel Routing (Layering)
    // Set Part 2 (channel index 1) Rcv Channel to 0 (MIDI ch 1)
    // SysEx: F0 43 10 4C 08 01 04 00 F7
    const uint8_t setPart2Rcv0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x01, 0x04, 0x00, 0xF7 };
    engine.handleSysExForTest (setPart2Rcv0, sizeof (setPart2Rcv0));
    logTestResult ("Part 2 Rcv Channel set to 0", engine.getPartRcvChannelForTest (1) == 0);

    // Send Note On to MIDI ch 1 -> Both Part 1 (ch 0) and Part 2 (ch 1) should sound (layered)
    juce::AudioBuffer<float> buf (2, 512);
    juce::MidiBuffer midi;
    midi.addEvent (juce::MidiMessage::noteOn (1, 60, (juce::uint8) 100), 0);
    engine.processBlock (buf, midi, nullptr, 0);

    logTestResult ("Layered parts both receive Note On",
                   engine.getPartActiveVoiceCountForTest (0) > 0 && engine.getPartActiveVoiceCountForTest (1) > 0);

    // Send Note Off to MIDI ch 1 -> Both should stop
    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOff (1, 60, (juce::uint8) 0), 0);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Layered parts both receive Note Off",
                   engine.getPartActiveVoiceCountForTest (0) == 0 && engine.getPartActiveVoiceCountForTest (1) == 0);

    // Restore Part 2 Rcv Channel back to 1
    const uint8_t restorePart2Rcv1[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x01, 0x04, 0x01, 0xF7 };
    engine.handleSysExForTest (restorePart2Rcv1, sizeof (restorePart2Rcv1));

    // 2. Receive Channel OFF (0x7F)
    // Set Part 3 (channel index 2) Rcv Channel to 0x7F (OFF)
    // SysEx: F0 43 10 4C 08 02 04 7F F7
    const uint8_t setPart3RcvOff[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x02, 0x04, 0x7F, 0xF7 };
    engine.handleSysExForTest (setPart3RcvOff, sizeof (setPart3RcvOff));
    logTestResult ("Part 3 Rcv Channel set to 0x7F (OFF)", engine.getPartRcvChannelForTest (2) == 0x7F);

    // Send Note On to MIDI ch 3 (normal Part 3 channel)
    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOn (3, 60, (juce::uint8) 100), 0);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Part with Rcv Channel OFF ignores Note On", engine.getPartActiveVoiceCountForTest (2) == 0);

    // 3. Same Note Assign: Single (0) vs Multi (1)
    // Set Part 1 (index 0) Same Note Assign to Single (0)
    // SysEx: F0 43 10 4C 08 00 06 00 F7 (Param 0x06 = Same Note Assign)
    const uint8_t setPart1Single[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x06, 0x00, 0xF7 };
    engine.handleSysExForTest (setPart1Single, sizeof (setPart1Single));
    logTestResult ("Part 1 Same Note Assign set to Single", engine.getPartSameNoteAssignForTest (0) == xg::SameNoteAssign::Single);

    // Note On twice on Note 64 (Single mode should cut preceding note)
    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
    engine.processBlock (buf, midi, nullptr, 0);

    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOn (1, 64, (juce::uint8) 100), 0);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Single assign cuts preceding note (voice count = 1)", engine.getPartActiveVoiceCountForTest (0) == 1);

    // Clear notes on ch 1
    midi.clear();
    midi.addEvent (juce::MidiMessage::allNotesOff (1), 0);
    engine.processBlock (buf, midi, nullptr, 0);

    // Set Part 4 (index 3) Same Note Assign to Multi (1)
    // SysEx: F0 43 10 4C 08 03 06 01 F7
    const uint8_t setPart4Multi[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x03, 0x06, 0x01, 0xF7 };
    engine.handleSysExForTest (setPart4Multi, sizeof (setPart4Multi));
    logTestResult ("Part 4 Same Note Assign set to Multi", engine.getPartSameNoteAssignForTest (3) == xg::SameNoteAssign::Multi);

    // Note On twice on Note 64 on ch 4 (Multi mode should layer duplicate note)
    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOn (4, 64, (juce::uint8) 100), 0);
    engine.processBlock (buf, midi, nullptr, 0);

    midi.clear();
    midi.addEvent (juce::MidiMessage::noteOn (4, 64, (juce::uint8) 100), 0);
    engine.processBlock (buf, midi, nullptr, 0);
    logTestResult ("Multi assign layers duplicate note (voice count = 2)", engine.getPartActiveVoiceCountForTest (3) == 2);

    midi.clear();
    midi.addEvent (juce::MidiMessage::allNotesOff (4), 0);
    engine.processBlock (buf, midi, nullptr, 0);

    // 4. Element Reserve parameter check
    // Set Part 1 Element Reserve to 10
    // SysEx: F0 43 10 4C 08 00 00 0A F7 (Param 0x00 = Element Reserve)
    const uint8_t setPart1Reserve10[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x00, 0x00, 0x0A, 0xF7 };
    engine.handleSysExForTest (setPart1Reserve10, sizeof (setPart1Reserve10));
    logTestResult ("Part 1 Element Reserve set to 10", engine.getPartElementReserveForTest (0) == 10);

    // Set Part 2 Element Reserve to 0
    // SysEx: F0 43 10 4C 08 01 00 00 F7
    const uint8_t setPart2Reserve0[] = { 0xF0, 0x43, 0x10, 0x4C, 0x08, 0x01, 0x00, 0x00, 0xF7 };
    engine.handleSysExForTest (setPart2Reserve0, sizeof (setPart2Reserve0));
    logTestResult ("Part 2 Element Reserve set to 0", engine.getPartElementReserveForTest (1) == 0);
}

int main()
{
    std::cout << "=======================================================" << std::endl;
    std::cout << "    GMSynth XG Compliance Regression Test Suite        " << std::endl;
    std::cout << "=======================================================" << std::endl;

    // Phase 1 Tests
    testE01_VariationTypeMsbConstants();
    testE03_EffectDefaultTables();
    testE04_ZeroValueHandling();
    testE05_EffectSendConversion();
    testE09_MultiEqPresets();
    testE10_MultiPartResetValues();
    testE08_PartModeDrums34Ignored();

    // Phase 2 Tests
    testE02_DelayFamilySpecialization();
    testS01_PhysicalMappingReverbChorus();
    testU02_EssentialVariationTypes();
    testS02_U03_SubtypesAndParameters11To16();

    // Phase 3 Tests
    testE07_ToneFallbackAndSilentVoice();
    testE06_MultiPartRoutingSameNoteAssignElementReserve();

    std::cout << "\n=======================================================" << std::endl;
    std::cout << "  TOTAL: " << (gPassedTests + gFailedTests)
              << " | PASSED: " << gPassedTests
              << " | FAILED: " << gFailedTests << std::endl;
    std::cout << "=======================================================" << std::endl;

    return gFailedTests > 0 ? 1 : 0;
}
