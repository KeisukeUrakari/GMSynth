# 配置仕様書によるXGレビュー再検証

検証日: 2026-09-11
対象: [xg-compliance-review.md](xg-compliance-review.md)

## 結論

主要な不具合指摘は維持する。ただし、以前この文書にあった「すべて正確（100%妥当）」という断定は撤回する。実装の存在・欠落を確認できることと、XGの必須要件違反を確定できることは同じではない。必須／オプション、音色代替の条件、公式仕様と解説の根拠を補足し、元レビューも修正した。

## 方法と参照資料

配置されたPDFをローカルでテキスト抽出し、該当表とソースコードを再照合した。Effect Mapは画像化して色分けも確認した（画像化では一部の文字が欠落するため、項目名は抽出テキストと照合）。外部検索による前回の解釈を、そのまま根拠として採用していない。

| 資料 | 確認した内容 |
|---|---|
| [spec.pdf](../specific/spec.pdf) p.5〜7、18〜19 | Program Change、Bank Select、SysEx形式 |
| [efctmap.pdf](../specific/efctmap.pdf) p.20 | 種類番号、サブタイプ、必須・オプションの色分け |
| [efctparamlist.pdf](../specific/efctparamlist.pdf) p.22〜36 | 種類別の値域・意味・初期値・有効パラメータ |
| [efctparamtbl.pdf](../specific/efctparamtbl.pdf) p.37〜38 | LFO、Delay、EQ、Reverbの値変換 |
| [efctparamdeflt.pdf](../specific/efctparamdeflt.pdf) p.39〜40 | エフェクト・Multi EQの初期値 |
| [xgparameterchangetable.pdf](../specific/xgparameterchangetable.pdf) p.41〜47 | Sendレベル、Multi Part、Drum Setup、オプション注記 |
| [xgmap.pdf](../specific/xgmap.pdf) p.117 | Proxy／Non-proxyバンクの代替規定 |
| [read_aoyama.pdf](../specific/read_aoyama.pdf) 図4-3直前 | 種類変更で詳細設定が初期化される旨の公式解説 |

ページは印刷ページ番号。統合版は[xg_v135_j.pdf](../specific/xg_v135_j.pdf)。音色・ドラムのリスト／初期値資料も配置されているが、今回SF2の実内容との全件照合は実施していない。17ファイルすべてを精読したという意味ではない。現配置は16 PDFで、資料一覧にあるMIDI1.0.pdfは見つからなかった。

## 前回指摘ごとの判定

| ID | 判定 | 再検証結果 |
|---|---|---|
| E01 | 維持・誤り | MSBの10件の不一致をEffect Mapで確認。直接数値で分岐する箇所も修正対象。 |
| E02 | 維持・誤り | Delay LCRの6=Cch Level、7=High Damp。Echoの2はLch Feedback Levelであり、同じ位置をRch Delayとして扱う現実装は誤り。 |
| E03 | 維持・根拠補足 | 種類別初期値表で不一致を確認。タイプ変更時の初期化は表だけでなく公式解説の注意書きでも裏付けた。 |
| E04 | 維持・誤り | Chorus／TremoloのDepthは0を取れるが、実装は基本値へ置換する。全パラメータで0が有効という意味ではない。 |
| E05 | 維持・誤り | 64=0 dB、127=+6 dBに対して実装は64/127、1.0。根拠はEffect 1のParameter Change Table。旧報告の「Table#1」は不正確で、Effect Parameter Tableのtable#1はLFO Frequencyである。 |
| E06 | 維持・誤り | Rcv Channel、Same Note Assign、Element Reserveは保存のみ。表の該当行はOpt.扱いではない。受信OFFを「既に発音中の音も即座に消音する」とまでは解釈しない。 |
| E07 | 維持・条件補足 | SFX無発音と未対応ドラムProgramの維持に加え、未対応Normal LSBでは前回LSBを維持する規定がある。部分対応バンク内の欠落ProgramのBank 0代替とは区別する。Proxyの例外もある。 |
| E08 | 限定して維持 | Setup 3・4が独立しない実装事実は正しいが、3・4はオプション。2セットのみであることは不具合ではない。既存Setupへの暗黙の割り当てを設計上の不整合として指摘する。 |
| S01 | 維持・一部簡易対応 | JUCEの近似処理に加え、値変換が規定と異なる。実機と同じDSP方式を要求する指摘ではない。 |
| S02 | 維持・一部簡易対応 | LSBを保存するだけで種類別の違いが反映されない。ただし、未対応LSBを基本タイプ扱いすることが常に誤りなのではなく、Effect Map上の同一基本タイプの指定を区別する必要がある。 |
| S03 | 維持・断定を限定 | SF2の内容が未検証のため適合は保証できない。全拡張音色の収録やヤマハ波形そのものが不可欠という意味ではない。 |
| U01 | 維持・未実装(将来対応) | Bulk DumpとRequest処理がないことを確認。送受信・製品の対応範囲を明示して管理する。 |
| U02 | 維持・優先度補足 | 専用DSPがないことは正しい。Hall系、Rotary Speaker、3-Band/2-Band EQの基本タイプなど、ESSENTIALに含まれるものまで任意拡張と扱ってはいけない。 |
| U03 | 維持・未実装(将来対応) | Variationの11〜16は保存のみ。パラメータ一覧の予約欄を除き、種類ごとの有効項目を対象とする。 |
| U04 | 維持・オプション明示 | 独立したSetup 3・4は未実装。ただし、標準の最低2セット要件とは別であり、必須の改修とはしない。 |

## 具体的な裏付け

### 初期値と値変換

Hall 1のパラメータ1〜5は十進数で18, 10, 8, 13, 49。実装のresetは64, 64, 0, 0, 64であり、値域外のDiffusion=64なども含まれる。Chorus 1は最初の4項目が6, 54, 77, 106だが、実装は64, 64, 0, 0。

Effect Parameter TableではLFO値0は0 Hz、64は2.69 Hz。実装は種類別の独自式で、ゼロでも非ゼロのレートを用いる箇所がある。規定表との不一致を、単なる実機の音質差としては扱えない。

### 音色の代替規定

- Normalの部分対応バンク内で未収録のProgramを基本音色に補うことは許容される。
- Normalの未対応バンクLSBでは、前回メロディーに用いたLSBを維持する。
- MSB 7Fの未収録ドラムキットでは、前回のProgramを維持する。
- MSB 40のSFXなど、代替しないバンクの未収録音色は無発音とする。
- 拡張マップのMSB 60〜6FはProxyとしてMSB 0への代替を規定している。全非ゼロMSBに同じ規則を適用しない。

### Drum Setupの範囲

Parameter Change Table p.47には最低2セット、n=2,3はOpt.との注記がある。p.44ではPart Mode値04/05をL3-80としている。したがって「Setup 3・4がないからXG不適合」とは判定しない。

## 再検証で追加確認した見落とし

### E09: Multi EQプリセット値が不一致【誤り】

[Source/XgModel.h](../Source/XgModel.h)の`MultiEqParameters::setPreset`はゲインだけを独自の値へ変更する。周波数・Q・形状をプリセット表へ戻さない。

例: JazzのGain1〜5は[efctparamdeflt.pdf](../specific/efctparamdeflt.pdf) p.40で58, 66, 68, 60, 58だが、実装は68, 64, 62, 66, 67。Flat選択時もゲインだけがリセットされる。

Multi EQ自体はオプションだが、実装しているプリセットの意味が異なるため「誤り」に分類する。

### E10: Multi Partのリセット値が不一致【誤り】

[Source/FluidSynthEngine.cpp](../Source/FluidSynthEngine.cpp)の`resetChannelState`と[Source/XgModel.h](../Source/XgModel.h)の`PartParameters::reset`を、Parameter Change Table p.44と比較した。

| 項目 | 規定 | 実装 |
|---|---|---|
| Part 10のPart Mode | 02=Drums1 | 01=Drum |
| Part 10のElement Reserve | 0 | 2 |
| 各PartのRcv Channel | 各Partのチャンネル番号 | 全Partで0 |

Rcv ChannelとElement Reserveは現状未接続のため、値の修正と処理への接続を合わせて行う必要がある。これらは前回の「初期化経路がある」という記述を、正しい初期値の保証として読めないことも示している。

## 未検証事項

実音・ビルド・MIDI入力試験・全SF2音色照合は未実施。今回の修正は文書のみであり、音源実装は変更していない。前回の確認用SysExも実行済みテストとしては扱わない。
