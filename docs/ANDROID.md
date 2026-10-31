# Android port

The port builds for Android phones and tablets (arm64-v8a, Android 9 / API 28 or newer):
the same runtime, recompiler and first-run setup as on Windows and Linux, Aurora on Dawn's
Vulkan backend (OpenGL ES fallback), SDL3 for the window, input and audio. The game module
that setup compiles from the player's disc is built **on the device**, with a clang + lld
that the APK carries. Everything Android-specific is behind `__ANDROID__` / `if (ANDROID)`,
the `android-arm64` CMake preset, `packaging/android/` and `tools/android/`; desktop builds
are unchanged.

**Status (October 2026):** builds and packages; not yet run on a device. See
"Not verified yet" at the end.

## For players

- Needs: a 64-bit ARM device with Android 9+, a Vulkan-capable GPU (most phones from 2018
  on), 4 GB of RAM or more recommended, about 1 GB of free storage, and your own disc image
  (ISO, RVZ, ...) of the USA (GLME01) and/or European (GLMP01) release, copied to the device.
- Install the APK (allow "install unknown apps" for your file manager or browser).
- First start: tap **Browse...**, pick the disc image in the system file picker, then
  **Install and play**. The game is translated and compiled on the phone once; that takes
  several minutes (keep the app open, ideally plugged in). Then the shaders are prepared.
  Later starts go straight to the game.
- Controls: a Bluetooth or USB controller works like on PC (F1 > Controls). Without one,
  touch controls are shown: the left side of the screen is the stick (it appears where you
  put your thumb), the yellow circle is the C-stick, A/B/X/Y on the right, L/R/Z at the top
  corners, START and MENU at the top. **MENU** (or the **Back** button) opens the options
  menu, which is the F1 menu of the PC version. F1 > Controls > Touch sets when the touch
  controls show, their size and opacity.
- Logs for bug reports: the app writes `last_run.log` (and `previous_run.log`) in its
  private files folder; with a USB cable and `adb`: `adb logcat -s LuigisMansion` while it
  runs, or `adb shell run-as dev.lmport.luigismansion cat files/last_run.log` for a
  debuggable build.

## Building

On a Linux (or WSL) machine:

```
# Once: the disc image library (nod has no Android release) and the on-device compiler.
export ANDROID_NDK=~/android-ndk-r29          # NDK r29 or newer (C++20 std::jthread)
rustup target add aarch64-linux-android        # for nod (Rust)
tools/android/build_nod.sh                     # -> build/android-nod/libnod-android-aarch64.tar.gz
tools/android/build_llvm.sh                    # -> build/android-llvm/libllvm.so (hours)

# The program (libmain.so) and the APK.
tools/android/build.sh                         # cmake --preset android-arm64 + build
export ANDROID_HOME=~/android-sdk              # build-tools 37 (34's d8 crashed here) + platforms;android-35
LM_ANDROID_LLVM=build/android-llvm/libllvm.so tools/android/build_apk.sh
# -> build/LuigisMansionPort-android-arm64.apk
adb install -r build/LuigisMansionPort-android-arm64.apk
```

Requirements: CMake 3.25+, Ninja, a host C++ compiler (clang), JDK 17+ (javac, keytool),
Python 3 (icon script only), git, curl, zip. No Gradle: `build_apk.sh` runs aapt2, javac,
d8, zipalign and apksigner from the SDK. It signs with `$LM_KEYSTORE` / `$LM_KEYSTORE_PASS`,
or with a local key it creates in `build/android-debug.keystore` (keep it: an update has
to be signed with the same key, otherwise Android asks to uninstall first, which deletes the
app's files: saves, settings, built game).

`build_llvm.sh` builds LLVM's clang and lld (release 21.1.8 by default, `LM_LLVM_VERSION`)
for Android with only the AArch64 target, as one multi-call binary. That build is long but
only needs redoing for a new LLVM. Without `LM_ANDROID_LLVM` the APK is built without a
compiler and setup reports it missing.

## How it works

| Area | Desktop | Android |
|------|---------|---------|
| Program | `LuigisMansionRecomp(.exe)` | `libmain.so`, loaded by SDL's Java activity (`dev.lmport.LMActivity`), entry `SDL_main` (aurora::main; linked with `-u SDL_main`) |
| Game module | `module.dll` / `module.so` (x86-64) | `module.so` for `aarch64-linux-android28`, same freestanding flags plus `-mno-outline-atomics` (no libgcc for the `__aarch64_*` helpers) and `-fsigned-char`; made read-only after the build (Android 14+ refuses writable code) |
| Bundled compiler | `llvm/bin/clang` + `ld.lld` / `lld-link` | `lib/arm64-v8a/libllvm.so` in the APK: the `llvm-driver` multi-call binary, run as `libllvm.so clang ...` / `libllvm.so ld.lld ...` |
| Compile jobs | one per hardware thread | also at most one per 2.5 GB of RAM (a large generated file can take over 1 GB; Android kills the app when memory runs out); `LM_COMPILE_JOBS` overrides |
| Settings, install.ini, modules, overlays, memory cards | `%APPDATA%` / XDG folders | `Context.getFilesDir()` (`LM_ANDROID_FILES`) |
| GPU caches | `%APPDATA%` / `$XDG_CACHE_HOME` | `Context.getCacheDir()` (`LM_ANDROID_CACHE`) |
| `pipelines.bin` | next to the executable | APK asset, copied to `files/res` on the first start of each app version (`LM_ANDROID_RES`) |
| Disc images | file paths | `content://` URIs from the system file picker (SDL's `ACTION_OPEN_DOCUMENT`); opened through the ContentResolver with a persistent read permission, read by nod and Aurora through SDL streams (`SDL_IOFromFile` opens content URIs; `port::discOpenPath`) |
| Restart (setup changes) | exec of the executable | `LMRestartActivity` in its own process (`:restart`) starts the game again once the old process has exited |
| Logging | console / launcher script | stdout + stderr to logcat (tag `LuigisMansion`) and `files/last_run.log` (`port::android::redirectOutput`) |
| Crashes | own signal/SEH handler | Android's debuggerd (tombstone + logcat backtrace) |
| Input | keyboard, controllers | controllers (SDL), touch controls (`src/recomp/touch.cpp`) merged into port 1 in PADRead, Back button = F1 (`SDL_HINT_ANDROID_TRAP_BACK_BUTTON`) |
| Window | windowed / fullscreen | always fullscreen (immersive), landscape |
| Default render resolution | window size | 2 x 480p (F1 > Visuals), for heat and battery |

Why the compiler is a "library": Android only lets an app run programs from its native
library folder (files in its data folder cannot be executed since Android 10), and the
installer only unpacks `lib*.so` files there (`android:extractNativeLibs="true"`). The game
module itself is loaded with `dlopen`, which is allowed from the data folder.

The JNI calls from native code (`src/port/android.cpp`) go to static methods of
`LMActivity` through the class of `SDL_GetAndroidActivity()`, which works on any thread.

The menus use the port's menu size (F1 > Visuals > General > Menu size, `ui_scale.cpp`): its
Auto measures the window in ImGui units, which SDL already scales by the screen density on
phones. The updater (`update.cpp`) is off: new versions come as a new APK.

Plain `char` is signed on x86 but unsigned on ARM, so the runtime and the module are built
with `-fsigned-char` to behave as the port does on PC.

### Adreno: OpenGL ES

On Qualcomm Adreno GPUs the port uses Dawn's OpenGL ES backend, not Vulkan (`main.cpp`,
`ro.hardware.egl` / `ro.hardware.vulkan` = `adreno`; `LM_BACKEND=vulkan` overrides for tests).
Adreno's Vulkan driver miscompiles the 16-bit unpacking in Aurora's vertex fetch (storage
buffer reads): on an Adreno 740 (Nubia NX729J, driver 512.863.1) the title backdrop and the
file-select pictures were drawn as vertical stripes (every pixel sampled the texture's first
row) and the file-select text was missing, while the 3D world looked right. Checked on the
device: the texture data, the texcoords the game sends (U16, frac 15) and the texcoord scales
are correct; the raw interpolated texcoord t was constant in the title draw. Rewriting the
unpacking (shift and mask instead of `extractBits`, byte loads) did not help (byte loads broke
positions too), as Aurora's author found on Adreno 7xx/8xx (Dusklight issue #750). GLES draws
all of it correctly.

### Depth copies on OpenGL ES (Luigi's shadow)

OpenGL ES runs Dawn in compatibility mode, where Aurora skipped EFB depth copies, so the game's
Z16 copy (Luigi's self-shadow map, `fn_8005D8CC`) was never made and Luigi cast no shadow.
Compatibility mode forbids `textureLoad` on depth textures and sampling them with a
non-comparison sampler (both confirmed with `AURORA_DAWN_VALIDATION=1`), so Aurora patch 0251
reads each texel's 24-bit depth by a binary search of 24 comparison samples
(`textureSampleCompareLevel`, nearest, LessEqual): the same 24-bit value as the core path's
`textureLoad`. Only compatibility devices use it; core devices keep the original shaders. The
24 steps are unrolled: the Adreno GLES compiler hung for over 100 s on the loop form (start-up
"stopped responding"). Verified on the Adreno 740: shadow back, 1186 Z16 copies with validation
on and no errors.

### GitHub Actions

`.github/workflows/android.yml` builds the APK on every push to `android-port` (and by hand:
Actions > Android > Run workflow); the APK is attached to the run. A tag `android-*` also
publishes it as a pre-release. The compiler (`libllvm.so`) is built only when its script, the
LLVM version or the NDK changes (about two hours) and kept in the Actions cache. For APKs that
install over each other, add the signing keystore as the secret `ANDROID_KEYSTORE_BASE64`
(base64 of the file; key alias `lmport`, password in `ANDROID_KEYSTORE_PASS` or `android`).

### Building on a restricted network

The build downloads Dawn, SDL3 and other dependencies at configure time like on desktop.
Where GitHub archive downloads are blocked but `git clone` works, a CMake dependency
provider (`-DCMAKE_PROJECT_TOP_LEVEL_INCLUDES=<provider.cmake>`) can point FetchContent at
local git checkouts; `tools/android/build.sh` passes extra arguments through to CMake.

## Not verified yet

Built and packaged in a cloud workspace without an Android device or a game disc, so:

- Nothing has run on a device yet: rendering on mobile GPUs (Aurora's accuracy features:
  EFB copies to RAM, 6-bit blending, shadows, enhanced lighting), performance, audio, the
  touch controls' feel, controller hot-plug.
- The on-device setup (recompiling the DOL, compiling 33 files with the bundled clang,
  linking, `dlopen`) has only been tried with a synthetic module (compile, link, symbol
  checks); memory use and time on a real phone are unknown.
- Large disc images from cloud-backed file providers (Google Drive
  and similar) may be slow or not seekable; a copy in the device's own storage
  (Downloads) is the safe choice.
- First things to check on a device: setup to the title screen, then F1 > Visuals >
  Rendering toggles if anything renders wrong or slowly.
