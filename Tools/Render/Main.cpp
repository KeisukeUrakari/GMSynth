#include "Renderer.h"

#include <cmath>
#include <iomanip>
#include <iostream>
#include <string>

namespace
{
constexpr auto help = R"(Usage: gmsynth-render INPUT.mid --soundfont FONT.sf2 --output OUTPUT.wav
                      [--tail-bars N | --tail-seconds N]

Render one MIDI file to 48000 Hz, 16-bit PCM stereo WAV, without an audio device.
The default tail is one bar at the last note/pedal release's tempo and meter.
The whole tail fades linearly to silence. N must be finite and >= 0; decimals
are allowed. N=0 disables both tail and fade. Trailing MIDI rests are omitted.
Existing output files are never overwritten. Supports SMF 0/1 with PPQ timing.
SoundFont and MIDI behaviour follow the GMSynth engine's default Auto mode.

  --help             Show this help
  --soundfont PATH   Required SoundFont
  --output PATH      Required destination (parent directory must exist)
  --tail-bars N      Fade duration in bars (default: 1)
  --tail-seconds N   Fade duration in seconds (exclusive with --tail-bars)

Exit codes: 0 success, 2 invalid arguments, 1 input/render/output failure.
)";

struct Arguments
{
    std::string input, soundFont, output;
    gmsynth::render::Tail tail;
};

Arguments parse (int argc, char** argv)
{
    Arguments args;
    bool hasTail = false;
    for (int i = 1; i < argc; ++i)
    {
        const std::string option = argv[i];
        if (option == "--soundfont" || option == "--output" || option == "--tail-bars" || option == "--tail-seconds")
        {
            if (++i == argc) throw std::invalid_argument ("Missing value for " + option);
            const std::string value = argv[i];
            if (option == "--soundfont" || option == "--output")
            {
                auto& target = option == "--soundfont" ? args.soundFont : args.output;
                if (! target.empty()) throw std::invalid_argument ("Repeated option: " + option);
                target = value;
            }
            else
            {
                if (hasTail) throw std::invalid_argument ("Specify only one tail option, once.");
                hasTail = true;
                size_t used = 0;
                try { args.tail.value = std::stod (value, &used); }
                catch (const std::exception&) { throw std::invalid_argument ("Invalid tail value: " + value); }
                if (used != value.size() || ! std::isfinite (args.tail.value) || args.tail.value < 0)
                    throw std::invalid_argument ("Tail must be a finite number >= 0.");
                args.tail.unit = option == "--tail-bars" ? gmsynth::render::Tail::Unit::bars : gmsynth::render::Tail::Unit::seconds;
            }
        }
        else if (! option.empty() && option[0] == '-') throw std::invalid_argument ("Unknown option: " + option);
        else
        {
            if (! args.input.empty()) throw std::invalid_argument ("Specify exactly one MIDI input.");
            args.input = option;
        }
    }
    if (args.input.empty() || args.soundFont.empty() || args.output.empty())
        throw std::invalid_argument ("MIDI input, --soundfont and --output are required.");
    return args;
}
}

int main (int argc, char** argv)
{
    for (int i = 1; i < argc; ++i)
        if (std::string (argv[i]) == "--help") { std::cout << help; return 0; }
    Arguments args;
    try { args = parse (argc, argv); }
    catch (const std::invalid_argument& e)
    {
        std::cerr << "Error: " << e.what() << "\nUse --help for usage.\n";
        return 2;
    }
    try
    {
        const auto cwd = juce::File::getCurrentWorkingDirectory();
        const auto song = gmsynth::render::readSong (cwd.getChildFile (juce::String::fromUTF8 (args.input.c_str())), args.tail);
        const auto output = cwd.getChildFile (juce::String::fromUTF8 (args.output.c_str()));
        const auto result = gmsynth::render::renderSong (song,
            cwd.getChildFile (juce::String::fromUTF8 (args.soundFont.c_str())), output);
        std::cout << "Output: " << output.getFullPathName() << "\n48000 Hz / 16-bit PCM / stereo\n"
                  << std::fixed << std::setprecision (6)
                  << "Song: " << (double) song.endSample / gmsynth::render::sampleRate << " s\n"
                  << "Tail (linear fade): " << (double) song.tailSamples / gmsynth::render::sampleRate << " s\n"
                  << "Total: " << (double) result.totalSamples / gmsynth::render::sampleRate << " s\n";
        if (song.releasedAtEof) std::cout << "Notice: unresolved notes/pedals released at MIDI EOF.\n";
        if (result.clipped) std::cout << "Notice: audio exceeded PCM range and was clipped.\n";
        return 0;
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error: " << e.what() << '\n';
        return 1;
    }
}
