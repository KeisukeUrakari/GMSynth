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
}

// =============================================================================
// TEST-E09: Multi EQ Preset Values and Filter Update
// =============================================================================
void testE09_MultiEqPresets()
{
    std::cout << "\n=== TEST-E09: Multi EQ Preset Values and Shape ===" << std::endl;

    xg::MultiEqParameters eq;

    // 1. Flat (0)
    eq.setPreset (0);
    logTestResult ("Flat preset: All gains are 64", eq.isFlat());
    logTestResult ("Flat frequencies: 12, 28, 34, 46, 52",
                   eq.freq1 == 12 && eq.freq2 == 28 && eq.freq3 == 34 && eq.freq4 == 46 && eq.freq5 == 52);

    // 2. Jazz (1): Gain {58, 66, 68, 60, 58}, Freq {8, 16, 33, 44, 50}, Q {7, 3, 3, 5, 7}
    eq.setPreset (1);
    logTestResult ("Jazz preset gains: 58, 66, 68, 60, 58",
                   eq.gain1 == 58 && eq.gain2 == 66 && eq.gain3 == 68 && eq.gain4 == 60 && eq.gain5 == 58);
    logTestResult ("Jazz preset frequencies: 8, 16, 33, 44, 50",
                   eq.freq1 == 8 && eq.freq2 == 16 && eq.freq3 == 33 && eq.freq4 == 44 && eq.freq5 == 50);
    logTestResult ("Jazz preset Q: 7, 3, 3, 5, 7",
                   eq.q1 == 7 && eq.q2 == 3 && eq.q3 == 3 && eq.q4 == 5 && eq.q5 == 7);

    // 3. Pops (2): Gain {68, 60, 67, 60, 70}, Freq {16, 24, 34, 40, 48}, Q {7, 20, 7, 20, 7}
    eq.setPreset (2);
    logTestResult ("Pops preset gains: 68, 60, 67, 60, 70",
                   eq.gain1 == 68 && eq.gain2 == 60 && eq.gain3 == 67 && eq.gain4 == 60 && eq.gain5 == 70);
    logTestResult ("Pops preset Q: 7, 20, 7, 20, 7",
                   eq.q1 == 7 && eq.q2 == 20 && eq.q3 == 7 && eq.q4 == 20 && eq.q5 == 7);

    // 4. Rock (3): Gain {71, 68, 60, 68, 66}, Freq {16, 20, 36, 41, 50}, Q {7, 7, 5, 10, 7}
    eq.setPreset (3);
    logTestResult ("Rock preset gains: 71, 68, 60, 68, 66",
                   eq.gain1 == 71 && eq.gain2 == 68 && eq.gain3 == 60 && eq.gain4 == 68 && eq.gain5 == 66);

    // 5. Concert (4): Gain {67, 68, 64, 66, 61}, Freq {12, 24, 34, 50, 52}, Q {7, 7, 5, 7, 7}
    eq.setPreset (4);
    logTestResult ("Concert preset gains: 67, 68, 64, 66, 61",
                   eq.gain1 == 67 && eq.gain2 == 68 && eq.gain3 == 64 && eq.gain4 == 66 && eq.gain5 == 61);

    // Resetting back to Flat after custom edit restores all parameters
    eq.gain1 = 80;
    eq.freq1 = 40;
    eq.setPreset (0);
    logTestResult ("Resetting to Flat cleans up custom edits", eq.gain1 == 64 && eq.freq1 == 12);
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

    std::cout << "\n=======================================================" << std::endl;
    std::cout << "  TOTAL: " << (gPassedTests + gFailedTests)
              << " | PASSED: " << gPassedTests
              << " | FAILED: " << gFailedTests << std::endl;
    std::cout << "=======================================================" << std::endl;

    return gFailedTests > 0 ? 1 : 0;
}
