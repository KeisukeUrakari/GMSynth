#include "Renderer.h"

#include <cmath>
#include <functional>
#include <iostream>
#include <limits>

using namespace gmsynth::render;
namespace
{
using Bytes = std::vector<uint8_t>;
void check (bool condition, const char* message)
{
    if (! condition) throw std::runtime_error (message);
}
void be (Bytes& b, uint32_t v, int n)
{
    for (int i = n - 1; i >= 0; --i) b.push_back ((uint8_t) (v >> (8 * i)));
}
void vlq (Bytes& b, uint32_t value)
{
    uint8_t bytes[4];
    int count = 0;
    bytes[count++] = (uint8_t) (value & 127);
    while ((value >>= 7) != 0) bytes[count++] = (uint8_t) ((value & 127) | 128);
    while (count != 0) b.push_back (bytes[--count]);
}
void event (Bytes& b, int delta, std::initializer_list<uint8_t> bytes)
{
    vlq (b, (uint32_t) delta);
    b.insert (b.end(), bytes);
}
void end (Bytes& b, int delta = 0) { event (b, delta, { 0xff, 0x2f, 0 }); }
Bytes smf (std::vector<Bytes> tracks, int format = 0, int ppq = 480)
{
    Bytes result;
    be (result, 0x4d546864, 4); be (result, 6, 4);
    be (result, (uint32_t) format, 2); be (result, (uint32_t) tracks.size(), 2); be (result, (uint32_t) ppq, 2);
    for (const auto& track : tracks)
    {
        be (result, 0x4d54726b, 4); be (result, (uint32_t) track.size(), 4);
        result.insert (result.end(), track.begin(), track.end());
    }
    return result;
}
Song read (const Bytes& b, Tail tail = {}) { return readSong (b.data(), b.size(), tail); }
void rejects (const Bytes& b, Tail tail = {})
{
    bool rejected = false;
    try { read (b, tail); } catch (const std::runtime_error&) { rejected = true; }
    check (rejected, "Expected input rejection");
}
Bytes simple()
{
    Bytes track;
    event (track, 0, { 0x90, 60, 100 });
    event (track, 480, { 0x80, 60, 0 });
    end (track, 4800);
    return smf ({ track });
}
}

int main()
{
    int failures = 0, count = 0;
    const auto test = [&] (const char* name, const std::function<void()>& body)
    {
        ++count;
        try { body(); std::cout << "PASS " << name << '\n'; }
        catch (const std::exception& e) { ++failures; std::cerr << "FAIL " << name << ": " << e.what() << '\n'; }
    };

    test ("default tail and trailing rest", []
    {
        const auto song = read (simple());
        check (song.endSample == 24000 && song.tailSamples == 96000, "Wrong default duration");
        check (! song.releasedAtEof && song.events.size() == 2, "Unexpected EOF correction");
        check (read (simple(), { Tail::Unit::seconds, 3.5 }).tailSamples == 168000, "Seconds tail");
        check (read (simple(), { Tail::Unit::bars, 0.5 }).tailSamples == 48000, "Fractional bars");
        check (read (simple(), { Tail::Unit::seconds, 0 }).tailSamples == 0, "Zero tail");
    });
    test ("tempo/meter at release, later changes ignored", []
    {
        Bytes conductor, notes;
        event (conductor, 480, { 0xff, 0x51, 3, 0x0f, 0x42, 0x40 }); // 60 BPM
        event (conductor, 480, { 0xff, 0x58, 4, 6, 3, 24, 8 }); // 6/8 at release
        event (conductor, 480, { 0xff, 0x58, 4, 3, 2, 24, 8 });
        event (conductor, 0, { 0xff, 0x51, 3, 7, 0xa1, 0x20 });
        end (conductor);
        event (notes, 0, { 0x90, 60, 100 });
        event (notes, 960, { 0x80, 60, 0 }); end (notes, 1920);
        const auto song = read (smf ({ conductor, notes }, 1));
        check (song.endSample == 72000 && song.tailSamples == 144000, "Tempo/meter mapping");
        Bytes triple;
        event (triple, 0, { 0xff, 0x58, 4, 3, 2, 24, 8 });
        event (triple, 0, { 0x90, 60, 100 }); event (triple, 480, { 0x80, 60, 0 }); end (triple);
        check (read (smf ({ triple })).tailSamples == 72000, "3/4 tail");
    });
    test ("leading and internal rests", []
    {
        Bytes t;
        event (t, 480, { 0x90, 60, 100 }); event (t, 480, { 0x80, 60, 0 });
        event (t, 960, { 0x90, 64, 100 }); event (t, 480, { 0x80, 64, 0 }); end (t, 4800);
        const auto song = read (smf ({ t }));
        check (song.events.front().sample == 24000 && song.endSample == 120000, "Rests were trimmed");
    });
    test ("sustain and velocity-zero note off", []
    {
        Bytes t;
        event (t, 0, { 0x90, 60, 100 }); event (t, 0, { 0xb0, 64, 127 });
        event (t, 480, { 0x90, 60, 0 }); event (t, 480, { 0xb0, 64, 0 });
        event (t, 480, { 0xb0, 64, 127 }); event (t, 480, { 0xb0, 64, 0 }); end (t);
        check (read (smf ({ t })).endSample == 48000, "Pedal without held notes extended song");
    });
    test ("sostenuto latch and overlapping notes", []
    {
        Bytes t;
        event (t, 0, { 0x90, 60, 100 }); event (t, 0, { 0xb0, 66, 127 });
        event (t, 0, { 0x90, 60, 100 });
        event (t, 480, { 0x80, 60, 0 }); event (t, 480, { 0xb0, 66, 0 });
        event (t, 480, { 0x80, 60, 0 }); end (t, 480);
        check (read (smf ({ t })).endSample == 72000, "Overlap pairing/latch failed");
        Bytes unlatched;
        event (unlatched, 0, { 0xb0, 66, 127 }); event (unlatched, 0, { 0x90, 60, 100 });
        event (unlatched, 480, { 0x80, 60, 0 }); end (unlatched, 480);
        check (read (smf ({ unlatched })).endSample == 24000, "Sostenuto latched a later note");
    });
    test ("controllers releasing notes", []
    {
        for (const int cc : { 120, 121, 123, 124, 125, 126, 127 })
        {
            Bytes t;
            event (t, 0, { 0x90, 60, 100 });
            if (cc == 121)
            {
                event (t, 0, { 0xb0, 64, 127 }); event (t, 240, { 0x80, 60, 0 });
                event (t, 240, { 0xb0, (uint8_t) cc, 0 });
            }
            else event (t, 480, { 0xb0, (uint8_t) cc, 0 });
            end (t, 480);
            check (read (smf ({ t })).endSample == 24000, "Controller release failed");
        }
        Bytes t;
        event (t, 0, { 0x90, 60, 100 }); event (t, 0, { 0xb0, 64, 127 });
        event (t, 480, { 0xb0, 123, 0 }); event (t, 480, { 0xb0, 64, 0 }); end (t);
        check (read (smf ({ t })).endSample == 48000, "All Notes Off must respect sustain");
    });
    test ("GM GS XG reset", []
    {
        for (const Bytes reset : { Bytes { 0xf0, 5, 0x7e, 0x7f, 9, 1, 0xf7 },
                                  Bytes { 0xf0, 10, 0x41, 0x10, 0x42, 0x12, 0x40, 0, 0x7f, 0, 0x41, 0xf7 },
                                  Bytes { 0xf0, 8, 0x43, 0x10, 0x4c, 0, 0, 0x7e, 0, 0xf7 } })
        {
            Bytes t;
            event (t, 0, { 0x90, 60, 100 }); vlq (t, 480); t.insert (t.end(), reset.begin(), reset.end()); end (t, 480);
            const auto song = read (smf ({ t }));
            check (song.endSample == 24000 && ! song.releasedAtEof, "Reset did not release");
        }
    });
    test ("EOF correction", []
    {
        Bytes t;
        event (t, 0, { 0x90, 60, 100 }); end (t, 960);
        const auto song = read (smf ({ t }));
        check (song.releasedAtEof && song.endSample == 48000 && song.events.size() == 49, "EOF correction missing");
        check (song.events.back().sample == song.endSample, "Incorrect correction time");
    });
    test ("event order, running status and sample boundary", []
    {
        Bytes a, b;
        event (a, 0, { 0xc0, 5 }); event (a, 0, { 0x90, 60, 100 });
        event (a, 512, { 60, 0 }); event (a, 0, { 60, 100 }); event (a, 0, { 0x80, 60, 0 }); end (a);
        event (b, 0, { 0xb0, 10, 32 }); end (b);
        const auto song = read (smf ({ a, b }, 1, 24000)); // one tick = one sample
        check (song.events[0].message.isProgramChange() && song.events[1].message.isNoteOn()
               && song.events[2].message.isController(), "Track order changed");
        check (song.events[3].sample == 512 && song.events[3].message.isNoteOff()
               && song.events[4].message.isNoteOn() && song.events[5].message.isNoteOff(), "Same-tick order changed");
    });
    test ("split SysEx", []
    {
        Bytes t;
        event (t, 0, { 0xf0, 2, 0x7e, 0x7f }); event (t, 0, { 0xf7, 3, 9, 1, 0xf7 });
        event (t, 0, { 0x90, 60, 100 }); event (t, 480, { 0x80, 60, 0 }); end (t);
        const auto song = read (smf ({ t }));
        check (song.events[0].message.isSysEx() && song.events[0].message.getSysExDataSize() == 4, "SysEx not reassembled");
    });
    test ("trim before sample rounding and long discarded rest", []
    {
        Bytes t;
        event (t, 0, { 0xff, 0x51, 3, 0, 0, 1 });
        event (t, 0, { 0x90, 60, 100 }); event (t, 1, { 0x80, 60, 0 });
        event (t, 1, { 0xb0, 7, 0 }); end (t);
        check (read (smf ({ t })).events.size() == 2, "Rounded trailing event was retained");
        Bytes longRest;
        event (longRest, 0, { 0x90, 60, 100 }); event (longRest, 480, { 0x80, 60, 0 });
        end (longRest, 100000000);
        check (read (smf ({ longRest })).endSample == 24000, "Long discarded rest rejected/retained");
    });
    test ("extension chunks and malformed chunk boundaries", []
    {
        Bytes extension;
        be (extension, 0x58464948, 4); be (extension, 3, 4);
        extension.insert (extension.end(), { 1, 2, 3 }); // No RIFF-style alignment padding in SMF.
        be (extension, 0x58464b4d, 4); be (extension, 0, 4);
        const auto original = simple();
        auto extended = original;
        extended.insert (extended.end(), extension.begin(), extension.end());
        auto beforeTrack = original;
        beforeTrack.insert (beforeTrack.begin() + 14, extension.begin(), extension.end());
        for (const auto& bytes : { extended, beforeTrack })
        {
            const auto song = read (bytes), expected = read (original);
            check (song.endSample == expected.endSample && song.tailSamples == expected.tailSamples
                   && song.events.size() == expected.events.size(), "Extension changed playback");
        }
        auto truncated = extended; truncated.pop_back(); rejects (truncated);
        auto oversized = original;
        be (oversized, 0x58464948, 4); be (oversized, 100, 4); rejects (oversized);
        auto extraTrack = original;
        extraTrack.insert (extraTrack.end(), original.begin() + 14, original.end()); rejects (extraTrack);
        auto missingTrack = original; missingTrack.resize (14); rejects (missingTrack);
        auto duplicateHeader = original;
        duplicateHeader.insert (duplicateHeader.end(), original.begin(), original.begin() + 14); rejects (duplicateHeader);
        auto garbage = original; garbage.push_back (0); rejects (garbage);
    });
    test ("invalid and unsupported input", []
    {
        rejects ({});
        auto truncated = simple(); truncated.pop_back(); rejects (truncated);
        Bytes noNotes; end (noNotes); rejects (smf ({ noNotes }));
        rejects (smf ({ noNotes }, 2)); rejects (smf ({ noNotes }, 0, 0xe728));
        rejects (smf ({ noNotes }, 0, 0));
        Bytes broken { 0, 0x90, 60 }; rejects (smf ({ broken }));
        Bytes badVlq { 0x80, 0x80, 0x80, 0x80, 0 }; rejects (smf ({ badVlq }));
        Bytes badTempo; event (badTempo, 0, { 0xff, 0x51, 3, 0, 0, 0 }); end (badTempo); rejects (smf ({ badTempo }));
        rejects (simple(), { Tail::Unit::seconds, -1 });
        rejects (simple(), { Tail::Unit::seconds, std::numeric_limits<double>::infinity() });
        rejects (simple(), { Tail::Unit::seconds, 1e100 });
    });
    test ("linear fade across block boundaries", []
    {
        constexpr int endSample = 509, tail = 777, total = endSample + tail;
        for (int position = 0; position < total; position += blockSize)
        {
            juce::AudioBuffer<float> audio (2, std::min (blockSize, total - position));
            for (int i = 0; i < audio.getNumSamples(); ++i)
            { audio.setSample (0, i, 0.5f); audio.setSample (1, i, -0.25f); }
            applyTailFade (audio, position, endSample, tail);
            for (int i = 0; i < audio.getNumSamples(); ++i)
            {
                const int offset = position + i - endSample;
                const float expected = offset < 0 ? 1.0f : (float) (tail - 1 - offset) / (tail - 1);
                check (std::abs (audio.getSample (0, i) - 0.5f * expected) < 1e-7f, "Incorrect linear gain");
                check (audio.getSample (0, i) == -2 * audio.getSample (1, i), "Stereo balance changed");
            }
        }
        juce::AudioBuffer<float> audio (2, 1);
        audio.setSample (0, 0, 1); applyTailFade (audio, 0, 0, 0);
        check (audio.getSample (0, 0) == 1, "Zero tail modified samples");
        applyTailFade (audio, 0, 0, 1);
        check (audio.getSample (0, 0) == 0, "One-sample tail not silent");
    });
    std::cout << count - failures << '/' << count << " tests passed\n";
    return failures == 0 ? 0 : 1;
}
