# GMSynth コマンドラインビルド手順

本ドキュメントでは、macOS 環境における GMSynth のコマンドラインビルド手順を説明します。

---

## 1. 前提条件

- **Xcode**: インストール済みであること（例: `/Volumes/WD2T/Applications/Xcode.app`）
- **CMake**: インストール済みであること (`cmake` コマンドが PATH に通っていること)
- **Projucer**: JUCE の Projucer が利用可能であること（例: `/Users/urakari/JUCE/Projucer.app`）

---

## 2. 初回セットアップ

リポジトリを新規クローンした直後や、`git clean` 後に `JuceLibraryCode/` が存在しない場合は、以下の手順で環境を準備します。

```bash
# プロジェクトのルートディレクトリに移動
cd /Volumes/proj/gmsynth/GMSynth

# 1. サブモジュールの初期化・取得
git submodule update --init --recursive

# 2. Projucer で JuceLibraryCode を生成
/Users/urakari/JUCE/Projucer.app/Contents/MacOS/Projucer --resave GMSynth.jucer

# 3. Git 上で管理されている Xcode プロジェクト設定をコミット状態に維持
# (ローカルの Projucer バージョン差異による差分をリセット)
git checkout -- Builds/MacOSX
```

---

## 3. コマンドラインビルド

プロジェクトのルートディレクトリで実行してください。

### 通常のビルド (Debug)

`JuceLibraryCode/` が生成済みであれば、以下のコマンドでビルドできます。
Pre-Build スクリプトにより、`external/fluidsynth` の静的ライブラリ (`libfluidsynth.a`) も自動的にビルドされます。

```bash
DEVELOPER_DIR="/Volumes/WD2T/Applications/Xcode.app/Contents/Developer" \
xcodebuild -project Builds/MacOSX/GMSynth.xcodeproj -scheme "GMSynth - All" build
```

### Release ビルド

配布用や最適化ビルドを行う場合は、`-configuration Release` を指定します。

```bash
DEVELOPER_DIR="/Volumes/WD2T/Applications/Xcode.app/Contents/Developer" \
xcodebuild -project Builds/MacOSX/GMSynth.xcodeproj -scheme "GMSynth - All" -configuration Release build
```

### クリーンビルド

既存のビルド成果物を削除してリビルドする場合:

```bash
DEVELOPER_DIR="/Volumes/WD2T/Applications/Xcode.app/Contents/Developer" \
xcodebuild -project Builds/MacOSX/GMSynth.xcodeproj -scheme "GMSynth - All" clean build
```

---

## 4. ビルド成果物の出力先

ビルドが完了すると、`Builds/MacOSX/build/<Configuration>/` 配下に以下の成果物が生成されます。

- **`GMSynth.app`**: Standalone プラグイン本体
- **`GMSynth.appex`**: AUv3 AppExtension
- **`libGMSynth.a`**: 共通静的ライブラリ

---

## 5. Tips: DEVELOPER_DIR の指定を省略する

ターミナル全体で既定の開発ツールを Xcode.app に設定すると、コマンド実行時の `DEVELOPER_DIR="..."` プレフィックスを省略できます。

```bash
sudo xcode-select -s /Volumes/WD2T/Applications/Xcode.app
```

設定後は、通常のコマンドのみでビルド可能です。

```bash
xcodebuild -project Builds/MacOSX/GMSynth.xcodeproj -scheme "GMSynth - All" build
```
