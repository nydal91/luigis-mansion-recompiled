// Entry point of the recompiled build: loads the DOL from the user's disc into
// guest memory, boots it on a guest thread, and runs the Aurora host loop.
#include <aurora/aurora.h>
#include <aurora/dvd.h>
#include <aurora/event.h>
#include <aurora/main.h>
#include <aurora/present_timing.h>
#include <dolphin/dvd.h>
#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_video.h>

#include "port/crash_handler.h"
#include "port/dol.h"
#include "port/install.h"
#include "port/options.h"
#include "port/recovery.h"
#include "port/settings.h"
#include "port/update.h"
#include "recomp/audio.h"
#include "recomp/cpu_qos.h"
#include "recomp/mem_report.h"
#include "recomp/frame.h"
#include "recomp/gx_bridge.h"
#include "recomp/menu.h"
#include "recomp/pacing.h"
#include "recomp/hle.h"
#include "recomp/interp.h"
#include "recomp/module_loader.h"
#include "recomp/precache.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"
#include "recomp/skip.h"
#include "recomp/touch.h"
#include "recomp/widescreen.h"
#ifndef LM_STATIC_MODULE
#include "setup/setup.h"
#endif
#ifdef __ANDROID__
#include "port/android.h"
#endif

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <chrono>

namespace recomp {
u32 copyDispCount();  // hle_gx.cpp
#ifdef __APPLE__
void hideWindowMacos(SDL_Window* w);  // macos_window.cpp
#endif
u32 viRetraceCount();  // hle_vi.cpp
#ifdef LM_STATIC_MODULE
extern const GameModule kGameModule;  // generated func_table.cpp, linked in (lmrecomp)
#endif
namespace vi {
void startTimer();
void stopTimer();
}  // namespace vi

namespace {

void loadDol(const u8* dol) {
    auto be = [&](u32 off) { return u32(dol[off]) << 24 | u32(dol[off + 1]) << 16 | u32(dol[off + 2]) << 8 | dol[off + 3]; };
    // .bss overlaps the small-data sections, so clear it before copying them in.
    std::memset(hostp(be(0xD8)), 0, be(0xDC));
    for (int i = 0; i < 18; i++) {
        const u32 off = be(i * 4), addr = be(0x48 + i * 4), size = be(0x90 + i * 4);
        if (size) std::memcpy(hostp(addr), dol + off, size);
    }
}

// Reads the `lis rD, hi; ori rD, rD, lo` pairs of __init_registers (r1, r2, r13).
void initRegisters(Ctx& c) {
    const u32 start = guestSymbol("__init_registers");
    for (u32 pc = start; pc < start + 0x18; pc += 8) {
        const u32 hi = rd32(pc), lo = rd32(pc + 4);
        const u32 reg = (hi >> 21) & 31;
        c.r[reg] = (hi << 16) | (lo & 0xFFFF);
    }
}

void boot() {
    Ctx init;
    initRegisters(init);
    const u32 stackTop = init.r[1];
    init.r[1] = stackTop - 8;
    wr32(init.r[1], 0);

    const u32 defaultThread = hleAlloc(0x310);
    GuestThread* t = sched::adoptBootThread(defaultThread, init);
    wr32(defaultThread + 0x304, stackTop);
    wr32(defaultThread + 0x308, stackTop - 0x10000);
    const u32 irqStack = hleAlloc(0x10000);
    sched::setIrqContext(init, irqStack + 0x10000 - 8);

    Ctx& c = sched::ctxOf(t);
    guestCall(c, guestSymbol("OSInit"));
    vi::startTimer();
    guestCall(c, guestSymbol("__init_user"));
    log("[boot] entering main");
    guestCall(c, guestSymbol("main"), 0, 0);
    log("[boot] main returned %d", s32(c.r[3]));
}

bool sNoGpuAdapter = false;  // Aurora found no usable graphics adapter (no Vulkan/D3D driver)

void logCallback(AuroraLogLevel level, const char* module, const char* message, unsigned int) {
    static const char* names[] = {"DEBUG", "INFO", "WARNING", "ERROR", "FATAL"};
    std::fprintf(level >= LOG_ERROR ? stderr : stdout, "[%s: %s] %s\n", names[level], module, message);
    std::fflush(stdout);
    if (port::recovery::runLog()) {
        const std::string line = std::string("[") + names[level] + ": " + module + "] " + message;
        port::recovery::logLine(line.c_str(), line.size());
    }
    if (level == LOG_FATAL) port::recovery::noteFatal(message);
    if (level == LOG_ERROR && std::strstr(message, "Failed to create adapter")) sNoGpuAdapter = true;
    if (level == LOG_FATAL) {
        std::fflush(stderr);
        // Without any usable GPU Aurora falls back to its null device and stops on the first
        // allocation; tell the player what is missing instead of only crashing.
        std::string text = std::string("The game stopped with an error:\n\n") + message +
                           "\n\nThe log of this run (last_run.log) is in the folder of settings.ini; please "
                           "attach it to a bug report. If the game no longer starts, the next start offers "
                           "to reset the settings or rebuild the game data.";
        if (sNoGpuAdapter) {
#ifdef _WIN32
            text = "No usable graphics adapter was found (Direct3D 12, Direct3D 11 or Vulkan).\n\n"
                   "Update your graphics driver.";
#else
            text = "No usable Vulkan graphics driver was found.\n\n"
                   "Install the Vulkan driver for your GPU (AMD/Intel: Mesa's Vulkan drivers, e.g. the "
                   "mesa-vulkan-drivers package; NVIDIA: the proprietary driver) and check it with "
                   "'vulkaninfo --summary'. See README.txt.";
#endif
        }
        if (!std::getenv("LM_BACKGROUND"))  // automated runs must not block on a dialog
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Luigi's Mansion Recompiled", text.c_str(), nullptr);
        std::abort();
    }
}

#ifdef LM_STATIC_MODULE
std::string findDisc(const char* gameId) {
    if (!port::options().disc.empty()) return port::options().disc;
    for (auto dir = std::filesystem::current_path();; dir = dir.parent_path()) {
        const auto candidate = dir / "orig" / (std::string(gameId) + ".iso");
        if (std::filesystem::exists(candidate)) return candidate.string();
        if (dir == dir.parent_path()) return {};
    }
}
#else
bool hasArg(int argc, char** argv, const char* name) {
    for (int i = 1; i < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return true;
    return false;
}

std::string argValue(int argc, char** argv, const char* name) {
    for (int i = 1; i + 1 < argc; i++)
        if (std::strcmp(argv[i], name) == 0) return argv[i + 1];
    return {};
}
#endif

// Hang reports (port/crash_handler.h): whether the game itself kept running and what Aurora's
// threads are doing, so a log tells a stopped game from a stopped display (issue #11: on a Steam
// Deck the game kept running at the file select while nothing was presented any more).
std::atomic<u32> sFramesAtStall{0};
std::atomic<bool> sStallOpen{false};
void hangDetail(char* out, size_t size, bool stallBegins) {
    const u32 frames = frame::guestFrames();
    if (stallBegins) {
        sFramesAtStall.store(frames, std::memory_order_relaxed);
        sStallOpen.store(true, std::memory_order_relaxed);
        return;
    }
    char act[400] = "";
    aurora_describe_activity(act, sizeof(act), 1000);
    char since[64] = "";
    if (sStallOpen.load(std::memory_order_relaxed))
        std::snprintf(since, sizeof(since), ", %u since the main thread stopped", frames - sFramesAtStall.load(std::memory_order_relaxed));
    std::snprintf(out, size, "game frames %u%s (%s)%s%s", frames, since,
                  frame::interpolating() ? "interpolated presentation" : "lockstep", act[0] ? "; aurora: " : "", act);
}

}  // namespace
}  // namespace recomp

int main(int argc, char* argv[]) {
    using namespace recomp;
#ifdef __ANDROID__
    port::android::redirectOutput();  // logcat + last_run.log (docs/ANDROID.md)
    port::android::startPerfLog();    // "[perf]" CPU / thermal lines
#endif
    // Windows: no console window for players; output to a terminal, a redirect or last_run.log.
    port::recovery::setupConsole(argc, argv);
    port::installCrashHandler();
    port::setHangDetail(&hangDetail);
    port::startHangWatchdog();
    port::parseOptions(argc, argv);
    port::setLaunchArgs(argc, argv);
    std::printf("Luigi's Mansion Recompiled %s\n", port::update::buildVersion());
    std::fflush(stdout);
    // A game folder too deep for the graphics start-up (docs/SETUP.md "Long paths"): say so
    // instead of stopping without a word.
    if (const std::string p = port::installPathProblem(); !p.empty()) {
        std::fprintf(stderr, "%s\n", p.c_str());
        if (!std::getenv("LM_BACKGROUND"))
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Luigi's Mansion Recompiled", p.c_str(), nullptr);
        return 1;
    }
    // Portable or system folders (docs/SETUP.md "Portable mode"), before any file is opened: a
    // data move the F1 menu asked for runs here.
    port::dataLocation();
    // Windows: last_run.log (recovery.h), in the folder dataLocation just chose (portable or system).
    port::recovery::openRunLog();

    AuroraConfig config{};
    config.appName = "Luigi's Mansion Recompiled";
    config.logCallback = &logCallback;
    port::loadSettings();
    // Start-up recovery (docs/SETUP.md "When the game does not start"): after a start that failed,
    // offers to reset the settings, rebuild the game data or put the saves aside.
#ifdef LM_STATIC_MODULE
    port::recovery::onStart(false);
#else
    port::recovery::onStart(true);
#endif
    // Updates (docs/SETUP.md "Updates"): remove what a finished update left, then look for a newer
    // release in the background (never blocks; the F1 menu shows the result).
    port::update::cleanupAfterUpdate();
    port::update::startupCheck();
    // A data folder too deep for the files Aurora writes with plain paths (docs/SETUP.md "Long paths").
    if (const std::string p = port::dataPathProblem(); !p.empty()) {
        std::fprintf(stderr, "%s\n", p.c_str());
        if (!std::getenv("LM_BACKGROUND"))  // automated runs must not block on a dialog
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_WARNING, "Luigi's Mansion Recompiled", p.c_str(), nullptr);
    }
    config.msaa = u32(port::settings().video.msaa);
    config.vsync = port::settings().video.vsync;
    // Under Gamescope (Steam Deck game mode) the swap chain always waits for the refresh (FIFO).
    // Gamescope composites every frame, so VSync off (Mailbox there) never tore and only lowered
    // latency, and interpolated presentation with Mailbox stopped showing frames on a Steam Deck
    // while the game ran on (issue #11). LM_GAMESCOPE_VSYNC=0 keeps the setting (test aid).
    if (!config.vsync && pacing::gamescope()) {
        const char* keep = std::getenv("LM_GAMESCOPE_VSYNC");
        if (!(keep && *keep == '0')) {
            config.vsync = true;
            std::printf("[video] Gamescope: VSync on (FIFO presentation; the VSync setting is ignored under Gamescope)\n");
        }
    }
    // LM_BACKEND=d3d12|d3d11|vulkan (testing): graphics backend instead of Aurora's choice.
    if (const char* b = std::getenv("LM_BACKEND")) {
        const std::string s = b;
        config.desiredBackend = s == "d3d12"    ? BACKEND_D3D12
                                : s == "d3d11"  ? BACKEND_D3D11
                                : s == "vulkan" ? BACKEND_VULKAN
                                : s == "opengl" ? BACKEND_OPENGL
                                : s == "gles"   ? BACKEND_OPENGLES
                                                : BACKEND_AUTO;
    }
    // LM_ALLOW_CPU_GPU=1: accept a software renderer (e.g. Mesa llvmpipe for Vulkan in WSL or a
    // VM without GPU passthrough; docs/LINUX.md). Slow, for testing only.
    if (const char* c = std::getenv("LM_ALLOW_CPU_GPU"); c && *c == '1') config.allowCpuAdapter = true;
    // LM_CACHE_DIR: separate shader/pipeline cache, e.g. for test runs of different builds side by side.
    // Default: next to settings.ini, so a scratch APPDATA (test runs) also gets its own cache
    // (Aurora's default, SDL_GetPrefPath, ignores the APPDATA variable).
    // Linux: $XDG_CACHE_HOME/Luigi's Mansion Recompiled (port::cacheDir; docs/LINUX.md).
    // Aurora takes UTF-8 paths (path::string() is the ANSI code page on Windows: a user name such
    // as "José" made Aurora's path conversion throw at startup).
    const auto utf8 = [](const std::filesystem::path& p) {
        const std::u8string s = p.u8string();
        return std::string(reinterpret_cast<const char*>(s.data()), s.size());
    };
    static const std::string sCacheDir = utf8(port::cacheDir());
    config.cachePath = sCacheDir.c_str();
    // Memory cards and imgui.ini (Aurora's userPath) in dataDir, which honours XDG_DATA_HOME /
    // APPDATA like the rest. On Windows this is the same folder SDL_GetPrefPath gives for players,
    // but SDL ignores the APPDATA variable, so test runs with a scratch APPDATA used to write
    // imgui.ini into the real install.
    // LM_CARD_DIR (test runs) is the user path too: Aurora's CARDInit builds the card paths from
    // userPath whenever one is set, so CARDSetBasePath alone no longer moved the cards.
    static const std::string sUserDir =
        std::getenv("LM_CARD_DIR") ? utf8(port::envPath("LM_CARD_DIR")) + "/" : utf8(port::dataDir()) + "/";
    config.userPath = sUserDir.c_str();
    static const std::string sCacheOverride = std::getenv("LM_CACHE_DIR") ? utf8(port::envPath("LM_CACHE_DIR")) : "";
    if (!sCacheOverride.empty()) config.cachePath = sCacheOverride.c_str();
    const port::Options& opts = port::options();
    config.startFullscreen = opts.fullscreen || port::settings().video.fullscreen;
#ifdef __ANDROID__
    config.startFullscreen = true;  // immersive: no status or navigation bar over the game
#endif
    config.windowWidth = opts.windowWidth;
    config.windowHeight = opts.windowHeight;
    if (!config.windowWidth && opts.aspectRatio > 0.0f) {  // fixed aspect: open a window of that shape
        config.windowHeight = 960;
        config.windowWidth = u32(960.0f * opts.aspectRatio + 0.5f);
    }
    precache::configure();
    port::setPhase("graphics initialization (device, render targets, anti-aliasing)");
    // LM_TOPMOST=1 (testing: screen-capture judder runs, tools/interp/screen_run.py): the window
    // stays on top and a fullscreen window is not minimized when another window takes the focus.
    const bool topmost = std::getenv("LM_TOPMOST") != nullptr;
    if (topmost) SDL_SetHint(SDL_HINT_VIDEO_MINIMIZE_ON_FOCUS_LOSS, "0");
    cpu::init();  // thread policy + Aurora thread hooks (issue #18), before Aurora starts its threads
#ifdef __ANDROID__
    // The Back button opens/closes the F1 menu (menu.cpp) instead of closing the app.
    SDL_SetHint(SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1");
#endif
    aurora_initialize(argc, argv, &config);
    mem::trackInit();  // LM_MEM_TRACK (test aid, docs/RENDERING.md "Memory use over time")
    if (topmost)
        if (SDL_Window* w = aurora_get_window()) SDL_SetWindowAlwaysOnTop(w, true);
    // LM_OFFSCREEN=1 (testing: automated runs on a machine someone is using): the window is moved far
    // outside every display, so it renders and presents as usual but is never seen (a hidden window
    // pauses the game; Start-Process -WindowStyle Hidden does not hide SDL's window).
    if (std::getenv("LM_OFFSCREEN"))
        if (SDL_Window* w = aurora_get_window()) {
#ifdef __APPLE__
            // macOS keeps windows on a display whatever position is asked for: fully transparent
            // and click-through instead (it still renders and presents; docs/MACOS.md).
            hideWindowMacos(w);
#else
            SDL_SetWindowPosition(w, -32000, -32000);
#endif
        }
    // LM_MAXIMIZE=1 (testing): open the window maximized, as players often run it.
    if (std::getenv("LM_MAXIMIZE"))
        if (SDL_Window* w = aurora_get_window()) SDL_MaximizeWindow(w);
    precache::init();
    ws::init();
    port::setPhase("loading the game");
    gx::installFifoErrorReport();

#ifdef LM_STATIC_MODULE
    // lmrecomp (developer build): the game was recompiled at build time and linked in.
    setGameModule(&kGameModule);
    const char* wantId = kGameModule.gameId;
    const std::string disc = findDisc(wantId);
    {
        auto& inst = port::install();
        inst.staticBuild = true;
        inst.running = std::strcmp(wantId, "GLMP01") == 0 ? port::GameVersion::PAL : port::GameVersion::USA;
        inst.usReady = inst.running == port::GameVersion::USA;
        inst.palReady = inst.running == port::GameVersion::PAL;
        inst.hmOverlayReady = true;  // dev: orig/hidden_mansion (tools/hidden_mansion_data.py), both directions
    }
#else
    // LuigisMansionRecomp: set up from the user's discs (first run, or after changes).
    setup::Options so;
    so.forceUi = hasArg(argc, argv, "--setup");
    so.autoStart = hasArg(argc, argv, "--setup-auto");
    so.usDisc = argValue(argc, argv, "--us-disc");
    so.palDisc = argValue(argc, argv, "--pal-disc");
    setup::LaunchInfo launch;
    if (!setup::run(so, launch)) {
        port::recovery::onExit();
        aurora_shutdown();
        std::_Exit(0);
    }
    const std::string disc = launch.disc;
    const char* wantId = port::gameId(launch.version);
#endif
    port::setPhase("loading the game");
    const std::string discOpen = port::discOpenPath(disc);  // Android: content:// URI -> descriptor
    if (discOpen.empty() || !aurora_dvd_open(discOpen.c_str())) {
        std::fprintf(stderr, "Could not open a Luigi's Mansion disc image. Pass --disc <path to .iso/.rvz>.\n");
#ifdef __ANDROID__  // no console to read: say it on screen
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Luigi's Mansion",
                                 "The disc image could not be opened (was it moved or deleted?). "
                                 "Start the app again and choose it on the setup screen.",
                                 nullptr);
        port::relaunch(true);
#endif
        aurora_shutdown();
        return 1;
    }
    const port::dol::Version discVersion = port::dol::load();
    const char* discId = discVersion == port::dol::Version::GLME01   ? "GLME01"
                         : discVersion == port::dol::Version::GLMP01 ? "GLMP01"
                                                                     : "unknown";
    if (std::strcmp(discId, wantId) != 0) {
        std::fprintf(stderr, "The game was recompiled from Luigi's Mansion %s; the disc is %s.\n", wantId, discId);
        aurora_shutdown();
        return 1;
    }
    s32 dolSize = 0;
    const u8* dol = DVDGetDOLLocation(&dolSize);

    menu::init();
    g_mem = allocGuestMemory();
    loadDol(dol);
    if (port::install().running == port::GameVersion::PAL) {
        // The PAL release's language (.sdata 0x804BD658: 1 English, 2 German, 3 French,
        // 4 Spanish, 5 Italian; docs/SETUP.md). The game reads it when it loads text.
        const int lang = port::settings().enhancements.palLanguage;
        wr32(0x804BD658, u32(lang >= 1 && lang <= 5 ? lang : 1));
    }
    initTimeBase();
    initTrace();
    initWatch();
#ifndef LM_STATIC_MODULE
    if (std::string err; !loadGameModule(launch.module, err)) {
        std::fprintf(stderr, "%s\n", err.c_str());
#ifdef __ANDROID__  // no console to read: say it on screen
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Luigi's Mansion",
                                 ("The game module could not be loaded:\n" + err).c_str(), nullptr);
#endif
        aurora_shutdown();
        return 1;
    }
#endif
    std::printf("Opened %s (%s)\n", disc.c_str(), discId);

    frame::start(&boot);
#ifdef __ANDROID__
    // Diagnostics for shared logs (docs/ANDROID.md): when the game itself runs below 10 frames
    // a second for 10 s (not paused), dump where every guest thread waits, a few times per run.
    std::thread([] {
        u32 last = skip::guestFrames();
        int dumps = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            const u32 now = skip::guestFrames();
            const u32 frames = now - last;
            last = now;
            if (frames > 0 && frames < 100 && dumps < 4) {
                dumps++;
                log("[slow] %u game frames in 10 s, retrace %u: guest threads", frames, viRetraceCount());
                sched::dumpThreads();
            }
        }
    }).detach();
#endif
    if (std::getenv("LM_WATCHDOG")) {
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(3));
                sched::dumpThreads();
                if (const char* peek = std::getenv("LM_PEEK")) {  // LM_PEEK=addr,addr,...: guest words
                    std::string line = "[peek]";
                    for (const char* p = peek; *p;) {
                        char* end = nullptr;
                        const u32 a = u32(std::strtoul(p, &end, 16));
                        char buf[32];
                        std::snprintf(buf, sizeof(buf), " %08X=%08X", a, rd32(a));
                        line += buf;
                        p = (*end == ',') ? end + 1 : end;
                        if (end == p && *p) break;
                    }
                    log("%s", line.c_str());
                }
            }
        }).detach();
    }

    bool exiting = false;
    bool paused = false;
    // Test aid LM_QUIT_AFTER=<seconds>: a normal quit after that long (the shader and pipeline
    // caches are written as on a user's exit; a killed test run loses what it compiled).
    const double quitAfter = [] {
        const char* v = std::getenv("LM_QUIT_AFTER");
        return v ? std::atof(v) : 0.0;
    }();
    const auto runStart = std::chrono::steady_clock::now();
    port::setPhase("game loop");
    // Stalls the watchdog cannot see because this loop keeps running (port::reportStall): no frame
    // presented (aurora_begin_frame failing), or with interpolation the game thread no longer
    // finishing frames while the display replays its last one.
    using SteadyClock = std::chrono::steady_clock;
    auto lastPresented = SteadyClock::now();
    auto lastGuestProgress = SteadyClock::now();
    auto lastIteration = SteadyClock::now();
    u32 lastGuestFrames = frame::guestFrames();
    bool presentStallReported = false, guestStallReported = false;
    while (!exiting) {
        port::heartbeat();
        sStallOpen.store(false, std::memory_order_relaxed);
        if (!paused) port::setPhase("game loop: window events (aurora_update)");  // (paused: nullptr, a wait on purpose)
        if (quitAfter > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - runStart).count() > quitAfter)
            break;
        for (const AuroraEvent* ev = aurora_update(); ev && ev->type != AURORA_NONE; ++ev) {
            if (ev->type == AURORA_SDL_EVENT && !touch::handleEvent(ev->sdl)) {
                touch::feedImGui(ev->sdl);  // taps on the F1 menu (Android; touch.h)
                menu::handleEvent(ev->sdl);
            }
            if (ev->type == AURORA_EXIT) exiting = true;
            else if (ev->type == AURORA_PAUSED) {
                paused = true;
                port::setPhase(nullptr);  // waits for the window to come back: not a hang
                audio::outSetPaused(true);
            } else if (ev->type == AURORA_UNPAUSED) {
                paused = false;
                port::setPhase("game loop");
                audio::outSetPaused(false);
            }
        }
        // Shaders first (precache.h holdGame): at startup the game does not start, and after a
        // settings change that needs other shader variants it waits, until they are compiled.
        const bool hold = !exiting && precache::holdGame();
        static bool sHoldAudio = false;
        if (hold != sHoldAudio) {
            sHoldAudio = hold;
            if (!paused) audio::outSetPaused(hold);
        }
        // Test aid LM_FREEZE_AT=<frame>: from that game frame (copyDispCount) on, the guest is held
        // as for a focus loss while the host keeps presenting its last tape with the post-process
        // chain (A/B screenshots of one game frame, e.g. CRT signals with LM_CRT_CYCLE). Needs
        // interpolated presentation (frame rate above 30).
        static const u32 sFreezeAt = [] {
            const char* v = std::getenv("LM_FREEZE_AT");
            return v ? u32(std::strtoul(v, nullptr, 10)) : 0u;
        }();
        const bool frozen = sFreezeAt && copyDispCount() >= sFreezeAt;
        frame::setHostPaused(paused || hold || frozen);
        {
            const auto now = SteadyClock::now();
            // (A gap between iterations is a stall of this thread itself, which the watchdog reports.)
            if (paused || now - lastIteration > std::chrono::seconds(1)) {
                lastPresented = now;
                lastGuestProgress = now;
            }
            lastIteration = now;
            if (const u32 g = frame::guestFrames(); g != lastGuestFrames || paused || hold || frozen || !frame::interpolating()) {
                lastGuestFrames = g;
                lastGuestProgress = now;
                if (guestStallReported) log("[hang] the game finishes frames again");
                guestStallReported = false;
            } else if (!guestStallReported && now - lastGuestProgress > std::chrono::seconds(15)) {
                guestStallReported = true;
                port::reportStall("the game has not finished a frame for 15 s while the display loop runs (interpolated "
                                  "presentation replays its last frame)",
                                  true);
                sched::dumpThreads();
            }
            if (!presentStallReported && now - lastPresented > std::chrono::seconds(10)) {
                presentStallReported = true;
                port::reportStall("the main loop runs but no frame could be begun for 10 s (aurora_begin_frame fails)", true);
            }
        }
        if (exiting || paused) continue;
        if (hold) {
            port::setPhase("game loop: shader preparation frame");
            const auto beginAt = SteadyClock::now();
            if (!aurora_begin_frame()) continue;
            const double beginMs = std::chrono::duration<double, std::milli>(SteadyClock::now() - beginAt).count();
            lastPresented = SteadyClock::now();
            presentStallReported = false;
            menu::draw();  // applies settings changes (they may change what is being compiled)
            touch::draw();
            precache::drawHold();
            precache::frame();
            aurora_end_frame();
            // Hold frames well below the display's refresh rate (issue #33): a swap chain image is
            // then always free, so the render worker never waits for one inside Dawn, where the wait
            // holds the device lock every pipeline creation needs.
            precache::holdFramePause(beginMs);
            if (menu::quitRequested()) break;
            continue;
        }
        // (Phases name the step in a hang report: issue #11.)
        port::setPhase("game loop: frame pacing wait");
        pacing::beforeFrame();
        interp::hostMark(0);
        port::setPhase("game loop: aurora_begin_frame (frame slot, staging buffer)");
        if (!aurora_begin_frame()) continue;
        lastPresented = SteadyClock::now();
        if (presentStallReported) log("[hang] frames are presented again");
        presentStallReported = false;
        pacing::afterBegin();
        interp::hostMark(1);
        ws::beginFrame();
        port::setPhase(frame::interpolating() ? "game loop: replaying a game frame" : "game loop: running the game (lockstep)");
        const bool running = frame::hostFrame();
        pacing::afterReplay();
        interp::hostMark(2);
        port::setPhase("game loop: F1 menu / overlays");
        menu::draw();
        touch::draw();  // Android on-screen pad (touch.h)
        precache::frame();
        port::setPhase("game loop: present hold");
        pacing::beforeEnd();
        port::setPhase("game loop: aurora_end_frame (GX FIFO drain, hand-off to the render worker)");
        aurora_end_frame();
        port::setPhase("game loop");
        pacing::afterFrame();
        interp::hostMark(3);
        frame::pace();
        interp::hostMark(4);
        if (!running || menu::quitRequested()) break;
    }

    port::setPhase("shutdown (releasing graphics objects, writing caches)");
    menu::shutdown();
    vi::stopTimer();
    frame::stop();
    audio::outShutdown();
    aurora_shutdown();
    port::recovery::onExit();
    std::_Exit(0);
}
