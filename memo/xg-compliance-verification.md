# XG規格対応状況の検証

検証日: 2026-09-12

対象: 検証時点のソースコードと `specific/` に保存されたXG V1.35仕様書。

## 結論と判定基準

主要なXG再生機能を実装しているが、規格全体としては一部対応であり、実装済み機能にも仕様との不一致がある。既存レビューの「Phase 1・2完了」を、そのままXG仕様への完全対応とは判断できない。

依頼の分類には「未対応」が重複していたため、以下の4区分で整理した。

- **対応済み**: 今回確認した範囲で、実装と仕様の整合を確認できた。
- **一部対応**: 一部の処理はあるが、未反映項目や定量検証の不足が残る。単に値を保存している場合も含む。
- **未対応**: 対象機能の受信・適用処理がない。
- **対応しているが誤りがある**: 実装が存在し、仕様との具体的な不一致を確認した。

「対応済み」は規格全体の適合認証を意味しない。また、「一部対応」には実装不足と検証不足の双方が含まれるため、各行の説明と併せて読むこと。

## 検証方法と根拠資料

ソースコードの受信・保存・DSP反映経路を仕様書と照合し、既存のXG回帰テストを実行した。

- [spec.pdf](../specific/spec.pdf): MIDIメッセージ、音色代替、リセット等
- [xgparameterchangetable.pdf](../specific/xgparameterchangetable.pdf): System、Effect、Multi Part、Drum Setup等のアドレスと値域
- [efctmap.pdf](../specific/efctmap.pdf): エフェクトの種類番号とサブタイプ
- [efctparamlist.pdf](../specific/efctparamlist.pdf): エフェクトごとのパラメータ配置と意味
- [efctparamtbl.pdf](../specific/efctparamtbl.pdf): 物理量換算表
- [efctparamdeflt.pdf](../specific/efctparamdeflt.pdf): エフェクト初期値
- [drum_default.pdf](../specific/drum_default.pdf): 音符別ドラム初期値

実行コマンド:

```sh
./scripts/run_xg_tests.sh
```

結果:

```text
TOTAL: 402 | PASSED: 402 | FAILED: 0
```

この結果は既存アサーションの成功を示すものであり、全仕様項目の一致を示すものではない。以下の不一致には、現在のテストでは検出できないものが含まれる。

## 機能別の対応状況

| 機能                                             | 分類                     | 確認結果                                                                       |
| ------------------------------------------------ | ------------------------ | ------------------------------------------------------------------------------ |
| 16パートのNote On／Off・基本演奏                 | 対応済み                 | FluidSynthへの発音経路を実装。基本発音の回帰テスト成功                         |
| Rcv Channel・同一チャンネルの複数パート受信・OFF | 対応済み                 | パート別に振り分け、OFFでは受信しない                                          |
| XG System On                                     | 対応済み                 | モード切替と主要パラメータ初期化を実装                                         |
| Master Volume・Master Tune・Transpose            | 一部対応                 | 受信・反映経路あり。全値域、他の調律設定との併用は未検証                       |
| Master Attenuator                                | 一部対応                 | 値を保存するが、マスターゲイン計算に使用していない                             |
| Bank Select・Program Change・音色代替            | 対応しているが誤りがある | SFX判定の定数取り違え。ドラムの従前キット維持も保証できない                    |
| XG Voice／Drum Voiceの収録                       | 一部対応                 | 外部SoundFontに依存。仕様音色一覧との全件一致は未確認                          |
| Reset All Controllers（CC121）                   | 対応しているが誤りがある | 仕様のリセット対象にないVolumeとPanまで初期化                                  |
| パート別フィルター・EG・ビブラート・NRPN         | 一部対応                 | 主要項目を実装。音響特性・全値域の仕様一致は未確認                             |
| MW／Bend／CAT／AC1／AC2制御                      | 一部対応                 | 主要な変調先への反映あり                                                       |
| Poly AftertouchのXG制御先設定                    | 一部対応                 | PAT設定を保存するが、受信時はFluidSynthへのキー圧転送のみ                      |
| Rcv NOTE／PROGRAM／CC等の個別受信スイッチ        | 未対応                   | `08 nn 30H〜40H`の受信・適用処理なし                                           |
| XG Scale Tuning                                  | 未対応                   | `08 nn 41H〜4CH`の処理なし。GS側の実装とは別                                   |
| Velocity Limit Low／High                         | 未対応                   | `08 nn 6DH／6EH`の処理なし                                                     |
| Element Reserve                                  | 対応しているが誤りがある | ノート数と実ボイス数が混在し、指定した発音予約数を保証しない                   |
| Same Note Assign                                 | 一部対応                 | Single／Multiの処理あり。INSTの音色別挙動は未検証                              |
| Part EQ                                          | 対応しているが誤りがある | 仕様の±12 dB相当の範囲に対し、実装は最大−64～＋63 dB                           |
| Drum Setup 1／2                                  | 一部対応                 | 音程・レベル・パン・Send・EG等を実装。音符別初期値表は反映していない           |
| Drum Setup 3／4                                  | 未対応                   | 明示的に無視する実装                                                           |
| エフェクト初期値・Type変更時ロード               | 一部対応                 | 実装対象の多数の種類を検査済み。ただしEffect Map全種類は未対応                 |
| Reverb／Chorus／Variationの結線・Send            | 一部対応                 | 主要結線とSendを実装・テスト。全パラメータの仕様一致とは別                     |
| Hall／Room／Stage／Plate                         | 一部対応                 | 残響・遅延・HPF等あり。独自DSPへの近似で、指定残響時間等の絶対精度は未証明     |
| White Room／Tunnel／Canyon／Basement             | 一部対応                 | 初期値と残響経路あり。固有のParameter 6〜9を未反映                             |
| System Chorus                                    | 対応しているが誤りがある | 種類ごとの配置・DSPを分離せず、共通Chorusとして処理                            |
| Variation Celeste                                | 一部対応                 | 初期値はあるがDSP選択に分岐がなく、原音通過                                    |
| Variation Symphonic                              | 対応しているが誤りがある | Parameter 3のDelay OffsetをFeedbackとして解釈                                  |
| Delay LCR／LR／Echo／Cross Delay                 | 一部対応                 | 主要タップ・フィードバックは実装・検査済み。Parameter 11／12のHPF・LPFが未反映 |
| Distortion／Overdrive                            | 一部対応                 | Edgeやサブタイプを実装。Output Level=0を既定値に置き換える問題あり             |
| Amp Simulator                                    | 対応しているが誤りがある | AMP Type・LPF・Output Levelの配置とDSP処理が不一致                             |
| Phaser                                           | 対応しているが誤りがある | Stage／Diffusion実装後も同じパラメータをMid EQとして二重解釈                   |
| Tremolo／Auto Pan                                | 一部対応                 | AM変調・位相差等あり。PM DepthやF/R Depth、PAN Direction等は未反映             |
| Rotary Speaker・3-Band EQ                        | 一部対応                 | DSPと代表的な音声テストあり。全パラメータの定量検証は未完了                    |
| Multi EQ                                         | 対応済み                 | 5プリセット・5バンド・両端Shapeを実装。初期値と代表周波数応答を検査            |
| Bulk Dump／Parameter Request／Dump Request       | 未対応                   | XG Parameter Change以外のメッセージ処理なし                                    |
| 独立Insertion Effect（Effect 2）                 | 未対応                   | `03 n aa`の処理なし。VariationのInsertion接続とは別機能                        |
| Display Letter／Bitmap・A/D Part                 | 未対応                   | 対応するXGアドレスの処理なし                                                   |

## 特に修正が必要な不一致

### 1. CC121が音量とパンを変更する

`spec.pdf` p.14のリセット対象はPitch Bend、Modulation、Expression、各ペダル等である。

`Source/FluidSynthEngine.cpp`の`dispatchMidiMessageToPart`内、`message.isResetAllControllers()`分岐はVolumeとPanも初期化する。曲中のCC121でミックスが変わる。

### 2. Amp Simulatorのパラメータ配置が異なる

`efctparamlist.pdf` p.30ではParameter 2がAMP Type、3がLPF、4がOutput Levelである。

`Source/VariationEffect.cpp`の`updateDistortionParameters`はParameter 5を出力レベルとして読む。Amp SimulatorのLPFは4500 Hz固定であり、さらにParameter 2／3をLow EQとしても解釈する。

### 3. Symphonicのパラメータ配置がChorusと混同されている

`efctparamlist.pdf`のSymphonicではParameter 3がDelay Offsetで、4は予約欄である。

`Source/VariationEffect.cpp`の`updateChorusParameters`は3をFeedback、4をDelay Offsetとして使用する。Symphonicもこの共通処理へ入る。`Source/FluidSynthEngine.cpp`の`updateChorusSettings`にも同じ問題がある。

### 4. PhaserのStage／DiffusionがEQにも作用する

仕様のPhaser 1 Parameter 11／12はStage／Diffusionである。

`Source/VariationEffect.cpp`の`updatePhaserParameters`では、これらをMid Frequency／Gainとしても使用する。「変更すると音が変わる」という現在のテストだけでは、誤った効果まで合格になる。

### 5. Part EQの増減幅が過大

Parameter Change Tableでは`00H〜7FH`が−12～＋12 dB相当である。

`Source/FluidSynthEngine.cpp`の`updatePartEqCoefficients`は`value - 64`をそのままdBとして使用し、最大−64～＋63 dBになる。

### 6. SFXのバンク判定が異なる

仕様のSFX VoiceはMSB `40H`である。

`Source/FluidSynthEngine.cpp`の`applyProgramChangeToSynth`は`bankMsbSfxKit`（`7EH`）と比較している。コメントと条件式が一致していない。

### 7. 拡張Reverbの固有パラメータを無視する

White Room等のParameter 6〜9はWidth／Height／Depth／Wall Varyである。Hall系では予約欄だが、拡張Reverbでは有効である。

現在は`Source/VariationEffect.cpp`の`updateVariationReverbParameters`等の共通Reverb更新処理へ入り、これらを使用しない。

## 評価の限界

XGは仕様自体が機能の省略を認めている。未対応のオプション機能が存在することと、実装済み機能が仕様と異なることは区別する必要がある。

規格全体の対応率をパーセントで示すには、必須機能・オプション機能・対象音色セットを分けた集計が必要であり、本調査では対応率を算出しない。

本調査は主要機能の静的照合と既存回帰テストによる評価であり、全仕様行の網羅検証、全SoundFont音色の照合、実機との音質比較は行っていない。主要なXG再生機能はあるが、メッセージの意味やパラメータ配置の互換性には修正が必要である。
