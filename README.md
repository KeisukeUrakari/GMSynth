English follows Japanese

# GMSynth

FluidSynthを音源エンジンに使用した、macOS向けのGM系MIDIソフトウェアシンセサイザーです。
JUCEとProjucerで構成され、AUv3とStandalone Pluginを生成します。

## Features

- SoundFont (`.sf2`) のファイル選択と、Security-Scoped bookmarkを利用したState保存・復元
- 16 MIDIチャンネルのミュート、Volume、Pan、Bank、Program操作
- GM準拠のCH10（MIDIチャンネル10、ゼロベースでは9）のドラム固定
- ドラムProgram Changeによるキット切り替え
- 指定Bankにプリセットがない場合のプリセットフォールバック
- GM/GS Reset、All Notes Off、All Sounds Offへの対応
- MIDI入力のタイムスタンプ・サンプルフレーム位置付きCSVログ

SoundFontは同梱していません。利用するSoundFontのライセンスに従ってください。

## Requirements

- macOS
- Xcode
- JUCE 9.0.0（`external/JUCE` のサブモジュール）
- CMake
- Projucer（`.jucer`からXcodeプロジェクトを再生成する場合）

JUCE 9.0.0は `external/JUCE` にサブモジュールとして固定されています。別途、ユーザーのホームディレクトリなどにJUCEを配置する必要はありません。

## Build

サブモジュールを含めて取得します。

```sh
git clone --recurse-submodules https://github.com/masanaohayashi/GMSynth.git
cd GMSynth
```

既にclone済みでサブモジュールだけ未取得の場合は、次を実行します。

```sh
git submodule update --init --recursive
```

FluidSynthの静的ライブラリをビルドします。

```sh
./scripts/build_fluidsynth.sh
```

`cmake`がPATHにない場合は、絶対パスで指定できます。

```sh
CMAKE_BIN=/opt/homebrew/bin/cmake ./scripts/build_fluidsynth.sh
```

その後、`Builds/MacOSX/GMSynth.xcodeproj`をXcodeで開き、`GMSynth - All`をビルドしてください。
Projucerで再生成する場合は、`GMSynth.jucer`を開いてXcode exporterを実行します。

## macOS Release

配布用DMGを作成する前に、初回だけApple Notary Service用のKeychainプロファイルを登録します。秘密鍵やパスワードはリポジトリに保存しないでください。

```sh
xcrun notarytool store-credentials GMSynthNotary \
  --key "/path/to/AuthKey_XXXXXXXXXX.p8" \
  --key-id "XXXXXXXXXX" \
  --issuer "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
```

その後、次のスクリプトを実行すると、Developer ID署名、Universal Releaseビルド、`Applications`リンク入りDMGの作成、Notarization、staple、GitHub Release公開までを行います。

```sh
./scripts/macos/package-release.sh --notary-profile GMSynthNotary
```

署名IDやTeam IDなどを変更する場合は、`scripts/macos/config.env.example`をコピーして `scripts/macos/config.env` を作成してください。

## MIDIからWAVへの変換（CUI）

`gmsynth-render` は、標準MIDIファイル1曲を **48 kHz・16 bit PCM・ステレオWAV** にオフライン変換します。オーディオデバイスやDAWは不要です。SoundFontは別途指定してください。

リポジトリのルートでビルドします（macOS、CMake 3.22以降、C++17対応コンパイラが必要）。JUCEとFluidSynthは同梱サブモジュールを使ってビルドするため、このCUIについては事前の `build_fluidsynth.sh` 実行は不要です。

```sh
cmake -S Tools/Render -B Builds/Render/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build Builds/Render/build --target gmsynth-render -j 4

Builds/Render/build/bin/gmsynth-render song.mid --soundfont sound.sf2 --output song.wav
```

`cmake` がPATHにない場合は `/opt/homebrew/bin/cmake` などの絶対パスで実行してください。

余韻は既定で1小節です。次のどちらか一方で変更できます。小数・0も指定でき、0の場合は余韻とフェードを付けません。

```sh
Builds/Render/build/bin/gmsynth-render song.mid --soundfont sound.sf2 --output song.wav --tail-bars 2
Builds/Render/build/bin/gmsynth-render song.mid --soundfont sound.sf2 --output song.wav --tail-seconds 3.5
```

- 最後のノートオフ、またはサステイン／ソステヌートで保持した最後の音の解放時点から余韻を生成します。音源固有の音の減衰とリバーブも、この時間に含まれます。
- 余韻の全期間で、エフェクト込みの左右の音声を同じ割合で線形フェードアウトし、最後のサンプルを無音にします。自動ノーマライズはしません。
- 小節の長さは解放時点のテンポ・拍子で計算し、小節境界には切り上げません。未指定時は120 BPM・4/4なので、1小節は2秒です。
- 曲頭・曲中の休符は保持します。最後の解放より後のイベント・末尾の休符は省き、その時点の音源設定で余韻を生成します。
- ノートオフやペダル解除が欠落している場合は、MIDI終端で解放してから余韻を生成し、完了時に通知します。
- `XFIH`・`XFKM` など演奏トラック以外の追加情報チャンクは、長さを検証して読み飛ばします。
- 出力先の親ディレクトリは事前に作成してください。既存ファイルは上書きせず、変換成功時だけWAVを確定します。PCM範囲を超えてクリップした場合は通知します。

対応範囲はSMF Format 0／1、PPQ時間形式、16 MIDIチャンネルです。Format 2、SMPTE時間形式、単独のF7エスケープイベント、発音ノートのないファイルはエラーにします。分割されたSysExは結合し、完結した時刻に処理します。音源はGMSynthの初期設定（Autoモード・初期ゲイン）を使い、アプリの保存設定は読み込みません。WAVはRIFFの4 GiB制限内、入力MIDIは200 MiB以下に限ります。

`--help` で使用方法を表示します。終了コードは成功0、引数エラー2、読み込み・変換・保存エラー1です。

テストにはPython 3も必要です。テスト用MIDI・SoundFontは一時ディレクトリに自動生成します。

```sh
cmake -S Tools/Render -B Builds/Render/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build Builds/Render/build --target gmsynth-render gmsynth-render-tests -j 4
ctest --test-dir Builds/Render/build -L gmsynth-render --output-on-failure
```

## Runtime

プラグインのUIにある `Load Sound Font` からSoundFontを選択します。
選択したパスとmacOSのSecurity-Scoped bookmarkはプラグインStateに保存され、ホストのプロジェクト保存・復元に使用されます。

AUv3のSandbox環境では、旧StateにSecurity-Scoped bookmarkがない場合、同じSoundFontを一度再選択してからプロジェクトを保存してください。

## Third-party software

- [FluidSynth](https://github.com/FluidSynth/fluidsynth) は `external/fluidsynth` にサブモジュールとして含まれています。ライセンスは同サブモジュールの `LICENSE` を参照してください。
- [JUCE](https://github.com/juce-framework/JUCE) 9.0.0は `external/JUCE` にサブモジュールとして含まれています。JUCEのライセンス条件は同サブモジュールの `LICENSE.md` を参照してください。

## License

GMSynth本体のソースコードはMIT Licenseです。詳細は [LICENSE](LICENSE) を参照してください。

---

# GMSynth

GMSynth is a GM-oriented MIDI software synthesizer for macOS that uses FluidSynth as its sound engine.
It is built with JUCE and Projucer and generates an AUv3 plug-in and a Standalone Plugin.

## Features

- Select SoundFonts (`.sf2`) and save/restore the selection in plug-in state using a Security-Scoped bookmark
- Mute, Volume, Pan, Bank, and Program controls for 16 MIDI channels
- GM-compatible fixed percussion routing for CH10 (MIDI channel 10, zero-based channel 9)
- Drum-kit selection through drum Program Changes
- Preset fallback when the requested bank does not contain the requested program
- GM/GS Reset, All Notes Off, and All Sounds Off handling
- MIDI CSV logging with timestamps and sample-frame positions

SoundFonts are not included. Use each SoundFont in accordance with its license.

## Requirements

- macOS
- Xcode
- JUCE 9.0.0 (included as the `external/JUCE` submodule)
- CMake
- Projucer (only required when regenerating the Xcode project from the `.jucer` file)

JUCE 9.0.0 is pinned as the `external/JUCE` submodule. No separate JUCE installation in the user's home directory is required.

## Build

Clone the repository including its submodules.

```sh
git clone --recurse-submodules https://github.com/masanaohayashi/GMSynth.git
cd GMSynth
```

If the repository was cloned without submodules, initialize them with:

```sh
git submodule update --init --recursive
```

Build the static FluidSynth library:

```sh
./scripts/build_fluidsynth.sh
```

If `cmake` is not on `PATH`, provide its absolute path:

```sh
CMAKE_BIN=/opt/homebrew/bin/cmake ./scripts/build_fluidsynth.sh
```

Then open `Builds/MacOSX/GMSynth.xcodeproj` in Xcode and build the `GMSynth - All` scheme.
To regenerate the project with Projucer, open `GMSynth.jucer` and run the Xcode exporter.

## macOS Release

Before creating a distributable DMG, register a Keychain profile for the Apple Notary Service once. Do not store private keys or passwords in the repository.

```sh
xcrun notarytool store-credentials GMSynthNotary \
  --key "/path/to/AuthKey_XXXXXXXXXX.p8" \
  --key-id "XXXXXXXXXX" \
  --issuer "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
```

Then run the following script. It performs Developer ID signing, a Universal Release build, DMG creation with an `Applications` link, notarization, stapling, and GitHub Release publication.

```sh
./scripts/macos/package-release.sh --notary-profile GMSynthNotary
```

To change the signing identity or Team ID, copy `scripts/macos/config.env.example` to `scripts/macos/config.env` and edit the local file.

## MIDI to WAV conversion (CLI)

`gmsynth-render` converts one standard MIDI file offline to **48 kHz, 16-bit PCM, stereo WAV**, using the shared GMSynth engine. No audio device or DAW is needed. Supply your own SoundFont.

Build from the repository root on macOS with CMake 3.22+ and a C++17 compiler. This builds the bundled JUCE and FluidSynth submodules; running `build_fluidsynth.sh` separately is unnecessary for the CLI.

```sh
cmake -S Tools/Render -B Builds/Render/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
cmake --build Builds/Render/build --target gmsynth-render -j 4

Builds/Render/build/bin/gmsynth-render song.mid --soundfont sound.sf2 --output song.wav
```

If CMake is not on PATH, use its absolute path, such as `/opt/homebrew/bin/cmake`.

The default tail is one bar. Override it using either `--tail-bars 2` or `--tail-seconds 3.5`, but not both. Nonnegative decimals are accepted; zero disables the tail and fade.

- The tail starts at the final note-off or release of the final sustain/sostenuto-held note. Sample release envelopes and reverb are included within this duration.
- Both output channels, including effects, fade linearly over the entire tail, ending at zero. No automatic normalisation is applied.
- Bar duration uses the tempo and meter at the release point, without rounding up to a bar boundary. Missing tempo/meter defaults to 120 BPM and 4/4, giving a two-second default tail.
- Leading and internal rests are retained. Events and rests after the final release are omitted; the tail uses the settings at that release point.
- Notes still held at MIDI EOF are released there before the tail, with a completion notice.
- Non-track extension chunks, including `XFIH` and `XFKM`, are skipped after validating their lengths.
- The output parent directory must exist. Existing files are never overwritten; output is published only on success. Clipping beyond PCM range is reported.

Supports SMF Format 0/1, PPQ timing and 16 MIDI channels. Format 2, SMPTE timing, standalone F7 escape events and files with no sounding Note On events are rejected. Split SysEx messages are reassembled and delivered at completion time. The engine uses its default Auto mode and gain, without loading saved app settings. WAV output must fit the RIFF 4 GiB limit; MIDI input is limited to 200 MiB.

Use `--help` for usage. Exit codes: 0 success, 2 argument error, 1 input/render/output failure.

Tests additionally require Python 3 and generate their own MIDI/SoundFont fixtures in a temporary directory:

```sh
cmake -S Tools/Render -B Builds/Render/build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build Builds/Render/build --target gmsynth-render gmsynth-render-tests -j 4
ctest --test-dir Builds/Render/build -L gmsynth-render --output-on-failure
```

## Runtime

Use `Load Sound Font` in the plug-in UI to select a SoundFont.
The selected path and macOS Security-Scoped bookmark are stored in the plug-in state and used when the host saves and restores a project.

In an AUv3 sandbox, if an old state does not contain a Security-Scoped bookmark, select the same SoundFont once again and save the project.

## Third-party software

- [FluidSynth](https://github.com/FluidSynth/fluidsynth) is included as a submodule at `external/fluidsynth`. See the submodule's `LICENSE` for its license.
- [JUCE](https://github.com/juce-framework/JUCE) 9.0.0 is included as the `external/JUCE` submodule. See the submodule's `LICENSE.md` for the JUCE license terms.

## License

The GMSynth source code is released under the MIT License. See [LICENSE](LICENSE) for details.
