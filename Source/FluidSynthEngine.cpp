#include "FluidSynthEngine.h"

#include <algorithm>

//==============================================================================
class FluidSynthEngine::ReclaimerThread final : public juce::Thread
{
public:
    explicit ReclaimerThread (FluidSynthEngine& ownerToUse)
        : juce::Thread ("FluidSynthReclaimer"), owner (ownerToUse)
    {
        startThread();
    }

    ~ReclaimerThread() override
    {
        stopThread (2000);
    }

private:
    void run() override
    {
        while (! threadShouldExit())
        {
            owner.reclaimRetired();
            wait (50);
        }

        owner.reclaimRetired();
    }

    FluidSynthEngine& owner;
};

//==============================================================================
FluidSynthEngine::SynthInstance::~SynthInstance()
{
    if (synth != nullptr)
        delete_fluid_synth (synth);

    if (settings != nullptr)
        delete_fluid_settings (settings);
}

FluidSynthEngine::FluidSynthEngine()
    : scratchBuffer (2, 1)
{
    for (auto& muted : channelMuted)
        muted.store (false, std::memory_order_relaxed);

    for (int channel = 0; channel < numMidiChannels; ++channel)
    {
        const auto index = static_cast<size_t> (channel);
        channelVolume[index].store (127, std::memory_order_relaxed);
        channelPan[index].store (64, std::memory_order_relaxed);

        const auto isDrum = (channel == 9);
        const auto msb = isDrum ? xg::bankMsbDrumKit : xg::bankMsbNormal;
        const auto lsb = 0;
        channelBankMsb[index].store (msb, std::memory_order_relaxed);
        channelBankLsb[index].store (lsb, std::memory_order_relaxed);
        channelBank[index].store ((msb << 7) | lsb, std::memory_order_relaxed);
        channelPartMode[index].store (static_cast<uint8_t> (isDrum ? xg::PartMode::Drum : xg::PartMode::Normal), std::memory_order_relaxed);
        drumPartProtectMode[index].store (isDrum, std::memory_order_relaxed);
        channelProgram[index].store (0, std::memory_order_relaxed);
    }

    for (auto& nrpn : nrpnStates)
        nrpn.reset();
    for (auto& part : partParameters)
        part.reset();
    drumSetup1.reset();
    drumSetup2.reset();

    appliedChannelMute.fill (false);
    appliedChannelVolume.fill (-1);
    appliedChannelPan.fill (-1);
    appliedChannelBank.fill (-1);
    appliedChannelBankMsb.fill (-1);
    appliedChannelBankLsb.fill (-1);
    appliedChannelPartMode.fill (255);
    appliedChannelProgram.fill (-1);
    reclaimerThread.reset (new ReclaimerThread (*this));
}

FluidSynthEngine::~FluidSynthEngine()
{
    // The host must have stopped calling processBlock before destroying the
    // processor. Stop the background reclaimer before touching its queue.
    reclaimerThread.reset();
    reclaimRetired();

    destroyChange (pendingChange.exchange (nullptr, std::memory_order_acq_rel));

    delete activeSynth;
    activeSynth = nullptr;
}

void FluidSynthEngine::prepare (double sampleRate, int samplesPerBlock)
{
    currentSampleRate.store (juce::jmax (1.0, sampleRate), std::memory_order_release);
    scratchBuffer.setSize (2, juce::jmax (1, samplesPerBlock), false, true, true);
}

std::unique_ptr<FluidSynthEngine::SynthInstance> FluidSynthEngine::createSynth (const juce::File& file,
                                                                                  double sampleRate,
                                                                                  juce::String& errorMessage)
{
    std::unique_ptr<SynthInstance> instance (new SynthInstance());

    instance->settings = new_fluid_settings();
    if (instance->settings == nullptr)
    {
        errorMessage = "FluidSynth could not create its settings.";
        return {};
    }

    if (fluid_settings_setnum (instance->settings, "synth.sample-rate", sampleRate) != FLUID_OK)
    {
        errorMessage = "FluidSynth rejected the requested sample rate.";
        return {};
    }

    instance->synth = new_fluid_synth (instance->settings);
    if (instance->synth == nullptr)
    {
        errorMessage = "FluidSynth could not create a synthesizer.";
        return {};
    }

    const auto soundFontId = fluid_synth_sfload (instance->synth,
                                                 file.getFullPathName().toRawUTF8(),
                                                 1);
    if (soundFontId < 0)
    {
        errorMessage = "FluidSynth could not load \"" + file.getFileName() + "\".";
        return {};
    }

    // General MIDI reserves channel 10 (zero-based channel 9) for drums.
    // Reassert the type after loading because the SoundFont load/reset path
    // is allowed to initialise channel presets and controller state.
    fluid_synth_set_channel_type (instance->synth, 9, CHANNEL_TYPE_DRUM);

    // Build the immutable bank/program index while the SoundFont is still on
    // the non-real-time loading path. Program changes can then resolve a
    // missing bank without searching or allocating on the audio thread.
    const auto soundFontCount = fluid_synth_sfcount (instance->synth);
    for (int soundFontIndex = 0; soundFontIndex < soundFontCount; ++soundFontIndex)
    {
        auto* soundFont = fluid_synth_get_sfont (instance->synth,
                                                 static_cast<unsigned int> (soundFontIndex));
        if (soundFont == nullptr)
            continue;

        const auto soundFontId = fluid_sfont_get_id (soundFont);
        const auto bankOffset = fluid_synth_get_bank_offset (instance->synth, soundFontId);

        fluid_sfont_iteration_start (soundFont);
        while (auto* preset = fluid_sfont_iteration_next (soundFont))
        {
            const auto bank = fluid_preset_get_banknum (preset) + bankOffset;
            const auto program = fluid_preset_get_num (preset);

            if (bank < 0 || bank > 16383 || program < 0 || program >= 128)
                continue;

            const PresetLocation location { soundFontId, bank, program };
            instance->presetsByProgram[static_cast<size_t> (program)].push_back (location);

            if (! instance->hasLowestPreset || bank < instance->lowestPreset.bank)
            {
                instance->lowestPreset = location;
                instance->hasLowestPreset = true;
            }

            // Bank 128 and above are the conventional SF2 percussion banks.
            // Prefer those for a drum channel, but keep the global fallback
            // available for SoundFonts that do not follow that convention.
            if (bank >= 128)
            {
                instance->percussionPresetsByProgram[static_cast<size_t> (program)].push_back (location);

                if (! instance->hasLowestPercussionPreset || bank < instance->lowestPercussionPreset.bank)
                {
                    instance->lowestPercussionPreset = location;
                    instance->hasLowestPercussionPreset = true;
                }
            }
        }
    }

    for (auto& presets : instance->presetsByProgram)
    {
        std::stable_sort (presets.begin(), presets.end(), [] (const auto& first, const auto& second)
        {
            return first.bank < second.bank;
        });
    }

    for (auto& presets : instance->percussionPresetsByProgram)
    {
        std::stable_sort (presets.begin(), presets.end(), [] (const auto& first, const auto& second)
        {
            return first.bank < second.bank;
        });
    }

    // Keep a little headroom for the plug-in's default output level.
    fluid_synth_set_gain (instance->synth, 0.8f);
    return instance;
}

juce::Result FluidSynthEngine::loadSoundFont (const juce::File& file)
{
    if (! file.existsAsFile())
        return juce::Result::fail ("The selected SoundFont file does not exist.");

    juce::String errorMessage;
    auto instance = createSynth (file, currentSampleRate.load (std::memory_order_acquire), errorMessage);
    if (instance == nullptr)
        return juce::Result::fail (errorMessage);

    // Configure the replacement before publishing it to the audio thread. The
    // audio-thread apply below remains the authoritative final pass, but this
    // removes the default-preset window that otherwise exists after sfload().
    initializeSynthChannelState (*instance);

    auto* change = new SynthChange();
    change->synth = instance.release();
    requestChange (change);
    return juce::Result::ok();
}

void FluidSynthEngine::clearSoundFont()
{
    requestChange (new SynthChange());
}

const FluidSynthEngine::PresetLocation* FluidSynthEngine::findPresetInBank (const std::vector<PresetLocation>& presets,
                                                                              int bank) noexcept
{
    const auto it = std::lower_bound (presets.begin(), presets.end(), bank,
                                      [] (const auto& preset, int bankToFind)
                                      {
                                          return preset.bank < bankToFind;
                                      });

    return it != presets.end() && it->bank == bank ? &*it : nullptr;
}

void FluidSynthEngine::applyProgramChangeToSynth (SynthInstance& instance,
                                                   int channel,
                                                   int program,
                                                   int requestedBank,
                                                   bool isPercussionChannel) noexcept
{
    if (instance.synth == nullptr
        || ! juce::isPositiveAndBelow (channel, numMidiChannels)
        || ! juce::isPositiveAndBelow (program, 128))
        return;

    const auto& allPresetsForProgram = instance.presetsByProgram[static_cast<size_t> (program)];
    const auto& percussionPresetsForProgram = instance.percussionPresetsByProgram[static_cast<size_t> (program)];

    fluid_synth_set_channel_type (instance.synth, channel,
                                  isPercussionChannel ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);

    const PresetLocation* chosen = nullptr;

    if (isPercussionChannel)
    {
        // 1. Try exact requested bank in percussion presets (e.g. XG drum bank 16256)
        chosen = findPresetInBank (percussionPresetsForProgram, requestedBank);

        // 2. If not found, try standard SF2 percussion bank 128
        if (chosen == nullptr && requestedBank != 128)
            chosen = findPresetInBank (percussionPresetsForProgram, 128);

        // 3. If not found, fallback to Standard Kit (Program 0) at bank 128
        if (chosen == nullptr)
        {
            const auto& stdKitPresets = instance.percussionPresetsByProgram[0];
            chosen = findPresetInBank (stdKitPresets, 128);
            if (chosen == nullptr && ! stdKitPresets.empty())
                chosen = &stdKitPresets.front();
        }

        // 4. If still not found, try any preset for this program in percussion banks
        if (chosen == nullptr && ! percussionPresetsForProgram.empty())
            chosen = &percussionPresetsForProgram.front();

        // 5. Global lowest percussion preset
        if (chosen == nullptr && instance.hasLowestPercussionPreset)
            chosen = &instance.lowestPercussionPreset;

        // 6. Any preset available
        if (chosen == nullptr && instance.hasLowestPreset)
            chosen = &instance.lowestPreset;
    }
    else
    {
        // 1. Try exact requested bank for melodic program
        chosen = findPresetInBank (allPresetsForProgram, requestedBank);

        // 2. XG fallback rule: Try basic GM voice set (Bank 0) for this program
        if (chosen == nullptr && requestedBank != 0)
            chosen = findPresetInBank (allPresetsForProgram, 0);

        // 3. Try any available bank for this program
        if (chosen == nullptr && ! allPresetsForProgram.empty())
            chosen = &allPresetsForProgram.front();

        // 4. Global lowest preset
        if (chosen == nullptr && instance.hasLowestPreset)
            chosen = &instance.lowestPreset;
    }

    if (chosen != nullptr)
    {
        if (fluid_synth_program_select (instance.synth,
                                        channel,
                                        chosen->sfontId,
                                        chosen->bank,
                                        chosen->program) == FLUID_OK)
        {
            if (requestedBank != chosen->bank)
                fluid_synth_bank_select (instance.synth, channel, requestedBank);
            return;
        }

        fluid_synth_bank_select (instance.synth, channel, chosen->bank);
        if (fluid_synth_program_change (instance.synth, channel, chosen->program) == FLUID_OK)
        {
            if (requestedBank != chosen->bank)
                fluid_synth_bank_select (instance.synth, channel, requestedBank);
            return;
        }
    }

    fluid_synth_program_change (instance.synth, channel, program);
}

void FluidSynthEngine::initializeSynthChannelState (SynthInstance& instance) noexcept
{
    if (instance.synth == nullptr)
        return;

    for (int channel = 0; channel < numMidiChannels; ++channel)
    {
        const auto index = static_cast<size_t> (channel);
        const auto muted = channelMuted[index].load (std::memory_order_acquire);
        const auto volume = juce::jlimit (0, 127, channelVolume[index].load (std::memory_order_acquire));
        const auto pan = juce::jlimit (0, 127, channelPan[index].load (std::memory_order_acquire));
        const auto bank = juce::jlimit (0, 16383, channelBank[index].load (std::memory_order_acquire));
        const auto bankMsb = juce::jlimit (0, 127, channelBankMsb[index].load (std::memory_order_acquire));
        const auto partMode = static_cast<xg::PartMode> (channelPartMode[index].load (std::memory_order_acquire));
        const auto isPercussion = xg::isDrumMode (partMode) || bankMsb == xg::bankMsbDrumKit || bankMsb == xg::bankMsbSfxKit;
        const auto program = juce::jlimit (0, 127, channelProgram[index].load (std::memory_order_acquire));

        fluid_synth_set_channel_type (instance.synth, channel, isPercussion ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
        fluid_synth_cc (instance.synth, channel, 7, muted ? 0 : volume);
        fluid_synth_cc (instance.synth, channel, 10, pan);
        fluid_synth_bank_select (instance.synth, channel, bank);

        applyProgramChangeToSynth (instance, channel, program, bank, isPercussion);

        const auto& params = partParameters[index];
        fluid_synth_set_gen (instance.synth, channel, GEN_FILTERFC, xg::cutoffOffsetToCents (params.filterCutoff));
        fluid_synth_set_gen (instance.synth, channel, GEN_FILTERQ, xg::resonanceOffsetToCentibels (params.filterResonance));
        const auto attackOfs = xg::attackOffsetToTimecents (params.egAttack);
        fluid_synth_set_gen (instance.synth, channel, GEN_VOLENVATTACK, attackOfs);
        fluid_synth_set_gen (instance.synth, channel, GEN_MODENVATTACK, attackOfs);
        const auto decayOfs = xg::decayOffsetToTimecents (params.egDecay);
        fluid_synth_set_gen (instance.synth, channel, GEN_VOLENVDECAY, decayOfs);
        fluid_synth_set_gen (instance.synth, channel, GEN_MODENVDECAY, decayOfs);
        const auto releaseOfs = xg::releaseOffsetToTimecents (params.egRelease);
        fluid_synth_set_gen (instance.synth, channel, GEN_VOLENVRELEASE, releaseOfs);
        fluid_synth_set_gen (instance.synth, channel, GEN_MODENVRELEASE, releaseOfs);
        fluid_synth_set_gen (instance.synth, channel, GEN_VIBLFOFREQ, xg::vibratoRateOffsetToCents (params.vibratoRate));
        fluid_synth_set_gen (instance.synth, channel, GEN_VIBLFOTOPITCH, xg::vibratoDepthOffsetToCents (params.vibratoDepth));
        fluid_synth_set_gen (instance.synth, channel, GEN_VIBLFODELAY, xg::vibratoDelayOffsetToTimecents (params.vibratoDelay));
    }
}

void FluidSynthEngine::requestChange (SynthChange* change)
{
    destroyChange (pendingChange.exchange (change, std::memory_order_acq_rel));
}

void FluidSynthEngine::adoptPendingChange() noexcept
{
    if (pendingChange.load (std::memory_order_acquire) == nullptr
        || retiredFifo.getFreeSpace() == 0)
        return;

    auto* change = pendingChange.exchange (nullptr, std::memory_order_acq_rel);
    if (change == nullptr)
        return;

    auto* oldSynth = activeSynth;
    activeSynth = change->synth;
    channelStateNeedsApply = true;

    auto write = retiredFifo.write (1);
    if (write.blockSize1 > 0)
    {
        retiredChanges[write.startIndex1] = { oldSynth, change };
    }
    else if (write.blockSize2 > 0)
    {
        retiredChanges[write.startIndex2] = { oldSynth, change };
    }
}

void FluidSynthEngine::setChannelMuted (int channel, bool muted) noexcept
{
    if (juce::isPositiveAndBelow (channel, numMidiChannels))
        channelMuted[static_cast<size_t> (channel)].store (muted, std::memory_order_release);
}

bool FluidSynthEngine::isChannelMuted (int channel) const noexcept
{
    return juce::isPositiveAndBelow (channel, numMidiChannels)
        && channelMuted[static_cast<size_t> (channel)].load (std::memory_order_acquire);
}

FluidSynthEngine::ChannelState FluidSynthEngine::getChannelState (int channel) const noexcept
{
    ChannelState state;
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return state;

    const auto index = static_cast<size_t> (channel);
    const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
    const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
    const auto lsb = channelBankLsb[index].load (std::memory_order_acquire);
    const auto partMode = static_cast<xg::PartMode> (partModeRaw);

    state.volume = channelVolume[index].load (std::memory_order_acquire);
    state.pan = channelPan[index].load (std::memory_order_acquire);
    state.bank = channelBank[index].load (std::memory_order_acquire);
    state.bankMsb = msb;
    state.bankLsb = lsb;
    state.program = channelProgram[index].load (std::memory_order_acquire);
    state.partMode = partMode;
    state.isDrum = xg::isDrumMode (partMode) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit;
    return state;
}

void FluidSynthEngine::setChannelVolume (int channel, int value) noexcept
{
    if (juce::isPositiveAndBelow (channel, numMidiChannels))
        channelVolume[static_cast<size_t> (channel)].store (juce::jlimit (0, 127, value),
                                                             std::memory_order_release);
}

void FluidSynthEngine::setChannelPan (int channel, int value) noexcept
{
    if (juce::isPositiveAndBelow (channel, numMidiChannels))
        channelPan[static_cast<size_t> (channel)].store (juce::jlimit (0, 127, value),
                                                           std::memory_order_release);
}

void FluidSynthEngine::setChannelBank (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto index = static_cast<size_t> (channel);
    const auto clamped = juce::jlimit (0, 16383, value);
    const auto msb = (clamped >> 7) & 0x7f;
    const auto lsb = clamped & 0x7f;
    channelBankMsb[index].store (msb, std::memory_order_release);
    channelBankLsb[index].store (lsb, std::memory_order_release);
    channelBank[index].store (clamped, std::memory_order_release);

    if (msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit)
        channelPartMode[index].store (static_cast<uint8_t> (xg::PartMode::Drum), std::memory_order_release);
    else if (msb == xg::bankMsbNormal)
        channelPartMode[index].store (static_cast<uint8_t> (xg::PartMode::Normal), std::memory_order_release);

    channelStateNeedsApply = true;
}

void FluidSynthEngine::setChannelBankMsb (int channel, int msb) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto index = static_cast<size_t> (channel);
    const auto clampedMsb = juce::jlimit (0, 127, msb);
    channelBankMsb[index].store (clampedMsb, std::memory_order_release);
    const auto lsb = channelBankLsb[index].load (std::memory_order_acquire);
    channelBank[index].store ((clampedMsb << 7) | lsb, std::memory_order_release);

    if (clampedMsb == xg::bankMsbDrumKit || clampedMsb == xg::bankMsbSfxKit)
        channelPartMode[index].store (static_cast<uint8_t> (xg::PartMode::Drum), std::memory_order_release);
    else if (clampedMsb == xg::bankMsbNormal)
        channelPartMode[index].store (static_cast<uint8_t> (xg::PartMode::Normal), std::memory_order_release);

    channelStateNeedsApply = true;
}

void FluidSynthEngine::setChannelBankLsb (int channel, int lsb) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto index = static_cast<size_t> (channel);
    const auto clampedLsb = juce::jlimit (0, 127, lsb);
    channelBankLsb[index].store (clampedLsb, std::memory_order_release);
    const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
    channelBank[index].store ((msb << 7) | clampedLsb, std::memory_order_release);
    channelStateNeedsApply = true;
}

void FluidSynthEngine::setChannelPartMode (int channel, xg::PartMode mode) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto index = static_cast<size_t> (channel);
    channelPartMode[index].store (static_cast<uint8_t> (mode), std::memory_order_release);
    channelStateNeedsApply = true;
}

void FluidSynthEngine::setChannelProgram (int channel, int value) noexcept
{
    if (juce::isPositiveAndBelow (channel, numMidiChannels))
        channelProgram[static_cast<size_t> (channel)].store (juce::jlimit (0, 127, value),
                                                              std::memory_order_release);
}

bool FluidSynthEngine::isXgMode() const noexcept
{
    return isXgModeActive.load (std::memory_order_acquire);
}

void FluidSynthEngine::setXgMode (bool enabled) noexcept
{
    isXgModeActive.store (enabled, std::memory_order_release);
}

void FluidSynthEngine::setMasterGain (float gain) noexcept
{
    masterGain.store (juce::jmax (0.0f, gain), std::memory_order_release);
}

float FluidSynthEngine::getMasterGain() const noexcept
{
    return masterGain.load (std::memory_order_acquire);
}

void FluidSynthEngine::resetChannelState (int channel) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto index = static_cast<size_t> (channel);
    const auto xgActive = isXgModeActive.load (std::memory_order_acquire);
    channelVolume[index].store (xgActive ? xg::defaultVolume : 127, std::memory_order_release);
    channelPan[index].store (xg::defaultPan, std::memory_order_release);

    const auto isDrum = (channel == 9);
    const auto msb = isDrum ? xg::bankMsbDrumKit : xg::bankMsbNormal;
    const auto lsb = 0;
    channelBankMsb[index].store (msb, std::memory_order_release);
    channelBankLsb[index].store (lsb, std::memory_order_release);
    channelBank[index].store ((msb << 7) | lsb, std::memory_order_release);
    channelPartMode[index].store (static_cast<uint8_t> (isDrum ? xg::PartMode::Drum : xg::PartMode::Normal), std::memory_order_release);
    drumPartProtectMode[index].store (isDrum, std::memory_order_release);
    channelProgram[index].store (0, std::memory_order_release);

    partParameters[index].reset();
    nrpnStates[index].reset();
    resetAllGenerators (channel);
}

void FluidSynthEngine::handleSysEx (const juce::uint8* data, int numBytes) noexcept
{
    if (data == nullptr || numBytes <= 0)
        return;

    const auto isGmReset = numBytes >= 4
        && data[0] == 0x7e
        && data[2] == 0x09
        && (data[3] == 0x01 || data[3] == 0x03);

    const auto isGsReset = numBytes >= 9
        && data[0] == 0x41
        && data[2] == 0x42
        && data[3] == 0x12
        && data[4] == 0x40
        && data[5] == 0x00
        && data[6] == 0x7f
        && data[7] == 0x00;

    const auto isXgSystemOn = numBytes >= 7
        && data[0] == 0x43
        && (data[1] & 0xf0) == 0x10
        && data[2] == 0x4c
        && data[3] == 0x00
        && data[4] == 0x00
        && data[5] == 0x7e
        && data[6] == 0x00;

    const auto isXgAllParamReset = numBytes >= 7
        && data[0] == 0x43
        && (data[1] & 0xf0) == 0x10
        && data[2] == 0x4c
        && data[3] == 0x00
        && data[4] == 0x00
        && data[5] == 0x7f
        && data[6] == 0x00;

    const auto isXgMultiPart = numBytes >= 7
        && data[0] == 0x43
        && (data[1] & 0xf0) == 0x10
        && data[2] == 0x4c
        && data[3] == 0x08;

    if (isGmReset)
    {
        isXgModeActive.store (false, std::memory_order_release);
        drumSetup1.reset();
        drumSetup2.reset();
    }
    else if (isXgSystemOn || isXgAllParamReset)
    {
        isXgModeActive.store (true, std::memory_order_release);
        drumSetup1.reset();
        drumSetup2.reset();
    }
    else if (isXgMultiPart)
    {
        const auto part = data[4] & 0x1f; // 0..15
        const auto param = data[5];
        const auto val = data[6];

        if (juce::isPositiveAndBelow (static_cast<int> (part), numMidiChannels))
        {
            const auto index = static_cast<size_t> (part);
            if (param == 0x01) // Bank Select MSB
            {
                channelBankMsb[index].store (val, std::memory_order_release);
                channelBank[index].store ((val << 7) | channelBankLsb[index].load (std::memory_order_acquire), std::memory_order_release);
                if (val == xg::bankMsbDrumKit || val == xg::bankMsbSfxKit)
                    channelPartMode[index].store (static_cast<uint8_t> (xg::PartMode::Drum), std::memory_order_release);
                else if (val == xg::bankMsbNormal)
                    channelPartMode[index].store (static_cast<uint8_t> (xg::PartMode::Normal), std::memory_order_release);
                channelStateNeedsApply = true;
            }
            else if (param == 0x02) // Bank Select LSB
            {
                channelBankLsb[index].store (val, std::memory_order_release);
                channelBank[index].store ((channelBankMsb[index].load (std::memory_order_acquire) << 7) | val, std::memory_order_release);
                channelStateNeedsApply = true;
            }
            else if (param == 0x03) // Program Number
            {
                channelProgram[index].store (val, std::memory_order_release);
                channelStateNeedsApply = true;
            }
            else if (param == 0x07) // Part Mode (0: Normal, 1: Drum, 2..5: Drums 1..4)
            {
                const auto mode = static_cast<xg::PartMode> (val);
                channelPartMode[index].store (static_cast<uint8_t> (mode), std::memory_order_release);
                if (mode == xg::PartMode::Normal)
                {
                    if (channelBankMsb[index].load (std::memory_order_acquire) >= 126)
                    {
                        channelBankMsb[index].store (0, std::memory_order_release);
                        channelBank[index].store (channelBankLsb[index].load (std::memory_order_acquire), std::memory_order_release);
                    }
                    if (part == 9)
                        drumPartProtectMode[9].store (false, std::memory_order_release);
                }
                else
                {
                    if (channelBankMsb[index].load (std::memory_order_acquire) < 126)
                    {
                        channelBankMsb[index].store (xg::bankMsbDrumKit, std::memory_order_release);
                        channelBank[index].store ((xg::bankMsbDrumKit << 7) | channelBankLsb[index].load (std::memory_order_acquire), std::memory_order_release);
                    }
                }
                channelStateNeedsApply = true;
            }
        }
        return;
    }
    else if (! isGsReset)
    {
        return;
    }
    else
    {
        drumSetup1.reset();
        drumSetup2.reset();
    }

    if (activeSynth != nullptr)
    {
        int handled = 0;
        fluid_synth_sysex (activeSynth->synth,
                           reinterpret_cast<const char*> (data),
                           numBytes,
                           nullptr,
                           nullptr,
                           &handled,
                           0);
    }

    for (int channel = 0; channel < numMidiChannels; ++channel)
        resetChannelState (channel);

    if (activeSynth != nullptr)
    {
        for (int channel = 0; channel < numMidiChannels; ++channel)
        {
            const auto partMode = static_cast<xg::PartMode> (channelPartMode[static_cast<size_t> (channel)].load (std::memory_order_acquire));
            const auto isDrum = xg::isDrumMode (partMode);
            fluid_synth_set_channel_type (activeSynth->synth, channel, isDrum ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
        }
    }

    channelStateNeedsApply = true;
}

void FluidSynthEngine::applyChannelState() noexcept
{
    if (activeSynth == nullptr)
        return;

    const auto needsApply = channelStateNeedsApply;
    const auto desiredMasterGain = masterGain.load (std::memory_order_acquire);
    if (needsApply || desiredMasterGain != appliedMasterGain)
    {
        fluid_synth_set_gain (activeSynth->synth, desiredMasterGain);
        appliedMasterGain = desiredMasterGain;
    }

    for (int channel = 0; channel < numMidiChannels; ++channel)
    {
        const auto index = static_cast<size_t> (channel);
        const auto muted = channelMuted[index].load (std::memory_order_acquire);
        const auto volume = juce::jlimit (0, 127, channelVolume[index].load (std::memory_order_acquire));
        const auto pan = juce::jlimit (0, 127, channelPan[index].load (std::memory_order_acquire));
        const auto bank = juce::jlimit (0, 16383, channelBank[index].load (std::memory_order_acquire));
        const auto bankMsb = juce::jlimit (0, 127, channelBankMsb[index].load (std::memory_order_acquire));
        const auto bankLsb = juce::jlimit (0, 127, channelBankLsb[index].load (std::memory_order_acquire));
        const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
        const auto program = juce::jlimit (0, 127, channelProgram[index].load (std::memory_order_acquire));

        const auto partMode = static_cast<xg::PartMode> (partModeRaw);
        const auto isDrum = xg::isDrumMode (partMode) || bankMsb == xg::bankMsbDrumKit || bankMsb == xg::bankMsbSfxKit;

        if (needsApply || partModeRaw != appliedChannelPartMode[index])
        {
            fluid_synth_set_channel_type (activeSynth->synth, channel, isDrum ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
            appliedChannelPartMode[index] = partModeRaw;
        }

        if (needsApply || volume != appliedChannelVolume[index]
            || muted != appliedChannelMute[index])
        {
            fluid_synth_cc (activeSynth->synth, channel, 7, muted ? 0 : volume);
            appliedChannelVolume[index] = volume;
            appliedChannelMute[index] = muted;
        }

        if (needsApply || pan != appliedChannelPan[index])
        {
            fluid_synth_cc (activeSynth->synth, channel, 10, pan);
            appliedChannelPan[index] = pan;
        }

        const auto bankChanged = needsApply || bank != appliedChannelBank[index];
        if (bankChanged)
        {
            fluid_synth_bank_select (activeSynth->synth, channel, bank);
            appliedChannelBank[index] = bank;
            appliedChannelBankMsb[index] = bankMsb;
            appliedChannelBankLsb[index] = bankLsb;
        }

        if (bankChanged || needsApply || program != appliedChannelProgram[index])
        {
            handleProgramChange (channel, program);
            appliedChannelProgram[index] = program;
        }
    }

    channelStateNeedsApply = false;
}

void FluidSynthEngine::handleProgramChange (int channel, int program) noexcept
{
    if (activeSynth == nullptr || ! juce::isPositiveAndBelow (program, 128))
        return;

    const auto index = static_cast<size_t> (channel);
    const auto requestedBank = channelBank[index].load (std::memory_order_acquire);
    const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
    const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
    const auto partMode = static_cast<xg::PartMode> (partModeRaw);
    const auto isPercussion = xg::isDrumMode (partMode) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit;

    fluid_synth_set_channel_type (activeSynth->synth, channel, isPercussion ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
    applyProgramChangeToSynth (*activeSynth, channel, program, requestedBank, isPercussion);
}

const xg::PartParameters& FluidSynthEngine::getPartParameters (int channel) const noexcept
{
    static const xg::PartParameters defaultParams;
    if (juce::isPositiveAndBelow (channel, numMidiChannels))
        return partParameters[static_cast<size_t> (channel)];
    return defaultParams;
}

const xg::DrumSetup& FluidSynthEngine::getDrumSetup (int setupIndex) const noexcept
{
    return (setupIndex == 2) ? drumSetup2 : drumSetup1;
}

const xg::NrpnState& FluidSynthEngine::getNrpnState (int channel) const noexcept
{
    static const xg::NrpnState defaultState;
    if (juce::isPositiveAndBelow (channel, numMidiChannels))
        return nrpnStates[static_cast<size_t> (channel)];
    return defaultState;
}

void FluidSynthEngine::resetAllGenerators (int channel) noexcept
{
    if (activeSynth == nullptr || ! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    fluid_synth_set_gen (activeSynth->synth, channel, GEN_FILTERFC, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_FILTERQ, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VOLENVATTACK, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODENVATTACK, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VOLENVDECAY, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODENVDECAY, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VOLENVRELEASE, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODENVRELEASE, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFOFREQ, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFOTOPITCH, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFODELAY, 0.0f);
}

void FluidSynthEngine::setPartFilterCutoff (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].filterCutoff = clamped;
    if (activeSynth != nullptr)
    {
        fluid_synth_set_gen (activeSynth->synth, channel, GEN_FILTERFC,
                             xg::cutoffOffsetToCents (clamped));
        fluid_synth_cc (activeSynth->synth, channel, 74, clamped);
    }
}

void FluidSynthEngine::setPartFilterResonance (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].filterResonance = clamped;
    if (activeSynth != nullptr)
    {
        fluid_synth_set_gen (activeSynth->synth, channel, GEN_FILTERQ,
                             xg::resonanceOffsetToCentibels (clamped));
        fluid_synth_cc (activeSynth->synth, channel, 71, clamped);
    }
}

void FluidSynthEngine::setPartEgAttack (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].egAttack = clamped;
    if (activeSynth != nullptr)
    {
        const auto offset = xg::attackOffsetToTimecents (clamped);
        fluid_synth_set_gen (activeSynth->synth, channel, GEN_VOLENVATTACK, offset);
        fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODENVATTACK, offset);
        fluid_synth_cc (activeSynth->synth, channel, 73, clamped);
    }
}

void FluidSynthEngine::setPartEgDecay (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].egDecay = clamped;
    if (activeSynth != nullptr)
    {
        const auto offset = xg::decayOffsetToTimecents (clamped);
        fluid_synth_set_gen (activeSynth->synth, channel, GEN_VOLENVDECAY, offset);
        fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODENVDECAY, offset);
        fluid_synth_cc (activeSynth->synth, channel, 75, clamped);
    }
}

void FluidSynthEngine::setPartEgRelease (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].egRelease = clamped;
    if (activeSynth != nullptr)
    {
        const auto offset = xg::releaseOffsetToTimecents (clamped);
        fluid_synth_set_gen (activeSynth->synth, channel, GEN_VOLENVRELEASE, offset);
        fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODENVRELEASE, offset);
        fluid_synth_cc (activeSynth->synth, channel, 72, clamped);
    }
}

void FluidSynthEngine::setPartVibratoRate (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].vibratoRate = clamped;
    if (activeSynth != nullptr)
    {
        fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFOFREQ,
                             xg::vibratoRateOffsetToCents (clamped));
        fluid_synth_cc (activeSynth->synth, channel, 76, clamped);
    }
}

void FluidSynthEngine::setPartVibratoDepth (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].vibratoDepth = clamped;
    if (activeSynth != nullptr)
    {
        fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFOTOPITCH,
                             xg::vibratoDepthOffsetToCents (clamped));
        fluid_synth_cc (activeSynth->synth, channel, 77, clamped);
    }
}

void FluidSynthEngine::setPartVibratoDelay (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].vibratoDelay = clamped;
    if (activeSynth != nullptr)
    {
        fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFODELAY,
                             xg::vibratoDelayOffsetToTimecents (clamped));
        fluid_synth_cc (activeSynth->synth, channel, 78, clamped);
    }
}

void FluidSynthEngine::setPartReverbSend (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].reverbSend = clamped;
    if (activeSynth != nullptr)
        fluid_synth_cc (activeSynth->synth, channel, 91, clamped);
}

void FluidSynthEngine::setPartChorusSend (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].chorusSend = clamped;
    if (activeSynth != nullptr)
        fluid_synth_cc (activeSynth->synth, channel, 93, clamped);
}

void FluidSynthEngine::setPartVariationSend (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].variationSend = clamped;
    if (activeSynth != nullptr)
        fluid_synth_cc (activeSynth->synth, channel, 94, clamped);
}

void FluidSynthEngine::handleNrpnDataEntry (int channel, int value, bool isMsb) noexcept
{
    juce::ignoreUnused (isMsb);
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto index = static_cast<size_t> (channel);
    const auto& state = nrpnStates[index];
    const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
    const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
    const auto partMode = static_cast<xg::PartMode> (partModeRaw);
    const auto isDrum = xg::isDrumMode (partMode) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit;

    const auto nrpnMsb = state.msb;
    const auto nrpnLsb = state.lsb;

    if (! isDrum)
    {
        // Melodic Part NRPN (MSB = 01H)
        if (nrpnMsb == 0x01)
        {
            switch (nrpnLsb)
            {
                case 0x08: setPartVibratoRate (channel, value); break;
                case 0x09: setPartVibratoDepth (channel, value); break;
                case 0x0A: setPartVibratoDelay (channel, value); break;
                case 0x20: setPartFilterCutoff (channel, value); break;
                case 0x21: setPartFilterResonance (channel, value); break;
                case 0x30: partParameters[index].eqBass = juce::jlimit (0, 127, value); break;
                case 0x31: partParameters[index].eqTreble = juce::jlimit (0, 127, value); break;
                case 0x34: partParameters[index].eqBassFreq = juce::jlimit (0, 127, value); break;
                case 0x35: partParameters[index].eqTrebleFreq = juce::jlimit (0, 127, value); break;
                case 0x63: setPartEgAttack (channel, value); break;
                case 0x64: setPartEgDecay (channel, value); break;
                case 0x66: setPartEgRelease (channel, value); break;
                default: break;
            }
        }
    }
    else
    {
        // Drum Part NRPN (MSB = 14H - 1FH, LSB = note number 0..127)
        if (nrpnMsb >= 0x14 && nrpnMsb <= 0x1F && juce::isPositiveAndBelow (static_cast<int> (nrpnLsb), 128))
        {
            auto& setup = (partMode == xg::PartMode::Drums2 || partMode == xg::PartMode::Drums4)
                              ? drumSetup2 : drumSetup1;
            auto& note = setup.notes[nrpnLsb];
            note.modified = true;

            const auto clamped = juce::jlimit (0, 127, value);
            switch (nrpnMsb)
            {
                case 0x14: note.filterCutoff = clamped; break;
                case 0x15: note.filterResonance = clamped; break;
                case 0x16: note.egAttack = clamped; break;
                case 0x17: note.egDecay1 = clamped; break;
                case 0x18: note.pitchCoarse = clamped; break;
                case 0x19: note.pitchFine = clamped; break;
                case 0x1A: note.level = clamped; break;
                case 0x1C: note.pan = clamped; break;
                case 0x1D: note.reverbSend = clamped; break;
                case 0x1E: note.chorusSend = clamped; break;
                case 0x1F: note.variationSend = clamped; break;
                default: break;
            }
        }
    }
}

void FluidSynthEngine::handleNrpnDataIncDec (int channel, int delta) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto index = static_cast<size_t> (channel);
    const auto& state = nrpnStates[index];
    const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
    const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
    const auto partMode = static_cast<xg::PartMode> (partModeRaw);
    const auto isDrum = xg::isDrumMode (partMode) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit;

    const auto nrpnMsb = state.msb;
    const auto nrpnLsb = state.lsb;

    if (! isDrum)
    {
        if (nrpnMsb == 0x01)
        {
            const auto& p = partParameters[index];
            switch (nrpnLsb)
            {
                case 0x08: setPartVibratoRate (channel, p.vibratoRate + delta); break;
                case 0x09: setPartVibratoDepth (channel, p.vibratoDepth + delta); break;
                case 0x0A: setPartVibratoDelay (channel, p.vibratoDelay + delta); break;
                case 0x20: setPartFilterCutoff (channel, p.filterCutoff + delta); break;
                case 0x21: setPartFilterResonance (channel, p.filterResonance + delta); break;
                case 0x30: partParameters[index].eqBass = juce::jlimit (0, 127, p.eqBass + delta); break;
                case 0x31: partParameters[index].eqTreble = juce::jlimit (0, 127, p.eqTreble + delta); break;
                case 0x34: partParameters[index].eqBassFreq = juce::jlimit (0, 127, p.eqBassFreq + delta); break;
                case 0x35: partParameters[index].eqTrebleFreq = juce::jlimit (0, 127, p.eqTrebleFreq + delta); break;
                case 0x63: setPartEgAttack (channel, p.egAttack + delta); break;
                case 0x64: setPartEgDecay (channel, p.egDecay + delta); break;
                case 0x66: setPartEgRelease (channel, p.egRelease + delta); break;
                default: break;
            }
        }
    }
    else
    {
        if (nrpnMsb >= 0x14 && nrpnMsb <= 0x1F && juce::isPositiveAndBelow (static_cast<int> (nrpnLsb), 128))
        {
            auto& setup = (partMode == xg::PartMode::Drums2 || partMode == xg::PartMode::Drums4)
                              ? drumSetup2 : drumSetup1;
            auto& note = setup.notes[nrpnLsb];
            note.modified = true;

            switch (nrpnMsb)
            {
                case 0x14: note.filterCutoff = juce::jlimit (0, 127, note.filterCutoff + delta); break;
                case 0x15: note.filterResonance = juce::jlimit (0, 127, note.filterResonance + delta); break;
                case 0x16: note.egAttack = juce::jlimit (0, 127, note.egAttack + delta); break;
                case 0x17: note.egDecay1 = juce::jlimit (0, 127, note.egDecay1 + delta); break;
                case 0x18: note.pitchCoarse = juce::jlimit (0, 127, note.pitchCoarse + delta); break;
                case 0x19: note.pitchFine = juce::jlimit (0, 127, note.pitchFine + delta); break;
                case 0x1A: note.level = juce::jlimit (0, 127, note.level + delta); break;
                case 0x1C: note.pan = juce::jlimit (0, 127, note.pan + delta); break;
                case 0x1D: note.reverbSend = juce::jlimit (0, 127, note.reverbSend + delta); break;
                case 0x1E: note.chorusSend = juce::jlimit (0, 127, note.chorusSend + delta); break;
                case 0x1F: note.variationSend = juce::jlimit (0, 127, note.variationSend + delta); break;
                default: break;
            }
        }
    }
}

void FluidSynthEngine::applyDrumNoteGenerators (fluid_voice_t* v, const xg::DrumNoteParameters& noteParams) noexcept
{
    if (v == nullptr)
        return;

    if (noteParams.pitchCoarse != 64)
    {
        fluid_voice_gen_incr (v, GEN_COARSETUNE, static_cast<float> (noteParams.pitchCoarse - 64));
        fluid_voice_update_param (v, GEN_COARSETUNE);
    }
    if (noteParams.pitchFine != 64)
    {
        fluid_voice_gen_incr (v, GEN_FINETUNE, static_cast<float> (noteParams.pitchFine - 64));
        fluid_voice_update_param (v, GEN_FINETUNE);
    }
    if (noteParams.filterCutoff != 64)
    {
        fluid_voice_gen_incr (v, GEN_FILTERFC, xg::cutoffOffsetToCents (noteParams.filterCutoff));
        fluid_voice_update_param (v, GEN_FILTERFC);
    }
    if (noteParams.filterResonance != 64)
    {
        fluid_voice_gen_incr (v, GEN_FILTERQ, xg::resonanceOffsetToCentibels (noteParams.filterResonance));
        fluid_voice_update_param (v, GEN_FILTERQ);
    }
    if (noteParams.egAttack != 64)
    {
        fluid_voice_gen_incr (v, GEN_VOLENVATTACK, xg::attackOffsetToTimecents (noteParams.egAttack));
        fluid_voice_update_param (v, GEN_VOLENVATTACK);
    }
    if (noteParams.egDecay1 != 64)
    {
        fluid_voice_gen_incr (v, GEN_VOLENVDECAY, xg::decayOffsetToTimecents (noteParams.egDecay1));
        fluid_voice_update_param (v, GEN_VOLENVDECAY);
    }
    if (noteParams.pan > 0)
    {
        const auto panVal = ((static_cast<float> (noteParams.pan) - 64.0f) / 64.0f) * 500.0f;
        fluid_voice_gen_set (v, GEN_PAN, panVal);
        fluid_voice_update_param (v, GEN_PAN);
    }
    else if (noteParams.pan == 0)
    {
        const auto randPan = juce::Random::getSystemRandom().nextFloat() * 1000.0f - 500.0f;
        fluid_voice_gen_set (v, GEN_PAN, randPan);
        fluid_voice_update_param (v, GEN_PAN);
    }
    if (noteParams.reverbSend >= 0)
    {
        fluid_voice_gen_set (v, GEN_REVERBSEND, (static_cast<float> (noteParams.reverbSend) / 127.0f) * 1000.0f);
        fluid_voice_update_param (v, GEN_REVERBSEND);
    }
    if (noteParams.chorusSend >= 0)
    {
        fluid_voice_gen_set (v, GEN_CHORUSSEND, (static_cast<float> (noteParams.chorusSend) / 127.0f) * 1000.0f);
        fluid_voice_update_param (v, GEN_CHORUSSEND);
    }
}

void FluidSynthEngine::handleMidiMessage (const juce::MidiMessage& message) noexcept
{
    if (message.isSysEx())
    {
        handleSysEx (message.getSysExData(), message.getSysExDataSize());
        return;
    }

    const auto channel = message.getChannel() - 1;
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    if (message.isNoteOn())
    {
        if (activeSynth != nullptr)
        {
            const auto noteNumber = message.getNoteNumber();
            const auto rawVelocity = message.getVelocity();
            const auto index = static_cast<size_t> (channel);
            const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
            const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
            const auto partMode = static_cast<xg::PartMode> (partModeRaw);
            const auto isDrum = xg::isDrumMode (partMode) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit;

            if (isDrum && juce::isPositiveAndBelow (noteNumber, 128))
            {
                const auto& setup = (partMode == xg::PartMode::Drums2 || partMode == xg::PartMode::Drums4)
                                        ? drumSetup2 : drumSetup1;
                const auto& noteParams = setup.notes[static_cast<size_t> (noteNumber)];

                if (noteParams.modified)
                {
                    if (noteParams.level == 0)
                        return;

                    const auto scaledVel = juce::jlimit (1, 127, static_cast<int> (std::round (rawVelocity * (static_cast<float> (noteParams.level) / 127.0f))));

                    std::array<fluid_voice_t*, 256> voiceBuf {};
                    fluid_synth_get_voicelist (activeSynth->synth, voiceBuf.data(), static_cast<int> (voiceBuf.size()), -1);
                    unsigned int maxIdBefore = 0;
                    for (auto* v : voiceBuf)
                    {
                        if (v == nullptr)
                            break;
                        maxIdBefore = juce::jmax (maxIdBefore, fluid_voice_get_id (v));
                    }

                    fluid_synth_noteon (activeSynth->synth, channel, noteNumber, scaledVel);

                    voiceBuf.fill (nullptr);
                    fluid_synth_get_voicelist (activeSynth->synth, voiceBuf.data(), static_cast<int> (voiceBuf.size()), -1);
                    for (auto* v : voiceBuf)
                    {
                        if (v == nullptr)
                            break;
                        if (fluid_voice_get_id (v) > maxIdBefore
                            && fluid_voice_get_channel (v) == channel
                            && fluid_voice_get_key (v) == noteNumber)
                        {
                            applyDrumNoteGenerators (v, noteParams);
                        }
                    }
                    return;
                }
            }

            fluid_synth_noteon (activeSynth->synth, channel, noteNumber, rawVelocity);
        }
    }
    else if (message.isNoteOff())
    {
        if (activeSynth != nullptr)
            fluid_synth_noteoff (activeSynth->synth, channel, message.getNoteNumber());
    }
    else if (message.isController())
    {
        const auto controller = message.getControllerNumber();
        auto value = message.getControllerValue();
        const auto index = static_cast<size_t> (channel);

        if (message.isAllNotesOff())
        {
            if (activeSynth != nullptr)
                fluid_synth_all_notes_off (activeSynth->synth, channel);
            return;
        }

        if (message.isAllSoundOff())
        {
            if (activeSynth != nullptr)
                fluid_synth_all_sounds_off (activeSynth->synth, channel);
            return;
        }

        // This engine is a fixed 16-channel GM synth. FluidSynth treats
        // channel-mode messages (CC124-127) as basic-channel group commands;
        // for example, CC126 on channel 1 can disable channels 2-16. Keep
        // the channel routing stable and only perform the required cleanup.
        if (controller >= 124 && controller <= 127)
        {
            if (activeSynth != nullptr)
                fluid_synth_all_notes_off (activeSynth->synth, channel);
            return;
        }

        if (message.isResetAllControllers())
        {
            const auto xgActive = isXgModeActive.load (std::memory_order_acquire);
            channelVolume[index].store (xgActive ? xg::defaultVolume : 127, std::memory_order_release);
            channelPan[index].store (xg::defaultPan, std::memory_order_release);
            nrpnStates[index].reset();
            if (activeSynth != nullptr)
                fluid_synth_cc (activeSynth->synth, channel, controller, value);
            channelStateNeedsApply = true;
            return;
        }

        // NRPN / RPN Parameter Selection & Data Entry
        if (controller == 99) // NRPN MSB
        {
            nrpnStates[index].msb = static_cast<uint8_t> (value);
            nrpnStates[index].activeSelection = xg::ParameterSelection::Nrpn;
            return;
        }
        if (controller == 98) // NRPN LSB
        {
            nrpnStates[index].lsb = static_cast<uint8_t> (value);
            nrpnStates[index].activeSelection = xg::ParameterSelection::Nrpn;
            return;
        }
        if (controller == 101) // RPN MSB
        {
            nrpnStates[index].msb = static_cast<uint8_t> (value);
            if (value == 127 && nrpnStates[index].lsb == 127)
                nrpnStates[index].activeSelection = xg::ParameterSelection::None;
            else
                nrpnStates[index].activeSelection = xg::ParameterSelection::Rpn;

            if (activeSynth != nullptr)
                fluid_synth_cc (activeSynth->synth, channel, controller, value);
            return;
        }
        if (controller == 100) // RPN LSB
        {
            nrpnStates[index].lsb = static_cast<uint8_t> (value);
            if (nrpnStates[index].msb == 127 && value == 127)
                nrpnStates[index].activeSelection = xg::ParameterSelection::None;
            else
                nrpnStates[index].activeSelection = xg::ParameterSelection::Rpn;

            if (activeSynth != nullptr)
                fluid_synth_cc (activeSynth->synth, channel, controller, value);
            return;
        }
        if (controller == 6) // Data Entry MSB
        {
            if (nrpnStates[index].activeSelection == xg::ParameterSelection::Nrpn)
            {
                handleNrpnDataEntry (channel, value, true);
                return;
            }
            if (nrpnStates[index].activeSelection == xg::ParameterSelection::Rpn)
            {
                if (nrpnStates[index].msb == 0 && nrpnStates[index].lsb == 0)
                    partParameters[index].pitchBendSensitivity = value;
            }
            if (activeSynth != nullptr)
                fluid_synth_cc (activeSynth->synth, channel, controller, value);
            return;
        }
        if (controller == 38) // Data Entry LSB
        {
            if (nrpnStates[index].activeSelection == xg::ParameterSelection::Nrpn)
                return; // XG NRPNs are 7-bit MSB only

            if (activeSynth != nullptr)
                fluid_synth_cc (activeSynth->synth, channel, controller, value);
            return;
        }
        if (controller == 96) // Data Increment
        {
            if (nrpnStates[index].activeSelection == xg::ParameterSelection::Nrpn)
            {
                handleNrpnDataIncDec (channel, 1);
                return;
            }
            if (activeSynth != nullptr)
                fluid_synth_cc (activeSynth->synth, channel, controller, value);
            return;
        }
        if (controller == 97) // Data Decrement
        {
            if (nrpnStates[index].activeSelection == xg::ParameterSelection::Nrpn)
            {
                handleNrpnDataIncDec (channel, -1);
                return;
            }
            if (activeSynth != nullptr)
                fluid_synth_cc (activeSynth->synth, channel, controller, value);
            return;
        }

        // Sound Controllers
        if (controller == 71) // Harmonic Content / Resonance
        {
            setPartFilterResonance (channel, value);
            return;
        }
        if (controller == 72) // Release Time
        {
            setPartEgRelease (channel, value);
            return;
        }
        if (controller == 73) // Attack Time
        {
            setPartEgAttack (channel, value);
            return;
        }
        if (controller == 74) // Brightness / Cutoff
        {
            setPartFilterCutoff (channel, value);
            return;
        }
        if (controller == 75) // Decay Time
        {
            setPartEgDecay (channel, value);
            return;
        }
        if (controller == 76) // Vibrato Rate
        {
            setPartVibratoRate (channel, value);
            return;
        }
        if (controller == 77) // Vibrato Depth
        {
            setPartVibratoDepth (channel, value);
            return;
        }
        if (controller == 78) // Vibrato Delay
        {
            setPartVibratoDelay (channel, value);
            return;
        }
        if (controller == 91) // Reverb Send
        {
            setPartReverbSend (channel, value);
            return;
        }
        if (controller == 93) // Chorus Send
        {
            setPartChorusSend (channel, value);
            return;
        }
        if (controller == 94) // Variation Send
        {
            setPartVariationSend (channel, value);
            return;
        }

        if (controller == 0)
        {
            if (channel == 9 && drumPartProtectMode[9].load (std::memory_order_acquire) && value < 126)
            {
                // Protected drum channel ignores melodic Bank MSB 0..125
            }
            else
            {
                channelBankMsb[index].store (value, std::memory_order_release);
                const auto lsb = channelBankLsb[index].load (std::memory_order_acquire);
                channelBank[index].store ((value << 7) | (lsb & 0x7f), std::memory_order_release);

                if (value == xg::bankMsbDrumKit || value == xg::bankMsbSfxKit)
                    channelPartMode[index].store (static_cast<uint8_t> (xg::PartMode::Drum), std::memory_order_release);
                else if (value == xg::bankMsbNormal)
                    channelPartMode[index].store (static_cast<uint8_t> (xg::PartMode::Normal), std::memory_order_release);
            }
        }
        else if (controller == 32)
        {
            const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
            const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
            const auto isDrum = xg::isDrumMode (static_cast<xg::PartMode> (partModeRaw)) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit;

            // XG note 4: Bank LSB is ignored/fixed to 0 for drum kit
            if (! isDrum)
            {
                channelBankLsb[index].store (value, std::memory_order_release);
                channelBank[index].store ((msb << 7) | value, std::memory_order_release);
            }
        }

        if (controller == 7)
        {
            channelVolume[index].store (value, std::memory_order_release);

            if (channelMuted[index].load (std::memory_order_acquire))
                value = 0;
        }
        else if (controller == 10)
        {
            channelPan[index].store (message.getControllerValue(), std::memory_order_release);
        }

        if (activeSynth == nullptr)
            return;

        fluid_synth_cc (activeSynth->synth, channel, controller, value);
        if (controller == 0)
        {
            const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
            const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
            const auto isDrum = xg::isDrumMode (static_cast<xg::PartMode> (partModeRaw)) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit;
            fluid_synth_set_channel_type (activeSynth->synth, channel, isDrum ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
        }
        if (controller == 7)
            appliedChannelVolume[index] = channelVolume[index].load (std::memory_order_acquire);
        else if (controller == 10)
            appliedChannelPan[index] = channelPan[index].load (std::memory_order_acquire);
        else if (controller == 0 || controller == 32)
        {
            appliedChannelBank[index] = channelBank[index].load (std::memory_order_acquire);
            appliedChannelBankMsb[index] = channelBankMsb[index].load (std::memory_order_acquire);
            appliedChannelBankLsb[index] = channelBankLsb[index].load (std::memory_order_acquire);
        }
    }
    else if (message.isProgramChange())
    {
        const auto program = message.getProgramChangeNumber();
        channelProgram[static_cast<size_t> (channel)].store (program, std::memory_order_release);
        if (activeSynth != nullptr)
        {
            handleProgramChange (channel, program);
            appliedChannelProgram[static_cast<size_t> (channel)] = program;
        }
    }
    else if (message.isPitchWheel())
    {
        if (activeSynth != nullptr)
            fluid_synth_pitch_bend (activeSynth->synth, channel, message.getPitchWheelValue());
    }
    else if (message.isChannelPressure())
    {
        if (activeSynth != nullptr)
            fluid_synth_channel_pressure (activeSynth->synth, channel, message.getChannelPressureValue());
    }
    else if (message.isAftertouch())
    {
        if (activeSynth != nullptr)
            fluid_synth_key_pressure (activeSynth->synth,
                                      channel,
                                      message.getNoteNumber(),
                                      message.getAfterTouchValue());
    }
}

void FluidSynthEngine::renderRange (const juce::MidiBuffer& midiMessages,
                                    int rangeStart,
                                    int rangeLength,
                                    float* left,
                                    float* right) noexcept
{
    if (activeSynth == nullptr || rangeLength <= 0)
        return;

    const auto rangeEnd = rangeStart + rangeLength;
    auto renderedUntil = rangeStart;

    for (const auto metadata : midiMessages)
    {
        if (metadata.samplePosition >= rangeEnd)
            break;

        // In the mono/no-output fallback this function is called once per
        // chunk. Events handled by an earlier chunk must not be replayed.
        if (rangeStart > 0 && metadata.samplePosition < rangeStart)
            continue;

        if (metadata.numBytes <= 0 || metadata.data == nullptr)
            continue;

        const auto eventPosition = juce::jmax (rangeStart, metadata.samplePosition);
        if (eventPosition > renderedUntil)
        {
            fluid_synth_write_float (activeSynth->synth,
                                     eventPosition - renderedUntil,
                                     left + (renderedUntil - rangeStart),
                                     0,
                                     1,
                                     right + (renderedUntil - rangeStart),
                                     0,
                                     1);
            renderedUntil = eventPosition;
        }

        if (metadata.data[0] == 0xf0)
        {
            if (metadata.numBytes >= 2 && metadata.data[metadata.numBytes - 1] == 0xf7)
                handleSysEx (metadata.data + 1, metadata.numBytes - 2);
        }
        else if (metadata.numBytes <= 4)
        {
            handleMidiMessage (juce::MidiMessage (metadata.data, metadata.numBytes));
        }
    }

    if (renderedUntil < rangeEnd)
    {
        fluid_synth_write_float (activeSynth->synth,
                                 rangeEnd - renderedUntil,
                                 left + (renderedUntil - rangeStart),
                                 0,
                                 1,
                                 right + (renderedUntil - rangeStart),
                                 0,
                                 1);
    }
}

void FluidSynthEngine::processBlock (juce::AudioBuffer<float>& buffer,
                                     const juce::MidiBuffer& midiMessages,
                                     const juce::MidiMessage* keyboardMessages,
                                     int numKeyboardMessages) noexcept
{
    adoptPendingChange();
    buffer.clear();

    if (activeSynth == nullptr)
    {
        for (const auto metadata : midiMessages)
        {
            if (metadata.numBytes <= 0 || metadata.data == nullptr)
                continue;

            if (metadata.data[0] == 0xf0)
            {
                if (metadata.numBytes >= 2 && metadata.data[metadata.numBytes - 1] == 0xf7)
                    handleSysEx (metadata.data + 1, metadata.numBytes - 2);
            }
            else if (metadata.numBytes <= 4)
            {
                handleMidiMessage (juce::MidiMessage (metadata.data, metadata.numBytes));
            }
        }

        for (int i = 0; i < numKeyboardMessages; ++i)
            handleMidiMessage (keyboardMessages[i]);

        return;
    }

    applyChannelState();

    for (int i = 0; i < numKeyboardMessages; ++i)
        handleMidiMessage (keyboardMessages[i]);

    const auto numSamples = buffer.getNumSamples();
    if (numSamples <= 0)
        return;

    if (buffer.getNumChannels() >= 2)
    {
        renderRange (midiMessages,
                     0,
                     numSamples,
                     buffer.getWritePointer (0),
                     buffer.getWritePointer (1));
        return;
    }

    const auto chunkCapacity = juce::jmax (1, scratchBuffer.getNumSamples());
    for (int offset = 0; offset < numSamples; offset += chunkCapacity)
    {
        const auto chunkSize = juce::jmin (chunkCapacity, numSamples - offset);
        scratchBuffer.clear (0, 0, chunkSize);
        scratchBuffer.clear (1, 0, chunkSize);
        renderRange (midiMessages,
                     offset,
                     chunkSize,
                     scratchBuffer.getWritePointer (0),
                     scratchBuffer.getWritePointer (1));

        if (buffer.getNumChannels() == 1)
            buffer.copyFrom (0, offset, scratchBuffer, 0, 0, chunkSize);
    }
}

void FluidSynthEngine::reclaimRetired() noexcept
{
    auto read = retiredFifo.read (maxRetiredChanges);

    for (int i = 0; i < read.blockSize1; ++i)
    {
        auto& retired = retiredChanges[read.startIndex1 + i];
        delete retired.synth;
        delete retired.change;
        retired = {};
    }

    for (int i = 0; i < read.blockSize2; ++i)
    {
        auto& retired = retiredChanges[read.startIndex2 + i];
        delete retired.synth;
        delete retired.change;
        retired = {};
    }
}

void FluidSynthEngine::destroyChange (SynthChange* change) noexcept
{
    if (change == nullptr)
        return;

    delete change->synth;
    delete change;
}
