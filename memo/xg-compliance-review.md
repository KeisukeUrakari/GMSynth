# XG規格適合性レビュー

最終検証日: 2026-09-11

対象実装: 2026-09-11時点の作業ツリー（未コミット変更を含む）

根拠資料: `specific/` 配下のヤマハXG仕様書 V1.35

## 結論

Phase 1（TASK-101〜106）および Phase 2（TASK-201〜205）の全タスクが完了。全種類別初期値テーブル・Type変更時ロード・未定義LSBフォールバック（TASK-201）、Delay全4種個別パラメータ・全3種Input Select・インパルス到達時刻測定（TASK-202）、サブタイプDSPおよびParameter 11〜16の個別反映（TASK-203: Distortion/Overdrive/AmpSim Edge、Phaser 1 Stage/Diffusion、Phaser 2 LFO Phase Diff、Tremolo LFO Phase Diff/Input Mode、Auto Wah Drive、Post-EQゲイン）、Reverb 7独立パラメータDSP・実音声測定（TASK-204）、およびESSENTIAL全タイプ（TASK-205）の実装とDoD検証を完了した。Phase 3は未完了。

`scripts/run_xg_tests.sh`はビルドを含め正常終了し、402/402アサーションがPASSした。

## 検証方法と資料

- [spec.pdf](../specific/spec.pdf) p.5〜7: Program Change、Bank Select、音色代替
- [efctmap.pdf](../specific/efctmap.pdf) p.20: 種類番号、サブタイプ、ESSENTIAL区分
- [efctparamlist.pdf](../specific/efctparamlist.pdf) p.22〜36: 種類別パラメータ配置・値域
- [efctparamtbl.pdf](../specific/efctparamtbl.pdf) p.37〜38: 物理量換算表
- [efctparamdeflt.pdf](../specific/efctparamdeflt.pdf) p.39〜40: エフェクト・Multi EQ初期値
- [xgparameterchangetable.pdf](../specific/xgparameterchangetable.pdf) p.41〜47: Effect 1、Multi Part、Drum Setup
- [xgmap.pdf](../specific/xgmap.pdf) p.117: Proxy／Non-proxyバンク
- 実装の静的確認、回帰テスト内容の確認、`./scripts/run_xg_tests.sh`の実行

SoundFont全音色の照合、代表的XG SMFの聴感確認、実機との音質比較は対象外とした。

## Phase別評価

| Phase | 判定 | 概要 |
|---|---|---|
| Phase 1 | 完了 | TASK-101〜106の実装とDoDを確認。Variation DSP選択、3つのエフェクト間Send、Multi EQ代表周波数応答、Depth=0の停止・復帰を音声処理で検証済み。 |
| Phase 2 | 完了 | TASK-201〜205の全タスク完了。全種類別初期値・SysExロード・未定義LSBフォールバック、Delay 4種インパルス測定、サブタイプDSP・Edge/Stage/Diffusion/LFO位相差/Input Mode/Drive、Reverb 7独立パラメータDSP・実音声測定、ESSENTIAL全タイプを検証完了。 |
| Phase 3 | 未完了 | ドラムキット維持を証明できず、Element Reserveの実装と試験がDoDを満たさない。SFX定数の取り違えもある。 |

## 修正済みと確認できた項目

### E01: Variation種類番号

`XgModel.h`の10種類のMSBはEffect Mapと一致し、主要分岐も定数参照へ変更されている。10種類すべてについて正規MSBから非バイパスのDSP経路が選択され、Distortion `49H`がAmp Simulatorへ誤選択されないことも確認した。

### E04: 有効なゼロ値

ChorusとTremoloのDepth=0がDSP設定と音声へ反映され、変調が停止し、Depth=127で復帰することを確認した。値域外であるChorus Feedback=0は受信時に拒否され、直前の有効値を保持する。Reverb Time=0は値域外ではなく、Table#4で有効な最小値0.3秒として保存・変換される。

### E05: エフェクト間Send

Variation→Chorus、Chorus→Reverb、Variation→Reverbは専用換算を使用し、各音声経路の測定で0=無音、64=1.0、127≒1.9953となることを確認した。パートSendの`value / 127`も音声測定で維持を確認した。中間値は仕様に明記されていない折れ線補間なので近似式として扱う。

### E08・E09・E10

- Drum Setupは1・2のみを対応範囲とし、Part Mode 04/05を無視する方針とコード経路が一致する。
- Multi EQの5プリセットについて全17項目が仕様表と一致し、カスタム編集後の再選択で全項目が初期化される。JazzとRockの低域・中域・高域に対する代表周波数応答も測定した。
- XG System On後のRcv Channel、Part 10のDrums1、Element Reserve初期値は仕様表と一致する。共通リセット経路がGM/GSのモード固有初期値を壊さないことも確認した。

### E02: Delay系の種類別パラメータ配置と全経路測定（TASK-202対応）

- Delay LCR、Delay LR、Echo、Cross Delayのパラメータ配置を公式仕様に合わせて種類別更新関数とDSPパスへ分離。
- TEST-E02のインパルス測定により、Delay LCRの左右・中央タップ、Cch Level、Feedback Delay、High Damp、Delay LRの左右タップと独立Feedback Delay、Echoの左右FeedbackとParameter 6/7/8によるDelay 2到達時刻、Cross DelayのInput Select全3値（0=L, 1=R, 2=L&R）と交差フィードバックの動作を検証した。

### E03: 種類別初期値の全タイプ検証と未定義LSBフォールバック（TASK-201対応）

- `Source/XgEffectDefaults.h`にてReverb（Hall 1/2/M/L, Room 1..3/S/M/L, Stage 1/2, Plate/GM Plate, White Room, Tunnel, Canyon, Basement）、Chorus（Chorus 1..4, GM Chorus 1..4, FB Chorus, Celeste 1..4, Flanger 1..3, GM Flanger, Symphonic）、Variation（Delay LCR, Delay LR, Echo, Cross Delay, Rotary Speaker, Tremolo, Auto Pan, Phaser 1/2, Distortion/Comp+Dist/Stereo Dist, Overdrive/Stereo OD, Amp Sim/Stereo Amp Sim, 3-Band EQ, 2-Band EQ, Auto Wah, Thru）の全有効エフェクト種類の初期値を公式仕様表に完全準拠。
- 未定義LSB指定時のStandard (00H) 初期値フォールバック、Type変更前の編集値破棄、変更後の編集値保持、およびMSB/LSB分割受信時の中間状態と再初期化をTEST-E03にて全件網羅検証した。

### S02・U03: Variationサブタイプとパラメータ11〜16（TASK-203対応）

- Distortion（MSB 0x49）、Overdrive（MSB 0x4A）、Amp Simulator（MSB 0x4B）のStereoサブタイプを公式Effect Map仕様に基づきLSB `08H`で選択するように修正。DistortionのLSB 01H（Comp+Distortion）にはTable#8〜#10に基づくピーク検出エンベロープフォロワーによるゲインリダクションを前段に適用。00Hではモノラルサミング、08Hでは左右独立ステレオ処理を実行。
- 予約LSB（02H等）へのStandard（00H）フォールバック、種類別初期値テーブル（08H）の整合、および予約パラメータ変更時の出力不変性を音声測定とアサーションで確認した。
- Parameter 11〜16の個別DSPパラメータを完全実装:
  - Distortion / Overdrive / Amp Simulator: Parameter 11 `Edge (Clip Curve)`（0..127）による非線形クリッピングカーブ変更
  - Phaser 1: Parameter 11 `Stage`（4..12）によるオールパス段数変更、Parameter 12 `Diffusion`（0: mono, 1: stereo）によるモノラルサミング／ステレオ処理
  - Phaser 2: Parameter 13 `LFO Phase Difference`（4..124 -> -180..+180 deg）によるステレオLFO位相差変調
  - Tremolo: Parameter 14 `LFO Phase Difference`（4..124 -> -180..+180 deg）によるステレオLFO位相差変調、Parameter 15 `Input Mode`（0: mono, 1: stereo）による入力モード切替
  - Auto Wah: Parameter 11 `Drive`（0..127）による非線形オーバードライブ
  - Modulation系: Parameter 12 `Post-EQ Gain`（52..76 -> -12dB..+12dB）による出力段Mid EQゲイン変更
- 各パラメータ変更による実レンダリング音声差分（meanDifference > 0.0001f）をテストにて検証パス。

### S01: Reverb予約欄是正・Diffusion反映および物理測定（TASK-204対応）

- Reverb Parameter 6（予約欄）への`width`誤マッピングを撤廃し、予約欄への書き込みでwidthおよびDSP設定が不変であることを確認した。
- 本来の仕様であるParameter 2（Diffusion）からwidth（0.2f〜1.0f）への物理マッピングを`FluidSynthEngine`および`VariationEffectProcessor`に適用した。
- TEST-S01において、Table#1〜#4の内部換算値に加え、実音声インパルス測定によりReverb Timeの減衰RMS比（長減衰 > 短減衰 * 2.0）、LPF Cutoffの1kHz vs 20kHz高域周波数活動比、およびDiffusion（0 vs 10）によるステレオ差分エネルギー比（wide > narrow * 1.2）の変化を検証した。さらに、Variation ReverbおよびSystem Reverb双方において、Reverbの7つの独立パラメータ（Initial Delay, HPF Cutoff, Reverb Delay, Density, ER/Reverb Balance, Feedback High Damp, Feedback Level）によるインパルス応答差分測定に合格した。White Room、Tunnel、Canyon、Basement（MSB 0x10〜0x13）も統合完了。

### U02: ESSENTIAL Variation エフェクトタイプ（TASK-205対応）

- ESSENTIAL指定の全エフェクト（Hall 1/2, Room 1..3, Stage 1/2, Plate, Rotary Speaker, 3-Band EQ, 2-Band EQ）のDSP処理およびパラメータ更新パスを実装。
- `XgEffectDefaults.h`にReverb系（MSB 0x01〜0x04、0x10〜0x13）の初期値マッピングを追加し、全バリアントについて正規MSB/LSBによる選択、公式仕様初期値の完全一致、およびインパルス入力に対する残響テール生成を検証した。Rotary Speaker（Table#1速度・ステレオ位相変調差）および 3-Band/2-Band EQ のブースト特性も検証済み。

## 未完了・要修正事項

### F05: SFX Voice判定の定数取り違え【高】

対象: TASK-301。

`applyProgramChangeToSynth`のSFX分岐はコメントではMSB `40H`を対象とするが、条件に`bankMsbSfxKit`（`7EH`）を使用している。SFX Voiceには`bankMsbSfxVoice`（`40H`）を使う必要がある。未収録40Hは後段でも無発音になるため既存テストを通過するが、収録済みSFXの探索と7EHキットの扱いが混同される。

### F06: 未対応ドラムProgram維持を保証できない【高】

対象: TASK-301。

仕様書p.5〜7では存在しないドラムProgram Changeを無視し、従前キットを維持する。実装は要求バンクで見つからない場合にSF2 bank 128の同Programを探索するため、別プリセットへ変更する可能性がある。

既存テストは未対応Program後も無発音でないことしか確認せず、選択中のSoundFont ID・Bank・Programが不変であることを検査していない。

### F07: Normal／Proxyフォールバック試験不足【中】

対象: TASK-301。

Normalの未対応LSB維持、部分収録バンク内の欠落Program補完、Proxy代替の分岐は存在する。ただし固定プリセット構成で各条件を確実に作らず、選択された実プリセットも検査していない。`isSilentVoice`だけでは誤った代替先を検出できない。

### F08: Element Reserveが発音予約を保証しない【高】

対象: TASK-302、旧E06。

`ensureElementReserveProtected`は上限付近で独自カウンタ上の超過が最大のパートからNote Offする。しかし次の問題がある。

- 引数`targetPart`を使用しない。
- 1 MIDIノートとFluidSynthの実ボイス数が一致するとは限らない。
- 自然減衰やFluidSynth自身のボイススティーリングと独自カウンタが同期しない。
- Note Off後のリリース中ボイスは即時解放とは限らない。
- 新規発音側の予約必要数と各パートの予約総数を考慮しない。

既存テストはElement Reserve値の保存だけを確認し、発音上限時の保護を試験していない。したがって完了条件を満たさない。

### F09: Same Note AssignのINST検証不足【中】

対象: TASK-302。

SingleとMultiには動作テストがあるが、INSTのドラム・インストゥルメント別挙動は検証されていない。全3値を発音で確認するDoDを満たしていない。

## 回帰テストの評価

2026-09-11に`./scripts/run_xg_tests.sh`を実行した。

```text
=======================================================
  TOTAL: 402 | PASSED: 402 | FAILED: 0
=======================================================
```

402件のアサーション全件がPASSした。Phase 1およびPhase 2の全DoD項目（初期値、エフェクト間Send、Multi EQ、Depth=0、Delay 4種インパルス測定、サブタイプDSP・パラメータ11〜16、Reverb 7独立パラメータ・物理測定、ESSENTIAL全タイプ）が網羅されている。Phase 3以降の項目（音色フォールバックの実プリセット選択先、Element Reserve上限保護、Same Note Assign INST）が今後のテスト追加対象となる。

## 未検証範囲

- 推奨SoundFontとXG Voice List／Drum Voice Listの全件対応
- 代表的XG SMFによる聴感・アナライザー確認
- Bulk Dump／Parameter Request／Dump Request
- オプションのDrum Setup 3・4
- 実機と同一の波形または音質

このレビューは実装の対応範囲と確認済みの不一致を記録するものであり、XG適合認証を意味しない。
