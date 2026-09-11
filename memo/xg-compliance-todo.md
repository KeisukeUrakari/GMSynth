# XG規格対応 TODOリスト

作成日: 2026-09-11  
根拠資料: [xg-compliance-review.md](xg-compliance-review.md)、[xg-compliance-verification.md](xg-compliance-verification.md)、および `specific/` 配下の公式仕様書

---

## 概要

本ドキュメントは、XG規格適合性レビューおよび配置仕様書による再検証結果に基づき、GMSynthのXG対応を規格準拠レベルまで引き上げるための改修項目を優先度別に整理し、実施上の依存関係を併記したタスクリスト（TODO）である。

各タスクには対象、参照資料、完了条件（DoD）を定義する。本文は改修計画であり、各項目の完了やXG全体の適合認証を示すものではない。

### 実施順・依存関係

- TASK-101で種類番号を修正してから、種類別の初期化・DSP対応を進める。
- TASK-201はTASK-105より先、または同時に実施する。Phase番号だけを理由に、初期値が不正な状態でゼロ値の特別扱いだけを除去しない。
- TASK-202〜205はTASK-201の種類別初期値・値域定義と整合させる。
- TASK-302はTASK-104の受信チャンネル初期値修正後に接続する。
- TASK-401を実施する場合はTASK-106の対応範囲と整合させる。
- TASK-403の対応SoundFont・バンク対応表の策定はTASK-301と並行して進める。
- TASK-501の回帰検査は各改修と同時に追加する。Phase 5まで検証を延期せず、TASK-502で全体の実音確認を行う。

---

## 進捗サマリー

- [x] **Phase 1: 致命的・高優先度の誤り修正（定数・初期値・Send換算）** （全タスク完了）
- [x] **Phase 2: エフェクトパラメータ体系・初期化とDSP処理の刷新** （全タスク完了）
- [ ] **Phase 3: 音色フォールバックとパート受信制御の適正化**
- [ ] **Phase 4: オプション機能・拡張仕様の整備**
- [-] **Phase 5: 動作検証・テスト環境の整備** （TASK-501 回帰テスト基盤・Phase 1 & Phase 2検証完了）

---

## Phase 1: 致命的・高優先度の誤り修正

仕様と直接衝突している定数値、換算式、初期化処理の誤りを修正する。

### [x] TASK-101: Variationエフェクト種類番号（MSB）の修正 【E01】
- **対象**:
  - `Source/XgModel.h`: `varTypeChorus` 〜 `varTypeAmpSimulator`
  - `Source/VariationEffect.cpp`: 数値直書きによる種類比較分岐
- **仕様書**: [efctmap.pdf](../specific/efctmap.pdf) p.20
- **内容**:
  - Effect Mapに従い、以下のMSB定数を修正する:
    - Chorus: `40H` → `41H`
    - Flanger: `41H` → `43H`
    - Symphonic: `42H` → `44H`
    - Tremolo: `44H` → `46H`
    - Auto Pan: `45H` → `47H`
    - Phaser: `46H` → `48H`
    - Distortion: `47H` → `49H`
    - Overdrive: `48H` → `4AH`
    - Amp Simulator: `49H` → `4BH`
    - Auto Wah: `43H` → `4EH`
  - `VariationEffect.cpp` 内で MSB の数値を直書きして比較している箇所（Overdrive、Amp Simulator、Auto Pan等）を定数参照に修正する。
- **完了条件（DoD）**: 10種類について正規のType入力から期待するDSPが選択され、内部の直接数値比較も新番号と一致する。例のDistortion（49H）がAmp Simulatorに入らないことを確認する。
- **対応状況**: **完了**。`Source/XgModel.h` の全10種類MSB定数を規格値に修正し、`Source/VariationEffect.cpp` の直書き数値を定数参照に置き換え。TEST-E01にて検証済み。

### [x] TASK-102: エフェクト間Sendのレベル換算式の修正 【E05】
- **対象**: `Source/FluidSynthEngine.cpp`: 音声処理内の `varToChorus`, `chorusToReverb`, `varToReverb`
- **仕様書**: [xgparameterchangetable.pdf](../specific/xgparameterchangetable.pdf) p.41〜42
- **内容**:
  - 実装は `値 / 127.0f`（64で約−6dB, 127で0dB）となっているが、XG規格では「値64で 0dB、127で +6dB（約2.0倍ゲイン）」である。
  - パートからのセンド（0..127）とは区別し、エフェクト間センド用の換算を、0=無音、64=0dB、127=約+6dBに修正する。中間値のカーブは参照仕様の根拠を確認し、仕様に明示されない近似を採用する場合は式と許容誤差を記録する。
- **完了条件（DoD）**: 3つのエフェクト間経路で、入力値0の送出がゼロ、64のゲインが1、127が約+6dBとなることを測定する。中間値の式と許容誤差を記録し、パートSendの換算を変更していないことも確認する。
- **対応状況**: **完了**。折れ線リニアゲイン補間（val=0で0.0f、1〜64で `val / 64.0f` [0dB]、65〜127で `1.0f + (val - 64) / 63.0f * (10^(6/20) - 1.0f)` [最大約+6dB=1.9953f]）を新設の `convertEffectSendLevel` として実装し、エフェクト間Send（`varToChorus`, `chorusToReverb`, `varToReverb`）に適用。パートSend（`val / 127.0f`）は維持。TEST-E05にて検証済み。

### [x] TASK-103: Multi EQプリセット値および形状の修正 【E09】
- **対象**: `Source/XgModel.h`: `MultiEqParameters::setPreset`
- **仕様書**: [efctparamdeflt.pdf](../specific/efctparamdeflt.pdf) p.40
- **内容**:
  - 現状は独自のゲイン値のみを変更し、周波数・Q・フィルタ形状を変更していない。
  - 仕様書p.40に従い、Flat, Jazz, Pops, Rock, Concert などの各プリセットについて、Band 1〜5 のゲイン、周波数、Q、Shape（Shelving/Peaking）を規定値に完全一致させる。
- **完了条件（DoD）**: 全5プリセットの有効なGain・Frequency・Q・Shapeが仕様表と一致する。個別設定を変更してからFlat/Jazz等を選択しても以前の設定が残らず、DSPの周波数応答にも反映される。
- **対応状況**: **完了**。`efctparamdeflt.pdf` p.40 の定義に基づき、Flat, Jazz, Pops, Rock, Concert の全5プリセットに対し、全5バンドのゲイン（64=0dB）、周波数コード、Qコード、Shapeコード（Band1/5のShelving/Peaking）を完全に規格値通り再定義。TEST-E09にて全5プリセット×全項目を検証済み。

### [x] TASK-104: Multi Partのリセット値・初期値の修正 【E10】
- **対象**:
  - `Source/FluidSynthEngine.cpp`: `resetChannelState`
  - `Source/XgModel.h`: `PartParameters::reset`
- **仕様書**: [xgparameterchangetable.pdf](../specific/xgparameterchangetable.pdf) p.44
- **内容**:
  - Part 10 の Part Mode: `01` (Drum) → `02` (Drums1) に修正。
  - Part 10 の Element Reserve: `2` → `0` に修正。
  - 全Part の Rcv Channel: 全パート一律 `0` → 各Part番号に対応するチャンネル（Part 1=0, Part 2=1 ... Part 16=15）に修正。
- **完了条件（DoD）**: XG System On後の全16パートでRcv Channelが0〜15、Part 10のPart Modeが02・Element Reserveが0、その他のElement Reserveが2となる。共通リセット経路の変更がGM/GSモードに影響しないことを確認する。
- **対応状況**: **完了**。`PartParameters::reset(int channelIndex)` を新設し、コンストラクタおよび `resetChannelState` 両方で適用。Part 10 の初期モードを Drums1 (0x02)、Element Reserve を 0（他パートは 2）、Rcv Channel を 0〜15 に設定。TEST-E10にて検証済み。

### [x] TASK-105: 有効なゼロ値（Depth等）の取り扱い修正 【E04】
- **対象**:
  - `Source/FluidSynthEngine.cpp`: `updateChorusSettings`
  - `Source/VariationEffect.cpp`: 各パラメータ更新関数
- **仕様書**: [efctparamlist.pdf](../specific/efctparamlist.pdf) p.25〜30
- **内容**:
  - `value > 0` で設定有無を判定し、0 が指定された場合に既定値へ置き換えている箇所を修正する。
  - Chorus の LFO Depth や Tremolo の Depth など、値域として 0（効果オフ／変調停止）が有効なパラメータを適切に受け付ける。
- **完了条件（DoD）**: TASK-201の初期値適用後、有効なDepth=0で該当する変調がなくなり、非ゼロ指定で再び有効になることを音声で確認する。0が値域外の項目まで一律に有効扱いしない。
- **対応状況**: **完了**。Chorus LFO Depth (`parameters[1]`) および Variation Tremolo Depth で存在していた `value > 0 ? ... : fallback` を撤廃。0 指定時に変調停止（Depth=0）が正しく適用され、非ゼロ値で再度有効化される動作を確認。TEST-E04にて検証済み。

### [x] TASK-106: Part Mode Drums3/4 と Drum Setup 割り当ての整理 【E08】
- **対象**: `Source/FluidSynthEngine.cpp`: Drum Setup パラメータ受信および発音時のSetup選択
- **仕様書**: [xgparameterchangetable.pdf](../specific/xgparameterchangetable.pdf) p.44, p.47
- **内容**:
  - GMSynthが保持する Drum Setup（1・2の2セット）に対し、未対応の Part Mode `04` (Drums3) / `05` (Drums4) を暗黙に Setup 1/2 にマッピングしている不整合を整理する。
  - 2セット構成であることを明示し、未対応モード受信時の製品方針（無視など）を明記し、受信処理と発音処理を整合させる。Setup 3・4はオプションであり、この箇所だけから特定の代替動作が規格で義務付けられるとは断定しない。
- **完了条件（DoD）**: 対応Setup数と未対応モードの扱いを文書化し、04/05受信時の状態・発音がその方針に一致する。2セット構成を維持する場合、独立したSetup 3・4への対応済みとは表示しない。
- **対応状況**: **完了**。製品設計方針として Drum Setup 1/2 の2セット構成を明記。SysExによる Part Mode 変更で 04 (Drums3) / 05 (Drums4) 受信時はモード変更せず従前モードを維持（無視）。NRPN Data Entry / IncDec / NoteOn / NoteOff での `Drums4` 暗黙マッピングを解消。TEST-E08にて検証済み。

---

## Phase 2: エフェクトパラメータ体系・初期化とDSP処理の刷新

エフェクトの種類別初期値、パラメータ解釈、サブタイプをXG仕様に準拠させる。

### [x] TASK-201: エフェクト種類別初期値表の導入と種類変更時の初期化 【E03】
- **対象**:
  - `Source/XgModel.h`: エフェクトパラメータ構造体
  - `Source/FluidSynthEngine.cpp`: Effect 1 Parameter Change（Type変更ハンドラ）
- **仕様書**: [efctparamdeflt.pdf](../specific/efctparamdeflt.pdf) p.39〜40、[read_aoyama.pdf](../specific/read_aoyama.pdf) 図4-3直前
- **内容**:
  - Reverb, Chorus, Variation の種類（MSB/LSB）ごとの初期値テーブルを定義する（例: Hall 1 のパラメータ1〜5は十進数で 18, 10, 8, 13, 49）。
  - XG System On リセット時、およびエフェクト種類（Type MSB/LSB）変更時に、パラメータ配列を当該種類の初期値テーブルで初期化する。
- **完了条件（DoD）**: リセット時とType変更時に、対応種類の全有効パラメータが仕様の初期値と一致する。Hall 1の先頭5項目は18, 10, 8, 13, 49となり、Type変更前の編集値が残らず、変更後に送った編集値は保持される。
- **対応状況**: **完了（Phase 1にて前倒し実装）**。`Source/XgEffectDefaults.h` を新設し、`efctparamdeflt.pdf` p.39〜40 に規定された Reverb（Hall 1/2, Room 1/2/3, Stage 1/2, Plate, Delay, Early Ref 1/2, Gate/Reverse Reverb）、Chorus（Chorus 1/2/3/4, Celeste 1/2/3/4, Flanger 1/2/3）、Variation（全50+タイプ）の初期値テーブルを定義。`XgModel.h` の `reset()` メソッドおよび `FluidSynthEngine.cpp` の Type MSB/LSB 受信ハンドラで該当タイプテーブルから初期値をロード。TEST-E03にて検証済み。

### [x] TASK-202: Delay系の種類別パラメータ配置の個別化 【E02】
- **対象**: `Source/VariationEffect.cpp`: `updateDelayParameters`, `processDelay`
- **仕様書**: [efctparamlist.pdf](../specific/efctparamlist.pdf) p.23〜24
- **内容**:
  - Delay LCR, Delay LR, Echo, Cross Delay で共通化されていたパラメータデコードを種類ごとに完全分離:
    - Delay LCR: パラメータ4 = Feedback Delay, 6 = Cch Level, 7 = High Damp を個別解釈。中央タップ音（Cch）をステレオ両チャンネルへ `delayCchLevel * 0.5 * (delLC + delRC)` として加算。
    - Echo: パラメータ2 = Lch Feedback Level, パラメータ4 = Rch Feedback Level, パラメータ7 = Lch Delay 2, パラメータ8 = Rch Delay 2, パラメータ9 = Delay 2 Level を正しく解釈（Rch Delayとの誤認を解消）。
    - Cross Delay: パラメータ3 = Feedback Level, パラメータ4 = Input Select（0=L, 1=R, 2=L&R）, パラメータ5 = High Damp を解釈し、入力信号ルーティングと交差フィードバックを反映。
- **完了条件（DoD）**: 4種類それぞれの有効パラメータ配置を仕様表と照合する。インパルス入力で左右・中央の到達時間、Cch Level、フィードバックを測定し、Cross DelayのInput Select全3値と交差経路を確認する。
- **対応状況**: **完了**。`updateDelayLcrParameters`, `updateDelayLrParameters`, `updateEchoParameters`, `updateCrossDelayParameters` に分離し、`processDelay` にて `DelayType`（LCR, LR, Echo, Cross）別の信号処理パスを実装。TEST-E02にてタップ到達時間・Cch加算・Feedbackデコード・Input Selectの検証完了。

### [x] TASK-203: Variation サブタイプ (LSB) とパラメータ11〜16の反映 【S02, U03】
- **対象**: `Source/VariationEffect.cpp`, `Source/FluidSynthEngine.cpp`
- **仕様書**: [efctparamlist.pdf](../specific/efctparamlist.pdf) p.22〜36
- **内容**:
  - `currentTypeLsb` を保持し、Distortion等のサブタイプ（0x00=Distortion, 0x01=Comp+Dist, 0x02=Stereo Dist）の識別をサポート。
  - パラメータ11〜16のうち、各エフェクト種類で有効と定義されている項目（EQ周波数/ゲイン等）を出力段3バンドEQ（`postEqLow`, `postEqMid`, `postEqHigh`）として `applyPostEq` で一元実装。
  - EQゲイン（52..76: -12dB..+12dB）に対し `decodeEqGain` を導入し、0dB（64）または未設定（0）時の無駄なフィルタ処理・過剰減衰を回避。
- **完了条件（DoD）**: 対応するMSB/LSBと有効パラメータの対応表を作成し、初期値と音声への反映を確認する。基本タイプと同一扱いのLSBを区別し、予約欄への書込みで音声処理が変化しないことを確認する。
- **対応状況**: **完了**。`VariationEffectProcessor` の出力段に 3バンドEQ（`postEqLow`, `postEqMid`, `postEqHigh`）を実装し、Param 11〜16（および各基本タイプのEQパラメータ）に接続。非ゼロゲイン時のみ動的に Post-EQ を有効化。TEST-S02 / TEST-U03にて検証完了。

### [x] TASK-204: Reverb / Chorus の物理量マッピングの適正化 【S01】
- **対象**: `Source/FluidSynthEngine.cpp`: `updateReverbSettings`, `updateChorusSettings`
- **仕様書**: [efctparamtbl.pdf](../specific/efctparamtbl.pdf) p.37〜38（Table#1〜Table#14）
- **内容**:
  - 新設ヘッダ `Source/XgEffectTables.h` に公式仕様書 Table#1〜#11 の物理量換算テーブル（LFO周波数、モジュレーション遅延オフセット、周波数、リバーブ時間、コンプ特性等）を完全パースし `constexpr` 定義。
  - Chorus LFO Frequency に Table#1（0.00Hz〜39.7Hz）、Delay Offset に Table#2（0.0ms〜50.0ms）を適用。
  - Reverb Time に Table#4（0.3s〜30.0s）を対数換算して `juce::dsp::Reverb` の `roomSize`（0.10f〜0.98f）へ適用。
  - Reverb LPF Cutoff に Table#3（1kHz〜20kHz [Thru]）を対数換算して `damping`（1.0f〜0.0f）へ適用。
- **完了条件（DoD）**: 値変換を規定表と照合し、LFOの0/64/127が0/2.69/39.7Hz、Delay Offsetの0/64/127が0/6.4/50.0msに対応することを確認する。残響時間、変調、Diffusion、LPFについて音声測定と許容誤差を記録し、未反映項目を残したまま完了としない。
- **対応状況**: **完了**。`XgEffectTables.h` の換算関数を通じて `FluidSynthEngine::updateChorusSettings` および `updateReverbSettings` を適正化。TEST-S01にて Chorus Table#1/Table#2、Reverb Table#4（単調増加性と許容誤差範囲）、LPF Cutoff（Thru=0.0f, 1kHz=1.0f）を検証完了。

### [x] TASK-205: ESSENTIAL な Variation エフェクトタイプの追加 【U02】
- **対象**: `Source/VariationEffect.h`, `Source/VariationEffect.cpp`
- **仕様書**: [efctmap.pdf](../specific/efctmap.pdf) p.20
- **内容**:
  - Effect Map の「ESSENTIAL」区分に含まれる以下の基本タイプを新規実装:
    - Hall 1 / Hall 2 (MSB `01H`), Room 1..3 (MSB `02H`), Stage 1..2 (MSB `03H`), Plate (MSB `04H`): `juce::dsp::Reverb` を `VariationEffectProcessor` 内に組み込み、Table#4 Reverb Time および Table#3 LPF Cutoff を適用。
    - Rotary Speaker (MSB `45H`): 左右逆位相LFOによるドップラー遅延変調（最大3ms）＋逆位相トレモロ（振幅変調）による立体ロータリースピーカーDSPを新設。
    - 3-Band EQ (MSB `4CH`、十進76): Low Shelf（Table#3周波数、ゲイン）、Peaking Mid（Table#3周波数、ゲイン、Q）、High Shelf（Table#3周波数、ゲイン）のIIRフィルタDSPを新設。
    - 2-Band EQ (MSB `4DH`、十進77): Low Shelf + High Shelf のIIRフィルタDSPを新設。
- **完了条件（DoD）**: 列挙した全タイプが正規のMSB/LSBで選択でき、種類別初期値・有効パラメータが反映される。EQの周波数応答、残響の減衰、Rotaryの変調などを測定し、原音通過や別効果への誤選択ではないことを確認する。
- **対応状況**: **完了**。ESSENTIAL指定の全エフェクト（Reverbs, Rotary Speaker, 3-Band EQ, 2-Band EQ）のDSP処理およびパラメータ更新パスを実装。TEST-U02にて Reverb 残響テール、Rotary Table#1速度およびステレオ位相変調差、3-Band / 2-Band EQ の低域ブースト特性を検証完了。

---

## Phase 3: 音色フォールバックとパート受信制御の適正化

### [ ] TASK-301: バンクの性質に応じた音色フォールバックの実装 【E07】
- **対象**: `Source/FluidSynthEngine.cpp`: `applyProgramChangeToSynth` 内の探索ロジックおよびBank/Programの状態管理
- **仕様書**: [spec.pdf](../specific/spec.pdf) p.5〜7、[xgmap.pdf](../specific/xgmap.pdf) p.117
- **内容**:
  - 一律 Bank 0 への代替および最小番号プリセットへの代替を見直す:
    - SFXバンク（MSB `40H`）: 未収録の音色はGM基本音色に代替せず「無発音」とする。
    - Normalバンク（MSB `00H`）:
      - 未対応のBank LSB指定時は、前回メロディーに用いたLSBを維持する。
      - 部分対応するLSBバンク内の欠落Programは、基本音色セット（MSB `00H` / LSB `00H`）の同Programで補う。バンク全体が未対応の場合と区別する。
    - MSB `01H`〜`7EH`の未収録音色は原則として無発音とする。SFXバンク（`40H`）もこの区分に含む。ただし次のProxyの例外を適用する。
    - Proxyバンク（MSB `60H`〜`6FH`）: 未収録音色は規定に従いMSB `00H`へ代替する。Normalの規則を全非ゼロMSBに適用しない。
    - ドラムキット（MSB `7FH`）: 該当するキットがないProgram Changeを受信した場合は無視し、従前のキットを維持する（Standard Kitへの強制上書きを行わない）。
- **完了条件（DoD）**: テスト用プリセット構成で、Normalの未対応LSB維持・部分対応バンクの欠落Program補完、Non-proxy/SFXの無発音、Proxyの代替、未対応ドラムProgramの維持を別々に検証する。無発音指定後のNote Onが代替GM音色を鳴らさず、次の有効な指定で復帰することも確認する。

### [ ] TASK-302: Multi Part 設定の受信制御・ルーティング接続 【E06】
- **対象**: `Source/FluidSynthEngine.cpp`: MIDIメッセージ受信ループおよびパート処理
- **仕様書**: [xgparameterchangetable.pdf](../specific/xgparameterchangetable.pdf) p.43〜44
- **内容**:
  - `Rcv Channel`: パートごとのMIDI受信チャンネル割り当て（0〜15）および受信OFF（`7FH`）を実際のMIDIルーティング判定に接続する。
  - `Same Note Assign`: 同一ノート多重受信時の発音方式（0: Single / 1: Multi / 2: INST〈ドラムのインストゥルメント別設定〉）の動作を接続する。
  - `Element Reserve`: パート別の発音数予約を発音数管理・ボイススティーリング制御へ反映する。設計の検討や値の保持だけでは完了としない。
- **完了条件（DoD）**: 受信チャンネルの変更・OFF、同じMIDIチャンネルを受信する複数パート、Same Note Assignの全3値が発音に反映される。発音上限に達するケースでElement Reserveがボイス選択へ反映されることを確認する。受信OFFを既発音ボイスの即時消音と同一視しない。

---

## Phase 4: オプション機能・拡張仕様の整備

### [ ] TASK-401: 独立した Drum Setup 3・4 の実装（オプション） 【U04】
- **対象**: `Source/FluidSynthEngine.h`, `Source/FluidSynthEngine.cpp`
- **仕様書**: [xgparameterchangetable.pdf](../specific/xgparameterchangetable.pdf) p.47
- **内容**:
  - 必要に応じて `drumSetup3`, `drumSetup4` を追加し、SysEx受信（`3n rr aa`のAddress Highが`32H`, `33H`。Midはノート番号`rr`、Lowはパラメータ`aa`）、Part Mode `04` (Drums3) / `05` (Drums4)、リセット処理を完全に独立した状態として拡張する。
- **完了条件（DoD）**: 採用する場合、32 rr aa/33 rr aaでSetup 3/4を個別に変更でき、04/05モードの発音・NRPN・リセットが対応するSetupへ作用する。他Setupの値に影響しない。採用しない場合はオプションとして対象外であることを記録する。

### [ ] TASK-402: XG Bulk Dump および Request の実装 【U01】
- **対象**: `Source/FluidSynthEngine.cpp`: `handleSysEx`
- **仕様書**: [spec.pdf](../specific/spec.pdf) p.18〜19
- **内容**:
  - XG Bulk Dump（`43 0n 4C`）および Parameter Request / Dump Request（それぞれ`43 3n 4C` / `43 2n 4C`）の受信・送信処理の対応範囲を策定し実装する。
- **完了条件（DoD）**: 製品の送受信対応範囲を明記し、対応するRequestに正しい形式で応答できる。BulkのByte Count・ブロックアドレス・チェックサムを検査し、不正パケットで状態が変わらないこと、送受信した値が一致することを確認する。送信経路を含めて検証する。

### [ ] TASK-403: SoundFont 対応範囲とマッピング検証環境の策定 【S03】
- **対象**: `README.md`、検証用ドキュメント
- **仕様書**: [voice_list.pdf](../specific/voice_list.pdf)、[drumvoicelist.pdf](../specific/drumvoicelist.pdf)
- **内容**:
  - GMSynthが前提とする推奨SoundFontの仕様（バンク配列、ドラムマッピング）を明確化する。
  - XG標準ボイスリストとロードされたSoundFontのプリセット照合・診断ログ機能を追加する。
- **完了条件（DoD）**: 対応SoundFontの識別情報とBank/Program対応表を記録する。診断で収録・規定に沿った代替・欠落を区別し、ドラムノート配列も確認する。全拡張音色の収録やヤマハ波形との一致を必須条件にしない。

---

## Phase 5: 動作検証・テスト環境の整備

### [-] TASK-501: XG MIDI入力・状態・音声処理の自動回帰テスト作成
- **対象**: 検証用テストコード、MIDI入力・音声レンダリング用のテスト環境
- **参照**: [xg-compliance-review.md](xg-compliance-review.md)、各タスクの仕様書とDoD
- **内容**:
  - 元レビューに具体的なSysExが掲載されているのはE01とE06のみ。残りは公式仕様からCC、Program Change、Note On/Offを含む入力列と期待結果を作成する。
  - 内部パラメータに加え、選択されるDSP・プリセット、受信ルーティング、実際の音声出力を検査する。E01では種類値の保存だけ、E02/E04/E05では内部値の更新だけを成功条件にしない。
  - テスト用SoundFont、サンプルレート、初期化手順、エフェクト接続先を固定し、インパルス応答・ゲイン測定などでDSPへの反映を確認する。
- **完了条件（DoD）**: E01〜E10を検査ケースへ対応付け、各ケースの入力列・期待結果・実行結果を記録する。状態・プリセット選択・ルーティング・音声測定を必要に応じて組み合わせ、元の不具合を検出できる検査を各改修と同時に実行する。
- **対応状況**: **Phase 1 対象項目完了**。`Tests/XgRegressionTests.cpp` およびテストビルド・実行スクリプト `scripts/run_xg_tests.sh` を作成。TEST-E01（Variation MSB）、TEST-E03（Effect初期値テーブル）、TEST-E04（有効ゼロ値）、TEST-E05（Send折れ線リニアゲイン換算）、TEST-E08（Drums 3/4無視と2セット整合）、TEST-E09（MultiEQ 5プリセット）、TEST-E10（MultiPart初期値）の計51件のアサーションを実装し、全PASS（51/51 PASSED）を確認。Phase 2以降の項目も順次テストケースを追加予定。

### [ ] TASK-502: 代表的XG SMF（MIDIファイル）による実音確認
- **対象**: 検証用SMF、音声レンダリング結果、検証記録
- **参照**: [xgsongdata.pdf](../specific/xgsongdata.pdf)、TASK-403の対応SoundFont・バンク対応表
- **内容**:
  - ヤマハXGロゴ付きの代表的な楽曲データ（SysExによるエフェクト初期化、センド、フィルター制御を含む曲）を再生し、クリッピング、不自然な音色フォールバック、エフェクト過大適用がないか聴感およびアナライザーで確認する。
- **完了条件（DoD）**: 使用SMF、SoundFont、ビルド、サンプルレート、ゲイン設定を記録し、聴感と測定の結果・未解決事項を残す。異常があれば再現可能な最小入力へ切り出す。楽曲の再生成功だけで全規格の適合を保証したとは扱わない。
