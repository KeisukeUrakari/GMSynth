# GS規格適合性レビューの検証結果

検証日: 2026-09-11  
対象: [gs-compliance-review.md](gs-compliance-review.md)

## 結論

主要な不具合の指摘は妥当だが、再現メッセージ、規格の解釈、「実装済み」の評価に訂正が必要である。「GS部分対応」という総評は支持できるものの、レビュー文書をそのまま修正仕様として使用することは推奨しない。

## 検証方法・制約

- `Source/FluidSynthEngine.cpp`、`Source/FluidSynthEngine.h`、`Source/GsModel.h`等を静的に照合した。
- Roland公式のSC-55、SC-88、SC-88STマニュアルの該当表を参照した。
- FluidSynth公式のRPN対応表を確認した。
- レビュー掲載の再現用SysExについて、チェックサムを計算した。
- ビルド、MIDI入力による動作試験、実音比較は実施していない。
- SC-88Pro固有の種類数など、今回一次資料との照合を完了していない事項は未検証としている。
- 実装コードおよび元のレビュー文書は変更していない。コミットは行っていない。

## 項目別判定

| 項目 | 検証結果 |
| :--- | :--- |
| E01 Drum Setup | アドレス誤りは正しい。ただし全パラメータが一律に1つずれているわけではない。 |
| E02 Chorus→Reverb | 未反映は正しい。再現例のチェックサムが誤り。 |
| E03 Scale Tuning | 未反映は正しい。再現例のチェックサムが誤り。 |
| E04 Master Pan | 未反映は正しい。再現例の値が規格範囲外。 |
| E05 Chorus Delay | 未反映は正しい。固定時間の説明は `2〜15ms` が正しい。 |
| E06 キットフォールバック | 実装の説明は正しい。「未定義なら直前を保持」という規格上の断定は今回確認できず。 |
| E07 GSリセット時のBank MSB | `127` が設定される指摘は正しい。実際の音色への影響はプリセット構成次第。 |
| E08 CC#94 | GSモードでもVariation Sendとして処理する指摘は正しい。 |
| P01 Pre-LPF等 | 未反映は正しい。 |
| P02 Random Pan | SysExについては正しいが、CC#10については誤り。 |
| P03 Chorusのゼロ値 | ゼロ指定でマクロ値へ戻る指摘は正しい。ただしRate=0を0Hzとする根拠は別途必要。 |
| U01 GS Delay | DSP未実装は正しい。受信アドレスの解釈にも誤りがある。 |
| U02 パート受信スイッチ | 記載された受信設定の未実装を確認。 |
| U03 パート動作モード | 記載されたSysEx設定の未実装を確認。 |
| U04 Key Range | 記載されたキー範囲設定の未実装とGSモードでの判定バイパスを確認。 |
| U05 Insertion Effect | GS用受信・DSP処理の不在を確認。種類数などのSC-88Pro固有仕様は今回未検証。 |
| U06 RPN Fine/Coarse Tune | 独自ハンドラ不在を理由に「未実装」とする分類は不適切。 |

## 重要な訂正事項

### 1. E02・E03の再現例はチェックサム不正

掲載例はチェックサム検証で破棄されるため、そのままではDSP未反映と受信拒否を切り分けられない。

| 項目 | 掲載チェックサム | 正しいチェックサム |
| :--- | :--- | :--- |
| E02 | `00H` | `01H` |
| E03 | `17H` | `1BH` |

修正済みメッセージ:

```text
E02: F0 41 10 42 12 40 01 3F 7F 01 F7
E03: F0 41 10 42 12 40 11 40 54 1B F7
```

E01・E04・E05の掲載チェックサムは計算上正しい。ただしE04はデータ値自体に問題がある。

### 2. E04のMaster Panは01H〜7FH

左端は `00H` ではなく `01H`。修正した再現例は以下となる。

```text
F0 41 10 42 12 40 00 06 01 39 F7
```

`masterPan` が保持されるだけで音声処理に使用されないという指摘自体は正しい。

参照: [SC-55公式資料 p.78](https://cdn.roland.com/assets/media/pdf/SC-55_OM.pdf#page=78)、[受信処理](../Source/FluidSynthEngine.cpp#L1341)。

### 3. P02はCC#10とSysExの区別が必要

公式表にはPart PanpotとCC#10の対応について「ランダムを除く」とある。CC#10=0で左端になることを不具合とする記述は削除すべきである。

一方、SysEx `40 1x 1C = 00H` によるランダム指定が未反映という指摘は残る。ドラムノート単位のランダムPanの実装は、パート全体のランダムPanへの対応を意味しない。

参照: [SC-55公式資料 p.80](https://cdn.roland.com/assets/media/pdf/SC-55_OM.pdf#page=80)、[Part Pan受信処理](../Source/FluidSynthEngine.cpp#L1474)。

### 4. U06はFluidSynthによる対応と評価すべき

RPN受信処理はCC#101・100・6・38をFluidSynthへ渡している。FluidSynth側もRPN 1（Fine Tune）・2（Coarse Tune）の対応を明記している。

したがって、独自デコードハンドラがないことだけを理由に未実装と判定することはできない。実音確認は未実施であり、ほかのチューニング設定との組み合わせを含む完全な動作保証までは行わない。

参照: [RPN受信処理](../Source/FluidSynthEngine.cpp#L3868)、[FluidSynth公式対応表](https://www.fluidsynth.org/wiki/FluidFeatures/)。

### 5. U01は受信デコードにも誤りがある

GS DelayはDSP未実装に加え、受信処理のアドレス解釈も一致していない。

| アドレス | 公式の意味 | 現実装 |
| :--- | :--- | :--- |
| `40 01 53` | Delay Time Ratio Left | Feedbackとして保持 |
| `40 01 59` | Delay Feedback | Send to Reverbとして保持 |
| `40 01 5A` | Delay Send Level to Reverb | 対象範囲外で未受信 |

「受信・パラメータ保持は実装済み、DSPだけ未実装」という説明は訂正が必要である。

参照: [SC-88ST公式資料 p.50](https://cdn.roland.com/assets/media/pdf/SC-88ST_OM.pdf#page=50)、[Delay受信処理](../Source/FluidSynthEngine.cpp#L1398)。

### 6. Pitch Offset Fineの「正確に実装済み」という評価は誤り

規格範囲はraw値 `08H〜F8H`、中心 `80H`、対応する周波数差は±12Hzである。従って周波数差の換算は以下となる。

```text
周波数差[Hz] = (raw - 128) × 0.1
```

現実装は `(raw - 128) × (12 / 128)` を使用するため、規格上の両端で±11.25Hzとなる。Hzからcentsへの対数変換の前段に誤りがあり、「高精度に反映」「正確に実装済み」という評価は修正すべきである。

参照: [SC-55公式資料 p.80](https://cdn.roland.com/assets/media/pdf/SC-55_OM.pdf#page=80)、[換算処理](../Source/GsModel.h#L148)。

### 7. E01はパラメータごとの修正が必要

Address Highが `41H` であるという指摘は正しい。ただしRxOff/RxOnの番号 `7/8` は現実装でも一致している。SC-88のDelay Sendは `9` であるのに対し、現実装は `6` を使用している。全caseを一律に1つずらしても修正にはならない。

また、実装がドラム設定として誤受信する `40 2x` は本来コントローラー設定領域である。正規の別機能のメッセージがドラム設定を変更する問題もある。

参照: [SC-88公式資料 pp.7-30〜7-32](https://cdn.roland.com/assets/media/pdf/SC-88_OM.pdf#page=132)、[Drum Setup受信処理](../Source/FluidSynthEngine.cpp#L1509)。

## その他の注意点

- E05: Flangerマクロの固定ディレイは2msであるため、レビュー記載の「5〜15ms」は「2〜15ms」に訂正する。
- E06: Standard Kit、さらにメロディック音色を含むプリセットへフォールバックするコードは存在する。ただし、SoundFontに未収録であることと対象機種の規格上未定義であることは区別すべきである。「常に直前のキットを保持する」をGS全体の修正方針とするには、対象機種の根拠が必要である。
- E07: GSリセット時にXG由来のBank MSB=127が入ることは確認できるが、実際の音色選択への影響は後段の探索とSoundFontの構成に依存する。
- E08: `gsPartParameters[channel].delaySend` の更新だけでは音声への反映は完了しない。U01のGS Delay処理系も必要である。
- P03: ゼロ値をマクロ既定値扱いする条件分岐は確認できる。ただし「Rate=0なら必ず0Hz」「Feedback=0ならLFOが停止」といった意味付けは避け、Rate・Depth・Feedbackを分けて評価すべきである。
- 「実装済み機能」の一覧は、処理の存在と規格適合の保証を区別する必要がある。今回の検証では実音比較や網羅的な適合試験は行っていない。

## 参照資料

- [Roland SC-55 Owner's Manual](https://cdn.roland.com/assets/media/pdf/SC-55_OM.pdf)
- [Roland SC-88 Owner's Manual](https://cdn.roland.com/assets/media/pdf/SC-88_OM.pdf)
- [Roland SC-88ST Owner's Manual](https://cdn.roland.com/assets/media/pdf/SC-88ST_OM.pdf)
- [FluidSynth公式機能・RPN対応表](https://www.fluidsynth.org/wiki/FluidFeatures/)

コードへの行番号リンクは検証時点のものであり、今後の変更によりずれる可能性がある。
