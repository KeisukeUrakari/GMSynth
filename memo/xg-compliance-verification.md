# XG規格適合性レビュー検証報告書

検証日: 2026-09-11  
対象文書: [memo/xg-compliance-review.md](file:///home/urakari/proj/GMSynth/memo/xg-compliance-review.md)  
検証対象ソースコード:

- `Source/XgModel.h`
- `Source/FluidSynthEngine.cpp`
- `Source/FluidSynthEngine.h`
- `Source/VariationEffect.cpp`
- `Source/VariationEffect.h`
- `README.md`

参照規格:

- ヤマハ XG Format Specifications V1.35
- ヤマハ XG Parameter Change Table

---

## 1. 検証結果の総括

`memo/xg-compliance-review.md` に記載されている全指摘事項（誤り E01〜E08、一部簡易対応 S01〜S03、未実装 U01〜U04）について、ソースコードの実装実態およびヤマハ公式の XG 仕様書 V1.35 と照合した結果、**すべての指摘が事実に基づいており、正確（100% 妥当）** であることを確認した。

特に **E01（Variation 種類番号のズレ）**、**E02（Delay系パラメータ誤読）**、**E04（ゼロ値無視）**、**E05（Send レベル換算ズレ）** は、MIDI 楽曲再生時の音色・エフェクト・音量バランスに直結する深刻な不具合である。

---

## 2. 誤り（E01〜E08）の詳細検証

### E01: Variationの種類番号が規格と異なる【重大】

- **判定**: **正しい（重大な不具合）**
- **規格仕様**: XG 仕様書 p.20（Effect Map）
  - Chorus: `41H`, Flanger: `43H`, Symphonic: `44H`, Tremolo: `46H`, Auto Pan: `47H`, Phaser: `48H`, Distortion: `49H`, Overdrive: `4AH`, Amp Simulator: `4BH`, Auto Wah: `4EH`
- **実装実態**:
  - `Source/XgModel.h` (L402-412):
    ```cpp
    constexpr uint8_t varTypeChorus       = 0x40;
    constexpr uint8_t varTypeFlanger      = 0x41;
    constexpr uint8_t varTypeSymphonic    = 0x42;
    constexpr uint8_t varTypeAutoWah      = 0x43;
    constexpr uint8_t varTypeTremolo      = 0x44;
    constexpr uint8_t varTypeAutoPan      = 0x45;
    constexpr uint8_t varTypePhaser       = 0x46;
    constexpr uint8_t varTypeDistortion   = 0x47;
    constexpr uint8_t varTypeOverdrive    = 0x48;
    constexpr uint8_t varTypeAmpSimulator = 0x49;
    ```
  - `Source/VariationEffect.cpp` (L156-161, L256):
    `currentTypeMsb == 0x48`（Overdrive判定）、`currentTypeMsb == 0x49`（AmpSim判定）、`currentTypeMsb == 0x45`（AutoPan判定）のようにハードコードされた比較が存在。
- **影響**: Distortion (`49H`) の SysEx (`F0 43 10 4C 02 01 40 49 00 F7`) を受信すると、実装の `varTypeAmpSimulator` (`49H`) に合致し、Amp Simulator が動作してしまう。

---

### E02: Delay系の種類別パラメータ配置を共用している【高】

- **判定**: **正しい（不具合）**
- **規格仕様**: XG 仕様書 p.23〜24（Delay L,C,R のパラメータ配置）
  - Param 1: Delay Time L
  - Param 2: Delay Time R
  - Param 3: Delay Time C
  - Param 4: Feedback Delay
  - Param 5: Feedback Level
  - Param 6: Cch Level
  - Param 7: High Damp
  - Param 10: Dry / Wet
- **実装実態**:
  - `Source/VariationEffect.cpp` (L116-152):
    Delay LCR、Delay LR、Echo、Cross Delay をすべて同一の `updateDelayParameters` で処理。
    - `parameters14Bit[3]`（Param 4）を参照していない（Feedback Delay が未反映）。
    - `parameters14Bit[5]`（Param 6）を High Damp として取得（Cch Level が High Damp に化け、本来の High Damp である Param 7 は未反映）。
  - `Source/VariationEffect.cpp` (L403-404):
    Center 音の加算比率が `0.707f` に固定されており、Cch Level による調整が効かない。

---

### E03: エフェクト初期値・種類変更時の初期化が不十分【高】

- **判定**: **正しい（不具合）**
- **規格仕様**: XG 仕様書 p.22 以降（Effect Parameter List）
  - エフェクトの種類ごとに固有のパラメータ初期値（Default）が規定されている。
- **実装実態**:
  - `Source/XgModel.h` (L362-373, L384-395, L431-448):
    Reverb、Chorus、Variation の `reset()` で、パラメータ配列が一律 `0` や `64` で初期化されており、種類固有の初期値表を持っていない。
  - `Source/FluidSynthEngine.cpp` (L2242-2349):
    Effect 1 Parameter Change でエフェクト種類（Type MSB/LSB）を変更した際、パラメータ配列（`parameters14Bit` など）が再初期化されないため、直前まで設定されていたエフェクトのパラメータ値が残存する。

---

### E04: 有効なゼロ値を未指定として扱う【高】

- **判定**: **正しい（不具合）**
- **実装実態**:
  - `Source/FluidSynthEngine.cpp` (L1044-1047):
    ```cpp
    float depthNorm = baseDepth;
    if (chorusParameters.parameters[1] > 0)
    {
        depthNorm = juce::jlimit (0.0f, 1.0f, static_cast<float> (chorusParameters.parameters[1]) / 127.0f);
    }
    ```
    Chorus の LFO Depth を `0`（変調なし）に設定しても、`> 0` の条件を満たさず `baseDepth`（Chorus 1 では 0.20）が残ってしまう。
  - `Source/VariationEffect.cpp` (L263):
    ```cpp
    const auto depthVal = params.parameters14Bit[1] > 0 ? static_cast<float> (params.parameters14Bit[1] & 0x7F) : 80.0f;
    ```
    Tremolo / AutoPan の Depth を `0` に設定しても `80.0f`（約 63%）に置き換えられてしまう。
  - 同様に複数箇所で `> 0` による未指定フォールバックが行われており、正規の `0` 指定が無効化される。

---

### E05: エフェクト間Sendのレベル換算が異なる【高】

- **判定**: **正しい（不具合）**
- **規格仕様**: XG Parameter Change Table (Table#1)
  - Send Chorus to Reverb (02 01 2E)
  - Send Variation to Reverb (02 01 58)
  - Send Variation to Chorus (02 01 59)
  - 値 `0`: $-\infty\text{ dB}$, 値 `64`: $0\text{ dB}$（ゲイン 1.0）, 値 `127`: $+6\text{ dB}$（ゲイン約 2.0）。
- **実装実態**:
  - `Source/FluidSynthEngine.cpp` (L4487, L4529, L4537):
    `chorusToReverb = static_cast<float> (chorusParameters.sendToReverb) / 127.0f` 等、一律 `値 / 127` で計算。
  - 値 64 のときゲインは約 0.504（$-5.95\text{ dB}$）、値 127 で 1.0（$0\text{ dB}$）となり、規格より約 $6\text{ dB}$ レベルが低下している。

---

### E06: 受信したMulti Part設定が機能しない【高】

- **判定**: **正しい（未配線）**
- **実装実態**:
  - `Source/FluidSynthEngine.cpp`:
    - L1800: `p.rcvChannel = val;`
    - L1812: `p.sameNoteAssign = val;`
    - L1775: `p.elementReserve = juce::jlimit (0, 32, static_cast<int> (val));`
  - 保存された `rcvChannel`, `sameNoteAssign`, `elementReserve` は、MIDI メッセージ処理や発音・ボイス管理から一切参照されていない。
  - そのため、SysEx `F0 43 10 4C 08 00 04 7F F7`（Part 1 受信 OFF）を送信しても、MIDI CH1 のノートメッセージを受信して通常通り発音してしまう。

---

### E07: 音色フォールバックがバンクの性質を区別しない【高】

- **判定**: **正しい（不具合）**
- **規格仕様**: XG 仕様書 p.5（Program Change とキット選択の規則）
  - 未収録のドラムキットを受信した場合、Program Change を無視し、直前のキットを維持する。
  - バンク区分（Normal, SFX Voice, SFX Kit, Drum Kit）ごとに適切な扱いが必要。
- **実装実態**:
  - `Source/FluidSynthEngine.cpp` (L420-434):
    メロディ音色で要求バンクが存在しない場合、一律 Bank 0（通常の GM 楽器）を検索し、それでもなければ任意のバンクや最低番号プリセット（Bank 0 / Prog 0 の Piano 等）にフォールバックする。SFX ボイスバンク（MSB 64）の音色が未収録の場合に通常のピアノやギター等が鳴る原因となる。
  - `Source/FluidSynthEngine.cpp` (L368-375):
    未収録のドラムキットが指定された場合、強制的に Standard Kit (Prog 0) へ切り替えてしまう。

---

### E08: Drums3・4が独立したSetupとして機能しない【中】

- **判定**: **正しい（設計の不整合）**
- **実装実態**:
  - `Source/FluidSynthEngine.h` (L278-279):
    メンバ変数として `drumSetup1` と `drumSetup2` の 2 つのみ保持。
  - `Source/FluidSynthEngine.cpp` (L2186-2188):
    Drum Setup Parameter Change では `setupIdx > 1`（Setup 3: `32H`, Setup 4: `33H`）を破棄（return）。
  - `Source/FluidSynthEngine.cpp` (L2972-2973, L3063, L3560, L3759):
    Part Mode で Drums3 は `drumSetup1`、Drums4 は `drumSetup2` を参照しているため、Drums1 と Drums3、Drums2 と Drums4 が同じ Setup メモリを共有・上書きし合ってしまう。

---

## 3. 一部簡易対応（S01〜S03）の詳細検証

### S01: Reverb・Chorusの音声処理と物理量へのマッピング

- **判定**: **正しい**
- **検証**:
  - `Source/FluidSynthEngine.cpp` (L884-897): JUCE の `juce::dsp::Reverb` を使用。Reverb Time を時間の物理量テーブルではなく `roomSize` のスケーリングに流用、Diffusion をステレオ幅 `width` に代用、LPF を `damping` に代用。
  - `Source/FluidSynthEngine.cpp` (L1038-1061): JUCE の `juce::dsp::Chorus` を使用。LFO 周波数や遅延時間を独自の二次関数や線形式で近似。

### S02: VariationのDSP・サブタイプ対応

- **判定**: **正しい**
- **検証**:
  - `Source/VariationEffect.cpp` (L311): `currentTypeLsb` を代入・保存しているが、その後のエフェクト処理・分岐で一切参照されていない。
  - また、エフェクトごとの全パラメータ（パラメータ 11〜16 や各専用パラメータ）が DSP に反映されていない。

### S03: SoundFontによる音色供給

- **判定**: **正しい**
- **検証**:
  - `README.md` (L18): SoundFont は同梱されず、ユーザーが任意の `.sf2` をロードする設計。
  - 一般的な SoundFont（GM 用）では XG 固有のバンク（MSB 0 / LSB 1〜127、SFX バンク 64 等）やドラムノート配置が存在しないため、完全な XG 再現には XG 対応 SoundFont とマッピング定義が不可欠である。

---

## 4. 未実装（U01〜U04）の詳細検証

### U01: XG Bulk Dump・各種Request

- **判定**: **正しい**
- **検証**: `Source/FluidSynthEngine.cpp` (L1559-1563) では SysEx ヘッダ `43 1n 4C`（Parameter Change）のみを判定しており、Bulk Dump（`43 0n 4C`）、Dump Request（`43 2n 4C`）、Parameter Request（`43 3n 4C`）の処理は存在しない。

### U02: Variationの未対応エフェクト

- **判定**: **正しい**
- **検証**: `Source/VariationEffect.cpp` (L320-362) の switch 文にない種類（Hall, Room, Stage, Plate, Rotary Speaker, EQ 等）は `default` により原音出力（Thru）となる。また、規格上 `45H` である Rotary Speaker は、現在実装の Auto Pan（`0x45`）と番号が衝突している。

### U03: 保存のみのエフェクトパラメータのDSP反映

- **判定**: **正しい**
- **検証**: `variationParameters.parameters11To16` は `FluidSynthEngine.cpp` (L2382) で受信・保存されるのみで、`VariationEffect.cpp` では参照されていない。Reverb / Chorus でも配列の一部パラメータのみが使用されている。

### U04: 独立したDrum Setup 3・4

- **判定**: **正しい**
- **検証**: E08 に記載の通り、Setup 3・4 の独立した状態・受信ルーチンは未実装である。

---

## 5. 推奨される改修ロードマップ

`memo/xg-compliance-review.md` に記載されている「推奨する対応順」は、依存関係および重要度を的確に捉えており、妥当である。

1. **フェーズ 1: Variation 種類番号の修正（E01）**
   - `Source/XgModel.h` の定数定義を XG 規格値（MSB: Chorus `41H`, Flanger `43H`, Distortion `49H` 等）に修正。
   - `Source/VariationEffect.cpp` 内のハードコードされた番号比較（`0x48`, `0x49`, `0x45` 等）を定数参照に修正。
2. **フェーズ 2: エフェクトパラメータ解釈と初期化の修正（E02, E03, E04）**
   - 種類ごとのパラメータ定義・デコード（Delay LCR の Feedback Delay, Cch Level, High Damp 分離など）を実装。
   - エフェクトリセット時・種類変更時の種類別デフォルト値テーブルを適用。
   - `> 0` による未指定判定を廃止し、有効な `0` 値（Depth=0 等）が正しく反映されるように修正。
3. **フェーズ 3: エフェクト間 Send レベル換算の修正（E05）**
   - Chorus→Reverb、Variation→Reverb、Variation→Chorus のレベル計算を Table#1（64 = 0 dB, 127 = +6 dB）に合わせた変換式へ修正。
4. **フェーズ 4: パート受信制御・音色フォールバック・Drum Setup の修正（E06, E07, E08）**
   - `rcvChannel` を MIDI 受信フィルタリングに接続（OFF 時に発音停止）。
   - ドラム未収録時のキット維持処理、および SFX バンク等のフォールバック境界を整理。
   - Drum Setup 3・4 の独立実体化、または Part Mode での制限対応。
