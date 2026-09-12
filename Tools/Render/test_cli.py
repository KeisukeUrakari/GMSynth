"""End-to-end tests. All MIDI, SoundFont and WAV data stays in a temporary directory."""
import math
from pathlib import Path
import resource
import signal
import struct
import subprocess
import sys
import tempfile
import wave


def chunk(kind, payload):
    return kind + struct.pack("<I", len(payload)) + payload + b"\0" * (len(payload) % 2)


def name(value):
    return value.encode("ascii").ljust(20, b"\0")


def soundfont(path):
    # A self-generated, looping sine sample with a two-second release.
    info = chunk(b"LIST", b"INFO" + chunk(b"ifil", struct.pack("<HH", 2, 1))
                 + chunk(b"isng", b"EMU8000\0") + chunk(b"INAM", b"GMSynth test\0\0"))
    samples = [round(12000 * math.sin(2 * math.pi * i / 100)) for i in range(1000)] + [0] * 46
    sdta = chunk(b"LIST", b"sdta" + chunk(b"smpl", struct.pack("<" + "h" * len(samples), *samples)))
    phdr = name("Sine") + struct.pack("<HHHIII", 0, 0, 0, 0, 0, 0)
    phdr += name("EOP") + struct.pack("<HHHIII", 0, 0, 1, 0, 0, 0)
    pbag = struct.pack("<HHHH", 0, 0, 1, 0)
    pgen = struct.pack("<HHHH", 41, 0, 0, 0)
    inst = name("Sine") + struct.pack("<H", 0) + name("EOI") + struct.pack("<H", 1)
    ibag = struct.pack("<HHHH", 0, 0, 3, 0)
    igen = struct.pack("<HhHHHHHH", 38, 1200, 54, 1, 53, 0, 0, 0)
    shdr = name("Sine") + struct.pack("<IIIIIBbHH", 0, 1000, 100, 900, 48000, 69, 0, 0, 1)
    shdr += name("EOS") + struct.pack("<IIIIIBbHH", 1000, 1000, 1000, 1000, 48000, 0, 0, 0, 1)
    pdta = chunk(b"LIST", b"pdta" + b"".join(chunk(k, v) for k, v in [
        (b"phdr", phdr), (b"pbag", pbag), (b"pmod", bytes(10)), (b"pgen", pgen),
        (b"inst", inst), (b"ibag", ibag), (b"imod", bytes(10)), (b"igen", igen), (b"shdr", shdr)]))
    path.write_bytes(chunk(b"RIFF", b"sfbk" + info + sdta + pdta))


def vlq(value):
    data = [value & 127]
    while value >> 7:
        value >>= 7
        data.insert(0, (value & 127) | 128)
    return bytes(data)


def midi(path, *, note_off=True, ppq=480, format_type=0, start=0, duration=480, rest=4800):
    track = vlq(start) + bytes([0x90, 69, 100])
    if note_off:
        track += vlq(duration) + bytes([0x80, 69, 0])
    track += vlq(rest) + bytes([0xFF, 0x2F, 0])
    path.write_bytes(b"MThd" + struct.pack(">IHHH", 6, format_type, 1, ppq)
                     + b"MTrk" + struct.pack(">I", len(track)) + track)


def read_wav(path, expected_frames):
    with wave.open(str(path), "rb") as wav:
        assert (wav.getframerate(), wav.getsampwidth(), wav.getnchannels(), wav.getcomptype()) == (48000, 2, 2, "NONE")
        assert wav.getnframes() == expected_frames, (wav.getnframes(), expected_frames)
        data = wav.readframes(wav.getnframes())
    pcm = struct.unpack("<" + "h" * (len(data) // 2), data)
    assert any(pcm), "WAV is entirely silent"
    return pcm


def main():
    executable = str(Path(sys.argv[1]).resolve())
    checks = 0
    with tempfile.TemporaryDirectory(prefix="gmsynth-render-tests-") as tmp:
        root = Path(tmp)
        sf = root / "音色 with spaces.sf2"
        mid = root / "演奏 with spaces.mid"
        soundfont(sf)
        midi(mid)

        def run(extra=(), *, expected=0, input_file=mid, output="result.wav", font=sf, **kwargs):
            nonlocal checks
            command = [executable, str(input_file), "--soundfont", str(font), "--output", str(root / output), *extra]
            result = subprocess.run(command, text=True, capture_output=True, timeout=30, **kwargs)
            assert result.returncode == expected, (command, result.returncode, result.stdout, result.stderr)
            checks += 1
            return result

        result = run()
        pcm = read_wav(root / "result.wav", 120000)
        assert pcm[-2:] == (0, 0)
        assert any(pcm[48000:60000]), "Release tail is silent"
        assert "Tail (linear fade): 2.000000" in result.stdout

        extended = root / "extended.mid"
        extended.write_bytes(mid.read_bytes() + b"XFIH" + struct.pack(">I", 3) + b"abc"
                             + b"XFKM" + struct.pack(">I", 2) + b"de")
        run(input_file=extended, output="extended.wav")
        assert read_wav(root / "extended.wav", 120000) == pcm, "Extension chunks changed audio"

        run(["--tail-bars", "0.5"], output="half.wav")
        assert read_wav(root / "half.wav", 72000)[-2:] == (0, 0)
        run(["--tail-seconds", "0.125"], output="seconds.wav")
        assert read_wav(root / "seconds.wav", 30000)[-2:] == (0, 0)
        run(["--tail-seconds", "0"], output="zero.wav")
        zero = read_wav(root / "zero.wav", 24000)
        assert zero == pcm[:48000], "Fade changed audio before note release"
        run(["--tail-seconds", str(1 / 48000)], output="one.wav")
        assert read_wav(root / "one.wav", 24001)[-2:] == (0, 0)

        boundary = root / "boundary.mid"
        midi(boundary, ppq=24000, start=512, duration=512, rest=4800)
        run(["--tail-seconds", "0.1"], input_file=boundary, output="boundary.wav")
        boundary_pcm = read_wav(root / "boundary.wav", 5824)
        assert not any(boundary_pcm[:1024]), "Note fired before its scheduled block"
        assert any(boundary_pcm[1024:2048]), "Boundary note did not sound"

        stuck = root / "stuck.mid"
        midi(stuck, note_off=False, rest=960)
        result = run(input_file=stuck, output="stuck.wav")
        assert "released at MIDI EOF" in result.stdout
        assert read_wav(root / "stuck.wav", 144000)[-2:] == (0, 0)

        sentinel = root / "existing.wav"
        sentinel.write_bytes(b"keep me")
        run(expected=1, output=sentinel.name)
        assert sentinel.read_bytes() == b"keep me"
        (root / "dangling.wav").symlink_to(root / "absent-target")
        run(expected=1, output="dangling.wav")
        assert (root / "dangling.wav").is_symlink()
        run(expected=1, output="missing-parent/out.wav")
        run(expected=1, font=root / "missing.sf2", output="missing-font.wav")
        assert not (root / "missing-font.wav").exists()
        run(expected=1, input_file=root / "missing.mid", output="missing-midi.wav")

        broken = root / "broken.mid"
        broken.write_bytes(mid.read_bytes()[:-1])
        run(expected=1, input_file=broken, output="broken.wav")
        for ppq, fmt in [(0xE728, 0), (480, 2)]:
            midi(broken, ppq=ppq, format_type=fmt)
            run(expected=1, input_file=broken, output="unsupported.wav")

        for args in [["--tail-bars", "1", "--tail-seconds", "1"], ["--tail-seconds", "-1"],
                     ["--tail-seconds", "nan"], ["--tail-seconds", "inf"],
                     ["--tail-bars", "oops"], ["--tail-bars", "1garbage"],
                     ["--tail-bars"], ["--unknown"], ["another.mid"]]:
            run(args, expected=2, output="invalid.wav")
        for args, code in [([], 2), (["--help"], 0)]:
            result = subprocess.run([executable, *args], capture_output=True, timeout=30)
            assert result.returncode == code
            checks += 1

        # Simulate ENOSPC during sample writes without filling the real filesystem.
        def limit_output():
            signal.signal(signal.SIGXFSZ, signal.SIG_IGN)
            resource.setrlimit(resource.RLIMIT_FSIZE, (1024, 1024))

        run(expected=1, output="write-failed.wav", preexec_fn=limit_output)
        assert not (root / "write-failed.wav").exists()
        assert not list(root.glob(".gmsynth-render-*")), "Temporary output leaked"

        # Concurrent conversions must publish exactly one complete file, never overwrite.
        cmd = [executable, str(mid), "--soundfont", str(sf), "--output", str(root / "race.wav")]
        processes = [subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE) for _ in range(2)]
        for process in processes:
            process.communicate(timeout=30)
        assert sorted(p.returncode for p in processes) == [0, 1]
        read_wav(root / "race.wav", 120000)
        assert not list(root.glob(".gmsynth-render-*"))
        checks += 1
    print(f"PASS {checks} CLI scenarios (generated MIDI/SoundFont, PCM, fade, errors, atomic output)")


if __name__ == "__main__":
    main()
