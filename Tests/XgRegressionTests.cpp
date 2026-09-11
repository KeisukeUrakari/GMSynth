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
int main()
{
    std::cout << "=======================================================" << std::endl;
    std::cout << "    GMSynth XG Compliance Phase 1 Regression Tests     " << std::endl;
    std::cout << "=======================================================" << std::endl;

    testE01_VariationTypeMsbConstants();
    testE03_EffectDefaultTables();
    testE04_ZeroValueHandling();
    testE05_EffectSendConversion();
    testE09_MultiEqPresets();
    testE10_MultiPartResetValues();
    testE08_PartModeDrums34Ignored();

    std::cout << "\n=======================================================" << std::endl;
    std::cout << "  TOTAL: " << (gPassedTests + gFailedTests)
              << " | PASSED: " << gPassedTests
              << " | FAILED: " << gFailedTests << std::endl;
    std::cout << "=======================================================" << std::endl;

    return gFailedTests > 0 ? 1 : 0;
}
