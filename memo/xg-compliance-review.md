# XG規格適合性の検証結果

検証日: 2026-09-11

配置された仕様書による再検証済み。判定の補足・変更は[xg-compliance-verification.md](xg-compliance-verification.md)を参照。

## 結論

現在の実装はXGの部分対応であり、規格に沿った動作を保証できる状態ではない。特にVariationの種類番号、種類別パラメータの解釈、音色フォールバックには、XGデータの再生結果を変える誤りがある。

XGには省略可能な機能もあるため、未実装という理由だけで規格違反とは判定しない。対応範囲を明示し、「受信」「値の保持」「音声処理への反映」を分けて管理する必要がある。

## 検証範囲・制約

- ヤマハ公開のXG仕様書V1.35と、SysEx受信、音色選択、エフェクト設定、音声処理を静的に照合した。
- 主な対象: `Source/XgModel.h`、`Source/FluidSynthEngine.cpp`、`Source/VariationEffect.cpp`。
- ビルド、MIDIを入力した動作試験、実音比較、SoundFont内のプリセット・波形・ドラム配列の検証は未実施。
- 以下の再現用メッセージはコードから導いた確認ケースであり、実行済みテストではない。
- 全パラメータの網羅的な適合認証ではなく、確認できた不一致と対応範囲の記録である。
- コードの修正・コミットは実施していない。

## 分類基準

| 分類             | 意味                                                                                       |
| ---------------- | ------------------------------------------------------------------------------------------ |
| 誤り             | 対応している入力を別の意味で解釈する、指定が効かない、または規定と異なる動作をする。       |
| 一部簡易対応     | 機能の一部は動作するが、処理・パラメータ・音色の再現範囲が限定的である。                   |
| 未実装(将来対応) | 対応する受信処理または音声処理が存在しない。必要性は対象とするXG機能範囲に応じて判断する。 |

一つの機能に複数の分類がある場合は、原因と対応を明確にするため項目を分割する。

## 誤り

### E01: Variationの種類番号が規格と異なる【重大】

参照: [XgModel.h](../Source/XgModel.h)、`varTypeChorus`〜`varTypeAmpSimulator`。

MSBは16進表記。規格値は仕様書p.20のEffect Mapによる。

| エフェクト    | XG規格のMSB | 実装のMSB |
| ------------- | ----------: | --------: |
| Chorus        |        `41` |      `40` |
| Flanger       |        `43` |      `41` |
| Symphonic     |        `44` |      `42` |
| Tremolo       |        `46` |      `44` |
| Auto Pan      |        `47` |      `45` |
| Phaser        |        `48` |      `46` |
| Distortion    |        `49` |      `47` |
| Overdrive     |        `4A` |      `48` |
| Amp Simulator |        `4B` |      `49` |
| Auto Wah      |        `4E` |      `43` |

確認ケース:

```text
F0 43 10 4C 02 01 40 49 00 F7
```

期待: VariationのDistortionを選択する。

現状: Amp Simulatorを選択する。実音確認には別途Variationの接続先と発音を設定する必要がある。

対応: 定数だけでなく、`VariationEffect.cpp`に直接書かれた種類番号の比較も修正する。

### E02: Delay系の種類別パラメータ配置を共用している【高】

参照: [VariationEffect.cpp](../Source/VariationEffect.cpp)、`updateDelayParameters`、`processDelay`。

Delay LCR・Delay LR・Echo・Cross Delayを共通の配置として処理しているが、種類によってパラメータの意味が異なる。

Delay LCRの具体例:

| パラメータ | XGでの意味     | 実装                |
| ---------- | -------------- | ------------------- |
| 4          | Feedback Delay | 未反映              |
| 6          | Cch Level      | High Dampとして解釈 |
| 7          | High Damp      | 未反映              |

中央音の加算量も固定されている。仕様書p.23〜24をもとに、種類ごとにデコードと処理を分ける必要がある。

### E03: エフェクト初期値・種類変更時の初期化が不十分【高】

参照: [XgModel.h](../Source/XgModel.h)の各エフェクトの`reset`、[FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp)のEffect 1 Parameter Change処理。

多数のパラメータを一律に`0`や`64`で初期化しており、種類ごとの初期値を持っていない。種類変更時もパラメータ配列を更新せず、前の種類の値が残る。

対応: 種類のMSB/LSBに対応する初期値表を用意し、リセットと種類変更に適用する。

再検証: [efctparamdeflt.pdf](../specific/efctparamdeflt.pdf) p.39に種類別初期値がある。例えばHall 1のパラメータ1〜5は十進数で18, 10, 8, 13, 49。種類変更による詳細設定の初期化は、公式解説[read_aoyama.pdf](../specific/read_aoyama.pdf)の図4-3直前の注意書きでも確認した。初期値表と解説の根拠を区別する。

### E04: 有効なゼロ値を未指定として扱う【高】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp)の`updateChorusSettings`、[VariationEffect.cpp](../Source/VariationEffect.cpp)の各パラメータ更新処理。

`value > 0`で指定の有無を判定する箇所がある。例えばChorusのLFO Depthを`0`にしても、基本値のDepthが残る。VariationのTremolo系にもDepthのゼロを既定値に置き換える処理がある。

対応: 初期値は初期化時に設定し、有効なゼロをそのまま処理する。値域は種類・パラメータごとに判断する。

### E05: エフェクト間Sendのレベル換算が異なる【高】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp)、音声処理内の`varToChorus`、`chorusToReverb`、`varToReverb`。

対象はChorus→Reverb、Variation→Reverb、Variation→Chorus。実装は`値 / 127`なので、64で約−6 dB、127で0 dBとなる。XG Parameter Change Tableでは64で0 dB、127で約＋6 dBである。

対応: エフェクト間Send用の値変換を修正する。パートからのSendとは区別して検証する。

### E06: 受信したMulti Part設定が機能しない【高】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp)、Multi Part Parameter Change処理。

| パラメータ       | 状態     | 影響                                 |
| ---------------- | -------- | ------------------------------------ |
| Rcv Channel      | 保存のみ | 受信チャンネル変更・OFFが効かない    |
| Same Note Assign | 保存のみ | 同一ノートの割り当て方式が変わらない |
| Element Reserve  | 保存のみ | パート別の発音数予約に反映されない   |

Rcv Channelの確認ケース:

```text
F0 43 10 4C 08 00 04 7F F7
```

期待: Part 1の受信をOFFにする。

現状: 値を保存するだけで、MIDIの受信停止につながらない。

対応: 受信ルーティング・ノート管理・発音数管理につなぐ。機能を省略する場合も「対応済み」として扱わない。

### E07: 音色フォールバックがバンクの性質を区別しない【高】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp)、`fluid_synth_program_select`前のプリセット選択処理。

未収録のメロディーバンクを一律にBank 0へ代替し、なければ同Programの任意バンク、さらに最小番号のプリセットへ代替する。このため、代替音を鳴らすべきでないSFX系でもGM楽器が鳴る可能性がある。

また、未収録のドラムキットはStandard Kitへ代替する。仕様書p.5では、該当するキットがないProgram Changeは無視して従前のキットを維持する。

対応: Normal、SFX、Drumなどのバンク区分ごとにフォールバックを定義し、未対応キット指定時には選択中のキットを保持する。

再検証による補足: [spec.pdf](../specific/spec.pdf) p.6〜7では、部分対応するバンク内の欠落音色をBank 0の同Programで補うことと、未対応のNormalバンクLSB指定時に前回のメロディーLSBを維持することを区別している。一律Bank 0への代替では後者も満たさない。MSB 01〜7Eの未収録音色は無発音の規定があるが、[xgmap.pdf](../specific/xgmap.pdf)には60〜6FのProxyについてMSB 0への代替規定もあるため、全拡張バンクを一律に無発音へ変更してはいけない。

### E08: Drums3・4が独立したSetupとして機能しない【中】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp)、Drum Setup Parameter Change処理と発音時のSetup選択。

SysExはSetup 1・2のみ受信する一方、Part ModeではDrums3・4も受け付ける。発音時にはDrums3がSetup 1、Drums4がSetup 2へ割り当てられる。

対応: 対応するSetup数と受け付けるPart Modeの整合を取る。Setup 3・4を追加する場合は独立した状態を持たせる。

再検証による限定: [xgparameterchangetable.pdf](../specific/xgparameterchangetable.pdf) p.47は最低2セット、Setup 3・4はオプションと規定する。2セットしか持たないこと自体は誤りではない。ここでの「誤り」は、未対応のPart Modeを既存Setupに黙って結び付ける実装上の不整合を指す。未対応モードの代替処理について、この箇所だけから一意の規格違反とは断定しない。

### E09: Multi EQプリセット値が不一致【高】

再検証で追加。`MultiEqParameters::setPreset`は独自のゲイン値のみを設定し、周波数・Q・形状を規定のプリセットへ戻さない。JazzのGain1〜5は規定58, 66, 68, 60, 58に対し、実装68, 64, 62, 66, 67。根拠: [efctparamdeflt.pdf](../specific/efctparamdeflt.pdf) p.40。Multi EQはオプションだが、実装済み機能の値の誤りとして扱う。

### E10: Multi Partのリセット値が不一致【高】

再検証で追加。`resetChannelState`と`PartParameters::reset`では、Part 10のPart ModeがDrums1ではなくDrum、Element Reserveが0ではなく2、全PartのRcv Channelが各チャンネル番号ではなく0になる。根拠: [xgparameterchangetable.pdf](../specific/xgparameterchangetable.pdf) p.44。E06の未接続処理と合わせて修正する。

## 一部簡易対応

### S01: Reverb・Chorusの音声処理と物理量へのマッピング【高】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp)、`updateReverbSettings`、`updateChorusSettings`。

ReverbはJUCEのReverb、Chorus系はJUCEのChorusを使って種類ごとに値を変える近似である。

- Reverb Timeを時間の表から変換せず、`roomSize`の倍率にしている。
- Diffusionをステレオ幅へ割り当てている。
- LPF指定を主にdampingへ割り当てている。
- ChorusのLFO周波数・遅延時間を独自の式で変換している。

効果自体はあるが、指定値の物理的意味・範囲を再現していない。音質の近似を許容する場合でも、パラメータの意味を保つ変換と、近似範囲の明示が必要。

### S02: VariationのDSP・サブタイプ対応【高】

参照: [VariationEffect.cpp](../Source/VariationEffect.cpp)、`updateParameters`と各`process`関数。

Delay、歪み、変調系の処理はあるが、種類別の全パラメータを反映していない。`currentTypeLsb`は保存のみで、サブタイプの処理に使われない。

まずE01の番号誤りを修正し、その後に種類ごとのパラメータ、サブタイプ、未収録サブタイプの扱いを整理する。

### S03: SoundFontによる音色供給【高・検証制約あり】

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp)のプリセット管理、[README.md](../README.md)。

SF2のBank/Programによる音色選択はあるが、音色の内容は利用するSoundFontに依存する。任意のSF2をロードする方式だけでは、XGの音色配列・ドラムノート配列・音色特性を保証できない。

全拡張音色の収録や、ヤマハ実機との波形一致を要求するものではない。規定に従った代替も許容される。特定SoundFontの不適合を確認したわけではない。対応対象のSoundFontとバンク対応表を定め、その内容を別途検証する必要がある。

## 未実装(将来対応)

### U01: XG Bulk Dump・各種Request

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp)、`handleSysEx`。

XGとして判定するのは`43 1n 4C`のParameter Change形式であり、Bulk Dump、Parameter Request、Dump Requestの処理はない。送受信の対応範囲を決めたうえで追加する。

### U02: Variationの未対応エフェクト

参照: [VariationEffect.cpp](../Source/VariationEffect.cpp)、`updateParameters`、`process`。

VariationとしてのHall、Room、Stage、Plate、Rotary Speaker、EQなどの専用処理がない。未対応番号は原音を通す。なお、Rotary SpeakerなどはE01の番号衝突により別効果になる場合があり、その誤りは先に修正する。

追加対象はXGの必須・オプション区分と製品の対応範囲に基づいて決定する。Effect Mapの色分けと抽出テキストを照合すると、VariationのHall 1/2、Room 1〜3、Stage 1/2、Plate、Rotary Speaker、3-Band/2-Band EQの基本タイプはESSENTIAL側である。これらをすべて任意の将来拡張と扱うのは不適切で、通常のXG対応を目標とする場合は優先して実装する。

### U03: 保存のみのエフェクトパラメータのDSP反映

参照: [FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp)のEffect 1 Parameter Change処理、[VariationEffect.cpp](../Source/VariationEffect.cpp)。

Variationのパラメータ11〜16は保存されるが、DSPでは参照されない。Reverb・Chorusにも保存されるだけで処理に使われないパラメータがある。

対応する種類に実際に定義されたパラメータについて、処理への反映を追加する。予約パラメータまで実装対象に含める必要はない。

### U04: 独立したDrum Setup 3・4

現在の実体とSysEx受信はSetup 1・2のみ。Setup 3・4は仕様書上オプションなので、必須対応とはしない。Setup数を拡張する場合は、E08を解消したうえで状態・受信・発音・リセットを一貫して追加する。

## 対応の土台が確認できた項目

以下は実装の存在を確認したもので、完全適合や実音一致を保証するものではない。

- XG System Onの認識と初期化経路。
- Master Tuneの受信とチューニング更新。
- 主要なMulti Partパラメータの受信。
- Variationパラメータ1〜10の14ビット値の組み立て。
- CC91・93・94によるSend設定。
- Reverb・Chorus・Variationのバス構造とVariationのInsertion/System切り替え。
- Drum Setup 1・2のパラメータ受信。

## 推奨する対応順

1. E01の種類番号と直接記述された番号比較を修正する。
2. 種類別にパラメータの意味・値域・初期値・物理量への変換を定義する。
3. E02〜E04を修正し、保存のみの値とDSP反映済みの値を整理する。
4. E05のSend換算を修正する。
5. E06〜E08の受信制御、音色選択、Setup管理を修正する。
6. 対応SoundFontとXG機能範囲を定め、未実装機能の優先度を決める。
7. SysEx入力後の状態確認と、代表的なエフェクト・音色の実音確認を実施する。

## 参照資料

- [ヤマハ XG仕様書 V1.35 本体](https://jp.yamaha.com/files/download/other_assets/0/321740/xg_v135_j.pdf)
  - p.3: 適応性・機能範囲の考え方。
  - p.5: Program Change、未収録ドラムキットの扱い。
  - p.20: Effect Map。
  - p.22以降: Effect Parameter List。
  - p.23〜24: Delay系のパラメータ。
  - p.41〜44: エフェクト・Multi PartのParameter Change Table。
- [ヤマハ XG Parameter Change Table](https://jp.yamaha.com/files/download/other_assets/8/321748/xgparameterchangetable.pdf)

ページ番号は仕様書に印刷された番号。PDFビューアーのページ番号とは1ページずれる場合がある。コード参照はファイル名・関数名で示しており、後続修正によって内容が変わる可能性がある。
