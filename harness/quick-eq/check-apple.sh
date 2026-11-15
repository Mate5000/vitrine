#!/usr/bin/env bash
# Compile the production adapter, DSP and settings against Apple's real iOS SDK.
# Link against UI stand-ins only for the two services supplied by Vitrine; this
# catches ObjC/C++ ABI mistakes without requiring Spotify or a private flag table.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../tweak/Sources"
FX="$SRC/Shared/QuickEQ"
OUT="$HERE/build/apple"
mkdir -p "$OUT"
SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
for file in AudioDSP.cpp QuickEQAudio.cpp QuickEQSettings.mm; do
    # Bash 3.2 (macOS /bin/bash) treats an empty array as unset with nounset.
    # Keep common flags in the array so the C++-only iterations are nonempty too.
    compile_flags=(-Wall -Wextra -Werror -Wno-deprecated-declarations -I"$SRC")
    [[ "$file" == *.mm ]] && compile_flags+=(-fobjc-arc)
    xcrun --sdk iphoneos clang++ -target arm64-apple-ios16.1 -isysroot "$SDK" -std=c++17 \
        "${compile_flags[@]}" \
        -c "$FX/$file" -o "$OUT/$file.o"
done
xcrun --sdk iphoneos clang -target arm64-apple-ios16.1 -isysroot "$SDK" -fobjc-arc -I"$SRC" \
    -c "$HERE/link-host.m" -o "$OUT/link-host.o"
xcrun --sdk iphoneos clang++ -target arm64-apple-ios16.1 -isysroot "$SDK" -dynamiclib \
    "$OUT"/*.o -framework UIKit -framework Foundation -framework AVFoundation -framework AudioToolbox \
    -o "$OUT/QuickEQ-link-check.dylib"
echo "PASS: iOS arm64 Quick EQ compilation and ObjC/C++ linkage (UI host stand-ins)"

# Compile the real Logos/UI entry points too: the link host above cannot catch
# a misspelled native hook or a type error in a player/menu/settings integration.
if [[ -n "${THEOS:-}" ]]; then
    for source in Native/Player/QuickEQ.x Redesigned/Player/PlayerFooter.x App/ModSettings.x Shared/AudioEffects/AudioEffects.x; do
        generated="$OUT/$(basename "$source").m"
        "$THEOS/bin/logos.pl" -c generator=internal "$SRC/$source" > "$generated"
        xcrun --sdk iphoneos clang -target arm64-apple-ios16.1 -isysroot "$SDK" -fobjc-arc -fsyntax-only \
            -Wno-deprecated-declarations -I"$SRC" -I"$(dirname "$SRC/$source")" \
            -I"$THEOS/include" -I"$THEOS/vendor/include" '-DSG_VERSION="0.0.0-check"' "$generated"
    done
    xcrun --sdk iphoneos clang -target arm64-apple-ios16.1 -isysroot "$SDK" -fobjc-arc -fsyntax-only \
        -Wno-deprecated-declarations -I"$SRC" "$SRC/Redesigned/Player/PlayerMenu.m"
    echo "PASS: Apple SDK syntax checks for real EQ hooks, player menu and Mod Settings"
fi
