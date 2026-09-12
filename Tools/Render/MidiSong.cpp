#include "Renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace gmsynth::render
{
namespace
{
[[noreturn]] void invalid (const char* reason) { throw std::runtime_error (reason); }

struct Cursor
{
    const uint8_t* p;
    size_t remaining;
    uint8_t byte()
    {
        if (remaining == 0) invalid ("Truncated MIDI data.");
        --remaining;
        return *p++;
    }
    uint32_t integer (int n)
    {
        uint32_t result = 0;
        while (n-- > 0) result = (result << 8) | byte();
        return result;
    }
    uint32_t vlq()
    {
        uint32_t result = 0;
        for (int n = 0; n < 4; ++n)
        {
            const auto b = byte();
            result = (result << 7) | (b & 0x7f);
            if ((b & 0x80) == 0) return result;
        }
        invalid ("Invalid MIDI variable-length quantity.");
    }
    Cursor take (size_t size)
    {
        if (size > remaining) invalid ("Truncated MIDI chunk/event.");
        Cursor result { p, size };
        p += size;
        remaining -= size;
        return result;
    }
};

struct TickEvent
{
    int64_t tick;
    juce::MidiMessage message;
};

void readTrack (Cursor track, std::vector<TickEvent>& events)
{
    int64_t tick = 0;
    uint8_t running = 0;
    bool ended = false;
    std::vector<uint8_t> sysex;
    while (track.remaining != 0)
    {
        tick += track.vlq();
        auto status = track.byte();
        std::vector<uint8_t> bytes;
        if (status < 0x80)
        {
            if (running == 0) invalid ("MIDI running status without a channel status.");
            bytes = { running, status };
            status = running;
        }
        else
        {
            bytes.push_back (status);
        }

        if (status < 0xf0)
        {
            running = status;
            const auto length = (status >> 4) == 0xc || (status >> 4) == 0xd ? 2u : 3u;
            while (bytes.size() < length) bytes.push_back (track.byte());
            for (size_t i = 1; i < bytes.size(); ++i)
                if (bytes[i] >= 0x80) invalid ("Invalid MIDI channel data byte.");
            events.push_back ({ tick, juce::MidiMessage (bytes.data(), (int) bytes.size()) });
        }
        else if (status == 0xff)
        {
            running = 0;
            const auto* start = track.p - 1;
            const auto type = track.byte();
            const auto size = track.vlq();
            const auto payload = track.take (size);
            if (type >= 0x80) invalid ("Invalid MIDI meta-event type.");
            if (type == 0x51 && (size != 3 || (payload.p[0] | payload.p[1] | payload.p[2]) == 0))
                invalid ("Invalid MIDI tempo.");
            if (type == 0x58 && (size != 4 || payload.p[0] == 0 || payload.p[1] > 7))
                invalid ("Invalid MIDI time signature.");
            if (type == 0x2f)
            {
                if (size != 0 || track.remaining != 0) invalid ("Invalid MIDI End of Track.");
                ended = true;
            }
            events.push_back ({ tick, juce::MidiMessage (start, (int) (track.p - start)) });
        }
        else if (status == 0xf0 || status == 0xf7)
        {
            running = 0;
            if (status == 0xf0)
            {
                if (! sysex.empty()) invalid ("Unfinished MIDI SysEx message.");
                sysex.push_back (0xf0);
            }
            else if (sysex.empty())
                invalid ("Standalone SMF F7 escape events are not supported.");
            const auto payload = track.take (track.vlq());
            for (size_t i = 0; i < payload.remaining; ++i)
                if (payload.p[i] >= 0x80 && ! (payload.p[i] == 0xf7 && i + 1 == payload.remaining))
                    invalid ("Invalid MIDI SysEx data byte.");
            sysex.insert (sysex.end(), payload.p, payload.p + payload.remaining);
            if (sysex.back() == 0xf7)
            {
                events.push_back ({ tick, juce::MidiMessage (sysex.data(), (int) sysex.size()) });
                sysex.clear();
            }
        }
        else invalid ("Unsupported event in standard MIDI file.");
    }
    if (! ended || ! sysex.empty()) invalid ("Missing End of Track or unfinished SysEx.");
}

bool isReset (const juce::MidiMessage& m)
{
    if (! m.isSysEx()) return false;
    const auto* d = m.getSysExData();
    const auto n = m.getSysExDataSize();
    if (n >= 4 && d[0] == 0x7e && d[2] == 9 && (d[3] == 1 || d[3] == 3)) return true;
    if (n >= 7 && d[0] == 0x43 && (d[1] & 0xf0) == 0x10 && d[2] == 0x4c
        && d[3] == 0 && d[4] == 0 && d[5] == 0x7e && d[6] == 0) return true;
    if (n >= 9 && d[0] == 0x41 && d[2] == 0x42 && d[3] == 0x12
        && (d[4] == 0x40 || d[4] == 0) && d[5] == 0 && d[6] == 0x7f && d[7] == 0)
    {
        int sum = 0;
        for (int i = 4; i < n; ++i) sum += d[i];
        return (sum & 0x7f) == 0;
    }
    return false;
}

struct Note { int pitch; bool key = true; bool sostenuto = false; };
struct Channel
{
    bool sustain = false, sostenuto = false;
    std::vector<Note> notes;
    void release()
    {
        notes.erase (std::remove_if (notes.begin(), notes.end(), [this] (const Note& n)
        { return ! n.key && ! sustain && ! (sostenuto && n.sostenuto); }), notes.end());
    }
};

class NoteTracker
{
public:
    bool sawNote = false;
    std::array<Channel, 16> channels;
    bool active() const
    {
        return std::any_of (channels.begin(), channels.end(), [] (const Channel& c) { return ! c.notes.empty(); });
    }
    void accept (const juce::MidiMessage& m)
    {
        if (isReset (m)) { channels = {}; return; }
        if (m.getChannel() < 1 || m.getChannel() > 16) return;
        auto& c = channels[(size_t) m.getChannel() - 1];
        if (m.isNoteOn())
        {
            sawNote = true;
            c.notes.push_back ({ m.getNoteNumber() });
        }
        else if (m.isNoteOff())
        {
            // Pair overlapping instances in arrival order, without inventing note-offs.
            auto it = std::find_if (c.notes.begin(), c.notes.end(), [&] (const Note& n)
            { return n.key && n.pitch == m.getNoteNumber(); });
            if (it != c.notes.end()) it->key = false;
        }
        else if (m.isController())
        {
            const int cc = m.getControllerNumber(), v = m.getControllerValue();
            if (cc == 64) c.sustain = v >= 64;
            if (cc == 66)
            {
                const bool down = v >= 64;
                if (down && ! c.sostenuto)
                    for (auto& n : c.notes) n.sostenuto = n.key;
                if (! down)
                    for (auto& n : c.notes) n.sostenuto = false;
                c.sostenuto = down;
            }
            if (cc == 120) c.notes.clear();
            if (cc == 121)
            {
                c.sustain = c.sostenuto = false;
                for (auto& n : c.notes) n.sostenuto = false;
            }
            if (cc >= 123 && cc <= 127)
                for (auto& n : c.notes) n.key = false;
        }
        c.release();
    }
};

int64_t samples (long double seconds)
{
    const auto count = seconds * sampleRate;
    if (! std::isfinite (count) || count < 0 || count >= (long double) std::numeric_limits<int64_t>::max())
        invalid ("MIDI/tail duration cannot be represented in sample frames.");
    return (int64_t) std::llround (count);
}
}

Song readSong (const juce::File& file, Tail tail)
{
    if (file.getSize() > 200 * 1024 * 1024) invalid ("MIDI file exceeds 200 MiB.");
    juce::MemoryBlock data;
    if (! file.loadFileAsData (data)) invalid ("Cannot read MIDI file.");
    return readSong (data.getData(), data.getSize(), tail);
}

Song readSong (const void* data, size_t size, Tail tail)
{
    if (size > 200 * 1024 * 1024) invalid ("MIDI file exceeds 200 MiB.");
    if (! std::isfinite (tail.value) || tail.value < 0) invalid ("Invalid tail duration.");
    Cursor file { static_cast<const uint8_t*> (data), size };
    if (file.integer (4) != 0x4d546864) invalid ("Expected an SMF MThd header.");
    auto header = file.take (file.integer (4));
    const auto format = header.integer (2), tracks = header.integer (2), ppq = header.integer (2);
    if (format > 1) invalid ("Only SMF Format 0 and 1 are supported.");
    if (tracks == 0 || (format == 0 && tracks != 1)) invalid ("Invalid MIDI track count.");
    if (ppq == 0 || (ppq & 0x8000) != 0) invalid ("Only PPQ MIDI timing is supported (no SMPTE).");
    std::vector<TickEvent> events;
    uint32_t tracksRead = 0;
    while (file.remaining != 0)
    {
        const auto type = file.integer (4);
        auto chunk = file.take (file.integer (4));
        if (type == 0x4d54726b)
        {
            if (++tracksRead > tracks) invalid ("More MIDI tracks than declared in the header.");
            readTrack (chunk, events);
        }
        else if (type == 0x4d546864)
            invalid ("Unexpected duplicate MIDI header.");
        // Extension chunks (including XFIH/XFKM) do not count as MIDI tracks.
        // Cursor::take validates their declared length even when we skip them.
    }
    if (tracksRead != tracks) invalid ("Fewer MIDI tracks than declared in the header.");
    std::stable_sort (events.begin(), events.end(), [] (const auto& a, const auto& b) { return a.tick < b.tick; });

    Song song;
    NoteTracker tracker;
    size_t endEventCount = 0;
    int64_t previousTick = 0;
    long double time = 0, tempo = 0.5L, endTime = 0, endBar = 2;
    int numerator = 4, denominator = 4;
    // Process timestamp groups so tempo/signature changes at the release point apply to the tail.
    for (size_t i = 0; i < events.size();)
    {
        const auto tick = events[i].tick;
        time += (tick - previousTick) * tempo / ppq;
        previousTick = tick;
        const auto frame = samples (time);
        bool relevant = false;
        do
        {
            const auto& m = events[i].message;
            const bool before = tracker.active();
            tracker.accept (m);
            relevant = relevant || before || tracker.active() || m.isNoteOn();
            if (m.isTempoMetaEvent()) tempo = m.getTempoSecondsPerQuarterNote();
            if (m.isTimeSignatureMetaEvent()) m.getTimeSignatureInfo (numerator, denominator);
            if (! m.isMetaEvent()) song.events.push_back ({ frame, m });
            ++i;
        } while (i < events.size() && events[i].tick == tick);
        if (relevant)
        {
            endTime = time;
            endBar = tempo * numerator * 4 / denominator;
            endEventCount = song.events.size();
        }
    }
    if (! tracker.sawNote) invalid ("MIDI file contains no sounding Note On events.");
    song.releasedAtEof = tracker.active();
    if (song.releasedAtEof)
    {
        endTime = time;
        endBar = tempo * numerator * 4 / denominator;
        endEventCount = song.events.size();
    }
    song.endSample = samples (endTime);
    song.tailSamples = samples (tail.unit == Tail::Unit::bars ? endBar * tail.value : tail.value);
    // Classic PCM RIFF WAV, with room for headers. Check only the retained duration,
    // allowing a long discarded End-of-Track rest without producing a huge WAV.
    constexpr int64_t maxFrames = (0xffffffffLL - 4096) / 4;
    if (song.endSample > maxFrames || song.tailSamples > maxFrames - song.endSample)
        invalid ("Duration exceeds the supported RIFF WAV size (4 GiB).");
    // Trim by original timestamp groups, not rounded sample times. A later event
    // can round to the same sample as the final release at high MIDI resolutions.
    song.events.resize (endEventCount);
    if (song.releasedAtEof)
        for (int channel = 1; channel <= 16; ++channel)
        {
            song.events.push_back ({ song.endSample, juce::MidiMessage::controllerEvent (channel, 64, 0) });
            song.events.push_back ({ song.endSample, juce::MidiMessage::controllerEvent (channel, 66, 0) });
            song.events.push_back ({ song.endSample, juce::MidiMessage::allNotesOff (channel) });
        }
    return song;
}
}
