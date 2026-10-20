#!/usr/bin/env bash
# Builds nod (disc image reader, Rust) for Android arm64 and packages it like nod's own
# release packages, for AURORA_NOD_PACKAGE_URL (nod publishes no Android build; docs/ANDROID.md).
#
#   ANDROID_NDK=<NDK r29+> tools/android/build_nod.sh
#
# Needs git, cargo and the aarch64-linux-android Rust target (rustup target add
# aarch64-linux-android). Without that target, LM_RUST_BUILD_STD=1 builds the standard
# library from rust-src instead (needs the rust-src component).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
: "${ANDROID_NDK:?set ANDROID_NDK to an Android NDK (r29 or newer)}"
version="$(sed -n 's/.*AURORA_NOD_VERSION "\([^"]*\)".*/\1/p' "$root/extern/aurora/cmake/AuroraDependencyVersions.cmake")"
out="$root/build/android-nod"
src="$out/nod-src"
mkdir -p "$out"
if [ ! -d "$src" ]; then
    git clone -q --depth 1 --branch "$version" https://github.com/encounter/nod.git "$src"
fi
tc="$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin"
export CARGO_TARGET_AARCH64_LINUX_ANDROID_LINKER="$tc/aarch64-linux-android28-clang"
export CC_aarch64_linux_android="$tc/aarch64-linux-android28-clang"
export AR_aarch64_linux_android="$tc/llvm-ar"
extra=()
if [ "${LM_RUST_BUILD_STD:-0}" = 1 ]; then
    export RUSTC_BOOTSTRAP=1
    extra=(-Zbuild-std=std,panic_abort)
fi
(cd "$src" && cargo build --release -p nod-ffi --no-default-features \
    --features compress-bzip2-static,compress-lzma-static,compress-zlib-static,compress-zstd-static,threading \
    --target aarch64-linux-android "${extra[@]}")

# Same layout as nod's release packages (libnod-<platform>.tar.gz): its CMake package files
# and header are platform-independent, so they are taken from the Linux package of the same
# version, with the libraries replaced by the Android ones.
pkg="$out/pkg"
rm -rf "$pkg"
mkdir -p "$pkg"
curl -fsSL "https://github.com/encounter/nod/releases/download/$version/libnod-linux-x86_64.tar.gz" | tar xz -C "$pkg"
rm -f "$pkg"/lib/libnod.*
cp "$src/target/aarch64-linux-android/release/libnod.a" "$src/target/aarch64-linux-android/release/libnod.so" "$pkg/lib/"
cp "$src/nod-ffi/include/nod.h" "$pkg/include/nod.h"
tar czf "$out/libnod-android-aarch64.tar.gz" -C "$pkg" .
echo "Built $out/libnod-android-aarch64.tar.gz"
