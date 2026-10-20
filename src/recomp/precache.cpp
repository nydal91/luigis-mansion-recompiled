// Shader/pipeline precaching: see precache.h and docs/RENDERING.md ("Pipeline precache").
#include "recomp/precache.h"

#include "port/install.h"
#include "recomp/enhanced_lighting.h"
#include "recomp/gx_bridge.h"
#include "recomp/hle.h"
#include "recomp/ui_scale.h"

#include <SDL3/SDL_video.h>
#include <aurora/aurora.h>
#include <aurora/pipeline_cache.h>
#include <imgui.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif
#ifdef __ANDROID__
#include "port/android.h"
#endif

namespace recomp::precache {
namespace {

using Clock = std::chrono::steady_clock;

bool sLog = false;

std::filesystem::path exeDir() {
#ifdef __ANDROID__
    return port::android::resourceDir();  // APK assets copied out by LMActivity (docs/ANDROID.md)
#elif defined(_WIN32)
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
#else
    return port::selfExeDir();
#endif
}

}  // namespace

void configure() {
    // LM_PIPELINE_THREADS=<n>: pipeline compile threads (default: half the hardware threads, 1..8).
    if (const char* t = std::getenv("LM_PIPELINE_THREADS")) aurora_pipeline_cache_set_threads(uint32_t(std::atoi(t)));
    // LM_CRASH_AT=<seconds> (test aid, crash journal): abort() that long after startup, a real crash
    // as far as the crash handler and the pipeline crash journal can tell (Aurora 0231).
    if (const char* c = std::getenv("LM_CRASH_AT")) {
        const double seconds = std::atof(c);
        std::thread([seconds] {
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
            std::abort();
        }).detach();
    }
}

void init() {
    // LM_PIPELINE_LOG=1: every pipeline creation (precache/demand, time), every draw that had to
    // wait for its pipeline, and a summary line every 5 s with the worst frame time.
    sLog = std::getenv("LM_PIPELINE_LOG") != nullptr;
    aurora_pipeline_cache_set_log(sLog);
    // LM_PIPELINE_WAIT=0 (testing): skip draws whose pipeline is not ready, as Aurora did before
    // patch 0013 (reproduces the first-run flicker).
    if (const char* w = std::getenv("LM_PIPELINE_WAIT"); w && std::atoi(w) == 0) aurora_pipeline_cache_set_wait(false);
    // The GX accuracy flags select shader variants; set them before the first frame builds the
    // known pipelines, or the first build is for the default flags and wasted.
    recomp::gx::applyAccuracySettings();
    // Likewise the enhanced-lighting parts that select shader variants (Aurora 0217): otherwise the
    // first build is for enhanced lighting off and the second frame queues its variants again.
    recomp::enh::applySettings();
    // LM_PRECACHE=0: no shipped list (cold-cache testing). LM_PIPELINE_LIST=<file>: another list.
    if (const char* p = std::getenv("LM_PRECACHE"); p && std::atoi(p) == 0) return;
    std::string list;
    if (const char* p = std::getenv("LM_PIPELINE_LIST")) list = p;
    else {
        // Aurora takes UTF-8 paths; past MAX_PATH the extended form (docs/SETUP.md "Long paths").
        std::filesystem::path file = port::plainPath(exeDir() / "pipelines.bin");
        if (file.native().size() > port::kMaxPlainPath) file = port::longPath(file);
        const std::u8string u8 = file.u8string();
        list.assign(reinterpret_cast<const char*>(u8.data()), u8.size());
    }
    const uint32_t n = aurora_pipeline_cache_seed(list.c_str());
    if (sLog) log("[precache] %u configs from %s", n, list.c_str());
}

namespace {
// Startup: the known set is (re)built on the first frames (Aurora begin_pipeline_frame) once the
// settings are applied (menu::draw on the first hold frame sets the enhanced-lighting parts, which
// can queue a second batch on the next frame), so the hold lasts at least kMinHoldFrames frames.
constexpr int kMinHoldFrames = 3;
// Test aid LM_PRECACHE_WARM=1 (docs/RENDERING.md "Warm starts"; off by default: the game starts
// only once every shader is compiled or loaded): once this many pipelines of a batch were built
// without a single shader cache miss, the rest of the batch loads in the background while the
// game runs (a draw that needs one first waits for it, Aurora 0013).
constexpr uint32_t kWarmSample = 256;
// A batch continued in the background is held again once this many of its lookups missed the
// cache (it was only partly cached: compiling in play would stutter).
constexpr uint32_t kRecompileHold = 8;
bool sStartupDone = false;
int sHoldFrames = 0;
Clock::time_point sHoldStart{};
Clock::time_point sStartupBegin = Clock::now();
bool sHolding = false;
uint32_t sHoldMax = 0;  // most pending pipelines seen in this hold (the bar's total)
bool sSettingsHold = false;  // the current hold is for a settings change (not the startup one)

// The batch of pipelines the last rebuild of the known set queued (Aurora stat rebuildCount).
struct Batch {
    uint32_t rebuild = UINT32_MAX;
    uint32_t doneAtStart = 0;
    uint32_t missesAtStart = 0;
    uint32_t storesAtStart = 0;
    uint32_t hitsAtStart = 0;
    Clock::time_point start{};
    bool background = false;  // warm: the rest loads while the game runs
    bool reheld = false;      // it was continued in the background, then turned out to need compiling
    bool startup = false;     // queued before the game started (the startup rebuild)
    bool reported = false;
    float worstMs = 0.0f;  // worst frame while loading in the background (game running)
    uint32_t frames = 0;
};
Batch sBatch;

void trackBatch(const AuroraPipelineStats& s) {
    if (s.rebuildCount == sBatch.rebuild) return;
    sBatch = Batch{};
    sBatch.rebuild = s.rebuildCount;
    sBatch.doneAtStart = s.precacheDone;
    sBatch.missesAtStart = s.blobMisses;
    sBatch.storesAtStart = s.blobStores;
    sBatch.hitsAtStart = s.blobHits;
    sBatch.start = Clock::now();
    sBatch.startup = !sStartupDone;
}
uint32_t batchMisses(const AuroraPipelineStats& s) { return s.blobMisses - sBatch.missesAtStart; }
bool batchCompiling(const AuroraPipelineStats& s) { return batchMisses(s) > 0 || s.blobCacheBroken; }

void logCacheStats(const char* when, const AuroraPipelineStats& s) {
    log("[precache] shader cache %s: %u loaded, %u missed, %u compiled and stored%s (file: %u entries at start, "
        "launch %u); thread time: WGSL %.0f ms, modules %.0f ms (%u reused), pipelines %.0f ms (cache reads %.0f "
        "ms, writes %.0f ms)",
        when, s.blobHits, s.blobMisses, s.blobStores, s.blobCacheBroken ? " (cache file unusable: see above)" : "",
        s.blobEntriesAtOpen, s.blobSession, s.wgslNs / 1e6, s.moduleNs / 1e6, s.modulesReused, s.pipelineNs / 1e6,
        s.blobLoadNs / 1e6, s.blobStoreNs / 1e6);
    if (s.blobEntriesAtOpen > 0 && s.blobMisses > 2 * s.blobHits)
        log("[precache] most of the %u cached shaders did not match: another graphics card or driver version wrote the "
            "cache",
            s.blobEntriesAtOpen);
}
}  // namespace

bool holdGame() {
    static const bool sNoWait = [] {
        const char* v = std::getenv("LM_PRECACHE_WAIT");
        return v && std::atoi(v) == 0;
    }();
    // LM_PRECACHE_WARM=1 (test aid): start the game after a warm sample, the rest in the background.
    static const bool sNoWarm = [] {
        const char* v = std::getenv("LM_PRECACHE_WARM");
        return !(v && std::atoi(v) == 1);
    }();
    // LM_PIPELINE_BOOST=0 (A/B): no extra creation threads while held for a batch from the cache.
    static const bool sNoBoost = [] {
        const char* v = std::getenv("LM_PIPELINE_BOOST");
        return v && std::atoi(v) == 0;
    }();
    AuroraPipelineStats s{};
    aurora_pipeline_cache_get_stats(&s);
    trackBatch(s);
    if (s.rebuildPending > 0 && !sNoWarm && !sBatch.background && !sBatch.reheld && batchMisses(s) == 0 &&
        !s.blobCacheBroken && !s.prewarmPending && s.precacheDone - sBatch.doneAtStart >= kWarmSample) {
        sBatch.background = true;
        log("[precache] shader cache warm (%u pipelines loaded, none compiled): the other %u load in the background",
            s.precacheDone - sBatch.doneAtStart, s.rebuildPending);
    }
    if (sBatch.background && s.rebuildPending > 0 && batchMisses(s) >= kRecompileHold) {
        sBatch.background = false;
        sBatch.reheld = true;
        log("[precache] shader cache incomplete (%u lookups missed): game held while the remaining %u pipelines "
            "are built",
            batchMisses(s), s.rebuildPending);
    }
    if (sBatch.background && s.rebuildPending == 0 && !sBatch.reported) {
        sBatch.reported = true;
        log("[precache] background loading done after %.2f s (game frames meanwhile: %u, worst %.1f ms)",
            std::chrono::duration<double>(Clock::now() - sBatch.start).count(), sBatch.frames, sBatch.worstMs);
        logCacheStats("after loading", s);
    }
    const bool shadersPending = s.rebuildPending > 0 && !sBatch.background;
    bool hold;
    if (!sStartupDone) {
        hold = sHoldFrames < kMinHoldFrames || shadersPending;
        if (sNoWait && sHoldFrames >= 1) hold = false;
        if (!hold) {
            sStartupDone = true;
            // LM_PIPELINE_EXPORT=<file> (list maintenance, tools/pipelines): write the known set
            // (shipped list + this cache, canonical form) once loaded.
            // (with LM_PRECACHE_WAIT=0 the export is written on the first frame, before compiling;
            // LM_PIPELINE_EXPORT_EXIT=1 then ends the program).
            if (const char* path = std::getenv("LM_PIPELINE_EXPORT")) {
                aurora_pipeline_cache_export(path);
                if (std::getenv("LM_PIPELINE_EXPORT_EXIT")) std::_Exit(0);
            }
            log("[precache] game starts after %.2f s of shader preparation (%.2f s after startup; %u pipelines "
                "built, %u loading in the background, %u threads)%s",
                std::chrono::duration<double>(Clock::now() - sHoldStart).count(),
                std::chrono::duration<double>(Clock::now() - sStartupBegin).count(), s.precacheDone,
                sBatch.background ? s.rebuildPending : 0u, s.workerThreads,
                sNoWait ? " (LM_PRECACHE_WAIT=0: not waited for)" : "");
            logCacheStats("at game start", s);
        }
    } else {
        hold = !sNoWait && shadersPending;
    }
    // Aurora 0233 (issue #14): a hold never waits silently forever. Without a single pipeline done for
    // 20 s, say so (and which creations are stuck in the driver), every 20 s; after kStallGiveUp s the
    // hold ends and the game starts (a draw that needs a pipeline still pending waits for it alone).
    // A pipeline whose creation fails does not stall anything: Aurora logs it, records the config as
    // bad and counts it done (draws using it are skipped).
    {
        static uint64_t sProgressKey = ~0ull;
        static Clock::time_point sProgressAt = Clock::now();
        static Clock::time_point sStallLog{};
        static bool sGaveUp = false;  // until this batch is done: the game runs, no more reports
        constexpr double kStallGiveUp = 90.0;
        const uint64_t key = (uint64_t(s.precacheDone) << 32) ^ s.rebuildPending ^ (uint64_t(s.blobHits) << 16);
        const auto now = Clock::now();
        const double stalled = std::chrono::duration<double>(now - sProgressAt).count();
        if (!hold) {
            sGaveUp = false;
            sProgressKey = key;
            sProgressAt = now;
            sStallLog = {};
        } else if (sGaveUp) {
            hold = false;
        } else if (key != sProgressKey) {
            if (sStallLog != Clock::time_point{})
                log("[precache] pipelines are being built again after %.0f s (%u pending)", stalled, s.rebuildPending);
            sProgressKey = key;
            sProgressAt = now;
            sStallLog = {};
        } else if (stalled >= kStallGiveUp) {
            sGaveUp = true;
            log("[precache] gave up waiting after %.0f s without progress: the game continues, %u pipelines are "
                "still pending (a draw that needs one waits for it)",
                stalled, s.rebuildPending);
            hold = false;
            if (!sStartupDone) sStartupDone = true;
        } else if (stalled >= 20.0 && (sStallLog == Clock::time_point{} || now - sStallLog >= std::chrono::seconds(20))) {
            sStallLog = now;
            char stuck[512];
            const uint32_t nStuck = aurora_pipeline_cache_describe_stuck(10'000'000'000ull, stuck, sizeof(stuck));
            log("[precache] no pipeline finished for %.0f s: %u pending, %u threads; %s%s", stalled, s.rebuildPending,
                s.workerThreads, nStuck ? "creations running for over 10 s (graphics driver): " : "no creation running",
                nStuck ? stuck : " (the pipeline workers are not taking work)");
        }
    }
    if (hold && !sHolding) {
        sHoldStart = Clock::now();
        sHoldMax = 0;
        sSettingsHold = sStartupDone;
        if (sStartupDone) log("[precache] game held for %u pipelines", s.rebuildPending);
    } else if (!hold && sHolding && sSettingsHold) {
        log("[precache] resumed after %.2f s", std::chrono::duration<double>(Clock::now() - sHoldStart).count());
        logCacheStats("now", s);
    }
    // 0227: while the game is held for a batch that so far loads from the cache (CPU bound, nothing
    // else runs), all hardware threads but one (at most 16) create pipelines. A batch that compiles keeps the
    // default count (the shader compiler needs much memory per thread).
    const bool boost = hold && !sNoBoost && s.rebuildPending > 0 && batchMisses(s) == 0 && !s.blobCacheBroken;
    static bool sBoost = false;
    if (boost != sBoost) {
        sBoost = boost;
        aurora_pipeline_cache_set_boost(boost);
        if (sLog) log("[precache] creation threads: %s", boost ? "all but one, max 16 (held, loading from the cache)" : "default");
    }
    // 0231: what a hold is doing, every 5 s (a long one used to log nothing until the game started).
    static Clock::time_point sProgressLog{};
    if (hold && s.rebuildPending > 0 && Clock::now() - sProgressLog >= std::chrono::seconds(5)) {
        if (sProgressLog != Clock::time_point{} && sHolding)
            log("[precache] preparing shaders: %u pending, %.0f s so far; this batch: %u loaded from the cache, "
                "%u compiled (%u threads)",
                s.rebuildPending, std::chrono::duration<double>(Clock::now() - sHoldStart).count(),
                s.blobHits - sBatch.hitsAtStart, s.blobStores - sBatch.storesAtStart, s.workerThreads);
        sProgressLog = Clock::now();
    }
    sHolding = hold;
    sHoldMax = std::max(sHoldMax, s.rebuildPending);
    return hold;
}

void drawHold() {
    sHoldFrames++;
    AuroraPipelineStats s{};
    aurora_pipeline_cache_get_stats(&s);
    // A warm cache needs a fraction of a second here (the rest loads in the background): show
    // nothing for the first moment, then say what is really happening.
    const double t = std::chrono::duration<double>(Clock::now() - sHoldStart).count();
    if (t < 0.3) return;
    const bool compiling = batchCompiling(s);
    const uint32_t total = std::max(sHoldMax, 1u);
    const uint32_t done = total - std::min(s.rebuildPending, total);
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    // Menu size (ui_scale.cpp) x 1.5 with the large font (rasterized at that size, sharp): as large
    // as before at 1080p (it used to be a stretched 13 px font) and larger with the menu size.
    const float scale = recomp::ui::scale() * 1.5f;
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(460 * scale, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.85f);
    ImGui::Begin("##preparing", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove);
    ImFont* large = recomp::ui::largeFont();
    if (large) ImGui::PushFont(large);
    if (!compiling && (sHoldMax < 64 || t < 2.0)) {
        ImGui::TextUnformatted("Loading...");
    } else if (!compiling) {
        ImGui::Text("Loading shaders... %u / %u", done, total);
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1, 0), "");
    } else {
        ImGui::Text("Compiling shaders for your graphics card... %u / %u", done, total);
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1, 0), "");
        if (!sBatch.startup)
            ImGui::TextWrapped("New graphics settings: the game continues when this is done.");
        else if (s.cachesReset)
            ImGui::TextWrapped("The game crashed at startup several times in a row, so the shader cache was "
                               "cleared to rule it out. It is rebuilt once.");
        else if (s.prevExit != 0 && s.blobEntriesAtOpen > 0 && s.blobMisses <= 2 * s.blobHits)
            ImGui::TextWrapped("The last session ended before all shaders were built (closed, killed or crashed): "
                               "only the missing ones are compiled; the rest load from the cache.");
        else if (s.blobCacheBroken)
            ImGui::TextWrapped("The shader cache file cannot be used, so this happens at every start (details in the "
                               "log).");
        else if (s.blobEntriesAtOpen == 0)
            ImGui::TextWrapped("First start with this shader cache: this happens once. Later starts load the shaders "
                               "from the cache in a few seconds at most.");
        else if (s.blobMisses > 2 * s.blobHits)
            ImGui::TextWrapped("The shader cache is from another graphics card or driver version, so the shaders "
                               "are compiled again, once.");
        else
            ImGui::TextWrapped("Some shaders are new (new graphics settings or a game update) and are compiled "
                               "once; the rest load from the cache.");
    }
    if (large) ImGui::PopFont();
    ImGui::End();
}

void holdFramePause(double beginMs) {
    // LM_HOLD_FPS=<n> (test aid): hold frames at n per second; 0 = no wait at all (the display's
    // refresh rate through VSync: reproduces issue #33 on any display); "old" = the 8 ms sleep of
    // 1.1-1.1.4 (vsync-bound on a display under ~115 Hz).
    static const double sFixed = [] {
        const char* v = std::getenv("LM_HOLD_FPS");
        if (!v) return -1.0;                 // default: refresh-aware
        if (*v == 'o') return -2.0;          // old
        const double fps = std::atof(v);
        return fps > 0 ? 1.0 / fps : 0.0;
    }();
    constexpr double kMinPeriod = 0.050;  // 20 hold frames a second: enough for a progress bar
    static Clock::time_point sNext{};
    const auto now = Clock::now();
    double period = kMinPeriod;
    float hz = 0.0f;
    if (sFixed == -2.0) {
        sNext = now + std::chrono::milliseconds(8);
    } else {
        if (sFixed >= 0.0) {
            period = sFixed;
        } else if (SDL_Window* w = aurora_get_window()) {
            // Never faster than every second refresh: the swap chain always has a free image, so
            // acquiring one never waits for a vertical blank (a 24-30 Hz TV gets 12-15 frames).
            if (const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(w)); m && m->refresh_rate > 0.0f) {
                hz = m->refresh_rate;
                period = std::max(period, 2.0 / double(hz));
            }
        }
        // (A late frame does not make the next ones faster: no catching up.)
        const auto step = std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(period));
        sNext = (sNext == Clock::time_point{} || now - sNext > step) ? now + step : sNext + step;
    }
    // LM_PIPELINE_LOG: once a second, how long the hold frames waited in aurora_begin_frame (a few
    // milliseconds every frame = the frames run at the refresh rate and the swap chain makes them wait).
    if (sLog) {
        static Clock::time_point sWindow = now;
        static double sBegin = 0, sMax = 0;
        static int sFrames = 0;
        sBegin += beginMs, sMax = std::max(sMax, beginMs), sFrames++;
        if (now - sWindow >= std::chrono::seconds(1)) {
            log("[precache] hold frames: %d in %.1f s (every %.0f ms, display %.0f Hz), aurora_begin_frame %.2f ms avg, "
                "%.1f max",
                sFrames, std::chrono::duration<double>(now - sWindow).count(), period * 1e3, double(hz), sBegin / sFrames,
                sMax);
            sWindow = now;
            sBegin = sMax = 0;
            sFrames = 0;
        }
    }
    std::this_thread::sleep_until(sNext);
}

Progress progress() {
    AuroraPipelineStats s{};
    aurora_pipeline_cache_get_stats(&s);
    return Progress{s.precacheDone, s.precacheTotal};
}

void frame() {
    static Clock::time_point start = Clock::now(), last = start, window = start;
    static float worstMs = 0.0f;
    static uint32_t frames = 0;
    const auto now = Clock::now();
    const float frameMs = std::chrono::duration<float, std::milli>(now - last).count();
    worstMs = std::max(worstMs, frameMs);
    if (sBatch.background && !sBatch.reported && !sHolding) {
        sBatch.worstMs = std::max(sBatch.worstMs, frameMs);
        sBatch.frames++;
    }
    // (LM_PIPELINE_LOG: slow frames in the first seconds of the game, e.g. while loading in the background)
    static int sGameFrames = 0;
    if (sStartupDone && !sHolding && ++sGameFrames < 600 && sLog && frameMs > 34.0f)
        log("[precache] slow frame %d: %.1f ms%s", sGameFrames, frameMs,
            sBatch.background && !sBatch.reported ? " (loading in the background)" : "");
    last = now;
    frames++;

    AuroraPipelineStats s{};
    aurora_pipeline_cache_get_stats(&s);
    // The startup precompile time (fresh cache: the whole shipped list), logged once.
    static bool sReported = false;
    if (sLog && !sReported && s.precacheTotal > 0 && s.precacheDone >= s.precacheTotal) {
        sReported = true;
        log("[precache] all %u known pipelines ready after %.2f s (%u threads)", s.precacheTotal,
            std::chrono::duration<double>(now - start).count(), s.workerThreads);
    }
    if (sLog && now - window >= std::chrono::seconds(5)) {
        log("[precache] %.0fs: precache %u/%u, pending %u, demand-created %u, stalled draws %u (%.1f ms total, "
            "max %.1f ms), dropped draws %u, worst frame %.1f ms over %u frames, %u threads",
            std::chrono::duration<double>(now - start).count(), s.precacheDone, s.precacheTotal, s.pending,
            s.demandCreated, s.stalledDraws, s.stallNs / 1e6, s.maxStallNs / 1e6, s.droppedDraws, worstMs, frames,
            s.workerThreads);
        window = now;
        worstMs = 0.0f;
        frames = 0;
    }

    // Small corner note while shaders are compiled in the background (LM_PRECACHE_WAIT=0, or the
    // few misses a batch continued in the background may have; loading from the cache shows nothing).
    if (!sHolding && s.rebuildPending > 0 && batchCompiling(s) && now - start > std::chrono::seconds(4)) {
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 8, vp->WorkPos.y + vp->WorkSize.y - 8),
                                ImGuiCond_Always, ImVec2(1, 1));
        ImGui::SetNextWindowBgAlpha(0.4f);
        ImGui::Begin("##precache", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                         ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                         ImGuiWindowFlags_NoNav);
        ImGui::Text("Compiling shaders: %u left", s.rebuildPending);
        ImGui::End();
    }
}

}  // namespace recomp::precache
