# GS規格適合性の検証結果

検証日: 2026-09-11

## 結論

現在の実装はGS規格（Roland SC-55 / SC-88 等）の部分対応であり、規格に沿った動作を保証できる状態ではない。特にDrum Setup SysExの受信アドレス体系の誤認（規格の `41H` ではなく `40H` を待機）、コーラスからリバーブへのセンドの強制遮断、スケールチューニングのDSP未反映、ドラム音色フォールバックの過剰動作には、GSデータの再生結果を変える誤りがある。

一方で、GS Resetの検知とモード自動切替、Master Tuneのニブル換算、Key Shiftによるトランスポーズ、Pitch Offset Fine（Hz→centsの対数変換）、Use for Rhythm PartによるCh10以外のドラム化、NRPNによるメロディック・ドラム詳細エディット、ドラムノート発音時のAuxスロットを用いたボイスパラメータ制御などは高度に実装されている。

## 検証範囲・制約

- Roland公開のGS Format Specification（SC-55, SC-55mkII, SC-88, SC-88Pro MIDI Implementation）と、SysEx受信、音色選択、エフェクト設定、音声処理を静的に照合した。
- 主な対象: `Source/GsModel.h`、`Source/FluidSynthEngine.cpp`、`Source/FluidSynthEngine.h`。
- ビルド、MIDI実機・ファイルを入力した動作試験、実音比較、SoundFont内のプリセット波形の検証は未実施。
- 以下の再現用メッセージはコードから導いた確認ケースであり、実行済みテストではない。
- 全パラメータの網羅的な適合認証ではなく、確認できた不一致と対応範囲の記録である。
- 既存コードの変更・コミットは実施していない。

## 分類基準

| 分類 | 意味 |
| :--- | :--- |
| 誤り | 対応している入力を別の意味で解釈する、指定が効かない、または規定と異なる動作をする。 |
| 一部簡易対応 | 機能の一部は動作するが、処理・パラメータ・音色の再現範囲が限定的である。 |
| 未実装(将来対応) | 対応する受信処理または音声処理が存在しない。必要性は対象とするGS機能範囲に応じて判断する。 |

---

## 誤り

### E01: Drum Setup SysEx のアドレス判定およびパラメータ番号の誤り【重大】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L1509-L1541)

実装は Drum Setup Parameter Change のアドレスを `40 2x aa` / `40 3x aa` と判定し、パラメータ種別を `paramType = addrMid & 0x0F`（0: Pitch, 1: Level, 2: AlternateGroup ...）と0始まりで解釈している。

| 項目 | GS規格値 | 実装値 |
| :--- | :--- | :--- |
| Address High | `41H` | `40H` |
| Address Mid (Map 1) | `01H`〜`08H` | `20H`〜`28H` |
| Address Mid (Map 2) | `11H`〜`18H` | `30H`〜`38H` |
| Parameter Offset | 1始まり（1: Pitch, 2: Level, 3: Group, 4: Pan, 5: Rev, 6: Cho, 7: RxOff, 8: RxOn） | 0始まり（`addrMid & 0x0F`） |

確認ケース:
```text
F0 41 10 42 12 41 02 26 50 47 F7
```
期待: Map 1のNote 38（Snare 1）のTVA Levelを80（50H）に設定する。  
現状: `addrHigh` が `41H` のため、`handleGsSysEx` 内の `addrHigh == 0x40` のチェックを通過できず無視される。仮に `40H` で送られた場合でも、オフセットのズレにより `40 22 26` は Alternate Group の変更として誤解釈される。  
対応: `addrHigh == 0x41` を認識し、`addrMid` の上位ニブルからマップ番号（`0`: Map 1, `1`: Map 2）、下位ニブルから1始まりのパラメータ種別を正しくデコードする。

---

### E02: GSモードにおける Chorus to Reverb Send の強制遮断【高】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L4529-L4534)

音声処理内のコーラス→リバーブ間センド処理において、GSモード時に強制的に `0.0f` が設定されている。

```cpp
const auto chorusToReverb = isXg ? (static_cast<float> (chorusParameters.sendToReverb) / 127.0f) : 0.0f;
```

SysEx `40 01 3F` で `gsChorusParameters.sendToReverb`（Chorus Send Level to Reverb）を受信・保持しているにもかかわらず、ミキシングループで一切反映されない。

確認ケース:
```text
F0 41 10 42 12 40 01 3F 7F 00 F7
```
期待: コーラス出力を最大レベルでリバーブバスへ送る。  
現状: 常にセンド量ゼロとして処理される。  
対応: `chorusToReverb` の計算において、GSモード時は `static_cast<float> (gsChorusParameters.sendToReverb) / 127.0f` を適用する。

---

### E03: Scale Tuning（音律スケールチューニング）が発音に未反映【中】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L1497-L1502)、[`updateChannelTuning`](../Source/FluidSynthEngine.cpp#L780-L796)

SysEx `40 1x 40..4B` で12音（C〜B）の音律微調整値（-64〜+63 cents）を受信し `part.scaleTuning` に格納後 `updateChannelTuning(channel)` を呼んでいる。  
しかし、`updateChannelTuning()` 内では `masterTuneCents` と `pitchOffsetFineCents` のみ加算されており、`scaleTuning` を一切参照していない。またノートオン処理内でも音階に応じたオフセット加算が行われていない。

確認ケース:
```text
F0 41 10 42 12 40 11 40 54 17 F7
```
期待: Part 1の「C」の音を+20セント高く発音する（54H = 84 = +20）。  
現状: 配列に値が保存されるだけで、発音ピッチは平均律のまま変わらない。  
対応: ノートオン時にノート番号のオクターブ剰余（`noteNumber % 12`）に応じた `scaleTuning` 値を取得し、ボイスのチューニングジェネレータ（`GEN_FINETUNE`）に加算する。

---

### E04: Master Panpot が音声出力に未反映【中】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L1341)

SysEx `40 00 06`（Master Panpot, 0..127, 40H = Center）を受信して `gsSystemParameters.masterPan` に保存しているが、オーディオレンダリングループやFluidSynthのマスターゲイン処理でこの値が使用されていない。

確認ケース:
```text
F0 41 10 42 12 40 00 06 00 3A F7
```
期待: シンセサイザー全体の出力を左端にパンする。  
現状: 値を保持するのみで出力ステレオバランスは変化しない。  
対応: マスターサミング出力時（または最終段）に `masterPan` に応じた左右ゲインスケーリングを適用する。

---

### E05: Chorus Delay パラメータが DSP に未反映【中】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L943-L981)、[FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L1385)

SysEx `40 01 3C`（Chorus Delay）を受信して `gsChorusParameters.delay` に保存しているが、`updateGsChorusSettings()` 内で参照されておらず、マクロごとに固定された初期値 `delayMs`（5.0ms〜15.0ms）のまま動作する。

確認ケース:
```text
F0 41 10 42 12 40 01 3C 7F 04 F7
```
期待: コーラスの初期ディレイ時間を変更する。  
現状: マクロ既定値のまま変化しない。  
対応: `updateGsChorusSettings()` 内で `gsChorusParameters.delay` のスケーリング値を `chorusProcessor.setCentreDelay()` に適用する。

---

### E06: ドラムキットのフォールバック動作が規格と異なる【中】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L368-L388)

GS規格（SC-55）では、未定義のドラムキット番号（Program Change）を受信した場合、**直前に選択されていたドラムキットを保持する**（変更しない）のが基本動作である。  
実装では、要求されたキットが存在しない場合に Standard Kit（Program 0 / Bank 128）へ代替し、さらに見つからない場合はメロディック音色を含む `lowestPreset`（Piano等）にフォールバックする。

確認ケース:
Ch 10で Room Kit（PC 9）を選択した状態で、未収録のキット番号（例: PC 60）を送信する。  
期待: 従前の Room Kit（PC 9）のまま演奏を継続する。  
現状: Standard Kit（PC 0）に切り替わる（またはメロディック音色が鳴る）。  
対応: ドラムチャンネルにおける未対応Program Change時は現在のプリセットを維持する。

---

### E07: GSリセット時の Ch 10 Bank MSB の初期値【小】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L1201-L1206)

`resetChannelState()` 内で `const auto msb = isDrum ? xg::bankMsbDrumKit : xg::bankMsbNormal;` となっており、Ch 10 の Bank MSB に XG規格のドラムバンク値 `127`（`xg::bankMsbDrumKit`）が代入される。  
GS規格ではドラムパートの Bank MSB (CC#0) は `0`（Capital）であるため、後続の音色選択処理にXGバンク値が混入する原因となる。

対応: モード判定を行い、GSモード時は `0` で初期化する。

---

### E08: CC#94 の機能衝突（Delay Send vs Variation Send）【小】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L3991-L3995)

SC-88以降のGS規格では CC#94 は Delay Send Level であるが、実装ではモード判別を行わず XG用の Variation Send として `setPartVariationSend` に渡している。`gsPartParameters[channel].delaySend` は更新されない。

対応: GSモード時は `gsPartParameters[channel].delaySend` の更新を行う。

---

## 一部簡易対応

### P01: Reverb / Chorus の Pre-LPF および Reverb Delay Feedback

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L1361)、[L1364](../Source/FluidSynthEngine.cpp#L1364)、[L1382](../Source/FluidSynthEngine.cpp#L1382)

- Reverb Pre-LPF (`40 01 32`) および Chorus Pre-LPF (`40 01 39`) はSysExを受信してパラメータに保持するが、DSPフィルタ（1次/2次ローパス）での帯域制限処理は行われていない。
- Reverb Delay Feedback (`40 01 35`) は受信・保持するが、JUCEの `juce::dsp::Reverb` にフィードバック引数が存在しないため未反映となっている。

### P02: Random Panpot (00H) の全体パートでの扱い

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L4091-L4094)、[L4112](../Source/FluidSynthEngine.cpp#L4112)

GS規格では Part Panpot = 00H（CC#10 = 0 または SysEx `40 1x 1C = 0`）はランダム定位を表す。  
ドラムノート発音処理内（`applyGsDrumNoteGenerators`）では `pan == 0` 時のランダムパンが実装されているが、メロディックパートでは FluidSynth の CC#10 に `0` がそのまま渡され、ハードレフト（左端固定）として発音される。

### P03: Chorus Rate / Depth / Feedback のゼロ値（変調停止）の扱い

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L962-L974)

`updateGsChorusSettings()` において、`if (gsChorusParameters.rate > 0)` などの判定があるため、`0` を指定してLFO変調を完全に停止させることができず、マクロの既定値が残り続ける。

---

## 未実装(将来対応)

### U01: GS Delay エフェクトブロック（SC-88）の音声処理系

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L1398-L1415)、[FluidSynthEngine.h](../Source/FluidSynthEngine.h#L287)

SysEx `40 01 50..59`（Delay Macro, Pre-LPF, Time, Feedback, Level, Send to Reverb）の受信・パラメータ保持は実装されているが、オーディオ処理系にディレイプロセッサ、ディレイバッファ、ミキシングループへのセンド/リターンルーティングが存在しない。

### U02: パート受信スイッチ群（Rx. Switches: `40 1x 02..12`）

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp#L1432-L1504)

GS規格に定義されているパート別のMIDI受信スイッチ（以下のパラメータ）のSysEx受信ハンドラおよびMIDIメッセージフィルタリングが未実装である。
- `40 1x 02`: Rx. Channel（受信チャンネル割り当て、`10H` = OFF）
- `40 1x 03`: Rx. Pitch Bend
- `40 1x 04`: Rx. Channel Pressure
- `40 1x 05`: Rx. Program Change
- `40 1x 06`: Rx. Control Change
- `40 1x 07`: Rx. Poly Pressure
- `40 1x 08`: Rx. Note Message
- `40 1x 09`: Rx. RPN
- `40 1x 0A`: Rx. NRPN
- `40 1x 0B`: Rx. Modulation
- `40 1x 0C`: Rx. Volume
- `40 1x 0D`: Rx. Panpot
- `40 1x 0E`: Rx. Expression
- `40 1x 0F`: Rx. Hold 1
- `40 1x 10`: Rx. Portamento
- `40 1x 11`: Rx. Sostenuto
- `40 1x 12`: Rx. Soft

### U03: パート動作モードのSysEx設定（`40 1x 13`, `40 1x 14`）

- `40 1x 13`: MONO/POLY Mode（0: Mono, 1: Poly）のSysEx受信が未実装（※CC#126/127による切り替えは実装済み）。
- `40 1x 14`: Assign Mode（0: Single, 1: Limited, 2: Full）のSysEx受信およびノート発音制御が未実装。

### U04: Key Range Low / High（`40 1x 1D..1E`）

GS規格のパートキー範囲設定（Low/High）は未実装。またコード上でも GSモード時は `!gsMode` によりキー範囲制限判定自体がバイパスされている（`FluidSynthEngine.cpp:L3435`）。

### U05: Insertion Effect / EFX（SC-88Pro 以降: `40 03 xx`）

SC-88Proで追加されたインサーションエフェクト（全64種類）のSysEx受信およびDSP処理は未実装。

### U06: RPN 00 01 (Fine Tune) / 00 02 (Coarse Tune) の明示的処理

RPN 00 00（Pitch Bend Sensitivity）は処理されているが、RPN 00 01（Fine Tuning）および RPN 00 02（Coarse Tuning）の明示的なデコードハンドラが存在しない（FluidSynthのネイティブ処理に委ねられている）。

---

## 実装済み機能（規格準拠点）

GMSynthにおいて、すでにGS規格に適合して良好に動作している主要機能は以下の通りである。

1. **GS Reset の完全な検知とモード遷移**
   - Standard GS Reset（`40 00 7F 00`）および System Mode Set（`00 00 7F 00`）をチェックサム検証付きで受信し、エンジンモードが `Auto` の場合に `ActiveMode::GS` へ自動遷移して全パート状態を初期化する。
2. **Master Tune（ニブルデコード・対数cents換算）**
   - `40 00 00..03` の4ニブルを合成し、`1024`（440.0Hz）を基準に `0.1 cents / step` で正確にセント換算して全チャンネルのチューニングに反映する。
3. **Master Key-Shift & Part Key Shift**
   - `40 00 05`（Master Key-Shift: 28H..58H, 40H = 0）および `40 1x 16`（Part Key-Shift）を受信し、ノートオン時に `noteNumber + transpose + shift` としてトランスポーズを正確に適用する。消音時のノートオフも移調後のノート番号と整合して管理される。
4. **Part Pitch Offset Fine（Hzからcentsへの対数変換）**
   - `40 1x 17..18` の周波数オフセット（-12.0Hz〜+12.0Hz）を受信し、$1200 \times \log_2((440 + \Delta f)/440)$ で対数セント換算してFluidSynthの `GEN_FINETUNE` に高精度に反映する。
5. **Use for Rhythm Part（動的ドラムパート切り替え）**
   - `40 1x 15` により、任意のパートを Normal / Drum 1 / Drum 2 に切り替え可能。FluidSynthのチャンネル種別（`CHANNEL_TYPE_DRUM` / `MELODIC`）と同期する。
6. **GS Melodic NRPN（01 08H..66H）**
   - Vibrato Rate/Depth/Delay、TVF Cutoff/Resonance、TVA Attack/Decay/Release のNRPN受信およびリアルタイム音色ジェネレータ制御が機能している。
7. **GS Drum NRPN（18H..1FH）とドラムボイス制御**
   - NRPN経由でのノート単位ピッチ（GEN_COARSETUNE）、音量スケーリング、パンポット、リバーブ/コーラスセンドが保持・反映される。
   - 発音時にはAuxドラムスロット（CH16以降の専用チャンネル）を動的に割り当て、独立したリバーブ/コーラスセンドを実現している。
   - オルタネートグループ（ハイハットのオープン/クローズ等の排他消音制御）および `rcvNoteOff = 0` によるシンバル等のノートオフ無視が機能している。
8. **音色選択と階層的フォールバック**
   - CC#0（Variation Bank）および CC#32（Tone Map Number）をデコードし、要求されたバリエーションバンクが存在しない場合に Capital Tone（Bank 0）へ戻る階層的探索が機能している。
