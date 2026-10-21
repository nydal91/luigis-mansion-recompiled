// HLE: OS (threads, messages, mutexes, interrupts, caches, time, reset) and
// the small debug/serial units.
#include "port/install.h"
#include "port/settings.h"
#include "recomp/efb_copy.h"
#include "recomp/hle.h"
#include "recomp/runtime.h"
#include "recomp/sched.h"
#include "recomp/skip.h"

#include <cmath>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>

namespace recomp {
namespace {

// .ctors (null-terminated function pointer list): `_ctors` in decomp/config/*/symbols.txt.
constexpr u32 kCtorsStartUs = 0x8020BAC0;
constexpr u32 kCtorsStartPal = 0x80210AA0;
// FST max length from each disc's boot.bin (+0x42C), for the console's arena high (OSInit).
constexpr u32 kFstMaxUs = 0x5179;
constexpr u32 kFstMaxPal = 0x5563;

bool palGame() { return port::install().running == port::GameVersion::PAL; }

// Initial stack pointer from __init_registers (`lis r1, hi; ori r1, r1, lo`). The arena
// starts just above the stack, where the linker put __ArenaLo (US: 0x804ED940).
u32 initialStack() {
    const u32 f = guestSymbol("__init_registers");
    return (rd32(f) << 16) | (rd32(f + 4) & 0xFFFF);
}

u32 sProgressiveMode = 0;
u32 sSoundMode = 1;  // stereo

void setBool(Ctx& c, bool v) { c.r[3] = v ? 1 : 0; }

}  // namespace

// --- boot -------------------------------------------------------------------

HLE(__init_cpp) {
    for (u32 p = palGame() ? kCtorsStartPal : kCtorsStartUs; rd32(p) != 0; p += 4) guestCall(c, rd32(p));
}
HLE(__init_user) { hle___init_cpp(c); }
HLE(_ExitProcess) {
    log("[os] _ExitProcess");
    std::exit(0);
}

HLE(OSInit) {
    // Boot info in low memory, as left by the IPL/apploader.
    std::memcpy(hostp(0x80000000), port::gameId(port::install().running), 6);
    wr8(0x80000006, 0);  // disc number
    wr8(0x80000007, 0);  // version
    wr32(0x8000001C, 0xC2339F3D);  // DVD magic
    wr32(0x80000020, 0x0D15EA5E);
    wr32(0x80000024, 1);
    wr32(0x80000028, kMem1Size);
    wr32(0x8000002C, 3);  // retail console
    wr32(0x800000CC, palGame() ? 1 : 0);  // VI_NTSC / VI_PAL
    wr32(0x800000F0, kMem1Size);
    wr32(0x800000F8, 162000000);  // bus clock
    wr32(0x800000FC, 486000000);  // core clock
    // Arena high as on the console: the apploader puts the FST at the top of MEM1
    // ((0x81800000 - FST max length) & ~31, boot.bin +0x42C) and reports that address as the
    // arena's end (0x80000034). Up to 1.1 this was 0x81700000, which left the game's heaps 1 MiB
    // short: the first E. Gadd event (event01, the Parlor door) then failed to allocate a model's
    // decompression buffer and the decompressor wrote 320 KiB at address 0 (low memory, the bus
    // clock: the European game's frame alarm then fired at once). Host-made guest objects
    // (hleAlloc) live above MEM1 instead (runtime.cpp).
    const u32 fstMax = palGame() ? kFstMaxPal : kFstMaxUs;
    const u32 arenaHi = (0x81800000u - fstMax) & ~31u;
    wr32(0x80000034, arenaHi);  // arena high
    wr32(0x80000038, arenaHi);  // FST location (the FST itself is not in guest memory: DVD is HLE)
    wr32(0x8000003C, fstMax);   // FST max length
    guestCall(c, guestSymbol("OSSetArenaLo"), (initialStack() + 8 + 31) & ~31u);
    guestCall(c, guestSymbol("OSSetArenaHi"), arenaHi);
}

// Boot values in low memory that game code reads at run time (the European frame pacer computes
// its alarm from the bus clock): checked once per frame (frame.cpp) and restored, with an error
// in the log, if anything wrote over them.
void verifyLowMemory() {
    struct Word {
        u32 addr, value;
    };
    const Word words[] = {{0x80000028, kMem1Size}, {0x800000F0, kMem1Size}, {0x800000F8, 162000000},
                          {0x800000FC, 486000000}, {0x800000CC, palGame() ? 1u : 0u}};
    for (const Word& w : words) {
        const u32 v = rd32(w.addr);
        if (v == w.value) continue;
        static int sReports = 0;
        if (sReports++ < 20) log("[os] ERROR: low memory %08X overwritten (%08X, expected %08X); restored", w.addr, v, w.value);
        wr32(w.addr, w.value);
    }
}

// --- alarms (used by the PAL release's frame pacing, LMDisplayUtil) -----------
// OSAlarm (0x28 bytes): handler at +0. A host timer raises the handler as an interrupt;
// re-arming or cancelling an alarm invalidates the pending expiry.

namespace {
std::mutex sAlarmM;
std::unordered_map<u32, u32> sAlarmGen;  // alarm -> generation of its pending expiry
using AlarmClock = std::chrono::steady_clock;
std::unordered_map<u32, AlarmClock::time_point> sAlarmFired;  // alarm -> when it last expired

void armAlarm(u32 alarm, s64 ticks, u32 handler, u64 period) {
    // OSTime is signed: an alarm set for a time already past (the European frame pacer after a
    // long frame, e.g. a room load) fires at once on the console. As an unsigned delay it was
    // ~2^64 ticks, a double the duration_cast below cannot hold: x86 converts it to INT64_MIN
    // (fires at once, by luck), ARM saturates to INT64_MAX, and the game stayed at ~7 fps from
    // then on (Android, docs/ANDROID.md).
    if (ticks < 0) ticks = 0;
    // Fast-forward (skip.h) speeds alarms up like it speeds up retraces.
    const double speed = std::max(1.0, skip::retraceSpeed());
    const auto delay = std::chrono::duration_cast<AlarmClock::duration>(
        std::chrono::duration<double>(std::min(double(ticks) / kTimerClock / speed, 86400.0)));  // no overflow
    const auto now = AlarmClock::now();
    auto when = now + delay;
    u32 gen;
    {
        std::lock_guard lk(sAlarmM);
        gen = ++sAlarmGen[alarm];
        // An alarm re-armed from its own expiry (the PAL frame pacer: wait for the alarm,
        // set it again) counts from the expiry, not from when the host got around to it:
        // on the console that gap is microseconds, here it includes host scheduling and
        // the lockstep wait for the display, which would otherwise slow the game down.
        if (auto it = sAlarmFired.find(alarm); it != sAlarmFired.end() && now - it->second < delay)
            when = it->second + delay;
    }
    wr32(alarm, handler);
    const double seconds = std::chrono::duration<double>(when - now).count();
    static const bool sAlarmLog = std::getenv("LM_ALARM_LOG") != nullptr;
    if (sAlarmLog)
        log("[alarm] %08X ticks %llu (%.2f ms) fires in %.2f ms speed %.2f", alarm, (unsigned long long)ticks,
            double(ticks) / kTimerClock * 1000.0, seconds * 1000.0, speed);
    sched::postIrqAfter(std::max(0.0, seconds), [alarm, gen, handler, period, when](Ctx& c) {
        {
            std::lock_guard lk(sAlarmM);
            if (sAlarmGen[alarm] != gen) return;  // cancelled or re-armed
            sAlarmFired[alarm] = when;
        }
        if (period) armAlarm(alarm, s64(period), handler, period);
        guestCall(c, handler, alarm, 0);
    });
}
}  // namespace

HLE(OSCreateAlarm) {
    std::memset(hostp(c.r[3]), 0, 0x28);
    std::lock_guard lk(sAlarmM);
    ++sAlarmGen[c.r[3]];
    sAlarmFired.erase(c.r[3]);
}
HLE(OSSetAlarm) { armAlarm(c.r[3], s64(u64(c.r[5]) << 32 | c.r[6]), c.r[7], 0); }
HLE(OSSetPeriodicAlarm) {
    // (alarm, s64 start r5:r6, s64 period r7:r8, handler r9); start is absolute time.
    const s64 now = s64(timeBase()), start = s64(u64(c.r[5]) << 32 | c.r[6]);
    const s64 period = s64(u64(c.r[7]) << 32 | c.r[8]);
    armAlarm(c.r[3], start > now ? start - now : 0, c.r[9], period > 0 ? u64(period) : 1);
}
HLE(OSCancelAlarm) {
    std::lock_guard lk(sAlarmM);
    ++sAlarmGen[c.r[3]];
}

// --- interrupts ---------------------------------------------------------------

HLE(OSDisableInterrupts) {
    const u32 old = (c.msr >> 15) & 1;
    c.msr &= ~0x8000u;
    c.r[3] = old;
}
HLE(OSEnableInterrupts) {
    const u32 old = (c.msr >> 15) & 1;
    c.msr |= 0x8000u;
    c.r[3] = old;
    c.poll();
}
HLE(OSRestoreInterrupts) {
    const u32 old = (c.msr >> 15) & 1;
    if (c.r[3]) c.msr |= 0x8000u;
    else c.msr &= ~0x8000u;
    c.r[3] = old;
    c.poll();
}
HLE(__OSSetInterruptHandler) { c.r[3] = 0; }
HLE(__OSUnmaskInterrupts) { c.r[3] = 0; }
HLE(__OSMaskInterrupts) { c.r[3] = 0; }

// --- threads ------------------------------------------------------------------

HLE(OSCreateThread) {
    setBool(c, sched::create(c.r[3], c.r[4], c.r[5], c.r[6], c.r[7], s32(c.r[8]), c.r[9]));
}
HLE(OSResumeThread) { c.r[3] = u32(sched::resume(c.r[3])); }
HLE(OSSuspendThread) { c.r[3] = u32(sched::suspend(c.r[3])); }
HLE(OSCancelThread) { sched::cancel(c.r[3]); }
HLE(OSExitThread) { sched::exitThread(c.r[3]); }
HLE(OSGetCurrentThread) { c.r[3] = sched::currentGuest(); }
HLE(OSGetThreadPriority) { c.r[3] = u32(sched::priority(c.r[3])); }
HLE(OSIsThreadTerminated) { setBool(c, sched::isTerminated(c.r[3])); }
HLE(OSDetachThread) { sched::detach(c.r[3]); }
HLE(OSInitThreadQueue) {
    wr32(c.r[3], 0);
    wr32(c.r[3] + 4, 0);
}
HLE(OSSleepThread) { sched::sleepOn(c.r[3]); }
HLE(OSWakeupThread) { sched::wakeup(c.r[3]); }
HLE(OSYieldThread) { c.poll(); }

// --- message queues (OSMessageQueue: send queue, receive queue, array, count, first, used)

HLE(OSInitMessageQueue) {
    const u32 mq = c.r[3];
    for (u32 i = 0; i < 0x10; i += 4) wr32(mq + i, 0);
    wr32(mq + 0x10, c.r[4]);
    wr32(mq + 0x14, c.r[5]);
    wr32(mq + 0x18, 0);
    wr32(mq + 0x1C, 0);
}

bool traceMq() {
    static const bool on = std::getenv("LM_TRACE_MQ") != nullptr;
    return on;
}

HLE(OSSendMessage) {
    const u32 mq = c.r[3], msg = c.r[4], flags = c.r[5];
    if (traceMq() && mq != 0x803A3608) log("[mq] send %08X msg %08X flags %u (lr %08X) used %u", mq, msg, flags, c.lr, rd32(mq + 0x1C));
    for (;;) {
        const u32 count = rd32(mq + 0x14), used = rd32(mq + 0x1C);
        if (used < count) break;
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sched::sleepOn(mq);
    }
    const u32 count = rd32(mq + 0x14), first = rd32(mq + 0x18), used = rd32(mq + 0x1C);
    wr32(rd32(mq + 0x10) + ((first + used) % count) * 4, msg);
    wr32(mq + 0x1C, used + 1);
    sched::wakeup(mq + 8);
    c.r[3] = 1;
}

HLE(OSJamMessage) {
    const u32 mq = c.r[3], msg = c.r[4], flags = c.r[5];
    for (;;) {
        if (rd32(mq + 0x1C) < rd32(mq + 0x14)) break;
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sched::sleepOn(mq);
    }
    const u32 count = rd32(mq + 0x14);
    const u32 first = (rd32(mq + 0x18) + count - 1) % count;
    wr32(rd32(mq + 0x10) + first * 4, msg);
    wr32(mq + 0x18, first);
    wr32(mq + 0x1C, rd32(mq + 0x1C) + 1);
    sched::wakeup(mq + 8);
    c.r[3] = 1;
}

HLE(OSReceiveMessage) {
    const u32 mq = c.r[3], out = c.r[4], flags = c.r[5];
    if (traceMq() && mq != 0x803A3608) log("[mq] recv %08X flags %u (lr %08X) used %u", mq, flags, c.lr, rd32(mq + 0x1C));
    while (rd32(mq + 0x1C) == 0) {
        if (!(flags & 1)) {
            c.r[3] = 0;
            return;
        }
        sched::sleepOn(mq + 8);
    }
    const u32 count = rd32(mq + 0x14), first = rd32(mq + 0x18);
    if (out) wr32(out, rd32(rd32(mq + 0x10) + first * 4));
    wr32(mq + 0x18, (first + 1) % count);
    wr32(mq + 0x1C, rd32(mq + 0x1C) - 1);
    sched::wakeup(mq);
    c.r[3] = 1;
}

// --- mutexes (OSMutex: queue, owner thread, count, link) ------------------------

HLE(OSInitMutex) {
    for (u32 i = 0; i < 0x18; i += 4) wr32(c.r[3] + i, 0);
}
HLE(OSLockMutex) {
    const u32 m = c.r[3], self = sched::currentGuest();
    for (;;) {
        const u32 owner = rd32(m + 8);
        if (owner == 0) {
            wr32(m + 8, self);
            wr32(m + 0xC, 1);
            return;
        }
        if (owner == self) {
            wr32(m + 0xC, rd32(m + 0xC) + 1);
            return;
        }
        sched::sleepOn(m);
    }
}
HLE(OSTryLockMutex) {
    const u32 m = c.r[3], self = sched::currentGuest();
    const u32 owner = rd32(m + 8);
    if (owner == 0 || owner == self) {
        wr32(m + 8, self);
        wr32(m + 0xC, rd32(m + 0xC) + 1);
        c.r[3] = 1;
    } else {
        c.r[3] = 0;
    }
}
HLE(OSUnlockMutex) {
    const u32 m = c.r[3];
    if (rd32(m + 8) != sched::currentGuest()) return;
    const u32 n = rd32(m + 0xC) - 1;
    wr32(m + 0xC, n);
    if (n == 0) {
        wr32(m + 8, 0);
        sched::wakeup(m);
    }
}

// --- time -----------------------------------------------------------------------

HLE(OSGetTime) {
    const u64 t = timeBase();
    c.r[3] = u32(t >> 32);
    c.r[4] = u32(t);
}
HLE(OSGetTick) { c.r[3] = u32(timeBase()); }
HLE(OSTicksToCalendarTime) {
    const s64 ticks = s64(u64(c.r[3]) << 32 | c.r[4]);
    const u32 td = c.r[5];
    const double secs = double(ticks) / kTimerClock;
    const std::time_t t = std::time_t(946684800 + s64(secs));
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    const double frac = secs - std::floor(secs);
    const u32 fields[10] = {u32(tm.tm_sec), u32(tm.tm_min), u32(tm.tm_hour), u32(tm.tm_mday), u32(tm.tm_mon),
                            u32(tm.tm_year + 1900), u32(tm.tm_wday), u32(tm.tm_yday), u32(frac * 1000),
                            u32(std::fmod(frac * 1e6, 1000.0))};
    for (int i = 0; i < 10; i++) wr32(td + i * 4, fields[i]);
}

// --- caches -------------------------------------------------------------------------

HLE(DCFlushRange) {}
HLE(DCFlushRangeNoSync) {}
HLE(DCStoreRange) {}
HLE(DCStoreRangeNoSync) {}
HLE(DCInvalidateRange) { efbcopy::cpuRead(c.r[3], c.r[4]); }  // EFB copies the CPU reads
HLE(ICInvalidateRange) {}
HLE(DCZeroRange) { std::memset(hostp(c.r[3] & ~31u), 0, (c.r[4] + 31) & ~31u); }
HLE(LCEnable) {}
HLE(LCDisable) {}
HLE(LCQueueWait) {}
HLE(LCLoadBlocks) {  // (lc dest, mem src, blocks)
    const u32 n = c.r[5] ? c.r[5] : 128;
    std::memmove(hostp(c.r[3]), hostp(c.r[4]), n * 32);
}
HLE(LCStoreBlocks) {  // (mem dest, lc src, blocks)
    const u32 n = c.r[5] ? c.r[5] : 128;
    std::memmove(hostp(c.r[3]), hostp(c.r[4]), n * 32);
}

// --- contexts / errors --------------------------------------------------------------

HLE(OSClearContext) {}
HLE(OSSetCurrentContext) {}
HLE(OSFillFPUContext) {}
HLE(OSSetErrorHandler) { c.r[3] = 0; }
HLE(OSPanic) { fatal("OSPanic %s:%u: %s", gstr(c.r[3]), c.r[4], gstr(c.r[5])); }
HLE(OSReport) { std::fputs(gstr(c.r[3]), stdout); }

// --- reset / settings ---------------------------------------------------------------

HLE(OSResetSystem) {
    log("[os] OSResetSystem(%u, %08X, %u)", c.r[3], c.r[4], c.r[5]);
    std::exit(0);
}
HLE(OSGetResetSwitchState) { c.r[3] = 0; }
HLE(OSGetResetCode) { c.r[3] = 0; }
HLE(OSGetProgressiveMode) { c.r[3] = sProgressiveMode; }
HLE(OSSetProgressiveMode) { sProgressiveMode = c.r[3]; }
HLE(OSGetSoundMode) { c.r[3] = sSoundMode; }
// Console language (SRAM: 0 English, 1 German, 2 French, 3 Spanish, 4 Italian, 5 Dutch).
// Only the PAL release asks; it uses it when its save has no language yet (docs/SETUP.md).
HLE(OSGetLanguage) {
    const int lang = port::settings().enhancements.palLanguage;  // 1..5 as the game numbers them
    c.r[3] = u32(lang >= 1 && lang <= 5 ? lang - 1 : 0);
}
HLE(OSSetSoundMode) { sSoundMode = c.r[3]; }

// --- EXI / serial (no devices attached; UART output goes to stdout) -----------------

HLE(EXIProbe) { c.r[3] = 0; }
HLE(EXIProbeEx) { c.r[3] = u32(-1); }
HLE(EXISelect) { c.r[3] = 0; }
HLE(EXIDeselect) { c.r[3] = 0; }
HLE(EXIImmEx) { c.r[3] = 0; }
HLE(EXILock) { c.r[3] = 0; }
HLE(EXIUnlock) { c.r[3] = 1; }
HLE(InitializeUART) { c.r[3] = 0; }
HLE(ReadUARTN) { c.r[3] = 0; }
HLE(WriteUARTN) {
    std::fwrite(hostp(c.r[3]), 1, c.r[4], stdout);
    std::fflush(stdout);
    c.r[3] = 0;
}

// --- debugger ---------------------------------------------------------------------------

HLE(DBInit) {}
HLE(DBPrintf) { std::fputs(gstr(c.r[3]), stdout); }

// --- PPC architecture helpers -----------------------------------------------------------

HLE(PPCMfmsr) { c.r[3] = c.msr; }
HLE(PPCMtmsr) { c.msr = c.r[3]; }
HLE(PPCMfhid2) { c.r[3] = c.mfspr(920); }
HLE(PPCMthid2) { c.mtspr(920, c.r[3]); }
HLE(PPCSync) {}
HLE(PPCHalt) { fatal("PPCHalt"); }

}  // namespace recomp
