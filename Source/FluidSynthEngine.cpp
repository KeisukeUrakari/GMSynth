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
        channelGsPartMode[index].store (static_cast<uint8_t> (isDrum ? gs::PartMode::Drum1 : gs::PartMode::Normal), std::memory_order_relaxed);
        drumPartProtectMode[index].store (isDrum, std::memory_order_relaxed);
        channelProgram[index].store (0, std::memory_order_relaxed);
        channelModulation[index].store (0, std::memory_order_relaxed);
    }

    systemParameters.reset();
    for (auto& nrpn : nrpnStates)
        nrpn.reset();
    for (auto& part : partParameters)
        part.reset();
    drumSetup1.reset();
    drumSetup2.reset();

    gsSystemParameters.reset();
    for (int channel = 0; channel < numMidiChannels; ++channel)
        gsPartParameters[static_cast<size_t> (channel)].reset (channel);
    gsDrumSetup1.reset();
    gsDrumSetup2.reset();
    gsReverbParameters.reset();
    gsChorusParameters.reset();
    gsDelayParameters.reset();

    for (auto& chanMap : activeNoteTransposition)
        for (size_t n = 0; n < 128; ++n)
            chanMap[n] = static_cast<uint8_t> (n);

    for (auto& chanGroups : activeGroupNote)
        chanGroups.fill (-1);

    reverbParameters.reset();
    chorusParameters.reset();
    variationParameters.reset();
    variationProcessor.reset();
    variationProcessor.updateParameters (variationParameters);
    multiEqParameters.reset();
    multiEqFiltersNeedUpdate = true;
    for (auto& chan : multiEqFilters)
        for (auto& f : chan)
            f.reset();

    for (auto& eq : partEqs)
        eq.reset();

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
    const auto safeSr = juce::jmax (1.0, sampleRate);
    const auto safeBlock = juce::jmax (1, samplesPerBlock);

    currentSampleRate.store (safeSr, std::memory_order_release);
    scratchBuffer.setSize (2, safeBlock, false, true, true);

    multiPartBuffer.setSize (totalSynthChannels * 2, safeBlock, false, true, true);
    for (auto& slot : auxDrumSlots)
        slot.reset();
    auxChannelBank.fill (-1);
    auxChannelProgram.fill (-1);

    reverbBusBuffer.setSize (2, safeBlock, false, true, true);
    chorusBusBuffer.setSize (2, safeBlock, false, true, true);
    variationInputBuffer.setSize (2, safeBlock, false, true, true);
    variationOutputBuffer.setSize (2, safeBlock, false, true, true);

    juce::dsp::ProcessSpec spec { safeSr, static_cast<juce::uint32> (safeBlock), 2 };
    reverbProcessor.prepare (spec);
    chorusProcessor.prepare (spec);
    variationProcessor.prepare (safeSr, safeBlock);

    updateReverbSettings();
    updateChorusSettings();
    variationProcessor.updateParameters (variationParameters);

    multiEqFiltersNeedUpdate = true;
    for (auto& chan : multiEqFilters)
        for (auto& f : chan)
            f.reset();

    updateAllPartEqCoefficients();
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

    fluid_settings_setint (instance->settings, "synth.audio-channels", totalSynthChannels);
    fluid_settings_setint (instance->settings, "synth.midi-channels", totalSynthChannels);
    fluid_settings_setint (instance->settings, "synth.effects-channels", 2);

    instance->synth = new_fluid_synth (instance->settings);
    if (instance->synth == nullptr)
    {
        errorMessage = "FluidSynth could not create a synthesizer.";
        return {};
    }

    fluid_synth_set_reverb_on (instance->synth, 0);
    fluid_synth_set_chorus_on (instance->synth, 0);

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
                                                   int bankMsb,
                                                   int bankLsb,
                                                   ActiveMode activeMode,
                                                   bool isPercussionChannel) noexcept
{
    if (instance.synth == nullptr
        || ! juce::isPositiveAndBelow (channel, static_cast<int> (totalSynthChannels))
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

        // 3. If in GS mode and map specified, check map-offset
        if (chosen == nullptr && activeMode == ActiveMode::GS && bankLsb > 0)
            chosen = findPresetInBank (percussionPresetsForProgram, 128 + bankLsb);

        // 4. Fallback to Standard Kit (Program 0) at bank 128
        if (chosen == nullptr)
        {
            const auto& stdKitPresets = instance.percussionPresetsByProgram[0];
            chosen = findPresetInBank (stdKitPresets, 128);
            if (chosen == nullptr && ! stdKitPresets.empty())
                chosen = &stdKitPresets.front();
        }

        // 5. If still not found, try any preset for this program in percussion banks
        if (chosen == nullptr && ! percussionPresetsForProgram.empty())
            chosen = &percussionPresetsForProgram.front();

        // 6. Global lowest percussion preset
        if (chosen == nullptr && instance.hasLowestPercussionPreset)
            chosen = &instance.lowestPercussionPreset;

        // 7. Any preset available
        if (chosen == nullptr && instance.hasLowestPreset)
            chosen = &instance.lowestPreset;
    }
    else
    {
        if (activeMode == ActiveMode::GS)
        {
            // GS Hierarchical Fallback Strategy:
            // 1. If map specified (e.g. SC-88 map), check map-offset or 14-bit bank first
            if (bankLsb > 0)
            {
                chosen = findPresetInBank (allPresetsForProgram, (bankLsb * 128) + bankMsb);
                if (chosen == nullptr)
                    chosen = findPresetInBank (allPresetsForProgram, (bankMsb << 7) | bankLsb);
            }

            // 2. SC-55 Map / Direct Variation Bank (Bank == CC#0)
            if (chosen == nullptr && bankMsb > 0)
                chosen = findPresetInBank (allPresetsForProgram, bankMsb);

            // 3. Capital Tone: Bank 0
            if (chosen == nullptr)
                chosen = findPresetInBank (allPresetsForProgram, 0);

            // 4. Any available bank for this program
            if (chosen == nullptr && ! allPresetsForProgram.empty())
                chosen = &allPresetsForProgram.front();

            // 5. Global lowest preset
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
    fluid_synth_set_reverb_on (instance.synth, 0);
    fluid_synth_set_chorus_on (instance.synth, 0);

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
        const auto bankLsb = juce::jlimit (0, 127, channelBankLsb[index].load (std::memory_order_acquire));
        const auto activeMode = getActiveMode();
        const auto gsMode = (activeMode == ActiveMode::GS);
        const auto gsPart = static_cast<gs::PartMode> (channelGsPartMode[index].load (std::memory_order_acquire));
        const auto partMode = static_cast<xg::PartMode> (channelPartMode[index].load (std::memory_order_acquire));
        const auto isPercussion = gsMode ? gs::isDrumMode (gsPart)
                                         : (xg::isDrumMode (partMode) || bankMsb == xg::bankMsbDrumKit || bankMsb == xg::bankMsbSfxKit);
        const auto program = juce::jlimit (0, 127, channelProgram[index].load (std::memory_order_acquire));

        fluid_synth_set_channel_type (instance.synth, channel, isPercussion ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
        fluid_synth_cc (instance.synth, channel, 7, muted ? 0 : volume);
        fluid_synth_cc (instance.synth, channel, 10, pan);
        fluid_synth_bank_select (instance.synth, channel, bank);

        applyProgramChangeToSynth (instance, channel, program, bank, bankMsb, bankLsb, activeMode, isPercussion);

        if (gsMode)
        {
            fluid_synth_set_gen (instance.synth, channel, GEN_FINETUNE, gsSystemParameters.masterTuneCents + gsPartParameters[index].pitchOffsetFineCents);
        }
        else
        {
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
    const auto gsPartModeRaw = channelGsPartMode[index].load (std::memory_order_acquire);
    const auto gsPart = static_cast<gs::PartMode> (gsPartModeRaw);

    state.volume = channelVolume[index].load (std::memory_order_acquire);
    state.pan = channelPan[index].load (std::memory_order_acquire);
    state.bank = channelBank[index].load (std::memory_order_acquire);
    state.bankMsb = msb;
    state.bankLsb = lsb;
    state.program = channelProgram[index].load (std::memory_order_acquire);
    state.partMode = partMode;
    state.gsPartMode = gsPart;

    if (getActiveMode() == ActiveMode::GS)
        state.isDrum = gs::isDrumMode (gsPart);
    else
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

FluidSynthEngine::EngineMode FluidSynthEngine::getEngineMode() const noexcept
{
    return configuredEngineMode.load (std::memory_order_acquire);
}

void FluidSynthEngine::setEngineMode (EngineMode mode) noexcept
{
    configuredEngineMode.store (mode, std::memory_order_release);
    if (mode == EngineMode::GM)
    {
        currentActiveMode.store (ActiveMode::GM, std::memory_order_release);
        isXgModeActive.store (false, std::memory_order_release);
    }
    else if (mode == EngineMode::GS)
    {
        currentActiveMode.store (ActiveMode::GS, std::memory_order_release);
        isXgModeActive.store (false, std::memory_order_release);
    }
    else if (mode == EngineMode::XG)
    {
        currentActiveMode.store (ActiveMode::XG, std::memory_order_release);
        isXgModeActive.store (true, std::memory_order_release);
    }
}

FluidSynthEngine::ActiveMode FluidSynthEngine::getActiveMode() const noexcept
{
    const auto conf = configuredEngineMode.load (std::memory_order_acquire);
    if (conf != EngineMode::Auto)
        return (conf == EngineMode::GS) ? ActiveMode::GS : ((conf == EngineMode::XG) ? ActiveMode::XG : ActiveMode::GM);

    return currentActiveMode.load (std::memory_order_acquire);
}

bool FluidSynthEngine::isXgMode() const noexcept
{
    return getActiveMode() == ActiveMode::XG;
}

void FluidSynthEngine::setXgMode (bool enabled) noexcept
{
    setEngineMode (enabled ? EngineMode::XG : EngineMode::GM);
}

bool FluidSynthEngine::isGsMode() const noexcept
{
    return getActiveMode() == ActiveMode::GS;
}

const gs::PartParameters& FluidSynthEngine::getGsPartParameters (int channel) const noexcept
{
    static const gs::PartParameters fallbackPart;
    if (juce::isPositiveAndBelow (channel, numMidiChannels))
        return gsPartParameters[static_cast<size_t> (channel)];
    return fallbackPart;
}

const gs::DrumSetup& FluidSynthEngine::getGsDrumSetup (int setupIndex) const noexcept
{
    return (setupIndex == 2) ? gsDrumSetup2 : gsDrumSetup1;
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
        const auto masterVol = (getActiveMode() == ActiveMode::GS) ? gsSystemParameters.masterVolume : systemParameters.masterVolume;
        const auto volRatio = static_cast<float> (masterVol) / 127.0f;
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
    float totalCents = 0.0f;
    if (getActiveMode() == ActiveMode::GS)
    {
        totalCents = gsSystemParameters.masterTuneCents + gsPartParameters[index].pitchOffsetFineCents;
    }
    else
    {
        totalCents = systemParameters.masterTuneCents + partParameters[index].detuneCents;
    }
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
    if (reverbParameters.typeMsb == 0x00)
    {
        juce::dsp::Reverb::Parameters p;
        p.wetLevel = 0.0f;
        p.dryLevel = 0.0f;
        p.roomSize = 0.0f;
        reverbProcessor.setParameters (p);
        return;
    }

    float baseRoom = 0.6f;
    float baseDamp = 0.4f;
    float baseWidth = 1.0f;

    switch (reverbParameters.typeMsb)
    {
        case 0x01: // Hall 1, Hall 2
            baseRoom = (reverbParameters.typeLsb == 1) ? 0.85f : 0.75f;
            baseDamp = 0.35f;
            baseWidth = 1.0f;
            break;

        case 0x02: // Room 1, Room 2, Room 3
            if (reverbParameters.typeLsb == 1)      baseRoom = 0.35f; // Room 2
            else if (reverbParameters.typeLsb >= 2) baseRoom = 0.45f; // Room 3
            else                                    baseRoom = 0.30f; // Room 1
            baseDamp = 0.55f;
            baseWidth = 0.85f;
            break;

        case 0x03: // Stage 1, Stage 2
            baseRoom = (reverbParameters.typeLsb == 1) ? 0.65f : 0.55f;
            baseDamp = 0.30f;
            baseWidth = 0.95f;
            break;

        case 0x04: // Plate
            baseRoom = 0.50f;
            baseDamp = 0.15f; // bright plate
            baseWidth = 1.0f;
            break;

        case 0x10: // White Room (dead room)
            baseRoom = 0.25f;
            baseDamp = 0.80f;
            baseWidth = 0.70f;
            break;

        case 0x11: // Tunnel
            baseRoom = 0.88f;
            baseDamp = 0.25f;
            baseWidth = 0.90f;
            break;

        case 0x12: // Canyon
            baseRoom = 0.95f;
            baseDamp = 0.20f;
            baseWidth = 1.0f;
            break;

        case 0x13: // Basement
            baseRoom = 0.28f;
            baseDamp = 0.65f;
            baseWidth = 0.60f;
            break;

        default:
            baseRoom = 0.60f;
            baseDamp = 0.40f;
            baseWidth = 1.0f;
            break;
    }

    // Param 1: Reverb Time (0..127, 64 is default)
    const auto timeParam = reverbParameters.parameters[0] > 0 ? reverbParameters.parameters[0] : 64;
    const auto timeScale = static_cast<float> (timeParam) / 64.0f;
    const auto roomSize = juce::jlimit (0.05f, 1.0f, baseRoom * timeScale);

    // Param 5: LPF Cutoff Frequency (0..127) - controls high frequency absorption/damping
    const auto lpfParam = reverbParameters.parameters[4] > 0 ? reverbParameters.parameters[4] : 64;
    const auto lpfFactor = 1.0f - (static_cast<float> (lpfParam) / 127.0f);
    const auto damping = juce::jlimit (0.0f, 1.0f, baseDamp * 0.6f + lpfFactor * 0.4f);

    // Param 6: Width (0..127, or diffusion from param 2)
    float width = baseWidth;
    if (reverbParameters.parameters[5] > 0)
        width = juce::jlimit (0.0f, 1.0f, static_cast<float> (reverbParameters.parameters[5]) / 64.0f * baseWidth);
    else if (reverbParameters.parameters[1] > 0) // Diffusion
        width = juce::jlimit (0.2f, 1.0f, static_cast<float> (reverbParameters.parameters[1]) / 10.0f * baseWidth);

    juce::dsp::Reverb::Parameters params;
    params.roomSize = roomSize;
    params.damping = damping;
    params.wetLevel = 1.0f;
    params.dryLevel = 0.0f;
    params.width = width;
    params.freezeMode = 0.0f;

    reverbProcessor.setParameters (params);
}

void FluidSynthEngine::updateGsReverbSettings() noexcept
{
    float baseRoom = 0.6f;
    float baseDamp = 0.4f;
    switch (gsReverbParameters.macro)
    {
        case 0: baseRoom = 0.25f; baseDamp = 0.6f; break; // Room 1
        case 1: baseRoom = 0.35f; baseDamp = 0.5f; break; // Room 2
        case 2: baseRoom = 0.45f; baseDamp = 0.5f; break; // Room 3
        case 3: baseRoom = 0.65f; baseDamp = 0.4f; break; // Hall 1
        case 4: baseRoom = 0.80f; baseDamp = 0.3f; break; // Hall 2
        case 5: baseRoom = 0.50f; baseDamp = 0.1f; break; // Plate
        case 6: baseRoom = 0.70f; baseDamp = 0.2f; break; // Delay
        case 7: baseRoom = 0.70f; baseDamp = 0.2f; break; // Pan Delay
        default: break;
    }

    const auto timeFactor = static_cast<float> (juce::jlimit (0, 127, static_cast<int> (gsReverbParameters.time))) / 64.0f;
    const auto roomsize = juce::jlimit (0.0f, 1.0f, baseRoom * timeFactor);
    const auto damping = juce::jlimit (0.0f, 1.0f, baseDamp + (static_cast<float> (gsReverbParameters.character) / 14.0f));

    juce::dsp::Reverb::Parameters params;
    params.roomSize = roomsize;
    params.damping = damping;
    params.wetLevel = 1.0f;
    params.dryLevel = 0.0f;
    params.width = 1.0f;
    params.freezeMode = 0.0f;

    reverbProcessor.setParameters (params);
}

void FluidSynthEngine::updateGsChorusSettings() noexcept
{
    float rateHz = 1.0f;
    float depthNorm = 0.25f;
    float feedback = 0.0f;
    float delayMs = 7.0f;

    switch (gsChorusParameters.macro)
    {
        case 0: rateHz = 0.8f; depthNorm = 0.2f; delayMs = 5.0f; break; // Chorus 1
        case 1: rateHz = 0.9f; depthNorm = 0.25f; delayMs = 6.0f; break; // Chorus 2
        case 2: rateHz = 1.0f; depthNorm = 0.3f; delayMs = 7.0f; break; // Chorus 3
        case 3: rateHz = 1.2f; depthNorm = 0.4f; delayMs = 8.0f; break; // Chorus 4
        case 4: rateHz = 0.9f; depthNorm = 0.3f; feedback = 0.4f; delayMs = 7.0f; break; // FB Chorus
        case 5: rateHz = 0.5f; depthNorm = 0.5f; feedback = 0.7f; delayMs = 2.0f; break; // Flanger
        case 6: rateHz = 0.2f; depthNorm = 0.1f; delayMs = 15.0f; break; // Short Delay
        case 7: rateHz = 0.2f; depthNorm = 0.1f; feedback = 0.4f; delayMs = 15.0f; break; // Short Delay FB
        default: break;
    }

    if (gsChorusParameters.rate > 0)
    {
        const auto speedNorm = static_cast<float> (gsChorusParameters.rate) / 127.0f;
        rateHz = juce::jlimit (0.2f, 8.0f, 0.4f + speedNorm * 5.0f);
    }
    if (gsChorusParameters.depth > 0)
    {
        depthNorm = juce::jlimit (0.0f, 1.0f, static_cast<float> (gsChorusParameters.depth) / 127.0f);
    }
    if (gsChorusParameters.feedback > 0)
    {
        feedback = juce::jlimit (-0.85f, 0.85f, static_cast<float> (gsChorusParameters.feedback) / 127.0f * 0.8f);
    }

    chorusProcessor.setRate (rateHz);
    chorusProcessor.setDepth (depthNorm);
    chorusProcessor.setCentreDelay (delayMs);
    chorusProcessor.setFeedback (feedback);
    chorusProcessor.setMix (gsChorusParameters.level > 0 ? 1.0f : 0.0f);
}

void FluidSynthEngine::updateChorusSettings() noexcept
{
    if (chorusParameters.typeMsb == 0x00)
    {
        chorusProcessor.setMix (0.0f);
        return;
    }

    float baseRateHz = 1.0f;
    float baseDepth = 0.25f;
    float baseDelayMs = 7.0f;
    float baseFeedback = 0.0f;

    switch (chorusParameters.typeMsb)
    {
        case 0x41: // Chorus 1..4
            if (chorusParameters.typeLsb == 1)      { baseRateHz = 0.9f; baseDepth = 0.25f; baseDelayMs = 8.0f; }
            else if (chorusParameters.typeLsb == 2) { baseRateHz = 1.1f; baseDepth = 0.32f; baseDelayMs = 9.0f; }
            else if (chorusParameters.typeLsb >= 3) { baseRateHz = 1.3f; baseDepth = 0.40f; baseDelayMs = 10.0f; }
            else                                    { baseRateHz = 0.8f; baseDepth = 0.20f; baseDelayMs = 7.0f; }
            baseFeedback = 0.05f;
            break;

        case 0x42: // Celeste 1..4 (faster rate, lighter depth, spatial swirl)
            if (chorusParameters.typeLsb == 1)      { baseRateHz = 1.8f; baseDepth = 0.18f; baseDelayMs = 4.5f; }
            else if (chorusParameters.typeLsb >= 2) { baseRateHz = 2.2f; baseDepth = 0.22f; baseDelayMs = 5.0f; }
            else                                    { baseRateHz = 1.5f; baseDepth = 0.15f; baseDelayMs = 4.0f; }
            baseFeedback = 0.0f;
            break;

        case 0x43: // Flanger 1..3 (shorter delay, high feedback)
            if (chorusParameters.typeLsb == 1)      { baseRateHz = 0.3f; baseDepth = 0.50f; baseDelayMs = 2.0f; baseFeedback = 0.70f; }
            else if (chorusParameters.typeLsb >= 2) { baseRateHz = 0.4f; baseDepth = 0.60f; baseDelayMs = 2.5f; baseFeedback = 0.75f; }
            else                                    { baseRateHz = 0.2f; baseDepth = 0.45f; baseDelayMs = 1.8f; baseFeedback = 0.65f; }
            break;

        case 0x44: // Symphonic 1..2 (rich ensemble modulation)
            baseRateHz = (chorusParameters.typeLsb >= 1) ? 0.8f : 0.65f;
            baseDepth = (chorusParameters.typeLsb >= 1) ? 0.45f : 0.38f;
            baseDelayMs = 14.0f;
            baseFeedback = 0.0f;
            break;

        default:
            baseRateHz = 1.0f;
            baseDepth = 0.25f;
            baseDelayMs = 7.0f;
            baseFeedback = 0.0f;
            break;
    }

    // Param 1: LFO Frequency (0..127 -> 0.05..15.0 Hz)
    float rateHz = baseRateHz;
    if (chorusParameters.parameters[0] > 0)
    {
        const auto speedNorm = static_cast<float> (chorusParameters.parameters[0]) / 127.0f;
        rateHz = juce::jlimit (0.05f, 15.0f, 0.1f + speedNorm * speedNorm * 6.0f * (baseRateHz / 1.0f));
    }

    // Param 2: LFO Depth (0..127)
    float depthNorm = baseDepth;
    if (chorusParameters.parameters[1] > 0)
    {
        depthNorm = juce::jlimit (0.0f, 1.0f, static_cast<float> (chorusParameters.parameters[1]) / 127.0f);
    }

    // Param 3: Feedback (1..127 -> -63..+63, 64 = 0)
    float feedback = baseFeedback;
    if (chorusParameters.parameters[2] > 0)
    {
        const auto fbVal = static_cast<int> (chorusParameters.parameters[2]);
        feedback = juce::jlimit (-0.85f, 0.85f, static_cast<float> (fbVal - 64) / 64.0f * 0.8f);
    }

    // Param 4: Delay Offset (0..127 -> 0.5..45.0 ms)
    float delayMs = baseDelayMs;
    if (chorusParameters.parameters[3] > 0)
    {
        delayMs = juce::jlimit (0.5f, 45.0f, static_cast<float> (chorusParameters.parameters[3]) / 127.0f * 35.0f + 0.5f);
    }

    chorusProcessor.setRate (rateHz);
    chorusProcessor.setDepth (depthNorm);
    chorusProcessor.setCentreDelay (delayMs);
    chorusProcessor.setFeedback (feedback);
    chorusProcessor.setMix (1.0f);
}

void FluidSynthEngine::updatePartEqCoefficients (int channel) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr <= 1.0)
        return;

    const auto nyquist = sr * 0.49;
    const auto index = static_cast<size_t> (channel);
    auto& eq = partEqs[index];
    const auto& p = partParameters[index];

    // Bass
    const auto bassGainDb = static_cast<float> (p.eqBass - 64);
    if (std::abs (bassGainDb) < 0.1f)
    {
        eq.bassActive = false;
        eq.bassFilters[0].makeInactive();
        eq.bassFilters[1].makeInactive();
    }
    else
    {
        const auto bassGainFactor = juce::Decibels::decibelsToGain (bassGainDb);
        const auto bassFreq = juce::jlimit (20.0, nyquist, static_cast<double> (xg::lookupEqFrequency (p.eqBassFreq)));
        const auto bassCoeff = juce::IIRCoefficients::makeLowShelf (sr, bassFreq, 0.707, bassGainFactor);
        eq.bassFilters[0].setCoefficients (bassCoeff);
        eq.bassFilters[1].setCoefficients (bassCoeff);
        eq.bassActive = true;
    }

    // Treble
    const auto trebleGainDb = static_cast<float> (p.eqTreble - 64);
    if (std::abs (trebleGainDb) < 0.1f)
    {
        eq.trebleActive = false;
        eq.trebleFilters[0].makeInactive();
        eq.trebleFilters[1].makeInactive();
    }
    else
    {
        const auto trebleGainFactor = juce::Decibels::decibelsToGain (trebleGainDb);
        const auto trebleFreq = juce::jlimit (20.0, nyquist, static_cast<double> (xg::lookupEqFrequency (p.eqTrebleFreq)));
        const auto trebleCoeff = juce::IIRCoefficients::makeHighShelf (sr, trebleFreq, 0.707, trebleGainFactor);
        eq.trebleFilters[0].setCoefficients (trebleCoeff);
        eq.trebleFilters[1].setCoefficients (trebleCoeff);
        eq.trebleActive = true;
    }
}

void FluidSynthEngine::updateAllPartEqCoefficients() noexcept
{
    for (int ch = 0; ch < numMidiChannels; ++ch)
        updatePartEqCoefficients (ch);
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
    const auto activeMode = getActiveMode();
    const auto xgActive = (activeMode == ActiveMode::XG);
    const auto gsActive = (activeMode == ActiveMode::GS);
    const auto defaultVol = (xgActive || gsActive) ? 100 : 127;

    channelVolume[index].store (defaultVol, std::memory_order_release);
    channelPan[index].store (xg::defaultPan, std::memory_order_release);

    const auto isDrum = (channel == 9);
    const auto msb = isDrum ? xg::bankMsbDrumKit : xg::bankMsbNormal;
    const auto lsb = 0;
    channelBankMsb[index].store (msb, std::memory_order_release);
    channelBankLsb[index].store (lsb, std::memory_order_release);
    channelBank[index].store ((msb << 7) | lsb, std::memory_order_release);
    channelPartMode[index].store (static_cast<uint8_t> (isDrum ? xg::PartMode::Drum : xg::PartMode::Normal), std::memory_order_release);
    channelGsPartMode[index].store (static_cast<uint8_t> (isDrum ? gs::PartMode::Drum1 : gs::PartMode::Normal), std::memory_order_release);
    drumPartProtectMode[index].store (isDrum, std::memory_order_release);
    channelProgram[index].store (0, std::memory_order_release);
    channelModulation[index].store (0, std::memory_order_release);

    partParameters[index].reset();
    gsPartParameters[index].reset (channel);
    partEqs[index].reset();
    updatePartEqCoefficients (channel);
    nrpnStates[index].reset();
    for (size_t n = 0; n < 128; ++n)
        activeNoteTransposition[index][n] = static_cast<uint8_t> (n);
    activeGroupNote[index].fill (-1);
    resetAllGenerators (channel);
    resetChannelControllers (channel);
    updateChannelModulation (channel);

    if (activeSynth != nullptr)
    {
        fluid_synth_cc (activeSynth->synth, channel, 121, 0); // Reset All Controllers
        fluid_synth_cc (activeSynth->synth, channel, 1, 0);   // Modulation Wheel = 0
        fluid_synth_cc (activeSynth->synth, channel, 64, 0);  // Sustain = 0
        fluid_synth_cc (activeSynth->synth, channel, 11, 127);// Expression = 127
        fluid_synth_cc (activeSynth->synth, channel, 91, gsActive ? gs::defaultReverbSend : xg::defaultReverbSend);
        fluid_synth_cc (activeSynth->synth, channel, 93, gsActive ? gs::defaultChorusSend : xg::defaultChorusSend);
        fluid_synth_pitch_bend (activeSynth->synth, channel, 8192);

        if (xgActive)
        {
            // Mute CC#1 pitch modulation range in XG mode so that Modulation Matrix exclusively controls vibrato
            fluid_synth_cc (activeSynth->synth, channel, 101, 0);
            fluid_synth_cc (activeSynth->synth, channel, 100, 5);
            fluid_synth_cc (activeSynth->synth, channel, 6, 0);
            fluid_synth_cc (activeSynth->synth, channel, 38, 0);
            fluid_synth_cc (activeSynth->synth, channel, 101, 127);
            fluid_synth_cc (activeSynth->synth, channel, 100, 127);

            // Set Pitch Bend Sensitivity to default 2 semitones
            fluid_synth_cc (activeSynth->synth, channel, 101, 0);
            fluid_synth_cc (activeSynth->synth, channel, 100, 0);
            fluid_synth_cc (activeSynth->synth, channel, 6, 2);
            fluid_synth_cc (activeSynth->synth, channel, 101, 127);
            fluid_synth_cc (activeSynth->synth, channel, 100, 127);
        }
    }
}

void FluidSynthEngine::resetGsState() noexcept
{
    gsSystemParameters.reset();
    for (int channel = 0; channel < numMidiChannels; ++channel)
        gsPartParameters[static_cast<size_t> (channel)].reset (channel);
    gsDrumSetup1.reset();
    gsDrumSetup2.reset();
    gsReverbParameters.reset();
    gsChorusParameters.reset();
    gsDelayParameters.reset();

    for (int channel = 0; channel < numMidiChannels; ++channel)
        resetChannelState (channel);

    if (activeSynth != nullptr)
    {
        for (int channel = 0; channel < numMidiChannels; ++channel)
        {
            const auto partMode = static_cast<gs::PartMode> (channelGsPartMode[static_cast<size_t> (channel)].load (std::memory_order_acquire));
            fluid_synth_set_channel_type (activeSynth->synth, channel, gs::isDrumMode (partMode) ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
        }
    }

    channelStateNeedsApply = true;
    updateMasterVolume();
    updateAllChannelTunings();
    updateGsReverbSettings();
    updateGsChorusSettings();
}

void FluidSynthEngine::handleGsSysEx (const juce::uint8* data, int numBytes) noexcept
{
    if (data == nullptr || numBytes < 8)
        return;

    // Checksum verification
    int sum = 0;
    for (int i = 4; i < numBytes - 1; ++i)
        sum += data[i];
    const auto expectedChecksum = static_cast<uint8_t> ((128 - (sum % 128)) & 0x7F);
    if (data[numBytes - 1] != expectedChecksum)
        return;

    const auto addrHigh = data[4];
    const auto addrMid  = data[5];
    const auto addrLow  = data[6];
    const auto numData  = numBytes - 8;
    const auto* dataPtr = data + 7;

    if (numData <= 0)
        return;

    // 1. System Parameters (40 00 aa or 00 00 7F)
    if ((addrHigh == 0x40 || addrHigh == 0x00) && addrMid == 0x00)
    {
        if (addrLow == 0x7F && numData >= 1 && dataPtr[0] == 0x00)
        {
            // GS Reset (Standard 40 00 7F 00 or System Mode Set 00 00 7F 00)
            if (configuredEngineMode.load (std::memory_order_acquire) == EngineMode::Auto)
            {
                currentActiveMode.store (ActiveMode::GS, std::memory_order_release);
                isXgModeActive.store (false, std::memory_order_release);
            }
            resetGsState();
            return;
        }

        for (int i = 0; i < numData; ++i)
        {
            const auto curAddr = addrLow + i;
            const auto val = dataPtr[i];
            if (curAddr >= 0x00 && curAddr <= 0x03)
            {
                gsSystemParameters.masterTuneRaw[static_cast<size_t> (curAddr)] = val & 0x0F;
                gsSystemParameters.updateMasterTuneFromRaw();
            }
            else if (curAddr == 0x04)
            {
                gsSystemParameters.masterVolume = val & 0x7F;
            }
            else if (curAddr == 0x05)
            {
                gsSystemParameters.masterKeyShift = val & 0x7F;
            }
            else if (curAddr == 0x06)
            {
                gsSystemParameters.masterPan = val & 0x7F;
            }
        }

        updateMasterVolume();
        updateAllChannelTunings();
        return;
    }

    // 2. Reverb Parameters (40 01 30..35)
    if (addrHigh == 0x40 && addrMid == 0x01 && addrLow >= 0x30 && addrLow <= 0x35)
    {
        for (int i = 0; i < numData; ++i)
        {
            const auto curAddr = addrLow + i;
            const auto val = dataPtr[i];
            switch (curAddr)
            {
                case 0x30: gsReverbParameters.macro = val & 0x07; break;
                case 0x31: gsReverbParameters.character = val & 0x07; break;
                case 0x32: gsReverbParameters.preLpf = val & 0x07; break;
                case 0x33: gsReverbParameters.level = val & 0x7F; break;
                case 0x34: gsReverbParameters.time = val & 0x7F; break;
                case 0x35: gsReverbParameters.delayFeedback = val & 0x7F; break;
                default: break;
            }
        }
        updateGsReverbSettings();
        return;
    }

    // 3. Chorus Parameters (40 01 38..40)
    if (addrHigh == 0x40 && addrMid == 0x01 && addrLow >= 0x38 && addrLow <= 0x40)
    {
        for (int i = 0; i < numData; ++i)
        {
            const auto curAddr = addrLow + i;
            const auto val = dataPtr[i];
            switch (curAddr)
            {
                case 0x38: gsChorusParameters.macro = val & 0x07; break;
                case 0x39: gsChorusParameters.preLpf = val & 0x07; break;
                case 0x3A: gsChorusParameters.level = val & 0x7F; break;
                case 0x3B: gsChorusParameters.feedback = val & 0x7F; break;
                case 0x3C: gsChorusParameters.delay = val & 0x7F; break;
                case 0x3D: gsChorusParameters.rate = val & 0x7F; break;
                case 0x3E: gsChorusParameters.depth = val & 0x7F; break;
                case 0x3F: gsChorusParameters.sendToReverb = val & 0x7F; break;
                case 0x40: gsChorusParameters.sendToDelay = val & 0x7F; break;
                default: break;
            }
        }
        updateGsChorusSettings();
        return;
    }

    // 4. Delay Parameters (40 01 50..59)
    if (addrHigh == 0x40 && addrMid == 0x01 && addrLow >= 0x50 && addrLow <= 0x59)
    {
        for (int i = 0; i < numData; ++i)
        {
            const auto curAddr = addrLow + i;
            const auto val = dataPtr[i];
            switch (curAddr)
            {
                case 0x50: gsDelayParameters.macro = val & 0x0F; break;
                case 0x51: gsDelayParameters.preLpf = val & 0x07; break;
                case 0x52: gsDelayParameters.timeCenter = val & 0x7F; break;
                case 0x53: gsDelayParameters.feedback = val & 0x7F; break;
                case 0x58: gsDelayParameters.level = val & 0x7F; break;
                case 0x59: gsDelayParameters.sendToReverb = val & 0x7F; break;
                default: break;
            }
        }
        return;
    }

    // 5. Part Parameters (40 1x aa)
    if (addrHigh == 0x40 && addrMid >= 0x10 && addrMid <= 0x1F)
    {
        const auto channel = gs::blockToChannel (addrMid);
        if (channel >= 0 && channel < numMidiChannels)
        {
            const auto index = static_cast<size_t> (channel);
            auto& part = gsPartParameters[index];

            for (int i = 0; i < numData; ++i)
            {
                const auto curAddr = addrLow + i;
                const auto val = dataPtr[i];

                switch (curAddr)
                {
                    case 0x00:
                        part.variationBank = val & 0x7F;
                        channelBankMsb[index].store (val & 0x7F, std::memory_order_release);
                        break;
                    case 0x01:
                        part.toneNumber = val & 0x7F;
                        channelProgram[index].store (val & 0x7F, std::memory_order_release);
                        if (activeSynth != nullptr)
                            handleProgramChange (channel, val & 0x7F);
                        break;
                    case 0x15:
                    {
                        const auto pMode = (val == 1) ? gs::PartMode::Drum1 : ((val == 2) ? gs::PartMode::Drum2 : gs::PartMode::Normal);
                        part.partMode = pMode;
                        channelGsPartMode[index].store (static_cast<uint8_t> (pMode), std::memory_order_release);
                        if (activeSynth != nullptr)
                            fluid_synth_set_channel_type (activeSynth->synth, channel, gs::isDrumMode (pMode) ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
                        channelStateNeedsApply = true;
                        break;
                    }
                    case 0x16:
                        part.keyShift = val & 0x7F;
                        break;
                    case 0x17:
                        part.pitchOffsetFineRaw[0] = val & 0x7F;
                        part.updatePitchOffsetFine();
                        updateChannelTuning (channel);
                        break;
                    case 0x18:
                        part.pitchOffsetFineRaw[1] = val & 0x7F;
                        part.updatePitchOffsetFine();
                        updateChannelTuning (channel);
                        break;
                    case 0x19:
                        part.level = val & 0x7F;
                        channelVolume[index].store (val & 0x7F, std::memory_order_release);
                        if (activeSynth != nullptr)
                            fluid_synth_cc (activeSynth->synth, channel, 7, val & 0x7F);
                        break;
                    case 0x1C:
                        part.pan = val & 0x7F;
                        channelPan[index].store (val & 0x7F, std::memory_order_release);
                        if (activeSynth != nullptr)
                            fluid_synth_cc (activeSynth->synth, channel, 10, val & 0x7F);
                        break;
                    case 0x21:
                        part.chorusSend = val & 0x7F;
                        if (activeSynth != nullptr)
                            fluid_synth_cc (activeSynth->synth, channel, 93, val & 0x7F);
                        break;
                    case 0x22:
                        part.reverbSend = val & 0x7F;
                        if (activeSynth != nullptr)
                            fluid_synth_cc (activeSynth->synth, channel, 91, val & 0x7F);
                        break;
                    case 0x23:
                        part.delaySend = val & 0x7F;
                        break;
                    case 0x26:
                        part.toneMap = val & 0x7F;
                        channelBankLsb[index].store (val & 0x7F, std::memory_order_release);
                        break;
                    default:
                        if (curAddr >= 0x40 && curAddr <= 0x4B)
                        {
                            part.scaleTuning[static_cast<size_t> (curAddr - 0x40)] = static_cast<int8_t> (static_cast<int> (val) - 64);
                            updateChannelTuning (channel);
                        }
                        break;
                }
            }
        }
        return;
    }

    // 6. Drum Setup (40 2x aa / 40 3x aa)
    if (addrHigh == 0x40 && (addrMid >= 0x20 && addrMid <= 0x38))
    {
        const auto mapIndex = (addrMid < 0x30) ? 1 : 2;
        auto& setup = (mapIndex == 1) ? gsDrumSetup1 : gsDrumSetup2;
        const auto paramType = addrMid & 0x0F;

        for (int i = 0; i < numData; ++i)
        {
            const auto noteNum = addrLow + i;
            if (noteNum >= 0 && noteNum < 128)
            {
                auto& note = setup.notes[static_cast<size_t> (noteNum)];
                note.modified = true;
                const auto val = dataPtr[i] & 0x7F;

                switch (paramType)
                {
                    case 0: note.pitch = val; break;
                    case 1: note.level = val; break;
                    case 2: note.alternateGroup = val; break;
                    case 3: note.pan = val; break;
                    case 4: note.reverbSend = val; break;
                    case 5: note.chorusSend = val; break;
                    case 6: note.delaySend = val; break;
                    case 7: note.rcvNoteOff = val; break;
                    case 8: note.rcvNoteOn = val; break;
                    default: break;
                }
            }
        }
        return;
    }
}

void FluidSynthEngine::handleSysEx (const juce::uint8* data, int numBytes) noexcept
{
    if (data == nullptr || numBytes <= 0)
        return;

    const auto isGmReset = numBytes >= 4
        && data[0] == 0x7e
        && data[2] == 0x09
        && (data[3] == 0x01 || data[3] == 0x03);

    const auto isGs = numBytes >= 8
        && data[0] == 0x41
        && data[2] == 0x42
        && data[3] == 0x12;

    const auto isXg = numBytes >= 7
        && data[0] == 0x43
        && (data[1] & 0xf0) == 0x10
        && data[2] == 0x4c;

    if (isGs)
    {
        handleGsSysEx (data, numBytes);
        return;
    }

    if (isGmReset)
    {
        if (configuredEngineMode.load (std::memory_order_acquire) == EngineMode::Auto)
        {
            currentActiveMode.store (ActiveMode::GM, std::memory_order_release);
            isXgModeActive.store (false, std::memory_order_release);
        }
        systemParameters.reset();
        gsSystemParameters.reset();
        drumSetup1.reset();
        drumSetup2.reset();
        gsDrumSetup1.reset();
        gsDrumSetup2.reset();
        reverbParameters.reset();
        chorusParameters.reset();
        gsReverbParameters.reset();
        gsChorusParameters.reset();
        gsDelayParameters.reset();
        variationParameters.reset();
        variationProcessor.reset();
        variationProcessor.updateParameters (variationParameters);
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
            if (configuredEngineMode.load (std::memory_order_acquire) == EngineMode::Auto)
            {
                currentActiveMode.store (ActiveMode::XG, std::memory_order_release);
                isXgModeActive.store (true, std::memory_order_release);
            }
            systemParameters.reset();
            drumSetup1.reset();
            drumSetup2.reset();
            reverbParameters.reset();
            chorusParameters.reset();
            variationParameters.reset();
            variationProcessor.reset();
            variationProcessor.updateParameters (variationParameters);
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
            variationProcessor.reset();
            variationProcessor.updateParameters (variationParameters);
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

                case 0x1d: // MW Pitch Control
                    p.ctrlMatrix.mw.pitch = juce::jlimit (0x28, 0x58, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x1e: // MW Filter Control
                    p.ctrlMatrix.mw.filter = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x1f: // MW Amp Control
                    p.ctrlMatrix.mw.amplitude = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x20: // MW LFO PMOD Depth
                    p.ctrlMatrix.mw.lfoPmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x21: // MW LFO FMOD Depth
                    p.ctrlMatrix.mw.lfoFmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x22: // MW LFO AMOD Depth
                    p.ctrlMatrix.mw.lfoAmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x23: // Bend Pitch Control
                {
                    p.ctrlMatrix.bend.pitch = juce::jlimit (0x28, 0x58, static_cast<int> (val));
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
                    updateChannelModulation (channel);
                    break;
                }

                case 0x24: // Bend Filter Control
                    p.ctrlMatrix.bend.filter = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x25: // Bend Amp Control
                    p.ctrlMatrix.bend.amplitude = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x26: // Bend LFO PMOD Depth
                    p.ctrlMatrix.bend.lfoPmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x27: // Bend LFO FMOD Depth
                    p.ctrlMatrix.bend.lfoFmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x28: // Bend LFO AMOD Depth
                    p.ctrlMatrix.bend.lfoAmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x4d: // CAT Pitch Control
                    p.ctrlMatrix.cat.pitch = juce::jlimit (0x28, 0x58, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x4e: // CAT Filter Control
                    p.ctrlMatrix.cat.filter = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x4f: // CAT Amp Control
                    p.ctrlMatrix.cat.amplitude = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x50: // CAT LFO PMOD Depth
                    p.ctrlMatrix.cat.lfoPmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x51: // CAT LFO FMOD Depth
                    p.ctrlMatrix.cat.lfoFmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x52: // CAT LFO AMOD Depth
                    p.ctrlMatrix.cat.lfoAmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x53: // PAT Pitch Control
                    p.ctrlMatrix.pat.pitch = juce::jlimit (0x28, 0x58, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x54: // PAT Filter Control
                    p.ctrlMatrix.pat.filter = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x55: // PAT Amp Control
                    p.ctrlMatrix.pat.amplitude = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x56: // PAT LFO PMOD Depth
                    p.ctrlMatrix.pat.lfoPmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x57: // PAT LFO FMOD Depth
                    p.ctrlMatrix.pat.lfoFmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x58: // PAT LFO AMOD Depth
                    p.ctrlMatrix.pat.lfoAmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x59: // AC1 Controller Number
                    p.ctrlMatrix.ac1ControllerNo = juce::jlimit (0, 95, static_cast<int> (val));
                    channelAc1Norm[chIdx] = 0.0f;
                    updateChannelModulation (channel);
                    break;

                case 0x5a: // AC1 Pitch Control
                    p.ctrlMatrix.ac1.pitch = juce::jlimit (0x28, 0x58, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x5b: // AC1 Filter Control
                    p.ctrlMatrix.ac1.filter = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x5c: // AC1 Amp Control
                    p.ctrlMatrix.ac1.amplitude = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x5d: // AC1 LFO PMOD Depth
                    p.ctrlMatrix.ac1.lfoPmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x5e: // AC1 LFO FMOD Depth
                    p.ctrlMatrix.ac1.lfoFmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x5f: // AC1 LFO AMOD Depth
                    p.ctrlMatrix.ac1.lfoAmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x60: // AC2 Controller Number
                    p.ctrlMatrix.ac2ControllerNo = juce::jlimit (0, 95, static_cast<int> (val));
                    channelAc2Norm[chIdx] = 0.0f;
                    updateChannelModulation (channel);
                    break;

                case 0x61: // AC2 Pitch Control
                    p.ctrlMatrix.ac2.pitch = juce::jlimit (0x28, 0x58, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x62: // AC2 Filter Control
                    p.ctrlMatrix.ac2.filter = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x63: // AC2 Amp Control
                    p.ctrlMatrix.ac2.amplitude = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x64: // AC2 LFO PMOD Depth
                    p.ctrlMatrix.ac2.lfoPmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x65: // AC2 LFO FMOD Depth
                    p.ctrlMatrix.ac2.lfoFmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x66: // AC2 LFO AMOD Depth
                    p.ctrlMatrix.ac2.lfoAmodDepth = juce::jlimit (0, 127, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

                case 0x70: // Bend Pitch Low Control
                    p.ctrlMatrix.bendPitchLow = juce::jlimit (0x28, 0x58, static_cast<int> (val));
                    updateChannelModulation (channel);
                    break;

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
                    partEqs[chIdx].needsUpdate = true;
                    break;

                case 0x73: // EQ Treble Gain
                    p.eqTreble = juce::jlimit (0, 127, static_cast<int> (val));
                    partEqs[chIdx].needsUpdate = true;
                    break;

                case 0x76: // EQ Bass Frequency
                    p.eqBassFreq = juce::jlimit (0, 127, static_cast<int> (val));
                    partEqs[chIdx].needsUpdate = true;
                    break;

                case 0x77: // EQ Treble Frequency
                    p.eqTrebleFreq = juce::jlimit (0, 127, static_cast<int> (val));
                    partEqs[chIdx].needsUpdate = true;
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
        bool variationChanged = false;

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
                variationChanged = true;
            }
            else if (curAddr == 0x41)
            {
                variationParameters.typeLsb = val;
                variationChanged = true;
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
                    variationChanged = true;
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
                variationChanged = true;
            }
            else if (curAddr == 0x5d)
            {
                variationParameters.bendControlDepth = val;
                variationChanged = true;
            }
            else if (curAddr == 0x5e)
            {
                variationParameters.catControlDepth = val;
                variationChanged = true;
            }
            else if (curAddr == 0x5f)
            {
                variationParameters.ac1ControlDepth = val;
                variationChanged = true;
            }
            else if (curAddr == 0x60)
            {
                variationParameters.ac2ControlDepth = val;
                variationChanged = true;
            }
            else if (curAddr >= 0x70 && curAddr <= 0x75)
            {
                variationParameters.parameters11To16[static_cast<size_t> (curAddr - 0x70)] = val;
                variationChanged = true;
            }
        }

        if (reverbChanged)
            updateReverbSettings();

        if (chorusChanged)
            updateChorusSettings();

        if (variationChanged)
            variationProcessor.updateParameters (variationParameters);

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
    const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
    const auto lsb = channelBankLsb[index].load (std::memory_order_acquire);
    const auto activeMode = getActiveMode();
    const auto gsMode = (activeMode == ActiveMode::GS);
    const auto gsPart = static_cast<gs::PartMode> (channelGsPartMode[index].load (std::memory_order_acquire));
    const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
    const auto partMode = static_cast<xg::PartMode> (partModeRaw);
    const auto isPercussion = gsMode ? gs::isDrumMode (gsPart)
                                     : (xg::isDrumMode (partMode) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit);

    fluid_synth_set_channel_type (activeSynth->synth, channel, isPercussion ? CHANNEL_TYPE_DRUM : CHANNEL_TYPE_MELODIC);
    applyProgramChangeToSynth (*activeSynth, channel, program, requestedBank, msb, lsb, activeMode, isPercussion);
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

    fluid_synth_set_gen (activeSynth->synth, channel, GEN_FINETUNE, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_FILTERFC, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_FILTERQ, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_ATTENUATION, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VOLENVATTACK, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODENVATTACK, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VOLENVDECAY, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODENVDECAY, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VOLENVRELEASE, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODENVRELEASE, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFOFREQ, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFOTOPITCH, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFODELAY, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODLFOTOPITCH, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODLFOTOFILTERFC, 0.0f);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODLFOTOVOL, 0.0f);
}

void FluidSynthEngine::resetChannelControllers (int channel) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto index = static_cast<size_t> (channel);
    channelModWheelNorm[index] = 0.0f;
    channelPitchBendNorm[index] = 0.0f;
    channelAftertouchNorm[index] = 0.0f;
    channelAc1Norm[index] = 0.0f;
    channelAc2Norm[index] = 0.0f;
}

void FluidSynthEngine::updateChannelModulation (int channel) noexcept
{
    if (activeSynth == nullptr || ! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto chIdx = static_cast<size_t> (channel);
    const auto& p = partParameters[chIdx];
    const auto& cm = p.ctrlMatrix;

    const float mwVal = channelModWheelNorm[chIdx];
    const float bendVal = channelPitchBendNorm[chIdx];
    const float catVal = channelAftertouchNorm[chIdx];
    const float ac1Val = channelAc1Norm[chIdx];
    const float ac2Val = channelAc2Norm[chIdx];

    // 1. Pitch Modulation
    float extraPitchCents = (mwVal * static_cast<float> (cm.mw.pitch - 64)
                           + catVal * static_cast<float> (cm.cat.pitch - 64)
                           + ac1Val * static_cast<float> (cm.ac1.pitch - 64)
                           + ac2Val * static_cast<float> (cm.ac2.pitch - 64)) * 100.0f;

    if (bendVal < 0.0f)
    {
        const float expectedLowSemitones = static_cast<float> (cm.bendPitchLow - 64);
        const float nativeLowSemitones = -static_cast<float> (p.pitchBendSensitivity);
        extraPitchCents += (-bendVal) * (expectedLowSemitones - nativeLowSemitones) * 100.0f;
    }

    const float totalFineTune = (getActiveMode() == ActiveMode::GS)
                              ? (gsSystemParameters.masterTuneCents + gsPartParameters[chIdx].pitchOffsetFineCents)
                              : (systemParameters.masterTuneCents + p.detuneCents + extraPitchCents);

    fluid_synth_set_gen (activeSynth->synth, channel, GEN_FINETUNE, totalFineTune);

    // 2. Filter Cutoff Modulation (-9600..+9450 cents, 40H = 0, 150 cents/step)
    float bendFilterOffset = 0.0f;
    if (bendVal != 0.0f)
        bendFilterOffset = bendVal * static_cast<float> (cm.bend.filter - 64) * 150.0f;

    const float netFilterCents = mwVal * static_cast<float> (cm.mw.filter - 64) * 150.0f
                               + bendFilterOffset
                               + catVal * static_cast<float> (cm.cat.filter - 64) * 150.0f
                               + ac1Val * static_cast<float> (cm.ac1.filter - 64) * 150.0f
                               + ac2Val * static_cast<float> (cm.ac2.filter - 64) * 150.0f;

    const float totalFilterCents = xg::cutoffOffsetToCents (p.filterCutoff) + netFilterCents;
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_FILTERFC, totalFilterCents);

    // 3. Amplitude Modulation (-100%..+100%, 40H = 0)
    float bendAmpDelta = 0.0f;
    if (bendVal != 0.0f)
        bendAmpDelta = bendVal * static_cast<float> (cm.bend.amplitude - 64) / 64.0f;

    const float ampFactor = 1.0f + (mwVal * static_cast<float> (cm.mw.amplitude - 64) / 64.0f)
                                 + bendAmpDelta
                                 + (catVal * static_cast<float> (cm.cat.amplitude - 64) / 64.0f)
                                 + (ac1Val * static_cast<float> (cm.ac1.amplitude - 64) / 64.0f)
                                 + (ac2Val * static_cast<float> (cm.ac2.amplitude - 64) / 64.0f);

    float attenCibels = 0.0f;
    if (ampFactor <= 0.0001f)
    {
        attenCibels = 1440.0f;
    }
    else if (ampFactor < 1.0f)
    {
        attenCibels = -200.0f * std::log10 (ampFactor);
        attenCibels = juce::jlimit (0.0f, 1440.0f, attenCibels);
    }
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_ATTENUATION, attenCibels);

    // 4. LFO PMOD Depth (Vibrato)
    float baseVibratoCents = 0.0f;
    if (p.vibratoDepth > 64)
        baseVibratoCents = static_cast<float> (p.vibratoDepth - 64) * 2.0f;

    const float netPmodCents = mwVal * static_cast<float> (cm.mw.lfoPmodDepth) * 5.0f
                             + std::abs (bendVal) * static_cast<float> (cm.bend.lfoPmodDepth) * 5.0f
                             + catVal * static_cast<float> (cm.cat.lfoPmodDepth) * 5.0f
                             + ac1Val * static_cast<float> (cm.ac1.lfoPmodDepth) * 5.0f
                             + ac2Val * static_cast<float> (cm.ac2.lfoPmodDepth) * 5.0f;

    const float totalPmodCents = baseVibratoCents + netPmodCents;
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFOTOPITCH, totalPmodCents);
    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODLFOTOPITCH, totalPmodCents);

    // 5. LFO FMOD Depth (Wah-wah filter sweep)
    const float netFmodCents = mwVal * static_cast<float> (cm.mw.lfoFmodDepth) * 25.0f
                             + std::abs (bendVal) * static_cast<float> (cm.bend.lfoFmodDepth) * 25.0f
                             + catVal * static_cast<float> (cm.cat.lfoFmodDepth) * 25.0f
                             + ac1Val * static_cast<float> (cm.ac1.lfoFmodDepth) * 25.0f
                             + ac2Val * static_cast<float> (cm.ac2.lfoFmodDepth) * 25.0f;

    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODLFOTOFILTERFC, netFmodCents);

    // 6. LFO AMOD Depth (Tremolo volume sweep)
    const float netAmodCibels = mwVal * static_cast<float> (cm.mw.lfoAmodDepth) * 1.5f
                              + std::abs (bendVal) * static_cast<float> (cm.bend.lfoAmodDepth) * 1.5f
                              + catVal * static_cast<float> (cm.cat.lfoAmodDepth) * 1.5f
                              + ac1Val * static_cast<float> (cm.ac1.lfoAmodDepth) * 1.5f
                              + ac2Val * static_cast<float> (cm.ac2.lfoAmodDepth) * 1.5f;

    fluid_synth_set_gen (activeSynth->synth, channel, GEN_MODLFOTOVOL, netAmodCibels);

    // Synchronize active Aux Drum slots
    for (size_t s = 0; s < numAuxDrumChannels; ++s)
    {
        if (auxDrumSlots[s].active && auxDrumSlots[s].sourceChannel == channel)
        {
            const auto auxChan = numMidiChannels + static_cast<int> (s);
            fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_FINETUNE, totalFineTune);
            fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_FILTERFC, totalFilterCents);
            fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_ATTENUATION, attenCibels);
            fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_VIBLFOTOPITCH, totalPmodCents);
            fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_MODLFOTOPITCH, totalPmodCents);
            fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_MODLFOTOFILTERFC, netFmodCents);
            fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_MODLFOTOVOL, netAmodCibels);
        }
    }
}

void FluidSynthEngine::setPartFilterCutoff (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].filterCutoff = clamped;
    updateChannelModulation (channel);
    if (activeSynth != nullptr)
        fluid_synth_cc (activeSynth->synth, channel, 74, clamped);
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
        if (clamped == 64)
        {
            fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFOFREQ, 0.0f);
        }
        else
        {
            // Limit rate offset to ±384 cents to prevent ultra-slow pitch modulation warble
            const auto rateOffset = juce::jlimit (-384.0f, 384.0f, static_cast<float> (clamped - 64) * 6.0f);
            fluid_synth_set_gen (activeSynth->synth, channel, GEN_VIBLFOFREQ, rateOffset);
        }
        fluid_synth_cc (activeSynth->synth, channel, 76, clamped);
    }
}

void FluidSynthEngine::setPartVibratoDepth (int channel, int value) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto clamped = juce::jlimit (0, 127, value);
    partParameters[static_cast<size_t> (channel)].vibratoDepth = clamped;
    updateChannelModulation (channel);

    if (activeSynth != nullptr)
    {
        // Mute CC#1 pitch modulation range in XG mode so that Modulation Matrix exclusively controls vibrato
        fluid_synth_cc (activeSynth->synth, channel, 101, 0);
        fluid_synth_cc (activeSynth->synth, channel, 100, 5);
        fluid_synth_cc (activeSynth->synth, channel, 6, 0);
        fluid_synth_cc (activeSynth->synth, channel, 38, 0);
        fluid_synth_cc (activeSynth->synth, channel, 101, 127);
        fluid_synth_cc (activeSynth->synth, channel, 100, 127);
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
    const auto activeMode = getActiveMode();
    const auto isGs = (activeMode == ActiveMode::GS);
    const auto gsPart = static_cast<gs::PartMode> (channelGsPartMode[index].load (std::memory_order_acquire));
    const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
    const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
    const auto partMode = static_cast<xg::PartMode> (partModeRaw);
    const auto isDrum = isGs ? gs::isDrumMode (gsPart)
                             : (xg::isDrumMode (partMode) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit);

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
                case 0x30: partParameters[index].eqBass = juce::jlimit (0, 127, value); partEqs[index].needsUpdate = true; break;
                case 0x31: partParameters[index].eqTreble = juce::jlimit (0, 127, value); partEqs[index].needsUpdate = true; break;
                case 0x34: partParameters[index].eqBassFreq = juce::jlimit (0, 127, value); partEqs[index].needsUpdate = true; break;
                case 0x35: partParameters[index].eqTrebleFreq = juce::jlimit (0, 127, value); partEqs[index].needsUpdate = true; break;
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
            const auto clamped = juce::jlimit (0, 127, value);
            if (isGs)
            {
                auto& setup = (gsPart == gs::PartMode::Drum2) ? gsDrumSetup2 : gsDrumSetup1;
                auto& note = setup.notes[nrpnLsb];
                note.modified = true;

                switch (nrpnMsb)
                {
                    case 0x18: note.pitch = clamped; break;
                    case 0x1A: note.level = clamped; break;
                    case 0x1C: note.pan = clamped; break;
                    case 0x1D: note.reverbSend = clamped; break;
                    case 0x1E: note.chorusSend = clamped; break;
                    case 0x1F: note.delaySend = clamped; break;
                    default: break;
                }
            }
            else
            {
                auto& setup = (partMode == xg::PartMode::Drums2 || partMode == xg::PartMode::Drums4)
                                  ? drumSetup2 : drumSetup1;
                auto& note = setup.notes[nrpnLsb];
                note.modified = true;

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
}

void FluidSynthEngine::handleNrpnDataIncDec (int channel, int delta) noexcept
{
    if (! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto index = static_cast<size_t> (channel);
    const auto& state = nrpnStates[index];
    const auto activeMode = getActiveMode();
    const auto isGs = (activeMode == ActiveMode::GS);
    const auto gsPart = static_cast<gs::PartMode> (channelGsPartMode[index].load (std::memory_order_acquire));
    const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
    const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
    const auto partMode = static_cast<xg::PartMode> (partModeRaw);
    const auto isDrum = isGs ? gs::isDrumMode (gsPart)
                             : (xg::isDrumMode (partMode) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit);

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
                case 0x30: partParameters[index].eqBass = juce::jlimit (0, 127, p.eqBass + delta); partEqs[index].needsUpdate = true; break;
                case 0x31: partParameters[index].eqTreble = juce::jlimit (0, 127, p.eqTreble + delta); partEqs[index].needsUpdate = true; break;
                case 0x34: partParameters[index].eqBassFreq = juce::jlimit (0, 127, p.eqBassFreq + delta); partEqs[index].needsUpdate = true; break;
                case 0x35: partParameters[index].eqTrebleFreq = juce::jlimit (0, 127, p.eqTrebleFreq + delta); partEqs[index].needsUpdate = true; break;
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
            if (isGs)
            {
                auto& setup = (gsPart == gs::PartMode::Drum2) ? gsDrumSetup2 : gsDrumSetup1;
                auto& note = setup.notes[nrpnLsb];
                note.modified = true;

                switch (nrpnMsb)
                {
                    case 0x18: note.pitch = juce::jlimit (0, 127, note.pitch + delta); break;
                    case 0x1A: note.level = juce::jlimit (0, 127, note.level + delta); break;
                    case 0x1C: note.pan = juce::jlimit (0, 127, note.pan + delta); break;
                    case 0x1D: note.reverbSend = juce::jlimit (0, 127, note.reverbSend + delta); break;
                    case 0x1E: note.chorusSend = juce::jlimit (0, 127, note.chorusSend + delta); break;
                    case 0x1F: note.delaySend = juce::jlimit (0, 127, note.delaySend + delta); break;
                    default: break;
                }
            }
            else
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
}

int FluidSynthEngine::allocateAuxDrumSlot (int channel, int noteNumber, int setupIdx) noexcept
{
    const auto now = ++auxDrumTimestamp;

    // 1. First priority: completely inactive slot
    for (size_t i = 0; i < numAuxDrumChannels; ++i)
    {
        if (! auxDrumSlots[i].active)
        {
            auxDrumSlots[i].reset();
            auxDrumSlots[i].active = true;
            auxDrumSlots[i].sourceChannel = channel;
            auxDrumSlots[i].noteNumber = noteNumber;
            auxDrumSlots[i].setupIndex = setupIdx;
            auxDrumSlots[i].triggerTime = now;
            return static_cast<int> (i);
        }
    }

    // 2. Second priority: slot that has decayed to silence
    for (size_t i = 0; i < numAuxDrumChannels; ++i)
    {
        if (auxDrumSlots[i].silentBlocks >= 4)
        {
            const auto auxChan = numMidiChannels + static_cast<int> (i);
            if (activeSynth != nullptr)
                fluid_synth_all_notes_off (activeSynth->synth, auxChan);

            auxDrumSlots[i].reset();
            auxDrumSlots[i].active = true;
            auxDrumSlots[i].sourceChannel = channel;
            auxDrumSlots[i].noteNumber = noteNumber;
            auxDrumSlots[i].setupIndex = setupIdx;
            auxDrumSlots[i].triggerTime = now;
            return static_cast<int> (i);
        }
    }

    // 3. Third priority: LRU (Least Recently Used) slot
    size_t oldestIdx = 0;
    uint64_t oldestTime = UINT64_MAX;
    for (size_t i = 0; i < numAuxDrumChannels; ++i)
    {
        if (auxDrumSlots[i].triggerTime < oldestTime)
        {
            oldestTime = auxDrumSlots[i].triggerTime;
            oldestIdx = i;
        }
    }

    const auto auxChan = numMidiChannels + static_cast<int> (oldestIdx);
    if (activeSynth != nullptr)
        fluid_synth_all_notes_off (activeSynth->synth, auxChan);

    auxDrumSlots[oldestIdx].reset();
    auxDrumSlots[oldestIdx].active = true;
    auxDrumSlots[oldestIdx].sourceChannel = channel;
    auxDrumSlots[oldestIdx].noteNumber = noteNumber;
    auxDrumSlots[oldestIdx].setupIndex = setupIdx;
    auxDrumSlots[oldestIdx].triggerTime = now;
    return static_cast<int> (oldestIdx);
}

void FluidSynthEngine::setupAuxDrumSlotEq (int slotIndex,
                                           const xg::DrumNoteParameters& noteParams,
                                           int sourceChannel) noexcept
{
    if (! juce::isPositiveAndBelow (slotIndex, static_cast<int> (numAuxDrumChannels))
        || ! juce::isPositiveAndBelow (sourceChannel, static_cast<int> (numMidiChannels)))
        return;

    auto& slot = auxDrumSlots[static_cast<size_t> (slotIndex)];
    const auto sr = currentSampleRate.load (std::memory_order_relaxed);
    if (sr <= 1.0)
        return;

    const auto nyquist = sr * 0.49;
    const auto& part = partParameters[static_cast<size_t> (sourceChannel)];

    // 1. Note EQ (Bass and Treble shelves)
    const auto noteBassDb = static_cast<float> (noteParams.eqBass - 64) * (12.0f / 64.0f);
    if (std::abs (noteBassDb) >= 0.1f)
    {
        const auto gainFactor = juce::Decibels::decibelsToGain (noteBassDb);
        const auto freq = juce::jlimit (20.0, nyquist, static_cast<double> (xg::lookupEqFrequency (part.eqBassFreq)));
        const auto coeff = juce::IIRCoefficients::makeLowShelf (sr, freq, 0.707, gainFactor);
        slot.noteBassFilters[0].setCoefficients (coeff);
        slot.noteBassFilters[1].setCoefficients (coeff);
        slot.noteBassActive = true;
    }
    else
    {
        slot.noteBassActive = false;
        slot.noteBassFilters[0].makeInactive();
        slot.noteBassFilters[1].makeInactive();
    }

    const auto noteTrebleDb = static_cast<float> (noteParams.eqTreble - 64) * (12.0f / 64.0f);
    if (std::abs (noteTrebleDb) >= 0.1f)
    {
        const auto gainFactor = juce::Decibels::decibelsToGain (noteTrebleDb);
        const auto freq = juce::jlimit (20.0, nyquist, static_cast<double> (xg::lookupEqFrequency (part.eqTrebleFreq)));
        const auto coeff = juce::IIRCoefficients::makeHighShelf (sr, freq, 0.707, gainFactor);
        slot.noteTrebleFilters[0].setCoefficients (coeff);
        slot.noteTrebleFilters[1].setCoefficients (coeff);
        slot.noteTrebleActive = true;
    }
    else
    {
        slot.noteTrebleActive = false;
        slot.noteTrebleFilters[0].makeInactive();
        slot.noteTrebleFilters[1].makeInactive();
    }

    // 2. Part EQ (in series)
    const auto partBassDb = static_cast<float> (part.eqBass - 64);
    if (std::abs (partBassDb) >= 0.1f)
    {
        const auto gainFactor = juce::Decibels::decibelsToGain (partBassDb);
        const auto freq = juce::jlimit (20.0, nyquist, static_cast<double> (xg::lookupEqFrequency (part.eqBassFreq)));
        const auto coeff = juce::IIRCoefficients::makeLowShelf (sr, freq, 0.707, gainFactor);
        slot.partBassFilters[0].setCoefficients (coeff);
        slot.partBassFilters[1].setCoefficients (coeff);
        slot.partBassActive = true;
    }
    else
    {
        slot.partBassActive = false;
        slot.partBassFilters[0].makeInactive();
        slot.partBassFilters[1].makeInactive();
    }

    const auto partTrebleDb = static_cast<float> (part.eqTreble - 64);
    if (std::abs (partTrebleDb) >= 0.1f)
    {
        const auto gainFactor = juce::Decibels::decibelsToGain (partTrebleDb);
        const auto freq = juce::jlimit (20.0, nyquist, static_cast<double> (xg::lookupEqFrequency (part.eqTrebleFreq)));
        const auto coeff = juce::IIRCoefficients::makeHighShelf (sr, freq, 0.707, gainFactor);
        slot.partTrebleFilters[0].setCoefficients (coeff);
        slot.partTrebleFilters[1].setCoefficients (coeff);
        slot.partTrebleActive = true;
    }
    else
    {
        slot.partTrebleActive = false;
        slot.partTrebleFilters[0].makeInactive();
        slot.partTrebleFilters[1].makeInactive();
    }

    // 3. Send scaling factors
    slot.noteReverbSendNorm = static_cast<float> (noteParams.reverbSend) / 127.0f;
    slot.noteChorusSendNorm = static_cast<float> (noteParams.chorusSend) / 127.0f;
    slot.noteVarSendNorm    = static_cast<float> (noteParams.variationSend) / 127.0f;

    // 4. Mirror source channel generators
    const auto auxChan = numMidiChannels + slotIndex;
    if (activeSynth != nullptr)
    {
        fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_FINETUNE, fluid_synth_get_gen (activeSynth->synth, sourceChannel, GEN_FINETUNE));
        fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_FILTERFC, fluid_synth_get_gen (activeSynth->synth, sourceChannel, GEN_FILTERFC));
        fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_ATTENUATION, fluid_synth_get_gen (activeSynth->synth, sourceChannel, GEN_ATTENUATION));
        fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_VIBLFOTOPITCH, fluid_synth_get_gen (activeSynth->synth, sourceChannel, GEN_VIBLFOTOPITCH));
        fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_MODLFOTOPITCH, fluid_synth_get_gen (activeSynth->synth, sourceChannel, GEN_MODLFOTOPITCH));
        fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_MODLFOTOFILTERFC, fluid_synth_get_gen (activeSynth->synth, sourceChannel, GEN_MODLFOTOFILTERFC));
        fluid_synth_set_gen (activeSynth->synth, auxChan, GEN_MODLFOTOVOL, fluid_synth_get_gen (activeSynth->synth, sourceChannel, GEN_MODLFOTOVOL));
    }
}

void FluidSynthEngine::setupGsAuxDrumSlotEq (int slotIndex,
                                             const gs::DrumNoteParameters& noteParams,
                                             int sourceChannel) noexcept
{
    if (! juce::isPositiveAndBelow (slotIndex, static_cast<int> (numAuxDrumChannels))
        || ! juce::isPositiveAndBelow (sourceChannel, static_cast<int> (numMidiChannels)))
        return;

    auto& slot = auxDrumSlots[static_cast<size_t> (slotIndex)];
    slot.noteBassActive = false;
    slot.noteTrebleActive = false;
    slot.partBassActive = false;
    slot.partTrebleActive = false;

    slot.noteReverbSendNorm = static_cast<float> (noteParams.reverbSend) / 127.0f;
    slot.noteChorusSendNorm = static_cast<float> (noteParams.chorusSend) / 127.0f;
    slot.noteVarSendNorm    = static_cast<float> (noteParams.delaySend) / 127.0f;
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

void FluidSynthEngine::applyGsDrumNoteGenerators (fluid_voice_t* v, const gs::DrumNoteParameters& noteParams) noexcept
{
    if (v == nullptr)
        return;

    if (noteParams.pitch != 64)
    {
        fluid_voice_gen_incr (v, GEN_COARSETUNE, static_cast<float> (noteParams.pitch - 64));
        fluid_voice_update_param (v, GEN_COARSETUNE);
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

void FluidSynthEngine::applyMelodicVoiceGenerators (fluid_voice_t* v, int channel) noexcept
{
    if (v == nullptr || ! juce::isPositiveAndBelow (channel, numMidiChannels))
        return;

    const auto index = static_cast<size_t> (channel);
    const auto& p = partParameters[index];
    const auto modWheel = channelModulation[index].load (std::memory_order_relaxed);

    // If Vibrato Depth is 0, completely suppress preset vibrato from the SoundFont!
    if (p.vibratoDepth == 0)
    {
        fluid_voice_gen_set (v, GEN_VIBLFOTOPITCH, 0.0f);
        fluid_voice_update_param (v, GEN_VIBLFOTOPITCH);

        if (modWheel == 0)
        {
            fluid_voice_gen_set (v, GEN_MODLFOTOPITCH, 0.0f);
            fluid_voice_update_param (v, GEN_MODLFOTOPITCH);
        }
    }
    else if (p.vibratoDepth < 64)
    {
        // Scale down preset vibrato depth
        const auto scale = static_cast<float> (p.vibratoDepth) / 64.0f;
        const auto curVal = fluid_voice_gen_get (v, GEN_VIBLFOTOPITCH);
        fluid_voice_gen_set (v, GEN_VIBLFOTOPITCH, curVal * scale);
        fluid_voice_update_param (v, GEN_VIBLFOTOPITCH);

        if (modWheel == 0)
        {
            const auto curMod = fluid_voice_gen_get (v, GEN_MODLFOTOPITCH);
            fluid_voice_gen_set (v, GEN_MODLFOTOPITCH, curMod * scale);
            fluid_voice_update_param (v, GEN_MODLFOTOPITCH);
        }
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

    const auto activeMode = getActiveMode();
    const auto gsMode = (activeMode == ActiveMode::GS);

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
            const auto gsPart = static_cast<gs::PartMode> (channelGsPartMode[index].load (std::memory_order_acquire));
            const auto isDrum = gsMode ? gs::isDrumMode (gsPart)
                                       : (xg::isDrumMode (partMode) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit);
            const auto& p = partParameters[index];

            if (! gsMode && (noteNumber < p.noteLimitLow || noteNumber > p.noteLimitHigh))
                return;

            if (isDrum && juce::isPositiveAndBelow (noteNumber, 128))
            {
                if (gsMode)
                {
                    const auto& setup = (gsPart == gs::PartMode::Drum2) ? gsDrumSetup2 : gsDrumSetup1;
                    const auto& noteParams = setup.notes[static_cast<size_t> (noteNumber)];

                    if (noteParams.modified)
                    {
                        if (noteParams.rcvNoteOn == 0 || noteParams.level == 0)
                            return;

                        if (noteParams.alternateGroup > 0 && noteParams.alternateGroup < 128)
                        {
                            const auto prevNote = activeGroupNote[index][static_cast<size_t> (noteParams.alternateGroup)];
                            if (prevNote >= 0 && prevNote != noteNumber)
                            {
                                fluid_synth_noteoff (activeSynth->synth, channel, prevNote);
                                for (size_t s = 0; s < numAuxDrumChannels; ++s)
                                {
                                    if (auxDrumSlots[s].active && auxDrumSlots[s].sourceChannel == channel && auxDrumSlots[s].noteNumber == prevNote)
                                        fluid_synth_noteoff (activeSynth->synth, numMidiChannels + static_cast<int> (s), prevNote);
                                }
                            }
                            activeGroupNote[index][static_cast<size_t> (noteParams.alternateGroup)] = noteNumber;
                        }

                        const auto scaledVel = juce::jlimit (1, 127, static_cast<int> (std::round (rawVelocity * (static_cast<float> (noteParams.level) / 127.0f))));

                        const bool hasCustomSends = (noteParams.reverbSend != 40
                                                  || noteParams.chorusSend != 0
                                                  || noteParams.delaySend != 0);

                        if (hasCustomSends)
                        {
                            const auto slotIdx = allocateAuxDrumSlot (channel, noteNumber, (gsPart == gs::PartMode::Drum2) ? 2 : 1);
                            const auto auxChan = numMidiChannels + slotIdx;

                            fluid_synth_set_channel_type (activeSynth->synth, auxChan, CHANNEL_TYPE_DRUM);
                            const auto bank = channelBank[index].load (std::memory_order_acquire);
                            const auto prog = channelProgram[index].load (std::memory_order_acquire);
                            if (auxChannelBank[static_cast<size_t> (slotIdx)] != bank || auxChannelProgram[static_cast<size_t> (slotIdx)] != prog)
                            {
                                fluid_synth_bank_select (activeSynth->synth, auxChan, bank);
                                applyProgramChangeToSynth (*activeSynth, auxChan, prog, bank,
                                                           channelBankMsb[index].load (std::memory_order_acquire),
                                                           channelBankLsb[index].load (std::memory_order_acquire),
                                                           activeMode, true);
                                auxChannelBank[static_cast<size_t> (slotIdx)] = bank;
                                auxChannelProgram[static_cast<size_t> (slotIdx)] = prog;
                            }

                            const auto vol = channelVolume[index].load (std::memory_order_acquire);
                            const auto pan = channelPan[index].load (std::memory_order_acquire);
                            fluid_synth_cc (activeSynth->synth, auxChan, 7, vol);
                            fluid_synth_cc (activeSynth->synth, auxChan, 10, pan);
                            fluid_synth_cc (activeSynth->synth, auxChan, 11, 127);
                            int pb = 8192;
                            fluid_synth_get_pitch_bend (activeSynth->synth, channel, &pb);
                            fluid_synth_pitch_bend (activeSynth->synth, auxChan, pb);

                            setupGsAuxDrumSlotEq (slotIdx, noteParams, channel);

                            std::array<fluid_voice_t*, 256> voiceBuf {};
                            fluid_synth_get_voicelist (activeSynth->synth, voiceBuf.data(), static_cast<int> (voiceBuf.size()), -1);
                            unsigned int maxIdBefore = 0;
                            for (auto* v : voiceBuf)
                            {
                                if (v == nullptr)
                                    break;
                                maxIdBefore = juce::jmax (maxIdBefore, fluid_voice_get_id (v));
                            }

                            fluid_synth_noteon (activeSynth->synth, auxChan, noteNumber, scaledVel);

                            voiceBuf.fill (nullptr);
                            fluid_synth_get_voicelist (activeSynth->synth, voiceBuf.data(), static_cast<int> (voiceBuf.size()), -1);
                            for (auto* v : voiceBuf)
                            {
                                if (v == nullptr)
                                    break;
                                if (fluid_voice_get_id (v) > maxIdBefore
                                    && fluid_voice_get_channel (v) == auxChan
                                    && fluid_voice_get_key (v) == noteNumber)
                                {
                                    applyGsDrumNoteGenerators (v, noteParams);
                                }
                            }
                            return;
                        }

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
                                applyGsDrumNoteGenerators (v, noteParams);
                            }
                        }
                        return;
                    }
                }
                else
                {
                    const auto setupIdx = (partMode == xg::PartMode::Drums2 || partMode == xg::PartMode::Drums4) ? 2 : 1;
                    const auto& setup = (setupIdx == 2) ? drumSetup2 : drumSetup1;
                    const auto& noteParams = setup.notes[static_cast<size_t> (noteNumber)];

                    if (noteParams.modified)
                    {
                        if (noteParams.rcvNoteOn == 0 || noteParams.level == 0)
                            return;

                        if (noteParams.alternateGroup > 0 && noteParams.alternateGroup < 128)
                        {
                            const auto prevNote = activeGroupNote[index][static_cast<size_t> (noteParams.alternateGroup)];
                            if (prevNote >= 0 && prevNote != noteNumber)
                            {
                                fluid_synth_noteoff (activeSynth->synth, channel, prevNote);
                                for (size_t s = 0; s < numAuxDrumChannels; ++s)
                                {
                                    if (auxDrumSlots[s].active && auxDrumSlots[s].sourceChannel == channel && auxDrumSlots[s].noteNumber == prevNote)
                                        fluid_synth_noteoff (activeSynth->synth, numMidiChannels + static_cast<int> (s), prevNote);
                                }
                            }
                            activeGroupNote[index][static_cast<size_t> (noteParams.alternateGroup)] = noteNumber;
                        }

                        const auto scaledVel = juce::jlimit (1, 127, static_cast<int> (std::round (rawVelocity * (static_cast<float> (noteParams.level) / 127.0f))));

                        const bool hasCustomEqOrSends = (noteParams.eqBass != 64
                                                      || noteParams.eqTreble != 64
                                                      || noteParams.reverbSend != 40
                                                      || noteParams.chorusSend != 0
                                                      || noteParams.variationSend != 0);

                        if (hasCustomEqOrSends)
                        {
                            const auto slotIdx = allocateAuxDrumSlot (channel, noteNumber, setupIdx);
                            const auto auxChan = numMidiChannels + slotIdx;

                            fluid_synth_set_channel_type (activeSynth->synth, auxChan, CHANNEL_TYPE_DRUM);
                            const auto bank = channelBank[index].load (std::memory_order_acquire);
                            const auto prog = channelProgram[index].load (std::memory_order_acquire);
                            if (auxChannelBank[static_cast<size_t> (slotIdx)] != bank || auxChannelProgram[static_cast<size_t> (slotIdx)] != prog)
                            {
                                fluid_synth_bank_select (activeSynth->synth, auxChan, bank);
                                applyProgramChangeToSynth (*activeSynth, auxChan, prog, bank,
                                                           channelBankMsb[index].load (std::memory_order_acquire),
                                                           channelBankLsb[index].load (std::memory_order_acquire),
                                                           activeMode, true);
                                auxChannelBank[static_cast<size_t> (slotIdx)] = bank;
                                auxChannelProgram[static_cast<size_t> (slotIdx)] = prog;
                            }

                            const auto vol = channelVolume[index].load (std::memory_order_acquire);
                            const auto pan = channelPan[index].load (std::memory_order_acquire);
                            fluid_synth_cc (activeSynth->synth, auxChan, 7, vol);
                            fluid_synth_cc (activeSynth->synth, auxChan, 10, pan);
                            fluid_synth_cc (activeSynth->synth, auxChan, 11, 127);
                            int pb = 8192;
                            fluid_synth_get_pitch_bend (activeSynth->synth, channel, &pb);
                            fluid_synth_pitch_bend (activeSynth->synth, auxChan, pb);

                            setupAuxDrumSlotEq (slotIdx, noteParams, channel);

                            std::array<fluid_voice_t*, 256> voiceBuf {};
                            fluid_synth_get_voicelist (activeSynth->synth, voiceBuf.data(), static_cast<int> (voiceBuf.size()), -1);
                            unsigned int maxIdBefore = 0;
                            for (auto* v : voiceBuf)
                            {
                                if (v == nullptr)
                                    break;
                                maxIdBefore = juce::jmax (maxIdBefore, fluid_voice_get_id (v));
                            }

                            fluid_synth_noteon (activeSynth->synth, auxChan, noteNumber, scaledVel);

                            voiceBuf.fill (nullptr);
                            fluid_synth_get_voicelist (activeSynth->synth, voiceBuf.data(), static_cast<int> (voiceBuf.size()), -1);
                            for (auto* v : voiceBuf)
                            {
                                if (v == nullptr)
                                    break;
                                if (fluid_voice_get_id (v) > maxIdBefore
                                    && fluid_voice_get_channel (v) == auxChan
                                    && fluid_voice_get_key (v) == noteNumber)
                                {
                                    applyDrumNoteGenerators (v, noteParams);
                                }
                            }
                            return;
                        }

                        // Otherwise (pitch/pan/filter only), play on regular channel
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
                return;
            }

            int effectiveNote = noteNumber;
            if (gsMode)
            {
                const auto transpose = gsSystemParameters.masterKeyShift - 64;
                const auto shift = gsPartParameters[index].keyShift - 64;
                effectiveNote = juce::jlimit (0, 127, noteNumber + transpose + shift);
            }
            else
            {
                const auto transpose = systemParameters.transpose - 64;
                const auto shift = p.noteShift - 64;
                effectiveNote = juce::jlimit (0, 127, noteNumber + transpose + shift);

                if (p.monoPolyMode == 0)
                    fluid_synth_all_notes_off (activeSynth->synth, channel);
            }

            if (juce::isPositiveAndBelow (noteNumber, 128))
                activeNoteTransposition[index][static_cast<size_t> (noteNumber)] = static_cast<uint8_t> (effectiveNote);

            std::array<fluid_voice_t*, 256> voiceBuf {};
            fluid_synth_get_voicelist (activeSynth->synth, voiceBuf.data(), static_cast<int> (voiceBuf.size()), -1);
            unsigned int maxIdBefore = 0;
            for (auto* v : voiceBuf)
            {
                if (v == nullptr)
                    break;
                maxIdBefore = juce::jmax (maxIdBefore, fluid_voice_get_id (v));
            }

            fluid_synth_noteon (activeSynth->synth, channel, effectiveNote, rawVelocity);

            voiceBuf.fill (nullptr);
            fluid_synth_get_voicelist (activeSynth->synth, voiceBuf.data(), static_cast<int> (voiceBuf.size()), -1);
            for (auto* v : voiceBuf)
            {
                if (v == nullptr)
                    break;
                if (fluid_voice_get_id (v) > maxIdBefore
                    && fluid_voice_get_channel (v) == channel
                    && fluid_voice_get_key (v) == effectiveNote)
                {
                    applyMelodicVoiceGenerators (v, channel);
                }
            }
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
            const auto gsPart = static_cast<gs::PartMode> (channelGsPartMode[index].load (std::memory_order_acquire));
            const auto isDrum = gsMode ? gs::isDrumMode (gsPart)
                                       : (xg::isDrumMode (partMode) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit);

            if (isDrum)
            {
                if (gsMode)
                {
                    const auto& setup = (gsPart == gs::PartMode::Drum2) ? gsDrumSetup2 : gsDrumSetup1;
                    if (juce::isPositiveAndBelow (noteNumber, 128))
                    {
                        const auto& noteParams = setup.notes[static_cast<size_t> (noteNumber)];
                        if (noteParams.modified && noteParams.rcvNoteOff == 0)
                            return;

                        const auto group = noteParams.alternateGroup;
                        if (group > 0 && group < 128 && activeGroupNote[index][static_cast<size_t> (group)] == noteNumber)
                            activeGroupNote[index][static_cast<size_t> (group)] = -1;
                    }
                }
                else
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
                }
                fluid_synth_noteoff (activeSynth->synth, channel, noteNumber);
                for (size_t s = 0; s < numAuxDrumChannels; ++s)
                {
                    if (auxDrumSlots[s].active && auxDrumSlots[s].sourceChannel == channel && auxDrumSlots[s].noteNumber == noteNumber)
                        fluid_synth_noteoff (activeSynth->synth, numMidiChannels + static_cast<int> (s), noteNumber);
                }
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
            {
                fluid_synth_all_notes_off (activeSynth->synth, channel);
                for (size_t s = 0; s < numAuxDrumChannels; ++s)
                {
                    if (auxDrumSlots[s].active && auxDrumSlots[s].sourceChannel == channel)
                        fluid_synth_all_notes_off (activeSynth->synth, numMidiChannels + static_cast<int> (s));
                }
            }
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
            channelModulation[index].store (0, std::memory_order_release);
            nrpnStates[index].reset();
            resetChannelControllers (channel);
            updateChannelModulation (channel);
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

        const auto isGs = (activeMode == ActiveMode::GS);

        if (controller == 0)
        {
            if (isGs)
            {
                // GS Mode: CC#0 is Variation Bank
                channelBankMsb[index].store (value, std::memory_order_release);
                gsPartParameters[index].variationBank = static_cast<uint8_t> (value);
                const auto lsb = channelBankLsb[index].load (std::memory_order_acquire);
                channelBank[index].store ((value << 7) | (lsb & 0x7f), std::memory_order_release);
            }
            else
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
        }
        else if (controller == 32)
        {
            if (isGs)
            {
                // GS Mode: CC#32 is Tone Map Number (0=Default/SC-88, 1=SC-55, 2=SC-88)
                channelBankLsb[index].store (value, std::memory_order_release);
                gsPartParameters[index].toneMap = static_cast<uint8_t> (value);
                const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
                channelBank[index].store ((msb << 7) | (value & 0x7f), std::memory_order_release);
            }
            else
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
        }

        if (controller == 1)
        {
            channelModulation[index].store (static_cast<uint8_t> (value), std::memory_order_release);
            channelModWheelNorm[index] = static_cast<float> (value) / 127.0f;
            updateChannelModulation (channel);
        }
        else if (controller == 7)
        {
            channelVolume[index].store (value, std::memory_order_release);

            if (channelMuted[index].load (std::memory_order_acquire))
                value = 0;
        }
        else if (controller == 10)
        {
            channelPan[index].store (message.getControllerValue(), std::memory_order_release);
        }

        const auto ac1No = partParameters[index].ctrlMatrix.ac1ControllerNo;
        const auto ac2No = partParameters[index].ctrlMatrix.ac2ControllerNo;
        if (controller == ac1No && ac1No != 1)
        {
            channelAc1Norm[index] = static_cast<float> (value) / 127.0f;
            updateChannelModulation (channel);
        }
        if (controller == ac2No && ac2No != 1)
        {
            channelAc2Norm[index] = static_cast<float> (value) / 127.0f;
            updateChannelModulation (channel);
        }

        if (activeSynth == nullptr)
            return;

        fluid_synth_cc (activeSynth->synth, channel, controller, value);
        if (controller == 0)
        {
            const auto gsPart = static_cast<gs::PartMode> (channelGsPartMode[index].load (std::memory_order_acquire));
            const auto partModeRaw = channelPartMode[index].load (std::memory_order_acquire);
            const auto msb = channelBankMsb[index].load (std::memory_order_acquire);
            const auto isDrum = isGs ? gs::isDrumMode (gsPart)
                                     : (xg::isDrumMode (static_cast<xg::PartMode> (partModeRaw)) || msb == xg::bankMsbDrumKit || msb == xg::bankMsbSfxKit);
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
        const auto rawValue = message.getPitchWheelValue();
        const auto index = static_cast<size_t> (channel);
        if (rawValue >= 8192)
            channelPitchBendNorm[index] = static_cast<float> (rawValue - 8192) / 8191.0f;
        else
            channelPitchBendNorm[index] = static_cast<float> (rawValue - 8192) / 8192.0f;

        updateChannelModulation (channel);

        if (activeSynth != nullptr)
        {
            fluid_synth_pitch_bend (activeSynth->synth, channel, rawValue);
            for (size_t s = 0; s < numAuxDrumChannels; ++s)
            {
                if (auxDrumSlots[s].active && auxDrumSlots[s].sourceChannel == channel)
                    fluid_synth_pitch_bend (activeSynth->synth, numMidiChannels + static_cast<int> (s), rawValue);
            }
        }
    }
    else if (message.isChannelPressure())
    {
        const auto val = message.getChannelPressureValue();
        const auto index = static_cast<size_t> (channel);
        channelAftertouchNorm[index] = static_cast<float> (val) / 127.0f;
        updateChannelModulation (channel);

        if (activeSynth != nullptr)
            fluid_synth_channel_pressure (activeSynth->synth, channel, val);
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
            renderSubRange (eventPosition - renderedUntil,
                            left + (renderedUntil - rangeStart),
                            right + (renderedUntil - rangeStart));
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
        renderSubRange (rangeEnd - renderedUntil,
                        left + (renderedUntil - rangeStart),
                        right + (renderedUntil - rangeStart));
    }
}

void FluidSynthEngine::renderSubRange (int totalSamples, float* destLeft, float* destRight) noexcept
{
    if (activeSynth == nullptr || totalSamples <= 0 || destLeft == nullptr || destRight == nullptr)
        return;

    const auto maxChunk = multiPartBuffer.getNumSamples();
    if (maxChunk <= 0)
        return;

    const auto activeMode = getActiveMode();
    const bool isXg = (activeMode == ActiveMode::XG);
    const bool isGs = (activeMode == ActiveMode::GS);
    const bool varIsInsertion = isXg && (variationParameters.connection == 0);
    const int varTargetPart = static_cast<int> (variationParameters.part);

    int samplesRendered = 0;
    while (samplesRendered < totalSamples)
    {
        const auto chunkSize = juce::jmin (maxChunk, totalSamples - samplesRendered);
        auto* curDestLeft = destLeft + samplesRendered;
        auto* curDestRight = destRight + samplesRendered;

        for (int ch = 0; ch < totalSynthChannels; ++ch)
        {
            partLeftPtrs[static_cast<size_t> (ch)] = multiPartBuffer.getWritePointer (ch * 2);
            partRightPtrs[static_cast<size_t> (ch)] = multiPartBuffer.getWritePointer (ch * 2 + 1);
            juce::FloatVectorOperations::clear (partLeftPtrs[static_cast<size_t> (ch)], chunkSize);
            juce::FloatVectorOperations::clear (partRightPtrs[static_cast<size_t> (ch)], chunkSize);
        }

        fluid_synth_nwrite_float (activeSynth->synth,
                                  chunkSize,
                                  partLeftPtrs.data(),
                                  partRightPtrs.data(),
                                  nullptr,
                                  nullptr);

        // Mute handling
        for (int ch = 0; ch < numMidiChannels; ++ch)
        {
            if (channelMuted[static_cast<size_t> (ch)].load (std::memory_order_relaxed))
            {
                juce::FloatVectorOperations::clear (partLeftPtrs[static_cast<size_t> (ch)], chunkSize);
                juce::FloatVectorOperations::clear (partRightPtrs[static_cast<size_t> (ch)], chunkSize);
            }
        }
        for (size_t s = 0; s < numAuxDrumChannels; ++s)
        {
            auto& slot = auxDrumSlots[s];
            if (slot.active && juce::isPositiveAndBelow (slot.sourceChannel, static_cast<int> (numMidiChannels)))
            {
                if (channelMuted[static_cast<size_t> (slot.sourceChannel)].load (std::memory_order_relaxed))
                {
                    const auto auxCh = numMidiChannels + static_cast<int> (s);
                    juce::FloatVectorOperations::clear (partLeftPtrs[static_cast<size_t> (auxCh)], chunkSize);
                    juce::FloatVectorOperations::clear (partRightPtrs[static_cast<size_t> (auxCh)], chunkSize);
                }
            }
        }

        // 1. Part EQ on each channel 0..15
        for (int ch = 0; ch < numMidiChannels; ++ch)
        {
            auto& eq = partEqs[static_cast<size_t> (ch)];
            if (eq.needsUpdate)
            {
                updatePartEqCoefficients (ch);
                eq.needsUpdate = false;
            }

            if (eq.bassActive)
            {
                eq.bassFilters[0].processSamples (partLeftPtrs[static_cast<size_t> (ch)], chunkSize);
                eq.bassFilters[1].processSamples (partRightPtrs[static_cast<size_t> (ch)], chunkSize);
            }
            if (eq.trebleActive)
            {
                eq.trebleFilters[0].processSamples (partLeftPtrs[static_cast<size_t> (ch)], chunkSize);
                eq.trebleFilters[1].processSamples (partRightPtrs[static_cast<size_t> (ch)], chunkSize);
            }
        }

        // 1b. Note EQ and Part EQ on Aux Drum channels 16..31
        for (size_t s = 0; s < numAuxDrumChannels; ++s)
        {
            auto& slot = auxDrumSlots[s];
            if (! slot.active)
                continue;

            const auto auxCh = numMidiChannels + static_cast<int> (s);
            auto* auxL = partLeftPtrs[static_cast<size_t> (auxCh)];
            auto* auxR = partRightPtrs[static_cast<size_t> (auxCh)];

            // Step 1: Note EQ
            if (slot.noteBassActive)
            {
                slot.noteBassFilters[0].processSamples (auxL, chunkSize);
                slot.noteBassFilters[1].processSamples (auxR, chunkSize);
            }
            if (slot.noteTrebleActive)
            {
                slot.noteTrebleFilters[0].processSamples (auxL, chunkSize);
                slot.noteTrebleFilters[1].processSamples (auxR, chunkSize);
            }

            // Step 2: Part EQ (in series)
            if (slot.partBassActive)
            {
                slot.partBassFilters[0].processSamples (auxL, chunkSize);
                slot.partBassFilters[1].processSamples (auxR, chunkSize);
            }
            if (slot.partTrebleActive)
            {
                slot.partTrebleFilters[0].processSamples (auxL, chunkSize);
                slot.partTrebleFilters[1].processSamples (auxR, chunkSize);
            }

            // Silence detection for slot reclamation
            const auto rangeL = juce::FloatVectorOperations::findMinAndMax (auxL, chunkSize);
            const auto rangeR = juce::FloatVectorOperations::findMinAndMax (auxR, chunkSize);
            if (std::max (std::abs (rangeL.getStart()), std::abs (rangeL.getEnd())) < 1e-5f
                && std::max (std::abs (rangeR.getStart()), std::abs (rangeR.getEnd())) < 1e-5f)
            {
                slot.silentBlocks++;
                if (slot.silentBlocks > 8)
                    slot.active = false;
            }
            else
            {
                slot.silentBlocks = 0;
            }
        }

        // 2. Variation Effect (Insertion or System)
        variationInputBuffer.clear (0, 0, chunkSize);
        variationInputBuffer.clear (1, 0, chunkSize);
        variationOutputBuffer.clear (0, 0, chunkSize);
        variationOutputBuffer.clear (1, 0, chunkSize);

        if (varIsInsertion && juce::isPositiveAndBelow (varTargetPart, numMidiChannels))
        {
            const auto partIdx = static_cast<size_t> (varTargetPart);
            variationProcessor.setModulationInputs (channelModWheelNorm[partIdx],
                                                    channelPitchBendNorm[partIdx],
                                                    channelAftertouchNorm[partIdx],
                                                    channelAc1Norm[partIdx],
                                                    channelAc2Norm[partIdx]);

            variationInputBuffer.copyFrom (0, 0, multiPartBuffer, static_cast<int> (partIdx * 2), 0, chunkSize);
            variationInputBuffer.copyFrom (1, 0, multiPartBuffer, static_cast<int> (partIdx * 2 + 1), 0, chunkSize);

            for (size_t s = 0; s < numAuxDrumChannels; ++s)
            {
                const auto& slot = auxDrumSlots[s];
                if (slot.active && slot.sourceChannel == varTargetPart)
                {
                    const auto auxCh = numMidiChannels + static_cast<int> (s);
                    variationInputBuffer.addFrom (0, 0, multiPartBuffer, auxCh * 2, 0, chunkSize);
                    variationInputBuffer.addFrom (1, 0, multiPartBuffer, auxCh * 2 + 1, 0, chunkSize);
                }
            }

            variationProcessor.process (variationInputBuffer, variationOutputBuffer, chunkSize);

            // Replace channel audio with Variation output
            multiPartBuffer.copyFrom (static_cast<int> (partIdx * 2), 0, variationOutputBuffer, 0, 0, chunkSize);
            multiPartBuffer.copyFrom (static_cast<int> (partIdx * 2 + 1), 0, variationOutputBuffer, 1, 0, chunkSize);

            // Clear aux channel audio as it has been merged into variation output
            for (size_t s = 0; s < numAuxDrumChannels; ++s)
            {
                const auto& slot = auxDrumSlots[s];
                if (slot.active && slot.sourceChannel == varTargetPart)
                {
                    const auto auxCh = numMidiChannels + static_cast<int> (s);
                    juce::FloatVectorOperations::clear (partLeftPtrs[static_cast<size_t> (auxCh)], chunkSize);
                    juce::FloatVectorOperations::clear (partRightPtrs[static_cast<size_t> (auxCh)], chunkSize);
                }
            }
        }
        else if (isXg && variationParameters.connection == 1)
        {
            // System mode: Part 1 (channel 0) controllers modulate variation
            variationProcessor.setModulationInputs (channelModWheelNorm[0],
                                                    channelPitchBendNorm[0],
                                                    channelAftertouchNorm[0],
                                                    channelAc1Norm[0],
                                                    channelAc2Norm[0]);

            // System mode: accumulate variation sends from all parts
            for (int ch = 0; ch < numMidiChannels; ++ch)
            {
                const auto vSend = static_cast<float> (partParameters[static_cast<size_t> (ch)].variationSend) / 127.0f;
                if (vSend > 0.0f)
                {
                    variationInputBuffer.addFrom (0, 0, multiPartBuffer, ch * 2, 0, chunkSize, vSend);
                    variationInputBuffer.addFrom (1, 0, multiPartBuffer, ch * 2 + 1, 0, chunkSize, vSend);
                }
            }

            for (size_t s = 0; s < numAuxDrumChannels; ++s)
            {
                const auto& slot = auxDrumSlots[s];
                if (slot.active && juce::isPositiveAndBelow (slot.sourceChannel, static_cast<int> (numMidiChannels)))
                {
                    const auto partVarSend = static_cast<float> (partParameters[static_cast<size_t> (slot.sourceChannel)].variationSend) / 127.0f;
                    const auto effVarSend = slot.noteVarSendNorm * partVarSend;
                    if (effVarSend > 0.0f)
                    {
                        const auto auxCh = numMidiChannels + static_cast<int> (s);
                        variationInputBuffer.addFrom (0, 0, multiPartBuffer, auxCh * 2, 0, chunkSize, effVarSend);
                        variationInputBuffer.addFrom (1, 0, multiPartBuffer, auxCh * 2 + 1, 0, chunkSize, effVarSend);
                    }
                }
            }

            variationProcessor.process (variationInputBuffer, variationOutputBuffer, chunkSize);
        }

        // 3. Chorus Bus Accumulation & Processing
        chorusBusBuffer.clear (0, 0, chunkSize);
        chorusBusBuffer.clear (1, 0, chunkSize);

        for (int ch = 0; ch < numMidiChannels; ++ch)
        {
            const auto cSend = static_cast<float> (partParameters[static_cast<size_t> (ch)].chorusSend) / 127.0f;
            if (cSend > 0.0f)
            {
                chorusBusBuffer.addFrom (0, 0, multiPartBuffer, ch * 2, 0, chunkSize, cSend);
                chorusBusBuffer.addFrom (1, 0, multiPartBuffer, ch * 2 + 1, 0, chunkSize, cSend);
            }
        }
        for (size_t s = 0; s < numAuxDrumChannels; ++s)
        {
            const auto& slot = auxDrumSlots[s];
            if (slot.active && juce::isPositiveAndBelow (slot.sourceChannel, static_cast<int> (numMidiChannels)))
            {
                const auto partChorusSend = static_cast<float> (partParameters[static_cast<size_t> (slot.sourceChannel)].chorusSend) / 127.0f;
                const auto effChorusSend = slot.noteChorusSendNorm * partChorusSend;
                if (effChorusSend > 0.0f)
                {
                    const auto auxCh = numMidiChannels + static_cast<int> (s);
                    chorusBusBuffer.addFrom (0, 0, multiPartBuffer, auxCh * 2, 0, chunkSize, effChorusSend);
                    chorusBusBuffer.addFrom (1, 0, multiPartBuffer, auxCh * 2 + 1, 0, chunkSize, effChorusSend);
                }
            }
        }

        if (isXg && variationParameters.connection == 1)
        {
            const auto varToChorus = static_cast<float> (variationParameters.sendToChorus) / 127.0f;
            if (varToChorus > 0.0f)
            {
                chorusBusBuffer.addFrom (0, 0, variationOutputBuffer, 0, 0, chunkSize, varToChorus);
                chorusBusBuffer.addFrom (1, 0, variationOutputBuffer, 1, 0, chunkSize, varToChorus);
            }
        }

        juce::dsp::AudioBlock<float> chorusBlock (chorusBusBuffer);
        auto subChorusBlock = chorusBlock.getSubBlock (0, static_cast<size_t> (chunkSize));
        juce::dsp::ProcessContextReplacing<float> chorusContext (subChorusBlock);
        chorusProcessor.process (chorusContext);

        // 4. Reverb Bus Accumulation & Processing
        reverbBusBuffer.clear (0, 0, chunkSize);
        reverbBusBuffer.clear (1, 0, chunkSize);

        for (int ch = 0; ch < numMidiChannels; ++ch)
        {
            const auto rSend = static_cast<float> (partParameters[static_cast<size_t> (ch)].reverbSend) / 127.0f;
            if (rSend > 0.0f)
            {
                reverbBusBuffer.addFrom (0, 0, multiPartBuffer, ch * 2, 0, chunkSize, rSend);
                reverbBusBuffer.addFrom (1, 0, multiPartBuffer, ch * 2 + 1, 0, chunkSize, rSend);
            }
        }
        for (size_t s = 0; s < numAuxDrumChannels; ++s)
        {
            const auto& slot = auxDrumSlots[s];
            if (slot.active && juce::isPositiveAndBelow (slot.sourceChannel, static_cast<int> (numMidiChannels)))
            {
                const auto partReverbSend = static_cast<float> (partParameters[static_cast<size_t> (slot.sourceChannel)].reverbSend) / 127.0f;
                const auto effReverbSend = slot.noteReverbSendNorm * partReverbSend;
                if (effReverbSend > 0.0f)
                {
                    const auto auxCh = numMidiChannels + static_cast<int> (s);
                    reverbBusBuffer.addFrom (0, 0, multiPartBuffer, auxCh * 2, 0, chunkSize, effReverbSend);
                    reverbBusBuffer.addFrom (1, 0, multiPartBuffer, auxCh * 2 + 1, 0, chunkSize, effReverbSend);
                }
            }
        }

        const auto chorusToReverb = isXg ? (static_cast<float> (chorusParameters.sendToReverb) / 127.0f) : 0.0f;
        if (chorusToReverb > 0.0f)
        {
            reverbBusBuffer.addFrom (0, 0, chorusBusBuffer, 0, 0, chunkSize, chorusToReverb);
            reverbBusBuffer.addFrom (1, 0, chorusBusBuffer, 1, 0, chunkSize, chorusToReverb);
        }
        if (isXg && variationParameters.connection == 1)
        {
            const auto varToReverb = static_cast<float> (variationParameters.sendToReverb) / 127.0f;
            if (varToReverb > 0.0f)
            {
                reverbBusBuffer.addFrom (0, 0, variationOutputBuffer, 0, 0, chunkSize, varToReverb);
                reverbBusBuffer.addFrom (1, 0, variationOutputBuffer, 1, 0, chunkSize, varToReverb);
            }
        }

        juce::dsp::AudioBlock<float> reverbBlock (reverbBusBuffer);
        auto subReverbBlock = reverbBlock.getSubBlock (0, static_cast<size_t> (chunkSize));
        juce::dsp::ProcessContextReplacing<float> reverbContext (subReverbBlock);
        reverbProcessor.process (reverbContext);

        // 5. Master Summing
        juce::FloatVectorOperations::clear (curDestLeft, chunkSize);
        juce::FloatVectorOperations::clear (curDestRight, chunkSize);

        for (int ch = 0; ch < numMidiChannels; ++ch)
        {
            const auto dryLevel = static_cast<float> (partParameters[static_cast<size_t> (ch)].dryLevel) / 127.0f;
            juce::FloatVectorOperations::addWithMultiply (curDestLeft,
                                                          partLeftPtrs[static_cast<size_t> (ch)],
                                                          dryLevel,
                                                          chunkSize);
            juce::FloatVectorOperations::addWithMultiply (curDestRight,
                                                          partRightPtrs[static_cast<size_t> (ch)],
                                                          dryLevel,
                                                          chunkSize);
        }
        for (size_t s = 0; s < numAuxDrumChannels; ++s)
        {
            const auto& slot = auxDrumSlots[s];
            if (slot.active && juce::isPositiveAndBelow (slot.sourceChannel, static_cast<int> (numMidiChannels)))
            {
                const auto dryLevel = static_cast<float> (partParameters[static_cast<size_t> (slot.sourceChannel)].dryLevel) / 127.0f;
                const auto auxCh = numMidiChannels + static_cast<int> (s);
                juce::FloatVectorOperations::addWithMultiply (curDestLeft,
                                                              partLeftPtrs[static_cast<size_t> (auxCh)],
                                                              dryLevel,
                                                              chunkSize);
                juce::FloatVectorOperations::addWithMultiply (curDestRight,
                                                              partRightPtrs[static_cast<size_t> (auxCh)],
                                                              dryLevel,
                                                              chunkSize);
            }
        }

        // Chorus Return
        const auto cReturn = isXg ? (static_cast<float> (chorusParameters.chorusReturn) / 64.0f)
                                  : (isGs ? (static_cast<float> (gsChorusParameters.level) / 127.0f) : 1.0f);
        if (cReturn > 0.0f)
        {
            const auto cPan = isXg ? (static_cast<float> (chorusParameters.chorusPan) / 127.0f) : 0.5f;
            const auto cGainL = cReturn * std::cos (cPan * 1.5707963f) * 1.4142f;
            const auto cGainR = cReturn * std::sin (cPan * 1.5707963f) * 1.4142f;
            juce::FloatVectorOperations::addWithMultiply (curDestLeft,
                                                          chorusBusBuffer.getReadPointer (0),
                                                          cGainL,
                                                          chunkSize);
            juce::FloatVectorOperations::addWithMultiply (curDestRight,
                                                          chorusBusBuffer.getReadPointer (1),
                                                          cGainR,
                                                          chunkSize);
        }

        // Reverb Return
        const auto rReturn = isXg ? (static_cast<float> (reverbParameters.reverbReturn) / 64.0f)
                                  : (isGs ? (static_cast<float> (gsReverbParameters.level) / 127.0f) : 1.0f);
        if (rReturn > 0.0f)
        {
            const auto rPan = isXg ? (static_cast<float> (reverbParameters.reverbPan) / 127.0f) : 0.5f;
            const auto rGainL = rReturn * std::cos (rPan * 1.5707963f) * 1.4142f;
            const auto rGainR = rReturn * std::sin (rPan * 1.5707963f) * 1.4142f;
            juce::FloatVectorOperations::addWithMultiply (curDestLeft,
                                                          reverbBusBuffer.getReadPointer (0),
                                                          rGainL,
                                                          chunkSize);
            juce::FloatVectorOperations::addWithMultiply (curDestRight,
                                                          reverbBusBuffer.getReadPointer (1),
                                                          rGainR,
                                                          chunkSize);
        }

        // Variation Return (System mode only)
        if (isXg && variationParameters.connection == 1)
        {
            const auto vReturn = static_cast<float> (variationParameters.varReturn) / 64.0f;
            if (vReturn > 0.0f)
            {
                const auto vPan = static_cast<float> (variationParameters.varPan) / 127.0f;
                const auto vGainL = vReturn * std::cos (vPan * 1.5707963f) * 1.4142f;
                const auto vGainR = vReturn * std::sin (vPan * 1.5707963f) * 1.4142f;
                juce::FloatVectorOperations::addWithMultiply (curDestLeft,
                                                              variationOutputBuffer.getReadPointer (0),
                                                              vGainL,
                                                              chunkSize);
                juce::FloatVectorOperations::addWithMultiply (curDestRight,
                                                              variationOutputBuffer.getReadPointer (1),
                                                              vGainR,
                                                              chunkSize);
            }
        }

        samplesRendered += chunkSize;
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
