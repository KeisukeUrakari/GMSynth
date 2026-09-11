# XG規格適合性レビュー

最終検証日: 2026-09-11

対象実装: `70d7b30`

根拠資料: `specific/` 配下のヤマハXG仕様書 V1.35

## 結論

Phase 1は主要な修正を確認できたが、完了条件に含まれる実音・周波数応答の測定には不足が残る。Phase 2とPhase 3は未完了であり、従来の「Phase 1〜3全タスク完了」は裏付けられない。

`scripts/run_xg_tests.sh`はビルドを含め正常終了し、107/107アサーションがPASSした。ただし、これは現在書かれているテストの通過を示すだけで、各タスクのDoDやXG規格全体への適合を証明しない。

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
| Phase 1 | 概ね実装済み・測定不足 | 定数、初期値、Send換算、Drum Setup方針は主要経路で修正済み。実音・全プリセット応答のDoDは未充足。 |
| Phase 2 | 未完了 | Variation LSBサブタイプのDSP差、Reverbの有効パラメータ反映、Delay全経路の測定が不足。 |
| Phase 3 | 未完了 | ドラムキット維持を証明できず、Element Reserveの実装と試験がDoDを満たさない。SFX定数の取り違えもある。 |

## 修正済みと確認できた項目

### E01: Variation種類番号

`XgModel.h`の10種類のMSBはEffect Mapと一致し、主要分岐も定数参照へ変更されている。Distortion `49H`、Overdrive `4AH`、Amp Simulator `4BH`のDSP選択テストも通過した。

### E04: 有効なゼロ値

ChorusとTremoloのDepth=0がDSP設定へ反映され、非ゼロ値で復帰することを確認した。ただし全種類のゼロ許容パラメータを網羅したものではない。

### E05: エフェクト間Send

Variation→Chorus、Chorus→Reverb、Variation→Reverbは専用換算を使用し、0=無音、64=1.0、127≒1.9953となる。パートSendの`value / 127`は維持されている。中間値は仕様に明記されていない折れ線補間なので近似式として扱う。

### E08・E09・E10

- Drum Setupは1・2のみを対応範囲とし、Part Mode 04/05を無視する方針とコード経路が一致する。
- Multi EQの5プリセット値は仕様表と一致する実装へ更新された。ただし全バンド・Shapeの実周波数応答は未測定。
- XG System On後のRcv Channel、Part 10のDrums1、Element Reserve初期値は仕様表と一致する。

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
TOTAL: 107 | PASSED: 107 | FAILED: 0
```

107は独立した仕様項目数ではなくアサーション数である。次のテスト追加が必要となる。

1. 全Effect Type MSB/LSBの初期値とType変更後の状態
2. Delay全タイプのインパルス到達時間、Input Select全値、Feedback、High Damp
3. DistortionサブタイプごとのDSP差と予約LSB
4. Reverbの実測減衰時間、Diffusion、LPF周波数応答
5. Multi EQ全プリセット・全バンド・Shapeの周波数応答
6. 固定プリセット構成によるNormal、SFX、Proxy、Drumの選択先
7. 発音上限到達時のElement ReserveとSame Note Assign INST

## 未検証範囲

- 推奨SoundFontとXG Voice List／Drum Voice Listの全件対応
- 代表的XG SMFによる聴感・アナライザー確認
- Bulk Dump／Parameter Request／Dump Request
- オプションのDrum Setup 3・4
- 実機と同一の波形または音質

このレビューは実装の対応範囲と確認済みの不一致を記録するものであり、XG適合認証を意味しない。
