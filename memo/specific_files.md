# specific ディレクトリ仕様書・資料一覧

`/Volumes/proj/gmsynth/GMSynth/specific` ディレクトリに格納されている、**MIDI 1.0 規格**および**ヤマハ XG 音源規格（XGフォーマット V1.35）**に関する公式仕様書、楽曲制作指針、解説資料（計17ファイル、すべてPDF形式）のまとめです。

---

## 全体構成と対応関係

当ディレクトリの資料は、大きく以下の4つに分類されます。

1. **XGフォーマット仕様書（統合版）**: `xg_v135_j.pdf` (全118ページ)
2. **XGフォーマット仕様書（分割版・別表）**: 13ファイル (合計118ページ)
   - 統合版仕様書（V1.35）の各章や別表が個別に分冊・参照できるように分割されたものです。
3. **MIDI 1.0 規格書**: `MIDI1.0.pdf` (全311ページ)
4. **楽曲制作指針・解説資料**: `xgsongdata.pdf` (12ページ), `read_aoyama.pdf` (183ページ)

---

## 1. XGフォーマット仕様書（統合版）

| ファイル名 | ページ数 | 説明 |
| :--- | :---: | :--- |
| [**xg_v135_j.pdf**](../specific/xg_v135_j.pdf) | 118 | **XGフォーマット仕様書 Ｖ1.35（2000.06.14 ヤマハ株式会社）**<br>XGフォーマットの基本概要からMIDIメッセージ仕様、各種パラメータチェンジテーブル、エフェクト仕様、ボイス/ドラムマップまでをすべて網羅した総合仕様書。 |

---

## 2. XGフォーマット仕様書（セクション別 分割版）

統合版 `xg_v135_j.pdf` の各セクション・別表を個別に切り出したファイル群です。

| ファイル名 | ページ数 | 対応セクション / 内容 |
| :--- | :---: | :--- |
| [**page_top.pdf**](../specific/page_top.pdf) | 1 | 仕様書 V1.35 表紙 |
| [**spec.pdf**](../specific/spec.pdf) | 19 | **§1 フォーマット概要 / §2 MIDI仕様**<br>開発背景、基本思想、GM上位互換性、コントロールチェンジ、NRPN、RPN、システムエクスクルーシブ等のメッセージ定義。 |
| [**xgparameterchangetable.pdf**](../specific/xgparameterchangetable.pdf) | 7 | **[別表 3-1 〜 3-4] パラメータチェンジテーブル**<br>System, Multi Effect, Multi Part, Drum Setup の各SysExアドレスマップ、パラメータ名、データ範囲、デフォルト値の定義。 |
| [**efctmap.pdf**](../specific/efctmap.pdf) | 2 | **XG EFFECT MAP**<br>Reverb / Chorus / Variation の Type MSB / LSB マップ一覧。 |
| [**efctparamlist.pdf**](../specific/efctparamlist.pdf) | 15 | **XG EFFECT PARAMETER**<br>各エフェクトタイプにおける個別パラメータ（No.1〜16）、データ範囲、表示値、コントロールノート、実装ブロックの定義。 |
| [**efctparamtbl.pdf**](../specific/efctparamtbl.pdf) | 2 | **XG EFFECT PARAMETER TABLE**<br>Table#1 〜 Table#14（LFO周波数、ディレイタイム、EQ周波数、リバーブタイム等の実効値換算テーブル）。 |
| [**efctparamdeflt.pdf**](../specific/efctparamdeflt.pdf) | 2 | **XG EFFECT DEFAULT DATA**<br>各エフェクトタイプ（Reverb, Chorus, Variation, Insertion）および Multi EQ ブロックの初期パラメータ設定値。 |
| [**voice_list.pdf**](../specific/voice_list.pdf) | 11 | **[別表-1] XG/XGlite VOICE MAP**<br>ノーマルボイス一覧（Bank MSB/LSB、プログラムチェンジ、楽器カテゴリ、音色名）。 |
| [**drumvoicelist.pdf**](../specific/drumvoicelist.pdf) | 9 | **[別表-2] XG/XGlite Drum Map**<br>ドラムキット一覧（Bank MSB/LSB、プログラムチェンジ、各ノートナンバーに割り当てられた打楽器音色名）。 |
| [**drum_default.pdf**](../specific/drum_default.pdf) | 11 | **XG Drum Setup Default Value [Standard]**<br>Standard Kit をはじめとする標準ドラムキット各ノートのピッチ、レベル、パン、リバーブ/コーラスセンド、フィルター初期値。 |
| [**drum_default_ext.pdf**](../specific/drum_default_ext.pdf) | 37 | **XG Drum Setup Default Value [Option]**<br>拡張ドラムキット（Room, Rock, Electronic, Analog, Jazz, Brush, Classic 等）の各ノートの初期パラメータ設定値。 |
| [**voiceextension.pdf**](../specific/voiceextension.pdf) | 1 | **ボイス拡張方式**<br>Bank Select MSB/LSB によるバリエーションボイスの作成・割当ルール（パラメータ変更による音色作成 Bank 1〜63、波形変更 Bank 64〜127 等）。 |
| [**xgmap.pdf**](../specific/xgmap.pdf) | 1 | **Bank MSB Category**<br>Bank MSB の上位/下位ビットによる音色カテゴリ（Normal, User Voice, Drum 等）の分類マップ表。 |

---

## 3. MIDI 基礎規格

| ファイル名 | ページ数 | 説明 |
| :--- | :---: | :--- |
| [**MIDI1.0.pdf**](../specific/MIDI1.0.pdf) | 311 | **MIDI 1.0 規格書 MIDI Standard & Recommended Practice（日本語版 98.1）**<br>社団法人 音楽電子事業協会（AMEI）発行。<br>・MIDI 1.0 電気的・機械的ハードウェア仕様<br>・MIDI 1.0 メッセージ仕様<br>・スタンダードMIDIファイル（SMF: Format 0/1/2）仕様<br>・各種 Recommended Practice（RP）規格 |

---

## 4. 楽曲制作指針・実践解説資料

| ファイル名 | ページ数 | 説明 |
| :--- | :---: | :--- |
| [**xgsongdata.pdf**](../specific/xgsongdata.pdf) | 12 | **XG 楽曲データ制作の指針 V2.01（1999.09.09 ヤマハ株式会社）**<br>XG対応ソングデータを制作する際のガイドライン。<br>・ヘッダー部分でのXG System Onリセット送信ルール<br>・推奨エフェクト設定とセンド構成<br>・パート設定・発音数（ボイス数）管理と互換性確保の注意点 |
| [**read_aoyama.pdf**](../specific/read_aoyama.pdf) | 183 | **YAMAHA \| XG READING PAGE**<br>ヤマハXGプロジェクト音楽ディレクター・青山忠英氏による解説記事集。<br>・XGフォーマットの概念と特徴<br>・音色エディット（フィルター、エンベロープ、モジュレーション等）の実践手法<br>・インサーションエフェクトやバリエーションエフェクトの活用ノウハウ<br>・楽曲ジャンル別の打ち込みテクニック |
