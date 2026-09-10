#pragma once

#include <JuceHeader.h>
#include <fluidsynth.h>
#include "XgModel.h"
#include "GsModel.h"
#include "VariationEffect.h"

#include <array>
#include <atomic>
#include <memory>
#include <vector>

class FluidSynthEngine
{
public:
    FluidSynthEngine();
    ~FluidSynthEngine();

    void prepare (double sampleRate, int samplesPerBlock);

    // Loading and clearing are intentionally non-real-time operations. The
    // newly-created synth is handed to the audio thread at the next block
    // boundary.
    juce::Result loadSoundFont (const juce::File& file);
    void clearSoundFont();

    void processBlock (juce::AudioBuffer<float>& buffer,
                       const juce::MidiBuffer& midiMessages,
                       const juce::MidiMessage* keyboardMessages,
                       int numKeyboardMessages) noexcept;

    void setChannelMuted (int channel, bool muted) noexcept;
    bool isChannelMuted (int channel) const noexcept;

    enum class EngineMode : uint8_t
    {
        Auto = 0,
        GM   = 1,
        GS   = 2,
        XG   = 3
    };

    enum class ActiveMode : uint8_t
    {
        GM   = 1,
        GS   = 2,
        XG   = 3
    };

    struct ChannelState
    {
        int volume = 127;
        int pan = 64;
        int bank = 0;
        int bankMsb = 0;
        int bankLsb = 0;
        int program = 0;
        xg::PartMode partMode = xg::PartMode::Normal;
        gs::PartMode gsPartMode = gs::PartMode::Normal;
        bool isDrum = false;
    };

    ChannelState getChannelState (int channel) const noexcept;
    void setChannelVolume (int channel, int value) noexcept;
    void setChannelPan (int channel, int value) noexcept;
    void setChannelBank (int channel, int value) noexcept;
    void setChannelBankMsb (int channel, int value) noexcept;
    void setChannelBankLsb (int channel, int value) noexcept;
    void setChannelPartMode (int channel, xg::PartMode mode) noexcept;
    void setChannelProgram (int channel, int value) noexcept;

    EngineMode getEngineMode() const noexcept;
    void setEngineMode (EngineMode mode) noexcept;
    ActiveMode getActiveMode() const noexcept;

    bool isXgMode() const noexcept;
    void setXgMode (bool enabled) noexcept;
    bool isGsMode() const noexcept;

    void setMasterGain (float gain) noexcept;
    float getMasterGain() const noexcept;

    const xg::SystemParameters& getSystemParameters() const noexcept { return systemParameters; }
    const xg::PartParameters& getPartParameters (int channel) const noexcept;
    const xg::DrumSetup& getDrumSetup (int setupIndex) const noexcept;
    const xg::NrpnState& getNrpnState (int channel) const noexcept;
    const xg::ReverbParameters& getReverbParameters() const noexcept { return reverbParameters; }
    const xg::ChorusParameters& getChorusParameters() const noexcept { return chorusParameters; }
    const xg::VariationParameters& getVariationParameters() const noexcept { return variationParameters; }
    const xg::MultiEqParameters& getMultiEqParameters() const noexcept { return multiEqParameters; }

    const gs::SystemParameters& getGsSystemParameters() const noexcept { return gsSystemParameters; }
    const gs::PartParameters& getGsPartParameters (int channel) const noexcept;
    const gs::DrumSetup& getGsDrumSetup (int setupIndex) const noexcept;
    const gs::ReverbParameters& getGsReverbParameters() const noexcept { return gsReverbParameters; }
    const gs::ChorusParameters& getGsChorusParameters() const noexcept { return gsChorusParameters; }
    const gs::DelayParameters& getGsDelayParameters() const noexcept { return gsDelayParameters; }

private:
    struct PresetLocation
    {
        int sfontId = 0;
        int bank = 0;
        int program = 0;
    };

    struct SynthInstance
    {
        fluid_settings_t* settings = nullptr;
        fluid_synth_t* synth = nullptr;

        std::array<std::vector<PresetLocation>, 128> presetsByProgram;
        std::array<std::vector<PresetLocation>, 128> percussionPresetsByProgram;
        PresetLocation lowestPreset;
        PresetLocation lowestPercussionPreset;
        bool hasLowestPreset = false;
        bool hasLowestPercussionPreset = false;

        ~SynthInstance();
    };

    struct SynthChange
    {
        SynthInstance* synth = nullptr;
    };

    struct RetiredChange
    {
        SynthInstance* synth = nullptr;
        SynthChange* change = nullptr;
    };

    class ReclaimerThread;

    enum
    {
        numMidiChannels = 16,
        maxRetiredChanges = 8
    };

    static std::unique_ptr<SynthInstance> createSynth (const juce::File& file,
                                                        double sampleRate,
                                                        juce::String& errorMessage);
    static void destroyChange (SynthChange* change) noexcept;
    static const PresetLocation* findPresetInBank (const std::vector<PresetLocation>& presets,
                                                   int bank) noexcept;
    static void applyProgramChangeToSynth (SynthInstance& instance,
                                           int channel,
                                           int program,
                                           int requestedBank,
                                           int bankMsb,
                                           int bankLsb,
                                           ActiveMode activeMode,
                                           bool isPercussionChannel) noexcept;

    void requestChange (SynthChange* change);
    void adoptPendingChange() noexcept;
    void initializeSynthChannelState (SynthInstance& instance) noexcept;
    void applyChannelState() noexcept;
    void handleMidiMessage (const juce::MidiMessage& message) noexcept;
    void handleProgramChange (int channel, int program) noexcept;
    void handleSysEx (const juce::uint8* data, int numBytes) noexcept;
    void resetChannelState (int channel) noexcept;
    void handleNrpnDataEntry (int channel, int value, bool isMsb) noexcept;
    void handleNrpnDataIncDec (int channel, int delta) noexcept;
    void setPartFilterCutoff (int channel, int value) noexcept;
    void setPartFilterResonance (int channel, int value) noexcept;
    void setPartEgAttack (int channel, int value) noexcept;
    void setPartEgDecay (int channel, int value) noexcept;
    void setPartEgRelease (int channel, int value) noexcept;
    void setPartVibratoRate (int channel, int value) noexcept;
    void setPartVibratoDepth (int channel, int value) noexcept;
    void setPartVibratoDelay (int channel, int value) noexcept;
    void setPartReverbSend (int channel, int value) noexcept;
    void setPartChorusSend (int channel, int value) noexcept;
    void setPartVariationSend (int channel, int value) noexcept;
    void updateMasterVolume() noexcept;
    void updateChannelTuning (int channel) noexcept;
    void updateAllChannelTunings() noexcept;
    void updateReverbSettings() noexcept;
    void updateChorusSettings() noexcept;
    void updateGsReverbSettings() noexcept;
    void updateGsChorusSettings() noexcept;
    void updateMultiEqCoefficients() noexcept;
    void updatePartEqCoefficients (int channel) noexcept;
    void updateAllPartEqCoefficients() noexcept;
    void resetAllGenerators (int channel) noexcept;
    void applyDrumNoteGenerators (fluid_voice_t* voice, const xg::DrumNoteParameters& noteParams) noexcept;
    void applyGsDrumNoteGenerators (fluid_voice_t* voice, const gs::DrumNoteParameters& noteParams) noexcept;
    void applyMelodicVoiceGenerators (fluid_voice_t* voice, int channel) noexcept;
    void handleGsSysEx (const juce::uint8* data, int numBytes) noexcept;
    void resetGsState() noexcept;
    void renderRange (const juce::MidiBuffer& midiMessages,
                      int rangeStart,
                      int rangeLength,
                      float* left,
                      float* right) noexcept;
    void renderSubRange (int chunkSize, float* destLeft, float* destRight) noexcept;
    void reclaimRetired() noexcept;

    std::atomic<EngineMode> configuredEngineMode { EngineMode::Auto };
    std::atomic<ActiveMode> currentActiveMode { ActiveMode::GM };
    std::atomic<bool> isXgModeActive { false };
    std::atomic<SynthChange*> pendingChange { nullptr };
    std::atomic<double> currentSampleRate { 44100.0 };
    std::array<std::atomic<bool>, numMidiChannels> channelMuted;
    std::array<std::atomic<int>, numMidiChannels> channelVolume;
    std::array<std::atomic<int>, numMidiChannels> channelPan;
    std::array<std::atomic<int>, numMidiChannels> channelBank;
    std::array<std::atomic<int>, numMidiChannels> channelBankMsb;
    std::array<std::atomic<int>, numMidiChannels> channelBankLsb;
    std::array<std::atomic<uint8_t>, numMidiChannels> channelPartMode;
    std::array<std::atomic<uint8_t>, numMidiChannels> channelGsPartMode;
    std::array<std::atomic<bool>, numMidiChannels> drumPartProtectMode;
    std::array<std::atomic<int>, numMidiChannels> channelProgram;
    std::array<std::atomic<uint8_t>, numMidiChannels> channelModulation;
    std::atomic<float> masterGain { 0.8f };

    xg::SystemParameters systemParameters;
    std::array<xg::NrpnState, numMidiChannels> nrpnStates;
    std::array<xg::PartParameters, numMidiChannels> partParameters;
    xg::DrumSetup drumSetup1;
    xg::DrumSetup drumSetup2;

    gs::SystemParameters gsSystemParameters;
    std::array<gs::PartParameters, numMidiChannels> gsPartParameters;
    gs::DrumSetup gsDrumSetup1;
    gs::DrumSetup gsDrumSetup2;
    gs::ReverbParameters gsReverbParameters;
    gs::ChorusParameters gsChorusParameters;
    gs::DelayParameters gsDelayParameters;

    std::array<std::array<uint8_t, 128>, numMidiChannels> activeNoteTransposition;
    std::array<std::array<int, 128>, numMidiChannels> activeGroupNote;

    xg::ReverbParameters reverbParameters;
    xg::ChorusParameters chorusParameters;
    xg::VariationParameters variationParameters;
    xg::MultiEqParameters multiEqParameters;
    std::array<std::array<juce::IIRFilter, 5>, 2> multiEqFilters;
    bool multiEqFiltersNeedUpdate = true;

    juce::AbstractFifo retiredFifo { maxRetiredChanges };
    std::array<RetiredChange, maxRetiredChanges> retiredChanges;
    std::unique_ptr<ReclaimerThread> reclaimerThread;

    SynthInstance* activeSynth = nullptr;
    bool channelStateNeedsApply = true;
    std::array<bool, numMidiChannels> appliedChannelMute;
    std::array<int, numMidiChannels> appliedChannelVolume;
    std::array<int, numMidiChannels> appliedChannelPan;
    std::array<int, numMidiChannels> appliedChannelBank;
    std::array<int, numMidiChannels> appliedChannelBankMsb;
    std::array<int, numMidiChannels> appliedChannelBankLsb;
    std::array<uint8_t, numMidiChannels> appliedChannelPartMode;
    std::array<int, numMidiChannels> appliedChannelProgram;
    float appliedMasterGain = 0.8f;
    struct PartEq
    {
        std::array<juce::IIRFilter, 2> bassFilters;
        std::array<juce::IIRFilter, 2> trebleFilters;
        bool bassActive = false;
        bool trebleActive = false;
        bool needsUpdate = true;

        void reset() noexcept
        {
            for (auto& f : bassFilters) f.reset();
            for (auto& f : trebleFilters) f.reset();
            bassActive = false;
            trebleActive = false;
            needsUpdate = true;
        }
    };

    std::array<PartEq, numMidiChannels> partEqs;

    juce::dsp::Reverb reverbProcessor;
    juce::dsp::Chorus<float> chorusProcessor;
    VariationEffectProcessor variationProcessor;

    juce::AudioBuffer<float> multiPartBuffer;
    std::array<float*, numMidiChannels> partLeftPtrs {};
    std::array<float*, numMidiChannels> partRightPtrs {};

    juce::AudioBuffer<float> reverbBusBuffer;
    juce::AudioBuffer<float> chorusBusBuffer;
    juce::AudioBuffer<float> variationInputBuffer;
    juce::AudioBuffer<float> variationOutputBuffer;

    juce::AudioBuffer<float> scratchBuffer;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FluidSynthEngine)
};
