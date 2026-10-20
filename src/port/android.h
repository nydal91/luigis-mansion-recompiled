#pragma once
// Android platform helpers (docs/ANDROID.md). Only built for Android; the Java side is
// packaging/android/java/dev/lmport/LMActivity.java.
#ifdef __ANDROID__
#include <filesystem>
#include <string>

namespace port::android {

// Sends stdout/stderr (all of the runtime's logging) to logcat (tag "LuigisMansion") and to
// last_run.log in filesDir() (the previous run's log is kept as previous_run.log). Android
// discards both streams otherwise. Call first thing in main.
void redirectOutput();

// App-private folders (Context.getFilesDir / getCacheDir).
std::filesystem::path filesDir();
std::filesystem::path cacheDir();

// Folder with files shipped in the APK's assets that the runtime reads by path
// (pipelines.bin), copied there by LMActivity on the first start of each app version.
std::filesystem::path resourceDir();

// Folder with the bundled compiler (the APK's native library folder: libllvm.so).
std::filesystem::path nativeLibDir();

// A path nod can open for a disc image the user picked: the system file picker returns
// content:// URIs; they are opened through the ContentResolver (keeping the permission
// across restarts); the URI is returned and opened with SDL_IOFromFile (which reads it through
// the ContentResolver). Other paths are returned as is.
// Empty (and `error` set) when the URI can no longer be opened (file moved or deleted).
std::string openDiscPath(const std::string& stored, std::string* error = nullptr);

// A short name for a stored disc path, for the setup screen (content URIs are long).
std::string displayName(const std::string& stored);

// Restarts the app (optionally on the setup screen); does not return.
[[noreturn]] void relaunch(bool setup);

}  // namespace port::android
#endif
