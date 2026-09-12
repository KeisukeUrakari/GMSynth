"""Generate a SoundFont locally and exercise the shared engine and CLI failure path."""
import sys
sys.dont_write_bytecode = True
import struct
import subprocess
import tempfile
from pathlib import Path
from test_cli import soundfont, vlq

with tempfile.TemporaryDirectory(prefix="gmsynth-engine-tests-") as tmp:
    root = Path(tmp)
    sf = root / "sine.sf2"
    soundfont(sf)
    result = subprocess.run([sys.argv[1], str(sf)], capture_output=True, text=True, timeout=60)
    print(result.stdout)
    print(result.stderr)
    assert result.returncode == 0
    assert "overlaps another group" not in result.stderr
    assert "Limiting this setting to audio-groups" not in result.stderr

    track = b"\0\x90\x45\x64" + vlq(240) + b"\xb0\x7e\x01" + vlq(240) + b"\x80\x45\0\0\xff\x2f\0"
    midi = root / "mode-change.mid"
    midi.write_bytes(b"MThd" + struct.pack(">IHHH", 6, 0, 1, 480) + b"MTrk" + struct.pack(">I", len(track)) + track)
    output = root / "failure.wav"
    result = subprocess.run([sys.argv[2], str(midi), "--soundfont", str(sf), "--output", str(output)],
                            capture_output=True, text=True, timeout=30)
    assert result.returncode == 1, (result.stdout, result.stderr)
    assert "channel configuration failed during rendering" in result.stderr, result.stderr
    assert not output.exists()
    assert not list(root.glob(".gmsynth-render-*"))
    print("PASS CLI runtime configuration failure: exit 1, no WAV or temporary file")
