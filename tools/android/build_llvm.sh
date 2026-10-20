#!/usr/bin/env bash
# Builds the compiler the APK carries for the first-run setup (docs/ANDROID.md): clang and
# lld from the LLVM release, cross-compiled to run on Android arm64, as one multi-call
# binary (LLVM_TOOL_LLVM_DRIVER_BUILD: "llvm clang ..." / "llvm ld.lld ..."), AArch64
# target only. The result is shipped as lib/arm64-v8a/libllvm.so.
#
#   ANDROID_NDK=<NDK r29+> tools/android/build_llvm.sh [llvm-project checkout]
#
# Takes a few hours on a laptop (most of it is clang). Needs a host clang (for LLVM's
# build-time tablegen tools), CMake and Ninja.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
: "${ANDROID_NDK:?set ANDROID_NDK to an Android NDK (r29 or newer)}"
llvm_version="${LM_LLVM_VERSION:-21.1.8}"
out="$root/build/android-llvm"
src="${1:-$out/llvm-project}"
mkdir -p "$out"
if [ ! -d "$src/llvm" ]; then
    git clone -q --depth 1 --branch "llvmorg-$llvm_version" https://github.com/llvm/llvm-project.git "$src"
fi
cmake -G Ninja -S "$src/llvm" -B "$out/build" \
    -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-28 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release \
    -DLLVM_ENABLE_PROJECTS="clang;lld" -DLLVM_TARGETS_TO_BUILD=AArch64 \
    -DLLVM_HOST_TRIPLE=aarch64-unknown-linux-android28 -DLLVM_DEFAULT_TARGET_TRIPLE=aarch64-unknown-linux-android28 \
    -DLLVM_TOOL_LLVM_DRIVER_BUILD=ON \
    -DLLVM_ENABLE_ZLIB=OFF -DLLVM_ENABLE_ZSTD=OFF -DLLVM_ENABLE_LIBXML2=OFF -DLLVM_ENABLE_TERMINFO=OFF \
    -DLLVM_ENABLE_LIBEDIT=OFF -DLLVM_ENABLE_BINDINGS=OFF -DLLVM_ENABLE_PIC=ON \
    -DLLVM_INCLUDE_TESTS=OFF -DLLVM_INCLUDE_EXAMPLES=OFF -DLLVM_INCLUDE_BENCHMARKS=OFF -DLLVM_INCLUDE_DOCS=OFF \
    -DLLVM_INCLUDE_UTILS=OFF -DLLVM_BUILD_TOOLS=OFF \
    -DCLANG_ENABLE_STATIC_ANALYZER=OFF -DCLANG_ENABLE_ARCMT=OFF -DCLANG_PLUGIN_SUPPORT=OFF \
    -DCLANG_INCLUDE_TESTS=OFF -DCLANG_INCLUDE_DOCS=OFF \
    -DCROSS_TOOLCHAIN_FLAGS_NATIVE="-DCMAKE_C_COMPILER=${HOST_CC:-clang};-DCMAKE_CXX_COMPILER=${HOST_CXX:-clang++}"
cmake --build "$out/build" --target llvm-driver
"$ANDROID_NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip" -o "$out/libllvm.so" "$out/build/bin/llvm"
echo "Built $out/libllvm.so (pass it to tools/android/build_apk.sh as LM_ANDROID_LLVM)"
