#!/usr/bin/env bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FX="$HERE/../../tweak/Sources/Shared/QuickEQ"
OUT="$HERE/build/${SANITIZE:-normal}"
flags=(-std=c++17 -O2 -Wall -Wextra -Werror -pthread)
if [[ -n "${SANITIZE:-}" ]]; then
    flags+=(-O1 -g "-fsanitize=$SANITIZE" -fno-omit-frame-pointer)
fi
mkdir -p "$OUT"
"${CXX:-c++}" "${flags[@]}" "$FX/AudioDSP.cpp" "$HERE/dsp.cpp" -o "$OUT/dsp"
"$OUT/dsp"
for test in render regressions; do
    "${CXX:-c++}" "${flags[@]}" -I"$HERE/shim" \
        "$FX/AudioDSP.cpp" "$FX/QuickEQAudio.cpp" "$HERE/$test.cpp" -o "$OUT/$test"
    "$OUT/$test"
done
