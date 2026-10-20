#pragma once
// What the user installed: their disc images and what setup derived from them
// (docs/SETUP.md). Persisted to %APPDATA%\Luigi's Mansion Recompiled\install.ini; the derived
// files live next to it. The F1 menu reads it to grey out features whose disc is missing.
#include <filesystem>
#include <string>
#include <vector>

namespace port {

enum class GameVersion { USA, PAL };  // GLME01 / GLMP01

const char* gameId(GameVersion v);         // "GLME01" / "GLMP01"
const char* versionName(GameVersion v);    // "USA" / "PAL"

struct InstallState {
    std::string usDisc;   // path to the user's US disc image (ISO/RVZ/...), empty if none
    std::string palDisc;  // path to the PAL disc image, empty if none
    GameVersion base = GameVersion::USA;  // which version runs (when both are installed)

    // Filled at startup from the setup results (not persisted).
    bool usReady = false;    // US game module built and current
    bool palReady = false;   // PAL game module built and current
    bool hmOverlayReady = false;  // PAL Hidden Mansion overlay for the US base (needs both discs)
    GameVersion running = GameVersion::USA;  // version of the game that is running

    bool staticBuild = false;  // lmrecomp: one version linked in, no setup

    bool both() const { return usReady && palReady; }
};

// Feature availability for the F1 menu (docs/SETUP.md, "Feature gating"). Only a
// single-disc install greys features out; with both discs everything stays selectable and
// options for the other version say where they apply.
enum class Avail {
    Yes,           // works in the running game
    OtherBase,     // works when the other version is the base (both discs installed)
    NeedsUsDisc,   // single-disc install without the USA disc
    NeedsPalDisc,  // single-disc install without the European disc
    NotYet,        // both discs installed, but not implemented for the running base yet
};
Avail usCodeFeature();      // enhancements written against the USA game's code/addresses
Avail palBaseFeature();     // 50/60 Hz, language: options of the European game itself
Avail hiddenMansionPal();   // the European Hidden Mansion rules
Avail hiddenMansionUs();    // the USA Hidden Mansion rules
const char* availNote(Avail a);  // short explanation, nullptr for Yes

InstallState& install();

// Where the port writes (created on demand; docs/SETUP.md, docs/LINUX.md):
//   Windows: all three are %APPDATA%\Luigi's Mansion Recompiled.
//   Linux:   configDir  $XDG_CONFIG_HOME/Luigi's Mansion Recompiled (~/.config/...): settings.ini, install.ini
//            dataDir    $XDG_DATA_HOME/Luigi's Mansion Recompiled (~/.local/share/...): game modules, Hidden
//                       Mansion overlays, memory cards, screenshots
//            cacheDir   $XDG_CACHE_HOME/Luigi's Mansion Recompiled (~/.cache/...): GPU pipeline/shader caches
//   Portable mode (both): all three are <program folder>/portable (the Windows layout).
std::filesystem::path configDir();
std::filesystem::path dataDir();
std::filesystem::path cacheDir();

// Portable mode (docs/SETUP.md "Portable mode"): a file portable.txt or a folder portable next to
// the program (or LM_PORTABLE=1) keeps every file of the port in <program folder>/portable.
// Decided once, at the first call (which also carries out a data move the F1 menu asked for
// before the restart); LM_PORTABLE=0 forces the system folders (testing).
struct DataLocation {
    bool portable = false;
    std::filesystem::path root;    // portable: <exeDir>/portable
    std::filesystem::path exeDir;  // folder of the running program
    bool requested = false;        // portable.txt / portable folder / LM_PORTABLE=1
    bool forcedByEnv = false;      // LM_PORTABLE=1: the menu cannot switch it off
    std::string problem;           // why requested portable mode is off (folder not writable)
    std::string moveResult;        // outcome of a data move carried out at this start ("" none)
    bool moveFailed = false;
};
const DataLocation& dataLocation();
std::filesystem::path exeDir();
// The system folders (Windows: one, Linux: config, data, cache), whatever mode runs.
std::vector<std::filesystem::path> systemDirs();
// Whether the program folder can be written (probe file).
bool exeDirWritable();
// F1 > Game: whether a move to (toPortable) or from portable mode can start now; "" or the reason.
std::string checkDataMove(bool toPortable);
// Saves the request next to the program and restarts; the next start moves the data before it
// opens any file (copy, verify, switch, then remove the old copy; nothing is lost on failure).
// Returns only on failure (the reason).
std::string requestDataMove(bool toPortable);
// A path inside the program folder (relative to it) that holds the player's data in portable
// mode: the updater never replaces or removes these (update.cpp).
bool isPortableDataPath(const std::filesystem::path& relToExeDir);
// An environment variable holding a path (read as UTF-16 on Windows, so any characters survive).
std::filesystem::path envPath(const char* name);

// PAL Hidden Mansion overlay built by setup for the US base: <dataDir>/hidden_mansion
std::filesystem::path hiddenMansionOverlayDir();

// Long paths on Windows (docs/SETUP.md "Long paths"). The executable's manifest declares
// longPathAware, but Windows honours that only when the system policy (LongPathsEnabled) is on;
// otherwise every file call fails past 259 characters and reports existing files as missing.
//   longPath:  p as an extended-length path (\\?\C:\... or \\?\UNC\server\share\...): absolute,
//              normalized, backslashes. File calls (std::filesystem, fstream, CreateFileW,
//              CreateProcessW's program, LoadLibraryW) take it at any length. Not for paths
//              given to other programs (clang/lld/tar reject it) or shown to the player.
//   plainPath: p without that prefix (messages, logs, tool arguments).
//   longPathsEnabled: this process gets long paths without the prefix (manifest + policy).
// Elsewhere both are p unchanged.
std::filesystem::path longPath(const std::filesystem::path& p);
std::filesystem::path plainPath(const std::filesystem::path& p);
std::string displayPath(const std::filesystem::path& p);  // UTF-8 of plainPath
bool longPathsEnabled();
// The longest path a program that is not long-path aware can open (MAX_PATH - 1 on Windows).
constexpr size_t kMaxPlainPath = 259;
// The deepest file the port writes below dataDir() (memory card files, the module build's
// obj/ and include/recomp/ files), in characters including the separator: a data folder longer
// than kMaxPlainPath minus this cannot be used without long paths (warned at startup).
constexpr size_t kDeepestDataFile = 52;
// The deepest game folder the program runs from (Windows): from a deeper one the prebuilt Dawn
// / Direct3D 12 start-up fails without a word (measured: 234 runs, 236 and deeper stop at the
// adapter request, with or without long paths), so main() refuses it with installPathProblem().
constexpr size_t kMaxInstallPath = 234;
std::string installPathProblem();  // "" when the program's folder is short enough
// A message for the player when long paths are off and the data folder is too deep for the
// parts that use plain paths (Aurora's memory cards, caches, imgui.ini); "" when all is well.
std::string dataPathProblem();
// The path to open a stored disc image path with (nod / aurora_dvd_open). The same path on
// desktops; on Android the file picker gives content:// URIs, opened here through the
// ContentResolver (port/android.h). Empty, with `error` set, if it can't be opened.
std::string discOpenPath(const std::string& stored, std::string* error = nullptr);
// How to show a stored disc path to the player (Android: the file's name instead of the URI).
std::string discDisplayName(const std::string& stored);

void loadInstall();
void saveInstall();

// Restarts the executable with the same arguments (after the user changed the install);
// `setup` opens the setup screen (add or change discs). `exe`: the program to start instead of
// the running one's file (the updater: on Linux /proc/self/exe names the replaced file); on Linux
// it is exec'd in place of this process (same PID, so a launcher script keeps waiting for it).
void relaunch(bool setup = false, const std::filesystem::path& exe = {});
#ifndef _WIN32
// The running program's file (Linux: /proc/self/exe, macOS: _NSGetExecutablePath, links resolved);
// empty when it cannot be found.
std::filesystem::path selfExePath();
// Its folder; never empty: the current directory when selfExePath() is empty (the behaviour
// before selfExePath existed), so callers never join paths onto an empty relative path.
std::filesystem::path selfExeDir();
#endif
void setLaunchArgs(int argc, char** argv);

}  // namespace port
