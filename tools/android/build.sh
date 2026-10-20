#!/usr/bin/env bash
# Builds the Android program (libmain.so) for arm64-v8a. docs/ANDROID.md.
#
#   ANDROID_NDK=<NDK r29+> tools/android/build.sh [extra cmake args...]
#
# Needs: CMake 3.25+, Ninja, a host C++ compiler (for lmembed), and a nod package for
# android-aarch64 (tools/android/build_nod.sh makes one; pass its path in LM_NOD_PACKAGE).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
: "${ANDROID_NDK:?set ANDROID_NDK to an Android NDK (r29 or newer)}"
build="$root/build/android-arm64"
host="$root/build/android-host"

# lmembed runs at build time, so it is built for this machine.
mkdir -p "$host"
if [ ! -x "$host/lmembed" ] || [ "$root/tools/recomp/lmembed.cpp" -nt "$host/lmembed" ]; then
    "${CXX:-c++}" -std=c++20 -O2 "$root/tools/recomp/lmembed.cpp" -o "$host/lmembed"
fi

nod_pkg="${LM_NOD_PACKAGE:-$root/build/android-nod/libnod-android-aarch64.tar.gz}"
if [ ! -f "$nod_pkg" ]; then
    echo "No nod package at $nod_pkg: run tools/android/build_nod.sh first (or set LM_NOD_PACKAGE)." >&2
    exit 1
fi

cmake --preset android-arm64 \
    -DLM_HOST_LMEMBED="$host/lmembed" \
    -DAURORA_NOD_PACKAGE_URL="file://$nod_pkg" \
    "$@"
cmake --build "$build" --target LuigisMansionRecomp
echo "Built $build/libmain.so"
