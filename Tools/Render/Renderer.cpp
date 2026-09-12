#include "Renderer.h"
#include "FluidSynthEngine.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <unistd.h>

namespace gmsynth::render
{
void applyTailFade (juce::AudioBuffer<float>& buffer, int64_t firstSample,
                    int64_t endSample, int64_t tailSamples)
{
    if (tailSamples <= 0) return;
    for (int i = 0; i < buffer.getNumSamples(); ++i)
    {
        const auto offset = firstSample + i - endSample;
        if (offset < 0) continue;
        const float gain = tailSamples == 1 ? 0.0f
            : (float) std::clamp (1.0 - (double) offset / (double) (tailSamples - 1), 0.0, 1.0);
        for (int c = 0; c < buffer.getNumChannels(); ++c)
            buffer.setSample (c, i, buffer.getSample (c, i) * gain);
    }
}

namespace
{
// Keep the actual stream alive after writer destruction, which finalises the WAV header,
// so even errors in the final header/flush are checked before publishing the file.
class BorrowedOutput final : public juce::OutputStream
{
public:
    explicit BorrowedOutput (juce::FileOutputStream& s) : stream (s) {}
    void flush() override { stream.flush(); }
    juce::int64 getPosition() override { return stream.getPosition(); }
    bool setPosition (juce::int64 p) override { return stream.setPosition (p); }
    bool write (const void* data, size_t size) override { return stream.write (data, size); }
private:
    juce::FileOutputStream& stream;
};

struct ScratchFile
{
    juce::File file;
    explicit ScratchFile (const juce::File& target)
    {
        const auto name = target.getParentDirectory().getChildFile (".gmsynth-render-XXXXXX").getFullPathName();
        std::vector<char> path (name.toRawUTF8(), name.toRawUTF8() + name.getNumBytesAsUTF8() + 1);
        const int fd = mkstemp (path.data());
        if (fd < 0) throw std::runtime_error ("Cannot create temporary output: " + std::string (std::strerror (errno)));
        ::close (fd);
        file = juce::File (juce::String::fromUTF8 (path.data()));
    }
    ~ScratchFile() { file.deleteFile(); }
};
}

RenderResult renderSong (const Song& song, const juce::File& soundFont, const juce::File& output)
{
    if (output.exists() || output.isSymbolicLink()) throw std::runtime_error ("Output already exists; refusing to overwrite it.");
    if (! output.getParentDirectory().isDirectory()) throw std::runtime_error ("Output directory does not exist.");

    FluidSynthEngine engine;
    engine.prepare (sampleRate, blockSize);
    const auto loaded = engine.loadSoundFont (soundFont);
    if (loaded.failed()) throw std::runtime_error (loaded.getErrorMessage().toStdString());

    ScratchFile temporary (output);
    auto fileStream = temporary.file.createOutputStream();
    if (! fileStream || fileStream->failedToOpen()) throw std::runtime_error ("Cannot open temporary WAV output.");
    std::unique_ptr<juce::OutputStream> stream = std::make_unique<BorrowedOutput> (*fileStream);
    juce::WavAudioFormat format;
    auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
        .withSampleRate (sampleRate).withNumChannels (2).withBitsPerSample (16));
    if (! writer) throw std::runtime_error ("Cannot create PCM WAV writer.");

    RenderResult result { song.endSample + song.tailSamples, false };
    size_t nextEvent = 0;
    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;
    for (int64_t position = 0; position < result.totalSamples;)
    {
        const int count = (int) std::min<int64_t> (blockSize, result.totalSamples - position);
        buffer.setSize (2, count, false, false, true);
        midi.clear();
        while (nextEvent < song.events.size() && song.events[nextEvent].sample < position + count)
        {
            const auto& event = song.events[nextEvent++];
            midi.addEvent (event.message, (int) (event.sample - position));
        }
        engine.processBlock (buffer, midi, nullptr, 0);
        applyTailFade (buffer, position, song.endSample, song.tailSamples);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < count; ++i)
            {
                auto& value = buffer.getWritePointer (c)[i];
                if (! std::isfinite (value)) throw std::runtime_error ("Synthesizer produced non-finite audio.");
                if (value > 1.0f || value < -1.0f)
                {
                    result.clipped = true;
                    value = std::clamp (value, -1.0f, 1.0f);
                }
            }
        if (! writer->writeFromAudioSampleBuffer (buffer, 0, count))
            throw std::runtime_error ("Failed to write WAV samples.");
        position += count;
    }
    if (! writer->flush()) throw std::runtime_error ("Failed to finalise WAV output.");
    writer.reset();
    fileStream->flush();
    if (fileStream->getStatus().failed()) throw std::runtime_error (fileStream->getStatus().getErrorMessage().toStdString());
    fileStream.reset();
    // Same-directory hard link atomically publishes without replacing a file created meanwhile.
    if (::link (temporary.file.getFullPathName().toRawUTF8(), output.getFullPathName().toRawUTF8()) != 0)
        throw std::runtime_error ("Cannot publish WAV output: " + std::string (std::strerror (errno)));
    return result;
}
}
