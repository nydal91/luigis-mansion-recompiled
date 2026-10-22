#!/usr/bin/env bash
# Packages build/android-arm64/libmain.so into an installable APK (docs/ANDROID.md).
#
#   ANDROID_HOME=<SDK with build-tools 37 and platforms;android-35> \
#   [LM_ANDROID_LLVM=<libllvm.so; default build/android-llvm/libllvm.so from build_llvm.sh>] \
#   tools/android/build_apk.sh [out.apk]
#
# No Gradle: aapt2, javac, d8, zipalign and apksigner from the SDK. Signs with
# $LM_KEYSTORE (password $LM_KEYSTORE_PASS), or a local debug key made on first use.
# The APK holds no game data: players pick their own disc image on first start.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
build="$root/build/android-arm64"
work="$root/build/android-apk"
out="${1:-$root/build/LuigisMansionPort-android-arm64.apk}"
: "${ANDROID_HOME:?set ANDROID_HOME to an Android SDK}"

bt="$(ls -d "$ANDROID_HOME"/build-tools/* | sort -V | tail -1)"
platform="$ANDROID_HOME/platforms/android-35/android.jar"
[ -f "$platform" ] || { echo "Need $platform (SDK platform 35)" >&2; exit 1; }
[ -f "$build/libmain.so" ] || { echo "Build libmain.so first (tools/android/build.sh)" >&2; exit 1; }

sdl_java="$(sed -n 's/^AURORA_SDL3_JAVA_SOURCE_DIR:INTERNAL=//p' "$build/CMakeCache.txt")"
aurora_java="$(sed -n 's/^AURORA_ANDROID_JAVA_SOURCE_DIR:INTERNAL=//p' "$build/CMakeCache.txt")"
[ -d "$sdl_java/org/libsdl/app" ] || { echo "SDL's Java sources not found ($sdl_java)" >&2; exit 1; }

version="$(sed -n 's/^CMAKE_PROJECT_VERSION:STATIC=//p' "$build/CMakeCache.txt")"
version="${version:-1.0.0}"
IFS=. read -r vmaj vmin vpat <<< "$version"
version_code=$(( ${vmaj:-1} * 10000 + ${vmin:-0} * 100 + ${vpat:-0} ))

rm -rf "$work"
mkdir -p "$work"/{res,classes,dex,apk/lib/arm64-v8a,assets}

# Manifest with the version filled in.
sed -e "s/@LM_VERSION_CODE@/$version_code/" -e "s/@LM_VERSION_NAME@/$version/" \
    "$root/packaging/android/AndroidManifest.xml" > "$work/AndroidManifest.xml"

# Resources and assets.
"$bt/aapt2" compile --dir "$root/packaging/android/res" -o "$work/res/res.zip"
cp "$root/data/pipelines.bin" "$work/assets/"
cp "$root/NOTICE.md" "$work/assets/NOTICE.md"
cp "$root/packaging/LLVM-LICENSE.TXT" "$work/assets/LLVM-LICENSE.TXT"
"$bt/aapt2" link -o "$work/base.apk" -I "$platform" --manifest "$work/AndroidManifest.xml" \
    -A "$work/assets" --min-sdk-version 28 --target-sdk-version 35 "$work/res/res.zip"

# Java: SDL's activity, Aurora's surface, the port's activity.
find "$sdl_java/org/libsdl/app" "$aurora_java" "$root/packaging/android/java" -name '*.java' > "$work/sources.txt"
javac -nowarn -source 8 -target 8 -encoding UTF-8 -bootclasspath "$platform" -classpath "$platform" \
    -d "$work/classes" @"$work/sources.txt" 2>&1 | grep -v -E "^warning: \[options\]|^[0-9]+ warnings?$|^Note: " || true
[ -n "$(find "$work/classes" -name LMActivity.class)" ] || { echo "javac failed" >&2; exit 1; }
"$bt/d8" --release --min-api 28 --lib "$platform" --output "$work/dex" $(find "$work/classes" -name '*.class')

# Native code: the program and, if given, the bundled compiler for the first-run setup.
cp "$build/libmain.so" "$work/apk/lib/arm64-v8a/libmain.so"
"${ANDROID_NDK:-$ANDROID_HOME/ndk-bundle}"/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-strip \
    "$work/apk/lib/arm64-v8a/libmain.so" 2>/dev/null || strip "$work/apk/lib/arm64-v8a/libmain.so" 2>/dev/null || true
# Default: tools/android/build_llvm.sh's output. LM_ANDROID_LLVM=none builds an APK without it
# (setup then cannot build the game on the device).
llvm="${LM_ANDROID_LLVM:-$root/build/android-llvm/libllvm.so}"
if [ "$llvm" = none ]; then
    echo "warning: no compiler in the APK: setup cannot build the game on the device" >&2
elif [ -f "$llvm" ]; then
    cp "$llvm" "$work/apk/lib/arm64-v8a/libllvm.so"
else
    echo "No compiler at $llvm: run tools/android/build_llvm.sh, set LM_ANDROID_LLVM, or LM_ANDROID_LLVM=none" >&2
    exit 1
fi

cp "$work/base.apk" "$work/unsigned.apk"
(cd "$work/dex" && zip -q -X "$work/unsigned.apk" classes.dex)
(cd "$work/apk" && zip -q -X -r "$work/unsigned.apk" lib)
"$bt/zipalign" -f -P 16 4 "$work/unsigned.apk" "$work/aligned.apk"

ks="${LM_KEYSTORE:-$root/build/android-debug.keystore}"
pass="${LM_KEYSTORE_PASS:-android}"
if [ ! -f "$ks" ]; then
    keytool -genkeypair -keystore "$ks" -storepass "$pass" -keypass "$pass" -alias lmport \
        -keyalg RSA -keysize 2048 -validity 10000 -dname "CN=Luigi's Mansion Port (local build)" >/dev/null 2>&1
fi
"$bt/apksigner" sign --ks "$ks" --ks-pass "pass:$pass" --key-pass "pass:$pass" --out "$out" "$work/aligned.apk"
"$bt/apksigner" verify "$out"
echo "Built $out ($(du -h "$out" | cut -f1))"
