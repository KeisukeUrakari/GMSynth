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

    systemParameters.reset();
    for (auto& nrpn : nrpnStates)
        nrpn.reset();
    for (auto& part : partParameters)
        part.reset();
    drumSetup1.reset();
    drumSetup2.reset();

    for (auto& chanMap : activeNoteTransposition)
        for (size_t n = 0; n < 128; ++n)
            chanMap[n] = static_cast<uint8_t> (n);

    for (auto& chanGroups : activeGroupNote)
        chanGroups.fill (-1);

    reverbParameters.reset();
    chorusParameters.reset();
    variationParameters.reset();
    multiEqParameters.reset();
    multiEqFiltersNeedUpdate = true;
    for (auto& chan : multiEqFilters)
        for (auto& f : chan)
            f.reset();

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
    multiEqFiltersNeedUpdate = true;
    for (auto& chan : multiEqFilters)
        for (auto& f : chan)
            f.reset();
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

    const auto baseGain = masterGain.load (std::memory_order_acquire);
    const auto volRatio = static_cast<float> (systemParameters.masterVolume) / 127.0f;
    fluid_synth_set_gain (instance.synth, baseGain * volRatio);

    updateReverbSettings();
    updateChorusSettings();

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
        const auto totalCents = systemParameters.masterTuneCents + params.detuneCents;
        fluid_synth_set_gen (instance.synth, channel, GEN_FINETUNE, totalCents);

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
    updateMasterVolume();
}

float FluidSynthEngine::getMasterGain() const noexcept
{
    return masterGain.load (std::memory_order_acquire);
}

void FluidSynthEngine::updateMasterVolume() noexcept
{
    if (activeSynth != nullptr)
    {
        const auto baseGain = masterGain.load (std::memory_order_acquire);
        const auto volRatio = static_cast<float> (systemParameters.masterVolume) / 127.0f;
        const auto desiredMasterGain = baseGain * volRatio;
        fluid_synth_set_gain (activeSynth->synth, desiredMasterGain);
        appliedMasterGain = desiredMasterGain;
    }
}

void FluidSynthEngine::updateChannelTuning (int channel) noexcept
{
    if (activeSynth == nullptr || ! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto index = static_cast<size_t> (channel);
    const auto totalCents = systemParameters.masterTuneCents + partParameters[index].detuneCents;
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_FINETUNE, totalCents);
}

void FluidSynthEngine::updateAllChannelTunings() noexcept
{
    if (activeSynth == nullptr)
        return;

    for (int channel = 0; channel < numMidiChannels; ++channel)
        updateChannelTuning (channel);
}

void FluidSynthEngine::updateReverbSettings() noexcept
{
    if (activeSynth == nullptr)
        return;

    if (reverbParameters.typeMsb == 0)
    {
        fluid_synth_set_reverb_on (activeSynth->synth, 0);
        return;
    }

    fluid_synth_set_reverb_on (activeSynth->synth, 1);

    double baseRoom = 0.6;
    if (reverbParameters.typeMsb == 0x01 || reverbParameters.typeMsb == 0x02)
        baseRoom = 0.8;
    else if (reverbParameters.typeMsb >= 0x03 && reverbParameters.typeMsb <= 0x05)
        baseRoom = 0.45;
    else if (reverbParameters.typeMsb == 0x06 || reverbParameters.typeMsb == 0x07)
        baseRoom = 0.65;
    else if (reverbParameters.typeMsb == 0x08)
        baseRoom = 0.55;

    const auto timeParam = reverbParameters.parameters[0] > 0 ? reverbParameters.parameters[0] : 64;
    const auto roomsize = juce::jlimit (0.0, 1.0, baseRoom * (static_cast<double> (timeParam) / 64.0));

    const auto dampParam = reverbParameters.parameters[1] > 0 ? reverbParameters.parameters[1] : 64;
    const auto damping = juce::jlimit (0.0, 1.0, static_cast<double> (dampParam) / 127.0);

    const auto level = juce::jlimit (0.0, 1.0, static_cast<double> (reverbParameters.reverbReturn) / 127.0);
    const auto width = 1.0;

    fluid_synth_set_reverb (activeSynth->synth, roomsize, damping, width, level);
}

void FluidSynthEngine::updateChorusSettings() noexcept
{
    if (activeSynth == nullptr)
        return;

    if (chorusParameters.typeMsb == 0)
    {
        fluid_synth_set_chorus_on (activeSynth->synth, 0);
        return;
    }

    fluid_synth_set_chorus_on (activeSynth->synth, 1);

    const auto nr = (chorusParameters.typeMsb >= 0x48) ? 4 : 3;
    const auto level = juce::jlimit (0.0, 1.5, static_cast<double> (chorusParameters.chorusReturn) / 64.0 * 0.6);
    const auto speedParam = chorusParameters.parameters[0] > 0 ? chorusParameters.parameters[0] : 64;
    const auto speedNorm = static_cast<double> (speedParam) / 127.0;
    const auto speed = juce::jlimit (0.1, 2.0, 0.1 + std::pow (speedNorm, 2.0) * 0.9);
    const auto depthParam = chorusParameters.parameters[1] > 0 ? chorusParameters.parameters[1] : 64;
    const auto depth_ms = juce::jlimit (0.0, 6.0, (static_cast<double> (depthParam) / 127.0) * 4.0);
    const auto type = 0;

    fluid_synth_set_chorus (activeSynth->synth, nr, level, speed, depth_ms, type);
}

void FluidSynthEngine::updateMultiEqCoefficients() noexcept
{
    const auto sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr <= 1.0)
        return;

    const auto nyquist = sr * 0.49;

    auto makeBandCoeff = [sr, nyquist] (int gainVal, int freqIdx, int qVal, int shape, bool isBand1, bool isBand5) -> juce::IIRCoefficients
    {
        const auto gainDb = static_cast<float> (gainVal - 64);
        const auto gainFactor = juce::Decibels::decibelsToGain (gainDb);
        const auto freq = juce::jlimit (20.0, nyquist, static_cast<double> (xg::lookupEqFrequency (freqIdx)));
        const auto q = juce::jlimit (0.1, 12.0, static_cast<double> (qVal) / 10.0);

        if (isBand1 && shape == 0)
            return juce::IIRCoefficients::makeLowShelf (sr, freq, q, gainFactor);

        if (isBand5 && shape == 0)
            return juce::IIRCoefficients::makeHighShelf (sr, freq, q, gainFactor);

        return juce::IIRCoefficients::makePeakFilter (sr, freq, q, gainFactor);
    };

    struct BandDef
    {
        int gain;
        int freq;
        int q;
        int shape;
        bool isBand1;
        bool isBand5;
    };

    const std::array<BandDef, 5> bands {{
        { multiEqParameters.gain1, multiEqParameters.freq1, multiEqParameters.q1, multiEqParameters.shape1, true, false },
        { multiEqParameters.gain2, multiEqParameters.freq2, multiEqParameters.q2, 1, false, false },
        { multiEqParameters.gain3, multiEqParameters.freq3, multiEqParameters.q3, 1, false, false },
        { multiEqParameters.gain4, multiEqParameters.freq4, multiEqParameters.q4, 1, false, false },
        { multiEqParameters.gain5, multiEqParameters.freq5, multiEqParameters.q5, multiEqParameters.shape5, false, true }
    }};

    for (size_t b = 0; b < 5; ++b)
    {
        if (bands[b].gain == 64)
        {
            multiEqFilters[0][b].makeInactive();
            multiEqFilters[1][b].makeInactive();
        }
        else
        {
            const auto coeff = makeBandCoeff (bands[b].gain, bands[b].freq, bands[b].q, bands[b].shape, bands[b].isBand1, bands[b].isBand5);
            multiEqFilters[0][b].setCoefficients (coeff);
            multiEqFilters[1][b].setCoefficients (coeff);
        }
    }
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
    for (size_t n = 0; n < 128; ++n)
        activeNoteTransposition[index][n] = static_cast<uint8_t> (n);
    activeGroupNote[index].fill (-1);
    resetAllGenerators (channel);
    updateChannelTuning (channel);
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

    const auto isXg = numBytes >= 7
        && data[0] == 0x43
        && (data[1] & 0xf0) == 0x10
        && data[2] == 0x4c;

    if (isGmReset || isGsReset)
    {
        isXgModeActive.store (false, std::memory_order_release);
        systemParameters.reset();
        drumSetup1.reset();
        drumSetup2.reset();
        reverbParameters.reset();
        chorusParameters.reset();
        variationParameters.reset();
        multiEqParameters.reset();
        multiEqFiltersNeedUpdate = true;
        for (auto& chan : multiEqFilters)
            for (auto& f : chan)
                f.reset();

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
        updateMasterVolume();
        updateAllChannelTunings();
        updateReverbSettings();
        updateChorusSettings();
        return;
    }

    if (! isXg)
        return;

    const auto addrHigh = data[3];
    const auto addrMid  = data[4];
    const auto addrLow  = data[5];
    const auto numData  = numBytes - 6;
    const auto* dataPtr = data + 6;

    // 1. XG System Data (00 00 aa)
    if (addrHigh == 0x00 && addrMid == 0x00)
    {
        if (addrLow == 0x7e && numData >= 1 && dataPtr[0] == 0x00)
        {
            // XG System On
            isXgModeActive.store (true, std::memory_order_release);
            systemParameters.reset();
            drumSetup1.reset();
            drumSetup2.reset();
            reverbParameters.reset();
            chorusParameters.reset();
            variationParameters.reset();
            multiEqParameters.reset();
            multiEqFiltersNeedUpdate = true;
            for (auto& chan : multiEqFilters)
                for (auto& f : chan)
                    f.reset();
            for (int channel = 0; channel < numMidiChannels; ++channel)
                resetChannelState (channel);

            if (activeSynth != nullptr)
            {
                for (int channel = 0; channel < numMidiChannels; ++channel)
                {
                    const auto partMode = static_cast<xg::PartMode> (channelPartMode[static_cast<size_t> (channel)].load (std::memory_order_acquire));
                    fluid_synth_set_channel_type (activeSynth->synth, channel, xg::isDrumMode (partMode) ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
                }
            }
            channelStateNeedsApply = true;
            updateMasterVolume();
            updateAllChannelTunings();
            updateReverbSettings();
            updateChorusSettings();
            return;
        }

        if (addrLow == 0x7f && numData >= 1 && dataPtr[0] == 0x00)
        {
            // All Parameter Reset
            isXgModeActive.store (true, std::memory_order_release);
            systemParameters.reset();
            drumSetup1.reset();
            drumSetup2.reset();
            reverbParameters.reset();
            chorusParameters.reset();
            variationParameters.reset();
            multiEqParameters.reset();
            multiEqFiltersNeedUpdate = true;
            for (auto& chan : multiEqFilters)
                for (auto& f : chan)
                    f.reset();
            for (int channel = 0; channel < numMidiChannels; ++channel)
                resetChannelState (channel);

            channelStateNeedsApply = true;
            updateMasterVolume();
            updateAllChannelTunings();
            updateReverbSettings();
            updateChorusSettings();
            return;
        }

        if (addrLow == 0x7d && numData >= 1)
        {
            // Drum Setup Reset: 0 for setup 1, 1 for setup 2
            const auto setupNum = dataPtr[0];
            if (setupNum == 0)
                drumSetup1.reset();
            else if (setupNum == 1)
                drumSetup2.reset();
            return;
        }

        // Other system parameters
        bool tuneChanged = false;
        for (int i = 0; i < numData; ++i)
        {
            const auto curAddr = addrLow + i;
            const auto val = dataPtr[i];

            if (curAddr >= 0x00 && curAddr <= 0x03)
            {
                systemParameters.masterTuneRaw[static_cast<size_t> (curAddr)] = val & 0x0f;
                tuneChanged = true;
            }
            else if (curAddr == 0x04)
            {
                systemParameters.masterVolume = juce::jlimit (0, 127, static_cast<int> (val));
                updateMasterVolume();
            }
            else if (curAddr == 0x05)
            {
                systemParameters.masterAttenuator = juce::jlimit (0, 127, static_cast<int> (val));
            }
            else if (curAddr == 0x06)
            {
                systemParameters.transpose = juce::jlimit (0x28, 0x58, static_cast<int> (val));
            }
        }

        if (tuneChanged)
        {
            systemParameters.updateMasterTuneFromRaw();
            updateAllChannelTunings();
        }
        return;
    }

    // 2. Multi Part Parameter Change (08 nn aa)
    if (addrHigh == 0x08)
    {
        const auto part = addrMid & 0x1f; // 0..15
        if (! juce::isPositiveAndBelow (static_cast<int> (part), numMidiChannels))
            return;

        const auto channel = static_cast<int> (part);
        const auto chIdx = static_cast<size_t> (channel);
        auto& p = partParameters[chIdx];

        for (int i = 0; i < numData; ++i)
        {
            const auto curAddr = addrLow + i;
            const auto val = dataPtr[i];

            switch (curAddr)
            {
                case 0x00: // Element Reserve
                    p.elementReserve = juce::jlimit (0, 32, static_cast<int> (val));
                    break;

                case 0x01: // Bank Select MSB
                    channelBankMsb[chIdx].store (val, std::memory_order_release);
                    channelBank[chIdx].store ((val << 7) | channelBankLsb[chIdx].load (std::memory_order_acquire), std::memory_order_release);
                    if (val == xg::bankMsbDrumKit || val == xg::bankMsbSfxKit)
                        channelPartMode[chIdx].store (static_cast<uint8_t> (xg::PartMode::Drum), std::memory_order_release);
                    else if (val == xg::bankMsbNormal)
                        channelPartMode[chIdx].store (static_cast<uint8_t> (xg::PartMode::Normal), std::memory_order_release);
                    channelStateNeedsApply = true;
                    break;

                case 0x02: // Bank Select LSB
                    channelBankLsb[chIdx].store (val, std::memory_order_release);
                    channelBank[chIdx].store ((channelBankMsb[chIdx].load (std::memory_order_acquire) << 7) | val, std::memory_order_release);
                    channelStateNeedsApply = true;
                    break;

                case 0x03: // Program Number
                    channelProgram[chIdx].store (val, std::memory_order_release);
                    channelStateNeedsApply = true;
                    break;

                case 0x04: // Rcv Channel
                    p.rcvChannel = val;
                    break;

                case 0x05: // Mono/Poly Mode
                    p.monoPolyMode = (val == 0 ? 0 : 1);
                    if (activeSynth != nullptr)
                        fluid_synth_cc (activeSynth->synth, channel, p.monoPolyMode == 0 ? 126 : 127, p.monoPolyMode == 0 ? 1 : 0);
                    break;

                case 0x06: // Same Note Assign
                    p.sameNoteAssign = val;
                    break;

                case 0x07: // Part Mode
                {
                    const auto mode = static_cast<xg::PartMode> (val);
                    channelPartMode[chIdx].store (static_cast<uint8_t> (mode), std::memory_order_release);
                    if (mode == xg::PartMode::Normal)
                    {
                        if (channelBankMsb[chIdx].load (std::memory_order_acquire) >= 126)
                        {
                            channelBankMsb[chIdx].store (0, std::memory_order_release);
                            channelBank[chIdx].store (channelBankLsb[chIdx].load (std::memory_order_acquire), std::memory_order_release);
                        }
                        if (part == 9)
                            drumPartProtectMode[9].store (false, std::memory_order_release);
                    }
                    else
                    {
                        if (channelBankMsb[chIdx].load (std::memory_order_acquire) < 126)
                        {
                            channelBankMsb[chIdx].store (xg::bankMsbDrumKit, std::memory_order_release);
                            channelBank[chIdx].store ((xg::bankMsbDrumKit << 7) | channelBankLsb[chIdx].load (std::memory_order_acquire), std::memory_order_release);
                        }
                    }
                    channelStateNeedsApply = true;
                    break;
                }

                case 0x08: // Note Shift
                    p.noteShift = juce::jlimit (0x28, 0x58, static_cast<int> (val));
                    break;

                case 0x09: // Detune MSB
                    p.detuneMsb = val & 0x0f;
                    p.updateDetuneCents();
                    updateChannelTuning (channel);
                    break;

                case 0x0a: // Detune LSB
                    p.detuneLsb = val & 0x0f;
                    p.updateDetuneCents();
                    updateChannelTuning (channel);
                    break;

                case 0x0b: // Volume
                    setChannelVolume (channel, val);
                    if (activeSynth != nullptr)
                        fluid_synth_cc (activeSynth->synth, channel, 7, val);
                    break;

                case 0x0c: // Velocity Sense Depth
                    p.velocitySenseDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    break;

                case 0x0d: // Velocity Sense Offset
                    p.velocitySenseOffset = juce::jlimit (0, 127, static_cast<int> (val));
                    break;

                case 0x0e: // Pan
                    setChannelPan (channel, val);
                    if (activeSynth != nullptr)
                        fluid_synth_cc (activeSynth->synth, channel, 10, val);
                    break;

                case 0x0f: // Note Limit Low
                    p.noteLimitLow = juce::jlimit (0, 127, static_cast<int> (val));
                    break;

                case 0x10: // Note Limit High
                    p.noteLimitHigh = juce::jlimit (0, 127, static_cast<int> (val));
                    break;

                case 0x11: // Dry Level
                    p.dryLevel = juce::jlimit (0, 127, static_cast<int> (val));
                    break;

                case 0x12: // Chorus Send
                    setPartChorusSend (channel, val);
                    break;

                case 0x13: // Reverb Send
                    setPartReverbSend (channel, val);
                    break;

                case 0x14: // Variation Send
                    setPartVariationSend (channel, val);
                    break;

                case 0x15: // Vibrato Rate
                    setPartVibratoRate (channel, val);
                    break;

                case 0x16: // Vibrato Depth
                    setPartVibratoDepth (channel, val);
                    break;

                case 0x17: // Vibrato Delay
                    setPartVibratoDelay (channel, val);
                    break;

                case 0x18: // Filter Cutoff
                    setPartFilterCutoff (channel, val);
                    break;

                case 0x19: // Filter Resonance
                    setPartFilterResonance (channel, val);
                    break;

                case 0x1a: // EG Attack
                    setPartEgAttack (channel, val);
                    break;

                case 0x1b: // EG Decay
                    setPartEgDecay (channel, val);
                    break;

                case 0x1c: // EG Release
                    setPartEgRelease (channel, val);
                    break;

                case 0x23: // Bend Pitch Control
                {
                    const auto semitones = std::abs (static_cast<int> (val) - 64);
                    p.pitchBendSensitivity = semitones;
                    if (activeSynth != nullptr)
                    {
                        fluid_synth_cc (activeSynth->synth, channel, 101, 0);
                        fluid_synth_cc (activeSynth->synth, channel, 100, 0);
                        fluid_synth_cc (activeSynth->synth, channel, 6, semitones);
                        fluid_synth_cc (activeSynth->synth, channel, 101, 127);
                        fluid_synth_cc (activeSynth->synth, channel, 100, 127);
                    }
                    break;
                }

                case 0x67: // Portamento Switch
                    p.portamentoSwitch = (val != 0 ? 1 : 0);
                    if (activeSynth != nullptr)
                        fluid_synth_cc (activeSynth->synth, channel, 65, val != 0 ? 127 : 0);
                    break;

                case 0x68: // Portamento Time
                    p.portamentoTime = juce::jlimit (0, 127, static_cast<int> (val));
                    if (activeSynth != nullptr)
                        fluid_synth_cc (activeSynth->synth, channel, 5, p.portamentoTime);
                    break;

                case 0x72: // EQ Bass Gain
                    p.eqBass = juce::jlimit (0, 127, static_cast<int> (val));
                    break;

                case 0x73: // EQ Treble Gain
                    p.eqTreble = juce::jlimit (0, 127, static_cast<int> (val));
                    break;

                case 0x76: // EQ Bass Frequency
                    p.eqBassFreq = juce::jlimit (0, 127, static_cast<int> (val));
                    break;

                case 0x77: // EQ Treble Frequency
                    p.eqTrebleFreq = juce::jlimit (0, 127, static_cast<int> (val));
                    break;

                default:
                    break;
            }
        }
        return;
    }

    // 3. Drum Setup Parameter Change (3n rr aa)
    if ((addrHigh & 0xf0) == 0x30)
    {
        const auto setupIdx = addrHigh & 0x0f; // 0: Setup 1 (30H), 1: Setup 2 (31H)
        if (setupIdx > 1)
            return;

        auto& setup = (setupIdx == 1 ? drumSetup2 : drumSetup1);
        const auto drumNote = addrMid;
        if (! juce::isPositiveAndBelow (static_cast<int> (drumNote), 128))
            return;

        auto& note = setup.notes[drumNote];
        note.modified = true;

        for (int i = 0; i < numData; ++i)
        {
            const auto curAddr = addrLow + i;
            const auto val = dataPtr[i];

            switch (curAddr)
            {
                case 0x00: note.pitchCoarse = val; break;
                case 0x01: note.pitchFine = val; break;
                case 0x02: note.level = val; break;
                case 0x03: note.alternateGroup = val; break;
                case 0x04: note.pan = val; break;
                case 0x05: note.reverbSend = val; break;
                case 0x06: note.chorusSend = val; break;
                case 0x07: note.variationSend = val; break;
                case 0x08: note.keyAssign = val; break;
                case 0x09: note.rcvNoteOff = val; break;
                case 0x0a: note.rcvNoteOn = val; break;
                case 0x0b: note.filterCutoff = val; break;
                case 0x0c: note.filterResonance = val; break;
                case 0x0d: note.egAttack = val; break;
                case 0x0e: note.egDecay1 = val; break;
                case 0x0f: note.egDecay2 = val; break;
                case 0x20: note.eqBass = val; break;
                case 0x21: note.eqTreble = val; break;
                default: break;
            }
        }
        return;
    }

    // 4. Effect 1 Parameter Change (02 01 aa)
    if (addrHigh == 0x02 && addrMid == 0x01)
    {
        bool reverbChanged = false;
        bool chorusChanged = false;

        for (int i = 0; i < numData; ++i)
        {
            const auto curAddr = addrLow + i;
            const auto val = dataPtr[i];

            // --- Reverb (00..1F) ---
            if (curAddr == 0x00)
            {
                reverbParameters.typeMsb = val;
                reverbChanged = true;
            }
            else if (curAddr == 0x01)
            {
                reverbParameters.typeLsb = val;
                reverbChanged = true;
            }
            else if (curAddr >= 0x02 && curAddr <= 0x0b)
            {
                reverbParameters.parameters[static_cast<size_t> (curAddr - 0x02)] = val;
                reverbChanged = true;
            }
            else if (curAddr == 0x0c)
            {
                reverbParameters.reverbReturn = val;
                reverbChanged = true;
            }
            else if (curAddr == 0x0d)
            {
                reverbParameters.reverbPan = val;
            }
            else if (curAddr >= 0x10 && curAddr <= 0x15)
            {
                reverbParameters.parameters[static_cast<size_t> (10 + curAddr - 0x10)] = val;
                reverbChanged = true;
            }

            // --- Chorus (20..3F) ---
            else if (curAddr == 0x20)
            {
                chorusParameters.typeMsb = val;
                chorusChanged = true;
            }
            else if (curAddr == 0x21)
            {
                chorusParameters.typeLsb = val;
                chorusChanged = true;
            }
            else if (curAddr >= 0x22 && curAddr <= 0x2b)
            {
                chorusParameters.parameters[static_cast<size_t> (curAddr - 0x22)] = val;
                chorusChanged = true;
            }
            else if (curAddr == 0x2c)
            {
                chorusParameters.chorusReturn = val;
                chorusChanged = true;
            }
            else if (curAddr == 0x2d)
            {
                chorusParameters.chorusPan = val;
            }
            else if (curAddr == 0x2e)
            {
                chorusParameters.sendToReverb = val;
            }
            else if (curAddr >= 0x30 && curAddr <= 0x35)
            {
                chorusParameters.parameters[static_cast<size_t> (10 + curAddr - 0x30)] = val;
                chorusChanged = true;
            }

            // --- Variation (40..7F) ---
            else if (curAddr == 0x40)
            {
                variationParameters.typeMsb = val;
            }
            else if (curAddr == 0x41)
            {
                variationParameters.typeLsb = val;
            }
            else if (curAddr >= 0x42 && curAddr <= 0x55)
            {
                const auto paramIdx = static_cast<size_t> ((curAddr - 0x42) / 2);
                const auto isLsb = ((curAddr - 0x42) % 2) != 0;
                if (paramIdx < 10)
                {
                    if (isLsb)
                        variationParameters.parameters14Bit[paramIdx] = static_cast<uint16_t> ((variationParameters.parameters14Bit[paramIdx] & 0x3f80) | (val & 0x7f));
                    else
                        variationParameters.parameters14Bit[paramIdx] = static_cast<uint16_t> ((variationParameters.parameters14Bit[paramIdx] & 0x007f) | ((val & 0x7f) << 7));
                }
            }
            else if (curAddr == 0x56)
            {
                variationParameters.varReturn = val;
            }
            else if (curAddr == 0x57)
            {
                variationParameters.varPan = val;
            }
            else if (curAddr == 0x58)
            {
                variationParameters.sendToReverb = val;
            }
            else if (curAddr == 0x59)
            {
                variationParameters.sendToChorus = val;
            }
            else if (curAddr == 0x5a)
            {
                variationParameters.connection = static_cast<uint8_t> (val & 0x01);
            }
            else if (curAddr == 0x5b)
            {
                variationParameters.part = val;
            }
            else if (curAddr == 0x5c)
            {
                variationParameters.mwControlDepth = val;
            }
            else if (curAddr == 0x5d)
            {
                variationParameters.bendControlDepth = val;
            }
            else if (curAddr == 0x5e)
            {
                variationParameters.catControlDepth = val;
            }
            else if (curAddr == 0x5f)
            {
                variationParameters.ac1ControlDepth = val;
            }
            else if (curAddr == 0x60)
            {
                variationParameters.ac2ControlDepth = val;
            }
            else if (curAddr >= 0x70 && curAddr <= 0x75)
            {
                variationParameters.parameters11To16[static_cast<size_t> (curAddr - 0x70)] = val;
            }
        }

        if (reverbChanged)
            updateReverbSettings();

        if (chorusChanged)
            updateChorusSettings();

        return;
    }

    // 5. Multi-EQ Parameter Change (02 40 aa)
    if (addrHigh == 0x02 && addrMid == 0x40)
    {
        for (int i = 0; i < numData; ++i)
        {
            const auto curAddr = addrLow + i;
            const auto val = dataPtr[i];

            switch (curAddr)
            {
                case 0x00: multiEqParameters.setPreset (val); break;
                case 0x01: multiEqParameters.gain1 = val; break;
                case 0x02: multiEqParameters.freq1 = val; break;
                case 0x03: multiEqParameters.q1 = val; break;
                case 0x04: multiEqParameters.shape1 = val; break;
                case 0x05: multiEqParameters.gain2 = val; break;
                case 0x06: multiEqParameters.freq2 = val; break;
                case 0x07: multiEqParameters.q2 = val; break;
                case 0x09: multiEqParameters.gain3 = val; break;
                case 0x0a: multiEqParameters.freq3 = val; break;
                case 0x0b: multiEqParameters.q3 = val; break;
                case 0x0d: multiEqParameters.gain4 = val; break;
                case 0x0e: multiEqParameters.freq4 = val; break;
                case 0x0f: multiEqParameters.q4 = val; break;
                case 0x11: multiEqParameters.gain5 = val; break;
                case 0x12: multiEqParameters.freq5 = val; break;
                case 0x13: multiEqParameters.q5 = val; break;
                case 0x14: multiEqParameters.shape5 = val; break;
                default: break;
            }
        }

        multiEqFiltersNeedUpdate = true;
        return;
    }
}

void FluidSynthEngine::applyChannelState() noexcept
{
    if (activeSynth == nullptr)
        return;

    const auto needsApply = channelStateNeedsApply;
    const auto baseGain = masterGain.load (std::memory_order_acquire);
    const auto volRatio = static_cast<float> (systemParameters.masterVolume) / 127.0f;
    const auto desiredMasterGain = baseGain * volRatio;
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

        if (needsApply)
            updateChannelTuning (channel);
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
            const auto& p = partParameters[index];

            if (noteNumber < p.noteLimitLow || noteNumber > p.noteLimitHigh)
                return;

            if (isDrum && juce::isPositiveAndBelow (noteNumber, 128))
            {
                const auto& setup = (partMode == xg::PartMode::Drums2 || partMode == xg::PartMode::Drums4)
                                        ? drumSetup2 : drumSetup1;
                const auto& noteParams = setup.notes[static_cast<size_t> (noteNumber)];

                if (noteParams.modified)
                {
                    if (noteParams.rcvNoteOn == 0 || noteParams.level == 0)
                        return;

                    if (noteParams.alternateGroup > 0 && noteParams.alternateGroup < 128)
                    {
                        const auto prevNote = activeGroupNote[index][static_cast<size_t> (noteParams.alternateGroup)];
                        if (prevNote >= 0 && prevNote != noteNumber)
                            fluid_synth_noteoff (activeSynth->synth, channel, prevNote);
                        activeGroupNote[index][static_cast<size_t> (noteParams.alternateGroup)] = noteNumber;
                    }

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

                fluid_synth_noteon (activeSynth->synth, channel, noteNumber, rawVelocity);
                return;
            }

            const auto transpose = systemParameters.transpose - 64;
            const auto shift = p.noteShift - 64;
            const auto effectiveNote = juce::jlimit (0, 127, noteNumber + transpose + shift);
            if (juce::isPositiveAndBelow (noteNumber, 128))
                activeNoteTransposition[index][static_cast<size_t> (noteNumber)] = static_cast<uint8_t> (effectiveNote);

            if (p.monoPolyMode == 0)
                fluid_synth_all_notes_off (activeSynth->synth, channel);

            fluid_synth_noteon (activeSynth->synth, channel, effectiveNote, rawVelocity);
        }
    }
    else if (message.isNoteOff())
    {
        if (activeSynth != nullptr)
        {
            const auto noteNumber = message.getNoteNumber();
            const auto index = static_cast<size_t> (channel);
            const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
            const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
            const auto partMode = static_cast<xg::PartMode> (partModeRaw);
            const auto isDrum = xg::isDrumMode (partMode) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit;

            if (isDrum)
            {
                const auto& setup = (partMode == xg::PartMode::Drums2 || partMode == xg::PartMode::Drums4)
                                        ? drumSetup2 : drumSetup1;
                if (juce::isPositiveAndBelow (noteNumber, 128))
                {
                    const auto& noteParams = setup.notes[static_cast<size_t> (noteNumber)];
                    if (noteParams.modified && noteParams.rcvNoteOff == 0)
                        return;

                    const auto group = noteParams.alternateGroup;
                    if (group > 0 && group < 128 && activeGroupNote[index][static_cast<size_t> (group)] == noteNumber)
                        activeGroupNote[index][static_cast<size_t> (group)] = -1;
                }
                fluid_synth_noteoff (activeSynth->synth, channel, noteNumber);
            }
            else
            {
                int effectiveNote = noteNumber;
                if (juce::isPositiveAndBelow (noteNumber, 128))
                    effectiveNote = activeNoteTransposition[index][static_cast<size_t> (noteNumber)];
                fluid_synth_noteoff (activeSynth->synth, channel, effectiveNote);
            }
        }
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
    }
    else
    {
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

    if (! multiEqParameters.isFlat())
    {
        if (multiEqFiltersNeedUpdate)
        {
            updateMultiEqCoefficients();
            multiEqFiltersNeedUpdate = false;
        }

        const auto numChans = juce::jmin (2, buffer.getNumChannels());
        for (int ch = 0; ch < numChans; ++ch)
        {
            auto* channelData = buffer.getWritePointer (ch);
            for (auto& f : multiEqFilters[static_cast<size_t> (ch)])
                f.processSamples (channelData, numSamples);
        }
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
