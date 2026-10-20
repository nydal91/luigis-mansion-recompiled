#include "port/install.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#else
#include <signal.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#endif
#ifdef __ANDROID__
#include "port/android.h"
#endif

namespace port {
namespace {

InstallState sInstall;
std::vector<std::string> sArgs;

}  // namespace

const char* gameId(GameVersion v) { return v == GameVersion::PAL ? "GLMP01" : "GLME01"; }
const char* versionName(GameVersion v) { return v == GameVersion::PAL ? "PAL" : "USA"; }

Avail usCodeFeature() {
    const auto& i = sInstall;
    if (i.running == GameVersion::USA) return Avail::Yes;
    return i.usReady ? Avail::OtherBase : Avail::NeedsUsDisc;
}

Avail palBaseFeature() {
    const auto& i = sInstall;
    if (i.running == GameVersion::PAL) return Avail::Yes;
    return i.palReady ? Avail::OtherBase : Avail::NeedsPalDisc;
}

// The other release's Hidden Mansion needs that release's data: the overlay setup builds from
// both discs (docs/SETUP.md; PAL rules on the USA game, USA rules on the European game).
Avail hiddenMansionPal() {
    const auto& i = sInstall;
    if (i.running == GameVersion::PAL || i.hmOverlayReady) return Avail::Yes;
    return Avail::NeedsPalDisc;
}

Avail hiddenMansionUs() {
    const auto& i = sInstall;
    if (i.running == GameVersion::USA || i.hmOverlayReady) return Avail::Yes;
    return Avail::NeedsUsDisc;
}

const char* availNote(Avail a) {
    switch (a) {
    case Avail::Yes: return nullptr;
    case Avail::OtherBase:
        return sInstall.running == GameVersion::USA ? "applies when playing the European version"
                                                    : "applies when playing the USA version";
    case Avail::NeedsUsDisc: return "needs the USA disc: F1 > Game > Add or change discs";
    case Avail::NeedsPalDisc: return "needs the European disc: F1 > Game > Add or change discs";
    case Avail::NotYet: return "not available on the European version yet; play the USA version";
    }
    return nullptr;
}

InstallState& install() { return sInstall; }

namespace {

namespace fs = std::filesystem;

// The product was called "Luigi's Mansion PC" up to 1.0.1; its folders are moved to the new
// name on the first start (docs/SETUP.md "What lives where").
constexpr const char* kFolderName = "Luigi's Mansion Recompiled";
constexpr const char* kOldFolderName = "Luigi's Mansion PC";

std::string u8(const fs::path& p) {
    const auto s = p.u8string();
    return std::string(s.begin(), s.end());
}

// True when the folder holds nothing but what the Linux launcher writes before the game
// starts (LuigisMansionRecompiled.sh logs into the data folder first).
bool onlyLauncherLogs(const fs::path& dir) {
    std::error_code ec;
    for (auto it = fs::directory_iterator(dir, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string n = u8(it->path().filename());
        if (n != "last_run.log" && n != "previous_run.log" && n != ".last_exit_status") return false;
    }
    return !ec;
}

// Moves <base>/<old name> to <base>/<new name> when only the old one exists: settings,
// install.ini, memory cards, game modules, overlays and caches carry over (paths inside them
// are not tied to the folder: module.key hashes the sources, install.ini holds disc paths).
// A rename that fails (a file held open by another program, another volume) falls back to a
// copy; the old folder is kept then.
void migrateOldFolder(const fs::path& base) {
    std::error_code ec;
    const fs::path oldDir = base / fs::path(std::u8string(reinterpret_cast<const char8_t*>(kOldFolderName)));
    const fs::path newDir = base / fs::path(std::u8string(reinterpret_cast<const char8_t*>(kFolderName)));
    if (!fs::is_directory(oldDir, ec)) return;
    fs::path heldLogs;  // the launcher's logs of this run, put back after the move
    if (fs::exists(newDir, ec)) {
        // A 1.0.x launcher that is still used writes its log into the old data folder again:
        // nothing else in it, so remove it (the forwarder replaces that launcher, update.cpp).
        if (onlyLauncherLogs(oldDir) && !onlyLauncherLogs(newDir)) {
            fs::remove_all(oldDir, ec);
            if (!ec) std::printf("[install] removed %s (held only launcher logs)\n", u8(oldDir).c_str());
            std::fflush(stdout);
            return;
        }
        if (!fs::is_directory(newDir, ec) || !onlyLauncherLogs(newDir)) return;
        heldLogs = base / fs::path(std::u8string(reinterpret_cast<const char8_t*>(kFolderName)) + u8".logs-tmp");
        fs::remove_all(heldLogs, ec);
        fs::rename(newDir, heldLogs, ec);
        if (ec) {
            std::printf("[install] %s holds only launcher logs, but could not be moved aside (%s); "
                        "the old folder %s is not used\n",
                        u8(newDir).c_str(), ec.message().c_str(), u8(oldDir).c_str());
            return;
        }
    }
    fs::rename(oldDir, newDir, ec);
    if (!ec) {
        std::printf("[install] moved %s to %s\n", u8(oldDir).c_str(), u8(newDir).c_str());
    } else {
        const std::string why = ec.message();
        if (!heldLogs.empty()) {
            std::error_code e2;
            fs::rename(heldLogs, newDir, e2);
            if (!e2) heldLogs.clear();
        }
        fs::create_directories(newDir, ec);
        fs::copy(oldDir, newDir, fs::copy_options::recursive | fs::copy_options::skip_existing, ec);
        if (!ec)
            std::printf("[install] could not rename %s (%s); copied it to %s and kept the old folder\n",
                        u8(oldDir).c_str(), why.c_str(), u8(newDir).c_str());
        else
            std::printf("[install] could not rename %s (%s) and copying it to %s failed (%s); the old folder is "
                        "kept, copy what is missing by hand\n",
                        u8(oldDir).c_str(), why.c_str(), u8(newDir).c_str(), ec.message().c_str());
    }
    if (!heldLogs.empty()) {
        for (auto it = fs::directory_iterator(heldLogs, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            std::error_code e2;
            fs::rename(it->path(), newDir / it->path().filename(), e2);  // replaces the old run's log
        }
        fs::remove_all(heldLogs, ec);
    }
    std::fflush(stdout);
}

fs::path fromU8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

// ---- System folders ------------------------------------------------------------------------
enum class Kind { Config, Data, Cache };

#ifdef _WIN32
fs::path sysDir(Kind, bool create) {
    const wchar_t* base = _wgetenv(L"APPDATA");
    if (!base) return fs::path(L".");
    static const bool sMigrated = (migrateOldFolder(base), true);
    (void)sMigrated;
    fs::path dir = fs::path(base) / L"Luigi's Mansion Recompiled";
    std::error_code ec;
    if (create) fs::create_directories(longPath(dir), ec);
    return dir;
}
#else
// XDG base directory: $<var> if set to an absolute path, else $HOME/<fallback>. macOS
// (docs/MACOS.md): ~/Library/Application Support (config and data) and ~/Library/Caches; the
// XDG variables still override them (scratch folders for test runs).
fs::path sysDir(Kind kind, bool create) {
    const char* var = kind == Kind::Config ? "XDG_CONFIG_HOME" : kind == Kind::Data ? "XDG_DATA_HOME" : "XDG_CACHE_HOME";
#ifdef __APPLE__
    const char* fallback = kind == Kind::Cache ? "Library/Caches" : "Library/Application Support";
#else
    const char* fallback = kind == Kind::Config ? ".config" : kind == Kind::Data ? ".local/share" : ".cache";
#endif
    fs::path base;
    if (const char* v = std::getenv(var); v && v[0] == '/') base = v;
    else if (const char* home = std::getenv("HOME"); home && home[0]) base = fs::path(home) / fallback;
    else base = ".";
    // Once per base folder (config, data and cache are separate on Linux).
    static std::mutex sM;
    static std::vector<fs::path> sDone;
    {
        std::lock_guard lk(sM);
        if (std::find(sDone.begin(), sDone.end(), base) == sDone.end()) {
            sDone.push_back(base);
            migrateOldFolder(base);
        }
    }
    fs::path dir = base / kFolderName;
    std::error_code ec;
    if (create) fs::create_directories(dir, ec);
    return dir;
}
#endif

// Which system folder a file of the (flat) portable layout belongs in (Linux splits them).
Kind kindOf(const std::string& name) {
    if (name == "settings.ini" || name == "install.ini" || name == "settings.ini.tmp") return Kind::Config;
    if (name.rfind("pipeline_cache.", 0) == 0 || name.rfind("dawn_cache.", 0) == 0 || name == "imgui.log" ||
        name == "texture_dumps")
        return Kind::Cache;
    return Kind::Data;
}

// ---- Portable mode ---------------------------------------------------------------------------
constexpr const char* kPortableDir = "portable";
constexpr const char* kPortableFile = "portable.txt";
constexpr const char* kMoveRequest = ".lm-data-move";       // next to the program, read at the next start
constexpr const char* kPortableTmp = "portable.lm-tmp";     // to-portable copy before it is switched on
constexpr const char* kPortableOld = "portable.lm-old";     // portable folder after a move back
constexpr const char* kTmpSuffix = ".lm-tmp";               // system folder copy before it is switched on

fs::path computeExeDir() {
#ifdef _WIN32
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), DWORD(buf.size()));
    buf.resize(n);
    return fs::path(buf).parent_path();
#else
    return selfExeDir();
#endif
}

bool writableDir(const fs::path& dir) {
    const fs::path probe = longPath(dir / ".lm-write-test");
    {
        std::ofstream o(probe, std::ios::binary);
        if (!o) return false;
        o << "x";
        if (!o) return false;
    }
    std::error_code ec;
    fs::remove(probe, ec);
    return true;
}

// Sum of the file sizes under p (a file or a folder).
std::uintmax_t treeSize(const fs::path& path) {
    const fs::path p = longPath(path);
    std::error_code ec;
    if (fs::is_regular_file(p, ec)) return fs::file_size(p, ec);
    std::uintmax_t n = 0;
    for (auto it = fs::recursive_directory_iterator(p, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec)) {
        std::error_code e;
        if (it->is_regular_file(e)) n += it->file_size(e);
    }
    return n;
}

// Free space where `p` (or its nearest existing parent) lives.
std::uintmax_t freeSpace(fs::path p) {
    std::error_code ec;
    while (!p.empty() && !fs::exists(p, ec)) {
        const fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    const auto s = fs::space(p, ec);
    return ec ? UINTMAX_MAX : s.available;
}

std::string sizeText(std::uintmax_t b) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.0f MB", double(b) / 1048576.0 + 0.5);
    return buf;
}

// Copies `src` (file or folder, recursively) to `dst` and checks every file's size afterwards.
bool copyChecked(const fs::path& src, const fs::path& dst, std::string& err) {
    std::error_code ec;
    if (fs::is_regular_file(src, ec)) {
        fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
        std::error_code e2;
        if (!ec && fs::file_size(dst, e2) != fs::file_size(src, ec)) ec = std::make_error_code(std::errc::io_error);
        if (ec) err = "could not copy " + displayPath(src) + ": " + ec.message();
        return !ec;
    }
    if (!fs::is_directory(src, ec)) return true;  // links and other special files: none of ours
    fs::create_directories(dst, ec);
    if (ec) {
        err = "could not create " + displayPath(dst) + ": " + ec.message();
        return false;
    }
    for (auto it = fs::directory_iterator(src, ec); !ec && it != fs::directory_iterator(); it.increment(ec))
        if (!copyChecked(it->path(), dst / it->path().filename(), err)) return false;
    if (ec) err = "could not read " + displayPath(src) + ": " + ec.message();
    return !ec;
}

// The previous process (it asked for the move and restarted) must be gone: its files (game
// module, SQLite caches) are only consistent and movable after it exited.
void waitForProcess(unsigned long pid) {
    if (!pid) return;
#ifdef _WIN32
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid))) {
        WaitForSingleObject(h, 15000);
        CloseHandle(h);
    }
#else
    for (int i = 0; i < 150; i++) {
        if (kill(pid_t(pid), 0) != 0) return;
#ifdef __APPLE__
        int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, int(pid)};
        kinfo_proc kp{};
        size_t len = sizeof(kp);
        if (sysctl(mib, 4, &kp, &len, nullptr, 0) == 0 && len && kp.kp_proc.p_stat == SZOMB) return;  // exited
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        continue;
#endif
        std::ifstream st("/proc/" + std::to_string(pid) + "/stat");
        std::string s;
        std::getline(st, s);
        const auto close = s.rfind(')');
        if (close != std::string::npos && close + 2 < s.size() && s[close + 2] == 'Z') return;  // exited
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
}

// The distinct system folders (Windows: one).
std::vector<fs::path> distinctSysDirs(bool create) {
    std::vector<fs::path> v;
    for (Kind k : {Kind::Config, Kind::Data, Kind::Cache}) {
        const fs::path d = sysDir(k, create);
        if (std::find(v.begin(), v.end(), d) == v.end()) v.push_back(d);
    }
    return v;
}

bool emptyOrLauncherLogs(const fs::path& dir) {
    std::error_code ec;
    return !fs::exists(dir, ec) || (fs::is_directory(dir, ec) && onlyLauncherLogs(dir));
}

// System folders -> <exe>/portable. Copy into portable.lm-tmp, rename it to portable (the
// switch), then delete the system folders. Before the switch the system folders are untouched.
bool moveToPortable(const fs::path& exe, std::string& err, std::string& warn) {
    std::error_code ec;
    const fs::path root = longPath(exe / kPortableDir), tmp = longPath(exe / kPortableTmp);
    if (fs::exists(root, ec)) {
        if (!fs::is_directory(root, ec) || !fs::is_empty(root, ec)) {
            err = displayPath(root) + " already exists and is not empty";
            return false;
        }
        fs::remove(root, ec);
    }
    std::vector<fs::path> sources;
    for (auto& d : distinctSysDirs(false)) sources.push_back(longPath(d));
    std::vector<std::string> names;
    std::uintmax_t total = 0;
    for (auto& s : sources) {
        for (auto it = fs::directory_iterator(s, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
            const std::string n = displayPath(it->path().filename());
            if (std::find(names.begin(), names.end(), n) != names.end()) {
                err = "two system folders both hold " + n;
                return false;
            }
            names.push_back(n);
        }
        ec.clear();
        total += treeSize(s);
    }
    if (const auto free = freeSpace(exe); free < total + (64u << 20)) {
        err = "not enough free space in " + displayPath(exe) + " (" + sizeText(total) + " needed, " + sizeText(free) + " free)";
        return false;
    }
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp, ec);
    if (ec) {
        err = "could not create " + displayPath(tmp) + ": " + ec.message();
        return false;
    }
    for (auto& s : sources) {
        std::error_code e;
        for (auto it = fs::directory_iterator(s, e); !e && it != fs::directory_iterator(); it.increment(e)) {
            if (!copyChecked(it->path(), tmp / it->path().filename(), err)) {
                fs::remove_all(tmp, ec);
                return false;
            }
        }
    }
    fs::rename(tmp, root, ec);
    if (ec) {
        err = "could not rename " + displayPath(tmp) + " to " + displayPath(root) + ": " + ec.message();
        fs::remove_all(tmp, ec);
        return false;
    }
    std::printf("[data] copied %s into %s\n", sizeText(total).c_str(), displayPath(root).c_str());
    for (auto& s : sources) {
        std::error_code e;
        fs::remove_all(s, e);
        if (e && fs::exists(s, e)) warn += "The old folder " + displayPath(s) + " could not be removed completely; delete it by hand. ";
    }
    return true;
}

// <exe>/portable -> system folders. Each system folder is built as <dir>.lm-tmp and renamed into
// place (the system folders must be empty: no other installation's data is overwritten), then
// portable.txt is removed and the portable folder renamed away (the switch) and deleted.
bool moveToSystem(const fs::path& exe, std::string& err, std::string& warn) {
    std::error_code ec;
    const fs::path root = longPath(exe / kPortableDir);
    if (!fs::is_directory(root, ec)) {
        err = "there is no portable folder " + displayPath(root);
        return false;
    }
    struct Target {
        fs::path dir, tmp;
        bool committed = false;
    };
    std::vector<Target> targets;
    auto targetFor = [&](const fs::path& dir) -> Target& {
        for (auto& t : targets)
            if (t.dir == dir) return t;
        fs::path tmp = dir;
        tmp += kTmpSuffix;
        targets.push_back({dir, tmp});
        return targets.back();
    };
    targets.reserve(3);  // entries point into it
    std::vector<std::pair<fs::path, Target*>> entries;
    for (Kind k : {Kind::Config, Kind::Data, Kind::Cache}) targetFor(longPath(sysDir(k, false)));
    for (auto it = fs::directory_iterator(root, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const std::string n = displayPath(it->path().filename());
        if (n == ".lm-write-test") continue;
        entries.push_back({it->path(), &targetFor(longPath(sysDir(kindOf(n), false)))});
    }
    if (ec) {
        err = "could not read " + displayPath(root) + ": " + ec.message();
        return false;
    }
    for (auto& t : targets)
        if (!emptyOrLauncherLogs(t.dir)) {
            err = displayPath(t.dir) + " already holds data (another installation?); move or delete it first";
            return false;
        }
    const std::uintmax_t total = treeSize(root);
    if (const auto free = freeSpace(targets.front().dir); free < total + (64u << 20)) {
        err = "not enough free space for " + displayPath(targets.front().dir) + " (" + sizeText(total) + " needed, " +
              sizeText(free) + " free)";
        return false;
    }
    auto undo = [&] {
        for (auto& t : targets) {
            std::error_code e;
            fs::remove_all(t.committed ? t.dir : t.tmp, e);
        }
    };
    for (auto& t : targets) {
        fs::remove_all(t.tmp, ec);
        fs::create_directories(t.tmp, ec);
        if (ec) {
            err = "could not create " + displayPath(t.tmp) + ": " + ec.message();
            undo();
            return false;
        }
    }
    for (auto& [path, t] : entries)
        if (!copyChecked(path, t->tmp / path.filename(), err)) {
            undo();
            return false;
        }
    for (auto& t : targets) {
        std::error_code e;
        if (fs::exists(t.dir, e)) {  // only launcher logs in it (or nothing): the portable run's own win
            for (auto it = fs::directory_iterator(t.dir, e); !e && it != fs::directory_iterator(); it.increment(e)) {
                std::error_code e2;
                if (!fs::exists(t.tmp / it->path().filename(), e2)) fs::rename(it->path(), t.tmp / it->path().filename(), e2);
            }
            fs::remove_all(t.dir, e);
        }
        fs::rename(t.tmp, t.dir, e);
        if (e) {
            err = "could not rename " + displayPath(t.tmp) + " to " + displayPath(t.dir) + ": " + e.message();
            undo();
            return false;
        }
        t.committed = true;
    }
    // The switch: without portable.txt and the portable folder the next start uses the system folders.
    const fs::path marker = longPath(exe / kPortableFile), old = longPath(exe / kPortableOld);
    const bool hadMarker = fs::exists(marker, ec);
    if (hadMarker) {
        fs::remove(marker, ec);
        if (ec) {
            err = "could not remove " + displayPath(marker) + ": " + ec.message();
            undo();
            return false;
        }
    }
    fs::remove_all(old, ec);
    fs::rename(root, old, ec);
    if (ec) {
        err = "could not rename " + displayPath(root) + ": " + ec.message() + " (is a file in it open in another program?)";
        if (hadMarker) std::ofstream(marker, std::ios::binary) << "portable\n";
        undo();
        return false;
    }
    std::printf("[data] copied %s from %s into the system folders\n", sizeText(total).c_str(), displayPath(root).c_str());
    fs::remove_all(old, ec);
    if (ec && fs::exists(old, ec)) warn += "The old folder " + displayPath(old) + " could not be removed completely; delete it by hand. ";
    return true;
}

// A move the F1 menu asked for before it restarted the program (requestDataMove).
void processMoveRequest(DataLocation& l) {
    const fs::path req = longPath(l.exeDir / kMoveRequest);
    std::error_code ec;
    if (!fs::exists(req, ec)) return;
    std::string what;
    unsigned long pid = 0;
    {
        std::ifstream in(req, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("pid=", 0) == 0) pid = std::strtoul(line.c_str() + 4, nullptr, 10);
            else if (!line.empty() && what.empty()) what = line;
        }
    }
    fs::remove(req, ec);  // once: a move that fails (or a crash in it) is not retried at every start
    if (what != "to-portable" && what != "to-system") return;
    const bool toPortable = what == "to-portable";
    std::printf("[data] moving the data %s\n", toPortable ? "into the portable folder" : "back to the system folders");
    std::fflush(stdout);
    waitForProcess(pid);
    std::string err, warn;
    const bool ok = toPortable ? moveToPortable(l.exeDir, err, warn) : moveToSystem(l.exeDir, err, warn);
    if (ok) {
        l.moveResult = toPortable ? "The data was moved into the portable folder. " : "The data was moved back to the system folder. ";
        l.moveResult += warn;
    } else {
        l.moveFailed = true;
        l.moveResult = std::string("Moving the data ") + (toPortable ? "into the portable folder" : "back to the system folder") +
                       " failed: " + err + ". Nothing was changed; the data is where it was.";
    }
    std::printf("[data] %s\n", l.moveResult.c_str());
    std::fflush(stdout);
}

bool envFlag(const char* name, bool& value) {
    const char* v = std::getenv(name);
    if (!v || !*v) return false;
    const std::string s = v;
    value = !(s == "0" || s == "false" || s == "no" || s == "off");
    return true;
}

DataLocation computeLocation() {
    DataLocation l;
    l.exeDir = computeExeDir();
    std::error_code ec;
    // Leftovers of a move that stopped half-way: the copy before its switch (the source is
    // untouched) or the old portable folder after a move back (its data is in the system folders).
    fs::remove_all(longPath(l.exeDir / kPortableTmp), ec);
    fs::remove_all(longPath(l.exeDir / kPortableOld), ec);
    processMoveRequest(l);
    bool env = false;
    const bool haveEnv = envFlag("LM_PORTABLE", env);
    const bool marker = fs::exists(l.exeDir / kPortableFile, ec) || fs::is_directory(l.exeDir / kPortableDir, ec);
    l.requested = haveEnv ? env : marker;
    l.forcedByEnv = haveEnv && env;
    if (l.requested) {
        const fs::path root = longPath(l.exeDir / kPortableDir);
        const bool existed = fs::is_directory(root, ec);
        fs::create_directories(root, ec);
        if (fs::is_directory(root, ec) && writableDir(root)) {
            l.portable = true;
            l.root = l.exeDir / kPortableDir;
        } else {
            if (!existed) fs::remove(root, ec);  // only if it is empty
            l.problem = "Portable mode is requested (" +
                        std::string(l.forcedByEnv ? "LM_PORTABLE=1" : "portable.txt or a portable folder next to the program") +
                        "), but the program folder " + displayPath(l.exeDir) +
                        " cannot be written (e.g. Program Files), so the data stays in the system folder. Move the "
                        "game to a folder you own to use portable mode.";
        }
    }
    if (l.portable) std::printf("[data] portable mode: %s\n", displayPath(l.root).c_str());
    else if (!l.problem.empty()) std::printf("[data] %s\n", l.problem.c_str());
    std::fflush(stdout);
    return l;
}

}  // namespace

const DataLocation& dataLocation() {
    static const DataLocation sLoc = computeLocation();
    return sLoc;
}

fs::path exeDir() { return dataLocation().exeDir; }

std::vector<fs::path> systemDirs() { return distinctSysDirs(false); }

bool exeDirWritable() { return writableDir(exeDir()); }

bool isPortableDataPath(const fs::path& rel) {
    if (rel.empty()) return false;
    const std::string first = displayPath(*rel.begin());
    return first == kPortableDir || first == kPortableFile || first == kMoveRequest || first == kPortableTmp ||
           first == kPortableOld;
}

std::string checkDataMove(bool toPortable) {
    const auto& l = dataLocation();
    std::error_code ec;
    if (toPortable) {
        if (l.portable) return "This installation is already portable.";
        if (!exeDirWritable())
            return "The program folder " + displayPath(l.exeDir) +
                   " cannot be written (e.g. Program Files). Move the game to a folder you own first.";
        const fs::path root = l.exeDir / kPortableDir;
        if (fs::exists(root, ec) && !(fs::is_directory(root, ec) && fs::is_empty(root, ec)))
            return displayPath(root) + " already exists and is not empty.";
        std::uintmax_t total = 0;
        for (auto& d : distinctSysDirs(false)) total += treeSize(d);
        if (const auto free = freeSpace(l.exeDir); free < total + (64u << 20))
            return "Not enough free space next to the program (" + sizeText(total) + " needed, " + sizeText(free) + " free).";
        return {};
    }
    if (!l.portable) return "This installation is not portable.";
    if (l.forcedByEnv) return "Portable mode is forced by the environment variable LM_PORTABLE=1; remove it first.";
    for (auto& d : distinctSysDirs(false))
        if (!emptyOrLauncherLogs(d))
            return displayPath(d) + " already holds data (another installation of the game?). Move or delete it first; nothing is "
                           "overwritten.";
    const auto free = freeSpace(sysDir(Kind::Data, false));
    if (const auto total = treeSize(l.root); free < total + (64u << 20))
        return "Not enough free space for the system folder (" + sizeText(total) + " needed, " + sizeText(free) + " free).";
    return {};
}

std::string requestDataMove(bool toPortable) {
    if (std::string err = checkDataMove(toPortable); !err.empty()) return err;
    const fs::path req = longPath(exeDir() / kMoveRequest);
    {
        std::ofstream out(req, std::ios::binary | std::ios::trunc);
#ifdef _WIN32
        const unsigned long pid = GetCurrentProcessId();
#else
        const unsigned long pid = (unsigned long)getpid();
#endif
        out << (toPortable ? "to-portable" : "to-system") << "\npid=" << pid << '\n';
        if (!out) return "Could not write " + displayPath(req) + ".";
    }
    std::printf("[data] restarting to move the data %s\n", toPortable ? "into the portable folder" : "to the system folder");
    std::fflush(stdout);
    relaunch();
    std::error_code ec;
    fs::remove(req, ec);
    return "The game could not restart; start it again to move the data.";
}

std::filesystem::path configDir() {
#ifdef __ANDROID__
    return android::filesDir();  // settings, install.ini (docs/ANDROID.md)
#endif
    const auto& l = dataLocation();
    return l.portable ? l.root : sysDir(Kind::Config, true);
}

std::filesystem::path dataDir() {
#ifdef __ANDROID__
    return android::filesDir();  // game modules, overlays, memory cards
#endif
    const auto& l = dataLocation();
    return l.portable ? l.root : sysDir(Kind::Data, true);
}

std::filesystem::path cacheDir() {
#ifdef __ANDROID__
    return android::cacheDir();  // GPU pipeline caches (the system may clear them)
#endif
    const auto& l = dataLocation();
    return l.portable ? l.root : sysDir(Kind::Cache, true);
}

std::string discOpenPath(const std::string& stored, std::string* error) {
#ifdef __ANDROID__
    return android::openDiscPath(stored, error);
#else
    (void)error;
    return stored;
#endif
}

std::string discDisplayName(const std::string& stored) {
#ifdef __ANDROID__
    return android::displayName(stored);
#else
    return stored;
#endif
}

std::filesystem::path envPath(const char* name) {
#ifdef _WIN32
    const std::wstring wname(name, name + std::strlen(name));
    if (const wchar_t* v = _wgetenv(wname.c_str())) return std::filesystem::path(v);
    return {};
#else
    const char* v = std::getenv(name);
    return v ? std::filesystem::path(v) : std::filesystem::path();
#endif
}

std::filesystem::path hiddenMansionOverlayDir() { return dataDir() / "hidden_mansion"; }

std::filesystem::path longPath(const std::filesystem::path& p) {
#ifdef _WIN32
    if (p.empty() || p.native().rfind(L"\\\\?\\", 0) == 0) return p;
    std::error_code ec;
    fs::path a = fs::absolute(p, ec);  // GetFullPathNameW: any length, resolves . and .., / -> \.
    if (ec) a = p;
    a = a.lexically_normal();
    a.make_preferred();
    const std::wstring& s = a.native();
    if (s.rfind(L"\\\\.\\", 0) == 0) return a;  // device path
    if (s.rfind(L"\\\\", 0) == 0) return fs::path(L"\\\\?\\UNC\\" + s.substr(2));
    if (s.size() >= 3 && s[1] == L':' && s[2] == L'\\') return fs::path(L"\\\\?\\" + s);
    return a;
#else
    return p;
#endif
}

std::filesystem::path plainPath(const std::filesystem::path& p) {
#ifdef _WIN32
    const std::wstring& s = p.native();
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) return fs::path(L"\\\\" + s.substr(8));
    if (s.rfind(L"\\\\?\\", 0) == 0) return fs::path(s.substr(4));
#endif
    return p;
}

std::string displayPath(const std::filesystem::path& p) { return u8(plainPath(p)); }

bool longPathsEnabled() {
#ifdef _WIN32
    // ntdll's answer for this process (Windows 10 1607+): the manifest's longPathAware and the
    // LongPathsEnabled policy together.
    static const bool sOn = [] {
        using Fn = BOOLEAN(NTAPI*)();
        const HMODULE nt = GetModuleHandleW(L"ntdll.dll");
        const auto fn = nt ? reinterpret_cast<Fn>(GetProcAddress(nt, "RtlAreLongPathsEnabled")) : nullptr;
        return fn && fn() != 0;
    }();
    return sOn;
#else
    return true;  // no such limit
#endif
}

std::string installPathProblem() {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    const std::string dir = displayPath(fs::path(std::wstring(buf, n)).parent_path());
    if (dir.size() <= kMaxInstallPath) return {};
    std::printf("[install] game folder %s is %zu characters long (at most %zu)\n", dir.c_str(), dir.size(),
                kMaxInstallPath);
    std::fflush(stdout);
    return "The game's folder\n\n" + dir + "\n\nis " + std::to_string(dir.size()) +
           " characters long. The graphics components the game loads from it stop working past " +
           std::to_string(kMaxInstallPath) +
           " characters.\n\nMove the game's folder to a shorter path (for example C:\\Games\\Luigi's Mansion "
           "Recompiled) and start it from there. Settings and saves are not in that folder and stay as they are.";
#else
    return {};
#endif
}

std::string dataPathProblem() {
#ifdef _WIN32
    const std::string dir = displayPath(dataDir());
    std::printf("[install] data folder %s (%zu characters), Windows long paths %s\n", dir.c_str(), dir.size(),
                longPathsEnabled() ? "on" : "off");
    std::fflush(stdout);
    if (longPathsEnabled() || dir.size() + kDeepestDataFile <= kMaxPlainPath) return {};
    return "The game's data folder\n\n" + dir + "\n\nis " + std::to_string(dir.size()) +
           " characters long. Windows' long path support is off on this PC, so files deeper than " +
           std::to_string(kMaxPlainPath) + " characters (memory card saves, shader caches) cannot be written there: "
           "saving may fail.\n\nUse " +
           std::string(dataLocation().portable ? "a shorter game folder (portable mode)" : "a %APPDATA% folder") +
           " of at most " + std::to_string(kMaxPlainPath - kDeepestDataFile) + " characters, or turn on long paths in Windows (the LongPathsEnabled setting, needs an administrator).";
#else
    return {};
#endif
}

namespace {

// Portable mode: install.ini also keeps each disc's path relative to the program folder, and a
// disc that is not at its saved path is looked for there (the folder moved to another drive or
// PC): at that relative path, then by name next to the program and in the portable folder.
std::string relToExe(const std::string& disc) {
    if (disc.empty()) return {};
    std::error_code ec;
    const fs::path abs = fs::absolute(fromU8(disc), ec).lexically_normal();
    const fs::path base = exeDir().lexically_normal();
    if (ec || abs.root_name() != base.root_name()) return {};
    const fs::path rel = abs.lexically_relative(base);
    const auto g = rel.generic_u8string();
    return std::string(g.begin(), g.end());
}

void findMovedDisc(std::string& disc, const std::string& rel) {
    const auto& l = dataLocation();
    std::error_code ec;
    if (!l.portable || disc.empty() || fs::exists(fromU8(disc), ec)) return;
    std::vector<fs::path> tries;
    if (!rel.empty()) tries.push_back((l.exeDir / fromU8(rel)).lexically_normal());
    const fs::path name = fromU8(disc).filename();  // also a Windows path read on Linux: last component
    std::string n = u8(name);
    if (const auto s = n.find_last_of("/\\"); s != std::string::npos) n = n.substr(s + 1);
    if (!n.empty()) {
        tries.push_back(l.exeDir / fromU8(n));
        tries.push_back(l.root / fromU8(n));
    }
    for (auto& t : tries)
        if (fs::is_regular_file(t, ec)) {
            std::printf("[install] disc %s not found; using %s\n", disc.c_str(), u8(t).c_str());
            disc = u8(t);
            return;
        }
}

}  // namespace

void loadInstall() {
    std::ifstream in(longPath(configDir() / "install.ini"));
    std::string line, usRel, palRel;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == ';' || line[0] == '[') continue;
        const std::string key = line.substr(0, eq), value = line.substr(eq + 1);
        if (key == "us_disc") sInstall.usDisc = value;
        else if (key == "pal_disc") sInstall.palDisc = value;
        else if (key == "us_disc_rel") usRel = value;
        else if (key == "pal_disc_rel") palRel = value;
        else if (key == "base") sInstall.base = value == "PAL" ? GameVersion::PAL : GameVersion::USA;
    }
    findMovedDisc(sInstall.usDisc, usRel);
    findMovedDisc(sInstall.palDisc, palRel);
}

void saveInstall() {
    std::ofstream out(longPath(configDir() / "install.ini"));
    const bool portable = dataLocation().portable;
    out << "[install]\n";
    out << "us_disc=" << sInstall.usDisc << '\n';
    if (portable) out << "us_disc_rel=" << relToExe(sInstall.usDisc) << '\n';
    out << "pal_disc=" << sInstall.palDisc << '\n';
    if (portable) out << "pal_disc_rel=" << relToExe(sInstall.palDisc) << '\n';
    out << "base=" << versionName(sInstall.base) << '\n';
}

void setLaunchArgs(int argc, char** argv) { sArgs.assign(argv, argv + argc); }

#ifndef _WIN32
fs::path selfExePath() {
    std::error_code ec;
#ifdef __APPLE__
    uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::string buf(size, '\0');
    if (_NSGetExecutablePath(buf.data(), &size) != 0) return {};
    buf.resize(std::strlen(buf.c_str()));
    const fs::path p = fs::canonical(buf, ec);
    return ec ? fs::path(buf) : p;
#else
    const fs::path p = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::path() : p;
#endif
}

fs::path selfExeDir() {
    const fs::path p = selfExePath();
    if (!p.empty()) return p.parent_path();
    std::error_code ec;
    fs::path cwd = fs::current_path(ec);  // what the program was started from (the old fallback)
    return ec ? fs::path(".") : cwd;
}
#endif

void relaunch(bool setup, const std::filesystem::path& exePath) {
#ifdef _WIN32
    wchar_t exe[MAX_PATH * 4];
    GetModuleFileNameW(nullptr, exe, DWORD(std::size(exe)));
    if (!exePath.empty()) wcsncpy_s(exe, exePath.wstring().c_str(), _TRUNCATE);
    // Re-use the original command line (minus the program name).
    std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
    int argc = 0;
    if (wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc)) {
        for (int i = 1; i < argc; i++) {
            const std::wstring a = argv[i];
            if (a == L"--setup" || a == L"--setup-auto") continue;
            cmd += L" \"" + a + L"\"";
        }
        LocalFree(argv);
    }
    if (setup) cmd += L" --setup";
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    // Past MAX_PATH the program is started by its extended path (CreateProcessW takes that at any
    // length; the command line keeps the plain name). A shorter one stays plain, so the new
    // process sees the same paths as a normal start.
    const fs::path plain = plainPath(fs::path(exe));
    const std::wstring app = plain.native().size() > kMaxPlainPath ? longPath(plain).wstring() : plain.wstring();
    cmd = L"\"" + plain.wstring() + cmd.substr(cmd.find(L'"', 1));
    if (CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    } else if (!exePath.empty()) {
        return;  // the updater reports it
    }
#elif defined(__ANDROID__)
    (void)exePath;
    android::relaunch(setup);  // a new activity instance; this process exits
#else
    std::vector<char*> argv;
    for (auto& a : sArgs) argv.push_back(a.data());
    static char setupArg[] = "--setup";
    if (setup) argv.push_back(setupArg);
    argv.push_back(nullptr);
    if (!exePath.empty()) {
        std::fflush(nullptr);
        execv(exePath.c_str(), argv.data());
        return;  // exec failed: the caller reports it
    }
#ifdef __APPLE__
    const fs::path self = selfExePath();
    if (!self.empty() && fork() == 0) execv(self.c_str(), argv.data());
#else
    if (fork() == 0) execv("/proc/self/exe", argv.data());  // works even when readlink failed
#endif
#endif
    std::_Exit(0);
}

}  // namespace port
