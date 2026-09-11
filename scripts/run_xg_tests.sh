#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

cd "${ROOT_DIR}"

export DEVELOPER_DIR="/Volumes/WD2T/Applications/Xcode.app/Contents/Developer"
SDK_PATH="$(xcrun --show-sdk-path)"

echo "=== 1. Building libGMSynth.a (Debug) ==="
xcodebuild -project Builds/MacOSX/GMSynth.xcodeproj -scheme "GMSynth - Shared Code" -configuration Debug build > /dev/null

echo "=== 2. Compiling XgRegressionTests ==="
mkdir -p Builds/bin

xcrun clang++ -std=c++17 -O1 \
    -isysroot "${SDK_PATH}" \
    -DDEBUG=1 \
    -I Source \
    -I JuceLibraryCode \
    -I external/JUCE/modules \
    -I external/fluidsynth/include \
    -I external/fluidsynth/build/include \
    -DJUCE_GLOBAL_MODULE_SETTINGS_INCLUDED=1 \
    -DJUCE_STANDALONE_APPLICATION=1 \
    Tests/XgRegressionTests.cpp \
    Builds/MacOSX/build/Debug/libGMSynth.a \
    external/fluidsynth/build/src/libfluidsynth.a \
    -framework Accelerate \
    -framework AudioToolbox \
    -framework CoreAudio \
    -framework CoreMIDI \
    -framework Cocoa \
    -framework IOKit \
    -framework WebKit \
    -framework QuartzCore \
    -framework Security \
    -framework Metal \
    -framework MetalKit \
    -o Builds/bin/XgRegressionTests

echo "=== 3. Running XgRegressionTests ==="
./Builds/bin/XgRegressionTests
