# XG規格適合性レビュー

最終検証日: 2026-09-11

対象実装: 2026-09-11時点の作業ツリー（未コミット変更を含む）

根拠資料: `specific/` 配下のヤマハXG仕様書 V1.35

## 結論

Phase 1（TASK-101〜106）は、仕様書、実装、完了条件（DoD）および回帰テストを再照合し、定義された対応範囲について完了と判定した。Phase 2とPhase 3は未完了であり、「Phase 1〜3全タスク完了」とは判定しない。

`scripts/run_xg_tests.sh`はビルドを含め正常終了し、153/153アサーションがPASSした。Phase 1については、定数・状態値だけでなく、Variation DSP選択、エフェクト間Send、Multi EQ周波数応答、Depth=0の変調停止・復帰も音声処理で確認した。ただし、これはXG規格全体への適合認証を意味しない。

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
| Phase 2 | 未完了 | Variation LSBサブタイプのDSP差、Reverbの有効パラメータ反映、Delay全経路の測定が不足。 |
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

## 未完了・要修正事項

### F01: Variationサブタイプは保存のみ【高】

対象: TASK-203、旧S02/U03。

`VariationEffectProcessor`は`typeLsb`を保持するが、Distortion系では`isStereoDistortion = (typeLsb != 0)`とするだけで、`00H` Distortion、`01H` Comp+Distortion、`02H` Stereo Distortionを実装し分けていない。Comp+Distortion用のコンプレッサ処理もなく、明確なDSP差を確認できない。

既存テストはLSB値の保存だけを検査し、音声差、種類別初期値、予約LSBが音声を変えないことを確認していない。

### F02: Reverb／Chorus物理量対応のDoD未充足【高】

対象: TASK-204、旧S01。

Table#1〜#4の参照値は導入済みだが、Reverb TimeをJUCE Reverbの`roomSize`へ、LPFを`damping`へ、Diffusion等を`width`へ近似的に割り当てている。独立した物理パラメータとしての反映ではなく、種類別の有効項目も網羅していない。

テストは換算値・内部値・単調性が中心で、実測残響時間、Diffusion、LPF周波数応答、許容誤差を記録していない。

### F03: Delay系の音声検証不足【中】

対象: TASK-202。

Delay LCR、LR、Echo、Cross Delayの処理パスは分離済み。ただしCross DelayのInput Selectは1値だけの確認で、0/1/2全値、交差フィードバック、Echo Delay 2の到達時間、High Dampの音響結果を網羅していない。

### F04: 種類別初期値の全タイプ検証不足【中】

対象: TASK-201。

初期値表とType変更時のロード経路は導入済みだが、テストは一部タイプに限られる。対応を表明する全MSB/LSB・全有効パラメータの仕様表照合がない。MSB/LSBを別々に受信した際の中間組合せや未定義LSBの扱いも明文化・検証が必要である。

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
TOTAL: 153 | PASSED: 153 | FAILED: 0
```

153は独立した仕様項目数ではなくアサーション数である。Phase 1のDoDに必要な検査は追加されたが、Phase 2以降について次のテスト追加が必要となる。

1. 全Effect Type MSB/LSBの初期値とType変更後の状態
2. Delay全タイプのインパルス到達時間、Input Select全値、Feedback、High Damp
3. DistortionサブタイプごとのDSP差と予約LSB
4. Reverbの実測減衰時間、Diffusion、LPF周波数応答
5. Multi EQの未測定プリセット・バンド・Shapeを含む、より詳細な周波数応答
6. 固定プリセット構成によるNormal、SFX、Proxy、Drumの選択先
7. 発音上限到達時のElement ReserveとSame Note Assign INST

## 未検証範囲

- 推奨SoundFontとXG Voice List／Drum Voice Listの全件対応
- 代表的XG SMFによる聴感・アナライザー確認
- Bulk Dump／Parameter Request／Dump Request
- オプションのDrum Setup 3・4
- 実機と同一の波形または音質

このレビューは実装の対応範囲と確認済みの不一致を記録するものであり、XG適合認証を意味しない。
