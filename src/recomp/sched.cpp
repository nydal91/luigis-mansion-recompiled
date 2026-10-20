#ifdef __ANDROID__
#include <pthread.h>
#endif
#include "recomp/sched.h"

#include "recomp/cpu_qos.h"
#include "recomp/hle.h"

#include <cstring>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <string>
#include <cstdio>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace recomp {

namespace {
bool sIrqPendingFlag = false;
std::atomic_ref<bool> irqPending() { return std::atomic_ref<bool>(sIrqPendingFlag); }
}  // namespace
bool* g_irqPending = &sIrqPendingFlag;  // read by guest code (Ctx::poll)
bool g_shutdown = false;

struct GuestThread {
    u32 guest = 0;
    Ctx ctx;
    s32 priority = 16;
    s32 suspendCount = 0;
    enum class St { Ready, Waiting, Moribund } st = St::Ready;
    u32 waitQueue = 0;
    bool cancelled = false;
    bool detached = false;
    u32 entry = 0, arg = 0;
    std::condition_variable cv;
    std::thread host;
};

}  // namespace recomp

namespace recomp {
u32 viRetraceCount();
}
namespace recomp::sched {
namespace {

// OSThread field offsets (Dolphin SDK).
constexpr u32 kState = 0x2C8;
constexpr u32 kSuspend = 0x2CC;
constexpr u32 kPriority = 0x2D0;
constexpr u32 kBase = 0x2D4;
constexpr u32 kVal = 0x2D8;
constexpr u32 kQueueJoin = 0x2E8;
constexpr u32 kStackBase = 0x304;
constexpr u32 kStackEnd = 0x308;
constexpr u16 kStateReady = 1, kStateRunning = 2, kStateWaiting = 4, kStateMoribund = 8;
constexpr u32 kHostWaitQueue = 0xFFFFFFFC;  // releaseForHost(): not a guest address

std::mutex sM;
std::vector<GuestThread*> sThreads;  // creation order breaks priority ties
GuestThread* sRunning = nullptr;
int sLive = 0;                        // created guest threads whose host thread has not finished
std::condition_variable sExitCv;      // signalled when sLive drops
GuestThread sIrqMarker;  // sRunning while the idle CPU services interrupts
thread_local GuestThread* tSelf = nullptr;
thread_local bool tInIrq = false;

std::mutex sIrqM;
std::deque<std::function<void(Ctx&)>> sIrqs;
std::atomic<s64> sPendingSince{0};  // LM_IRQ_STALL: when the oldest queued interrupt was posted
Ctx sIrqCtx;
u32 sIrqStackTop = 0;

GuestThread* find(u32 guest) {
    for (auto* t : sThreads)
        if (t->guest == guest) return t;
    return nullptr;
}

void writeState(GuestThread* t, u16 s) {
    if (t->guest) wr16(t->guest + kState, s);
}

bool eligible(const GuestThread* t) { return t->st == GuestThread::St::Ready && t->suspendCount <= 0; }

GuestThread* pick(GuestThread* prefer) {
    GuestThread* best = nullptr;
    for (auto* t : sThreads) {
        if (!eligible(t)) continue;
        if (!best || t->priority < best->priority) best = t;
    }
    if (best && prefer && eligible(prefer) && prefer->priority <= best->priority) return prefer;
    return best;
}

void handOff(GuestThread* next) {
    if (sRunning && sRunning != &sIrqMarker && sRunning->st == GuestThread::St::Ready) writeState(sRunning, kStateReady);
    sRunning = next;
    if (next) {
        writeState(next, kStateRunning);
        next->cv.notify_all();
    }
}

void waitForCpu(std::unique_lock<std::mutex>& lk, GuestThread* self) {
    self->cv.wait(lk, [&] { return sRunning == self || g_shutdown; });
    cpu::tick();
    if (g_shutdown) {
        lk.unlock();
        throw ThreadExit{};
    }
    if (self->cancelled) {
        lk.unlock();
        throw ThreadExit{};
    }
}

// Gives the CPU to the best thread; returns once `self` owns it again.
void reschedule(std::unique_lock<std::mutex>& lk, GuestThread* self) {
    GuestThread* next = pick(self);
    if (next == self) return;
    handOff(next);
    waitForCpu(lk, self);
}

void runIrqs() {
    tInIrq = true;
    for (;;) {
        std::function<void(Ctx&)> fn;
        {
            std::lock_guard lk(sIrqM);
            if (sIrqs.empty()) {
                irqPending().store(false);
                break;
            }
            fn = std::move(sIrqs.front());
            sIrqs.pop_front();
            if (sIrqs.empty()) sPendingSince = 0;
        }
        Ctx& c = sIrqCtx;
        c.r[1] = sIrqStackTop;
        c.msr = 0;
        fn(c);
    }
    tInIrq = false;
}

void finishThread(GuestThread* t, u32 val) {
    std::unique_lock lk(sM);
    t->st = GuestThread::St::Moribund;
    if (t->guest) {
        wr32(t->guest + kVal, val);
        writeState(t, kStateMoribund);
    }
    // Wake joiners.
    for (auto* o : sThreads)
        if (o->st == GuestThread::St::Waiting && o->waitQueue == t->guest + kQueueJoin) o->st = GuestThread::St::Ready;
    if (sRunning == t) handOff(pick(nullptr));
    if (--sLive == 0) sExitCv.notify_all();
}

void threadMain(GuestThread* t) {
    tSelf = t;
    cpu::setCurrent(cpu::Role::Guest);
#ifdef __ANDROID__
    pthread_setname_np(pthread_self(), "guest");  // named in the [perf] log lines
#endif
    u32 val = 0;
    try {
        {
            std::unique_lock lk(sM);
            waitForCpu(lk, t);
        }
        Ctx& c = t->ctx;
        c.r[3] = t->arg;
        c.lr = 0;
        c.call(t->entry);
        val = c.r[3];
    } catch (const ThreadExit&) {
        val = t->ctx.r[3];
    }
    finishThread(t, val);
}

}  // namespace

GuestThread* adoptBootThread(u32 guest, const Ctx& init) {
    auto* t = new GuestThread;
    t->guest = guest;
    t->ctx = init;
    t->ctx.thread = t;
    t->priority = 16;
    tSelf = t;
    std::lock_guard lk(sM);
    sThreads.push_back(t);
    sRunning = t;
    if (guest) {
        wr16(guest + kState, kStateRunning);
        wr32(guest + kPriority, 16);
        wr32(guest + kBase, 16);
    }
    return t;
}

void setIrqContext(const Ctx& base, u32 stackTop) {
    sIrqCtx = base;
    sIrqCtx.thread = nullptr;
    sIrqStackTop = stackTop;
}

GuestThread* current() { return tSelf; }
u32 currentGuest() { return tSelf ? tSelf->guest : 0; }
Ctx& ctxOf(GuestThread* t) { return t->ctx; }
bool inIrq() { return tInIrq; }
bool interruptsEnabled() { return tSelf && !tInIrq && (tSelf->ctx.msr & 0x8000) != 0; }

bool create(u32 guest, u32 entry, u32 arg, u32 stackTop, u32 stackSize, s32 prio, u32 attr) {
    if (prio < 0 || prio > 31) return false;
    auto* t = new GuestThread;
    t->guest = guest;
    t->entry = entry;
    t->arg = arg;
    t->priority = prio;
    t->suspendCount = 1;
    t->detached = (attr & 1) != 0;
    // Same registers as the creator (r2/r13 small-data bases, GQRs), fresh stack.
    t->ctx = tSelf ? tSelf->ctx : sIrqCtx;
    t->ctx.thread = t;
    t->ctx.r[1] = (stackTop & ~7u) - 8;
    wr32(t->ctx.r[1], 0);
    t->ctx.msr = 0x8000;
    std::memset(hostp(guest), 0, 0x310);
    wr16(guest + kState, kStateReady);
    wr16(guest + kState + 2, u16(attr));
    wr32(guest + kSuspend, 1);
    wr32(guest + kPriority, u32(prio));
    wr32(guest + kBase, u32(prio));
    wr32(guest + kStackBase, stackTop);
    wr32(guest + kStackEnd, stackTop - stackSize);
    wr32(stackTop - stackSize, 0xDEADBABE);
    {
        std::lock_guard lk(sM);
        // A thread struct can be reused once its previous thread has terminated.
        std::erase_if(sThreads, [&](GuestThread* o) { return o->guest == guest && o->st == GuestThread::St::Moribund; });
        sThreads.push_back(t);
        ++sLive;
    }
    t->host = std::thread(threadMain, t);
    t->host.detach();
    return true;
}

s32 resume(u32 guest) {
    std::unique_lock lk(sM);
    GuestThread* t = find(guest);
    if (!t) return 0;
    const s32 old = t->suspendCount;
    if (t->suspendCount > 0) t->suspendCount--;
    wr32(guest + kSuspend, u32(std::max(t->suspendCount, 0)));
    if (old == 1 && tSelf && !tInIrq) reschedule(lk, tSelf);
    return old;
}

s32 suspend(u32 guest) {
    std::unique_lock lk(sM);
    GuestThread* t = find(guest);
    if (!t) return 0;
    const s32 old = t->suspendCount++;
    wr32(guest + kSuspend, u32(t->suspendCount));
    if (t == tSelf && !tInIrq) reschedule(lk, tSelf);
    return old;
}

void cancel(u32 guest) {
    std::unique_lock lk(sM);
    GuestThread* t = find(guest);
    if (!t || t->st == GuestThread::St::Moribund) return;
    if (t == tSelf) {
        lk.unlock();
        exitThread(0);
    }
    t->cancelled = true;
    t->st = GuestThread::St::Ready;
    t->suspendCount = 0;  // let it run so it can unwind
    t->priority = 32;     // ...but only when nothing else wants the CPU
}

void exitThread(u32 val) {
    tSelf->ctx.r[3] = val;
    throw ThreadExit{};
}

bool isTerminated(u32 guest) {
    std::lock_guard lk(sM);
    GuestThread* t = find(guest);
    return !t || t->st == GuestThread::St::Moribund;
}

s32 priority(u32 guest) {
    std::lock_guard lk(sM);
    GuestThread* t = find(guest);
    return t ? t->priority : 0;
}

void detach(u32 guest) {
    std::lock_guard lk(sM);
    if (GuestThread* t = find(guest)) t->detached = true;
}

void sleepOn(u32 queue) {
    GuestThread* self = tSelf;
    if (!self || tInIrq) fatal("sleepOn(%08X) outside a guest thread", queue);
    std::unique_lock lk(sM);
    self->st = GuestThread::St::Waiting;
    self->waitQueue = queue;
    writeState(self, kStateWaiting);
    GuestThread* next = pick(nullptr);
    if (!next && irqPending().load()) {
        // Going idle with interrupts queued: service them on this host thread.
        sRunning = &sIrqMarker;
        for (;;) {
            lk.unlock();
            runIrqs();
            lk.lock();
            std::lock_guard ilk(sIrqM);
            if (sIrqs.empty()) break;
        }
        next = pick(nullptr);
    }
    handOff(next);
    waitForCpu(lk, self);
}

// The current thread waits for the host (frame handoff): it gives up the CPU like a
// blocking OS call, so interrupts are taken and other threads run meanwhile.
void releaseForHost() {
    GuestThread* self = tSelf;
    if (!self || tInIrq) return;
    std::unique_lock lk(sM);
    if (sRunning != self) return;
    self->st = GuestThread::St::Waiting;
    self->waitQueue = kHostWaitQueue;
    writeState(self, kStateWaiting);
    GuestThread* next = pick(nullptr);
    if (!next && irqPending().load()) {
        sRunning = &sIrqMarker;
        for (;;) {
            lk.unlock();
            runIrqs();
            lk.lock();
            std::lock_guard ilk(sIrqM);
            if (sIrqs.empty()) break;
        }
        next = pick(nullptr);
    }
    handOff(next);
}

// After releaseForHost(): ready again; returns once this thread owns the CPU.
void reacquireAfterHost() {
    GuestThread* self = tSelf;
    if (!self || tInIrq) return;
    std::unique_lock lk(sM);
    if (self->st != GuestThread::St::Waiting || self->waitQueue != kHostWaitQueue) return;
    self->st = GuestThread::St::Ready;
    self->waitQueue = 0;
    writeState(self, kStateReady);
    if (sRunning == nullptr) {
        handOff(pick(self));
    } else if (sRunning != &sIrqMarker) {
        // The running thread only switches at a scheduling point: an empty interrupt makes
        // its next safe point reschedule (we win if our priority is higher).
        irqPending().store(true);
    }  // (an idle-CPU interrupt service hands the CPU to the best thread when it is done)
    waitForCpu(lk, self);
}

void wakeup(u32 queue) {
    std::unique_lock lk(sM);
    for (auto* t : sThreads) {
        if (t->st == GuestThread::St::Waiting && t->waitQueue == queue) {
            t->st = GuestThread::St::Ready;
            t->waitQueue = 0;
            writeState(t, kStateReady);
        }
    }
    if (tSelf && !tInIrq && sRunning == tSelf) reschedule(lk, tSelf);
}

// LM_IRQ_STALL=1 (debugging): logs the running thread whenever an interrupt has waited more
// than 10 ms, i.e. something held the guest CPU without reaching a safe point (a long host
// call in an HLE...). With LM_TRACE=ring the thread's recent functions are included.
void stallSampler() {
    using clk = std::chrono::steady_clock;
    s64 reported = 0;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        const s64 since = sPendingSince.load();
        if (!since) continue;
        const s64 now = clk::now().time_since_epoch().count();
        const double ms = double(now - since) / 1e6;
        if (ms < 10 || since == reported) continue;
        reported = since;
        GuestThread* r = sRunning;
        if (r && r != &sIrqMarker) {
            std::string s;
            for (u32 i = r->ctx.ringPos > 12 ? r->ctx.ringPos - 12 : 0; i < r->ctx.ringPos; i++) {
                char b[12];
                std::snprintf(b, sizeof b, " %08X", r->ctx.ring[i & 31]);
                s += b;
            }
            log("[stall] irq pending %.1f ms: running %08X prio %d lr %08X fn %08X msr %04X recent%s", ms, r->guest,
                r->priority, r->ctx.lr, r->ctx.lastFn, r->ctx.msr, s.c_str());
        } else {
            log("[stall] irq pending %.1f ms: running %s", ms, r ? "irq" : "none");
        }
    }
}

void postIrq(std::function<void(Ctx&)> fn) {
    {
        std::lock_guard lk(sIrqM);
        static const bool sStall = [] {
            if (!std::getenv("LM_IRQ_STALL")) return false;
            std::thread(stallSampler).detach();
            return true;
        }();
        if (sStall && sIrqs.empty()) sPendingSince = std::chrono::steady_clock::now().time_since_epoch().count();
        sIrqs.push_back(std::move(fn));
        irqPending().store(true);
    }
    // An idle CPU is serviced by the poster itself.
    std::unique_lock lk(sM);
    if (sRunning != nullptr || g_shutdown) return;
    sRunning = &sIrqMarker;
    for (;;) {
        lk.unlock();
        runIrqs();
        lk.lock();
        // An interrupt posted while we were finishing saw the CPU busy and left it
        // to us; check under sM so it cannot be stranded.
        std::lock_guard ilk(sIrqM);
        if (sIrqs.empty()) break;
    }
    handOff(pick(nullptr));
}

void dumpThreads() {
    std::lock_guard lk(sM);
    log("[sched] running=%08X irqPending=%d", sRunning == &sIrqMarker ? 0xFFFFFFFF : sRunning ? sRunning->guest : 0,
        int(irqPending().load()));
    for (auto* t : sThreads) {
        log("[sched]   %08X prio %d suspend %d state %d wait %08X lr %08X fn %08X msr %04X", t->guest, t->priority,
            t->suspendCount, int(t->st), t->waitQueue, t->ctx.lr, t->ctx.lastFn, t->ctx.msr);
        if (t->st == GuestThread::St::Ready && t->ctx.ringPos) {
            std::string line = "[sched]     recent:";
            for (u32 i = t->ctx.ringPos > 32 ? t->ctx.ringPos - 32 : 0; i < t->ctx.ringPos; i++) {
                char buf[12];
                std::snprintf(buf, sizeof(buf), " %08X", t->ctx.ring[i & 31]);
                line += buf;
            }
            log("%s", line.c_str());
        }
        }
    log("[sched]   mq 803A3608: used %u base %u interval %u retrace %u", rd32(0x803A3608 + 0x1C), rd32(0x803A3600),
        rd32(0x803A3604), viRetraceCount());
    log("[sched]   irq ctx: lr %08X fn %08X, queued %zu", sIrqCtx.lr, sIrqCtx.lastFn, sIrqs.size());
}

namespace {
struct TimedIrq {
    std::chrono::steady_clock::time_point when;
    std::function<void(Ctx&)> fn;
};
std::mutex sTimerM;
std::condition_variable sTimerCv;
std::multimap<std::chrono::steady_clock::time_point, std::function<void(Ctx&)>> sTimed;
std::thread sTimerThread;

void timerMain() {
    cpu::setCurrent(cpu::Role::Timer);
#ifdef _WIN32
    timeBeginPeriod(1);  // 1 ms scheduler resolution for timed interrupts
#endif
    std::unique_lock lk(sTimerM);
    while (!g_shutdown) {
        if (sTimed.empty()) {
            sTimerCv.wait(lk);
            continue;
        }
        const auto when = sTimed.begin()->first;
        if (const auto now = std::chrono::steady_clock::now(); now < when) {
            // Waits are only as precise as the OS timer (15.6 ms on Windows by default), too
            // coarse for the PAL release's frame alarms: sleep coarsely, then yield.
            constexpr auto kSpin = std::chrono::milliseconds(3);
            if (when - now > kSpin) {
                sTimerCv.wait_until(lk, when - kSpin);
            } else {
                lk.unlock();
                std::this_thread::yield();
                lk.lock();
            }
            continue;
        }
        auto fn = std::move(sTimed.begin()->second);
        sTimed.erase(sTimed.begin());
        lk.unlock();
        postIrq(std::move(fn));
        lk.lock();
    }
}
}  // namespace

void postIrqAfter(double seconds, std::function<void(Ctx&)> fn) {
    const auto when = std::chrono::steady_clock::now() +
                      std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::duration<double>(seconds));
    std::lock_guard lk(sTimerM);
    // One timer thread for the whole run. (A detached std::thread is never joinable, so
    // testing joinable() started a new thread on every call: the European game's per-frame
    // alarms leaked ~30 threads a second until thread creation failed and the game aborted.)
    static bool sStarted = false;
    if (!sStarted) {
        sStarted = true;
        sTimerThread = std::thread(timerMain);
        sTimerThread.detach();
    }
    sTimed.emplace(when, std::move(fn));
    sTimerCv.notify_one();
}

void shutdown() {
    std::lock_guard lk(sM);
    g_shutdown = true;
    for (auto* t : sThreads) t->cv.notify_all();
}

bool waitForThreads(double seconds) {
    std::unique_lock lk(sM);
    return sExitCv.wait_for(lk, std::chrono::duration<double>(seconds), [] { return sLive == 0; });
}

void serviceOnThread(Ctx& c) {
    GuestThread* self = c.thread;
    if (!self || tInIrq || self != tSelf) return;
    runIrqs();
    std::unique_lock lk(sM);
    if (sRunning == self) reschedule(lk, self);
}

}  // namespace recomp::sched

namespace recomp {
void service_irq(Ctx& c) { sched::serviceOnThread(c); }
}  // namespace recomp
