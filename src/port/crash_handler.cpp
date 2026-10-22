// Prints a symbolized stack trace on abort() / std::terminate / unhandled SEH exceptions
// (Windows) or fatal signals (Linux: SIGSEGV, SIGBUS, SIGILL, SIGFPE), so crashes in game or
// library code can be located from a log.
#include "port/crash_handler.h"
#include "port/recovery.h"

#include <aurora/pipeline_cache.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <exception>
#include <initializer_list>
#include <thread>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#elif defined(__ANDROID__)
// Android: the system's crash reporter (debuggerd) writes a symbolized tombstone and logcat
// backtrace for fatal signals; this file only adds the reason line (docs/ANDROID.md).
#include <unistd.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach/mach.h>
#include <pthread.h>
#include <sys/ucontext.h>
#else
#include <sys/syscall.h>
#include <ucontext.h>
#endif
#endif

namespace port {
namespace {

void printStackTo(std::FILE* out, const char* reason) {
    std::fprintf(out, "\n=== crash: %s ===\n", reason);
#ifdef _WIN32
    void* frames[64];
    const USHORT count = CaptureStackBackTrace(0, 64, frames, nullptr);
    // Module + offset first, before loading symbols: symbol loading for a large game module
    // can take long enough that a killed or hung process never prints the symbolized trace
    // (llvm-symbolizer --obj=<module> 0x<offset> resolves these offline).
    std::fprintf(out, "  thread %lu, raw frames:\n", static_cast<unsigned long>(GetCurrentThreadId()));
    for (USHORT i = 0; i < count; i++) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &mod))
            GetModuleFileNameA(mod, name, sizeof(name));
        const char* base = std::strrchr(name, '\\');
        std::fprintf(out, "  #%02u %s+0x%llx\n", i, base ? base + 1 : name,
                     static_cast<unsigned long long>(static_cast<const char*>(frames[i]) - reinterpret_cast<const char*>(mod)));
    }
    std::fflush(out);
    HANDLE process = GetCurrentProcess();
    static bool sSymInit = SymInitialize(process, nullptr, TRUE);
    (void)sSymInit;
    alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
    auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
    for (USHORT i = 0; i < count; i++) {
        symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
        symbol->MaxNameLen = 255;
        DWORD64 displacement = 0;
        IMAGEHLP_LINE64 line{};
        line.SizeOfStruct = sizeof(line);
        DWORD lineDisp = 0;
        const DWORD64 addr = reinterpret_cast<DWORD64>(frames[i]);
        const bool haveSym = SymFromAddr(process, addr, &displacement, symbol);
        const bool haveLine = SymGetLineFromAddr64(process, addr, &lineDisp, &line);
        std::fprintf(out, "  #%02u %s+0x%llx", i, haveSym ? symbol->Name : "?",
                     static_cast<unsigned long long>(displacement));
        if (haveLine) {
            std::fprintf(out, " (%s:%lu)", line.FileName, line.LineNumber);
        }
        std::fputc('\n', out);
    }
#elif defined(__ANDROID__)
    std::fflush(stderr);  // the system crash report (logcat / tombstone) has the stack
#else
    // backtrace_symbols_fd writes "binary(symbol+off) [addr]" lines without allocating; the
    // executable's own functions are named only for exported symbols, so the offsets are
    // what to feed to addr2line (docs/LINUX.md).
    std::fflush(out);
    void* frames[64];
    const int count = backtrace(frames, 64);
    backtrace_symbols_fd(frames, count, fileno(out));
#endif
    std::fflush(out);
}

// The stack to stderr and (Windows) into last_run.log (port/recovery.h).
void printStack(const char* reason) {
    printStackTo(stderr, reason);
    if (std::FILE* log = recovery::runLog(); log && !recovery::stdioInLog()) printStackTo(log, reason);
}

void onAbort(int) {
    aurora_pipeline_cache_note_crash();  // a real crash for the pipeline crash journal (Aurora 0231)
    recovery::onCrash("abort()");
    printStack("abort()");
    std::_Exit(3);
}

#ifdef _WIN32
// First chance, before anything unwinds: stack overflows and faults in the game module DLL
// may never reach the unhandled-exception filter with enough stack to symbolize, so report
// the code, the faulting address and the module it is in with minimal stack use.
LONG WINAPI onVectored(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_STACK_OVERFLOW && code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION)
        return EXCEPTION_CONTINUE_SEARCH;
    static volatile LONG sOnce = 0;
    if (InterlockedExchange(&sOnce, 1)) return EXCEPTION_CONTINUE_SEARCH;
    char buf[256];
    HMODULE mod = nullptr;
    char name[128] = "?";
    const void* pc = info->ExceptionRecord->ExceptionAddress;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(pc), &mod))
        GetModuleFileNameA(mod, name, sizeof(name));
    const int n = wsprintfA(buf, "\n=== exception 0x%08lX at %p (%s+0x%lX), rsp %p ===\n", code, pc, name,
                            static_cast<unsigned long>(static_cast<const char*>(pc) - reinterpret_cast<const char*>(mod)),
                            reinterpret_cast<void*>(info->ContextRecord->Rsp));
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), buf, DWORD(n), &written, nullptr);
    return EXCEPTION_CONTINUE_SEARCH;
}

LONG WINAPI onException(EXCEPTION_POINTERS* info) {
    aurora_pipeline_cache_note_crash();
    char reason[64];
    std::snprintf(reason, sizeof(reason), "exception 0x%08lX", info->ExceptionRecord->ExceptionCode);
    recovery::onCrash(reason);
    printStack(reason);
    return EXCEPTION_EXECUTE_HANDLER;
}
#elif !defined(__ANDROID__)
// Fatal signals (Linux): report the signal, the faulting address, the instruction and the
// binary it is in (e.g. the game module .so), then the stack. Runs on an alternate stack on
// the main thread so a stack overflow there can still be reported. Afterwards the default
// action runs (core dump / exit status of the signal).
volatile sig_atomic_t sInSignal = 0;

void onSignal(int sig, siginfo_t* info, void* uctx) {
    if (sInSignal) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    }
    sInSignal = 1;
    aurora_pipeline_cache_note_crash();
    recovery::onCrash(strsignal(sig));
    const void* pc = nullptr;
    const void* sp = nullptr;
#if defined(__APPLE__) && defined(__aarch64__)
    if (uctx) {
        const auto* uc = static_cast<const ucontext_t*>(uctx);
        pc = reinterpret_cast<const void*>(uc->uc_mcontext->__ss.__pc);
        sp = reinterpret_cast<const void*>(uc->uc_mcontext->__ss.__sp);
    }
#elif defined(__APPLE__) && defined(__x86_64__)
    if (uctx) {
        const auto* uc = static_cast<const ucontext_t*>(uctx);
        pc = reinterpret_cast<const void*>(uc->uc_mcontext->__ss.__rip);
        sp = reinterpret_cast<const void*>(uc->uc_mcontext->__ss.__rsp);
    }
#elif defined(__x86_64__)
    if (uctx) {
        const auto* uc = static_cast<const ucontext_t*>(uctx);
        pc = reinterpret_cast<const void*>(uc->uc_mcontext.gregs[REG_RIP]);
        sp = reinterpret_cast<const void*>(uc->uc_mcontext.gregs[REG_RSP]);
    }
#endif
    Dl_info dl{};
    const bool haveDl = pc && dladdr(pc, &dl) && dl.dli_fname;
    char buf[512];
    const int n = std::snprintf(buf, sizeof(buf), "\n=== signal %d (%s) at pc %p (%s+0x%lx), fault address %p, sp %p ===\n",
                                sig, strsignal(sig), pc, haveDl ? dl.dli_fname : "?",
                                haveDl ? static_cast<unsigned long>(static_cast<const char*>(pc) -
                                                                    static_cast<const char*>(dl.dli_fbase))
                                       : 0ul,
                                info ? info->si_addr : nullptr, sp);
    if (n > 0) (void)!write(STDERR_FILENO, buf, size_t(n) < sizeof(buf) ? size_t(n) : sizeof(buf) - 1);
    printStack(strsignal(sig));
    signal(sig, SIG_DFL);
    raise(sig);
}

void installSignalHandlers() {
    // backtrace() loads libgcc_s on first use (allocates): do that now, not in a handler.
    void* warm[1];
    backtrace(warm, 1);
    static char sAltStack[64 * 1024];
    stack_t ss{};
    ss.ss_sp = sAltStack;
    ss.ss_size = sizeof(sAltStack);
    sigaltstack(&ss, nullptr);
    struct sigaction sa{};
    sa.sa_sigaction = &onSignal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE}) sigaction(sig, &sa, nullptr);
}
#endif

// ---- Hang watchdog ----
std::atomic<unsigned long long> sHeartbeat{0};
std::atomic<const char*> sPhase{"startup"};

#ifdef _WIN32

void printThreadStack(HANDLE process, DWORD tid) {
    HANDLE th = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!th) return;
    if (SuspendThread(th) == DWORD(-1)) {
        CloseHandle(th);
        return;
    }
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(th, &ctx)) {
        std::fprintf(stderr, "--- thread %lu ---\n", static_cast<unsigned long>(tid));
        STACKFRAME64 sf{};
        sf.AddrPC.Offset = ctx.Rip;
        sf.AddrPC.Mode = AddrModeFlat;
        sf.AddrFrame.Offset = ctx.Rbp;
        sf.AddrFrame.Mode = AddrModeFlat;
        sf.AddrStack.Offset = ctx.Rsp;
        sf.AddrStack.Mode = AddrModeFlat;
        alignas(SYMBOL_INFO) char buffer[sizeof(SYMBOL_INFO) + 256];
        auto* symbol = reinterpret_cast<SYMBOL_INFO*>(buffer);
        for (int i = 0; i < 40; i++) {
            if (!StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, th, &sf, &ctx, nullptr, SymFunctionTableAccess64,
                             SymGetModuleBase64, nullptr) ||
                !sf.AddrPC.Offset)
                break;
            symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
            symbol->MaxNameLen = 255;
            DWORD64 disp = 0;
            IMAGEHLP_LINE64 line{};
            line.SizeOfStruct = sizeof(line);
            DWORD lineDisp = 0;
            const bool haveSym = SymFromAddr(process, sf.AddrPC.Offset, &disp, symbol);
            const bool haveLine = SymGetLineFromAddr64(process, sf.AddrPC.Offset, &lineDisp, &line);
            std::fprintf(stderr, "  #%02d %s+0x%llx", i, haveSym ? symbol->Name : "?", static_cast<unsigned long long>(disp));
            if (haveLine) std::fprintf(stderr, " (%s:%lu)", line.FileName, line.LineNumber);
            std::fputc('\n', stderr);
        }
    }
    ResumeThread(th);
    CloseHandle(th);
}

// Every thread's stack (except the watchdog's own), symbolized: where a hung process waits.
void dumpAllThreads(const char* reason) {
    std::fprintf(stderr, "\n=== %s: all thread stacks ===\n", reason);
    HANDLE process = GetCurrentProcess();
    static bool sSymInit = SymInitialize(process, nullptr, TRUE);
    (void)sSymInit;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    const DWORD self = GetCurrentThreadId(), pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
        if (te.th32OwnerProcessID == pid && te.th32ThreadID != self) printThreadStack(process, te.th32ThreadID);
    CloseHandle(snap);
    std::fprintf(stderr, "=== end of thread stacks ===\n");
    std::fflush(stderr);
}
#elif defined(__ANDROID__)
// Android: no thread dump of our own (debuggerd's tombstone has every thread's stack).
void dumpAllThreads(const char*) {}
#else
// Linux (issue #11: a Steam Deck log said nothing about where the game stopped): each thread is
// sent a signal whose handler writes its own backtrace ("binary(symbol+off) [addr]" lines, without
// allocating) to stderr, which the launcher puts into last_run.log. One thread at a time, each
// acknowledged (or given up on after 300 ms: a thread with the signal blocked).
std::atomic<int> sDumpAck{0};
void onDumpSignal(int) {
    void* frames[48];
    const int n = backtrace(frames, 48);
    backtrace_symbols_fd(frames, n, STDERR_FILENO);
    sDumpAck.store(1, std::memory_order_release);
}

#ifdef __APPLE__
// macOS has no /proc or tgkill: the threads come from the Mach task, the signal goes through
// pthread_kill (SIGUSR2: no real-time signals; nothing else here uses it).
void dumpAllThreads(const char* reason) {
    const int sig = SIGUSR2;
    static const bool sInstalled = [sig] {
        struct sigaction sa{};
        sa.sa_handler = &onDumpSignal;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        return sigaction(sig, &sa, nullptr) == 0;
    }();
    if (!sInstalled) return;
    std::fprintf(stderr, "\n=== %s: all thread stacks ===\n", reason);
    std::fflush(stderr);
    thread_act_array_t threads = nullptr;
    mach_msg_type_number_t count = 0;
    if (task_threads(mach_task_self(), &threads, &count) != KERN_SUCCESS) return;
    const pthread_t self = pthread_self();
    for (mach_msg_type_number_t i = 0; i < count; i++) {
        const pthread_t th = pthread_from_mach_thread_np(threads[i]);
        if (th && th != self) {
            char name[64] = "?";
            pthread_getname_np(th, name, sizeof(name));
            std::fprintf(stderr, "--- thread %u (%s) ---\n", unsigned(threads[i]), name);
            std::fflush(stderr);
            sDumpAck.store(0, std::memory_order_relaxed);
            if (pthread_kill(th, sig) == 0) {
                for (int k = 0; k < 300 && !sDumpAck.load(std::memory_order_acquire); k++)
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
                if (!sDumpAck.load(std::memory_order_acquire)) std::fprintf(stderr, "  (no answer: signal blocked)\n");
            }
        }
        mach_port_deallocate(mach_task_self(), threads[i]);
    }
    vm_deallocate(mach_task_self(), vm_address_t(threads), vm_size_t(count * sizeof(thread_act_t)));
    std::fprintf(stderr, "=== end of thread stacks ===\n");
    std::fflush(stderr);
}
#else
void dumpAllThreads(const char* reason) {
    const int sig = SIGRTMIN + 7;
    static const bool sInstalled = [sig] {
        struct sigaction sa{};
        sa.sa_handler = &onDumpSignal;
        sa.sa_flags = SA_RESTART;
        sigemptyset(&sa.sa_mask);
        return sigaction(sig, &sa, nullptr) == 0;
    }();
    if (!sInstalled) return;
    std::fprintf(stderr, "\n=== %s: all thread stacks ===\n", reason);
    std::fflush(stderr);
    const pid_t pid = getpid();
    const pid_t self = pid_t(syscall(SYS_gettid));
    DIR* dir = opendir("/proc/self/task");
    if (!dir) return;
    while (const dirent* e = readdir(dir)) {
        const pid_t tid = pid_t(std::atoi(e->d_name));
        if (tid <= 0 || tid == self) continue;
        char path[64], name[64] = "?";
        std::snprintf(path, sizeof(path), "/proc/self/task/%d/comm", int(tid));
        if (std::FILE* f = std::fopen(path, "r")) {
            if (std::fgets(name, sizeof(name), f)) name[std::strcspn(name, "\n")] = 0;
            std::fclose(f);
        }
        std::fprintf(stderr, "--- thread %d (%s) ---\n", int(tid), name);
        std::fflush(stderr);
        sDumpAck.store(0, std::memory_order_relaxed);
        if (syscall(SYS_tgkill, pid, tid, sig) != 0) continue;
        for (int i = 0; i < 300 && !sDumpAck.load(std::memory_order_acquire); i++)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        if (!sDumpAck.load(std::memory_order_acquire)) std::fprintf(stderr, "  (no answer: signal blocked)\n");
    }
    closedir(dir);
    std::fprintf(stderr, "=== end of thread stacks ===\n");
    std::fflush(stderr);
}
#endif
#endif

std::atomic<HangDetailFn> sHangDetail{nullptr};

// One "[hang]" line: stderr (Linux: last_run.log through the launcher) and last_run.log (Windows).
void hangLine(const char* line) {
    std::fputs(line, stderr);
    std::fflush(stderr);
    recovery::logLine(line, std::strlen(line));
}

}  // namespace

void setHangDetail(HangDetailFn fn) { sHangDetail.store(fn, std::memory_order_release); }

void reportStall(const char* what, bool dumpThreads) {
    char detail[512] = "";
    if (const HangDetailFn fn = sHangDetail.load(std::memory_order_acquire)) fn(detail, sizeof(detail), false);
    char line[1024];
    std::snprintf(line, sizeof(line), "[hang] %s%s%s\n", what, detail[0] ? "; " : "", detail);
    hangLine(line);
    if (dumpThreads) {
        static std::atomic<int> sDumps{0};
        if (sDumps.fetch_add(1) < 2) dumpAllThreads(what);
    }
}

void heartbeat() {
    sHeartbeat.fetch_add(1, std::memory_order_relaxed);
}

void setPhase(const char* phase) { sPhase.store(phase, std::memory_order_relaxed); }

void startHangWatchdog() {
    // Always on (issue #14: a start that stopped after the shader cache opened left no reason in the
    // log): when the main thread has not come back to its loop for 15 s, say what it was doing
    // (setPhase), again every 30 s while it lasts and once when it continues. A program that is
    // only slow (a long load) gets a few lines; one that hangs has its last phase in the log.
    // LM_HANG_DUMP=<seconds> also prints every thread's stack (three samples a second apart). Linux
    // prints them once by itself at the first report of a run (the log is the only evidence a
    // player can send; issue #11). Each report also says what Aurora's threads are doing and
    // whether the game kept running (setHangDetail).
    const char* env = std::getenv("LM_HANG_DUMP");
    const int dumpSeconds = env ? std::atoi(env) : 0;
    const auto t0 = std::chrono::steady_clock::now();
    std::thread([dumpSeconds, t0] {
        const auto up = [t0] { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); };
        unsigned long long last = sHeartbeat.load();
        int still = 0;
        const char* stalledIn = nullptr;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const unsigned long long now = sHeartbeat.load();
            const char* phase = sPhase.load(std::memory_order_relaxed);
            if (now != last || phase == nullptr) {  // progress, or waiting on purpose (window paused)
                if (still >= 15 && stalledIn) {
                    char line[256];
                    std::snprintf(line, sizeof(line), "[hang] %.0f s after start: main thread continued after %d s (%s)\n",
                                  up(), still, stalledIn);
                    std::fputs(line, stderr);
                    recovery::logLine(line, std::strlen(line));
                    recovery::noteHang(nullptr);
                }
                last = now;
                still = 0;
                stalledIn = nullptr;
                continue;
            }
            ++still;
            if (still == 1) {
                if (const HangDetailFn fn = sHangDetail.load(std::memory_order_acquire)) {
                    char ignored[8];
                    fn(ignored, sizeof(ignored), true);  // the stall's start (game frames since)
                }
            }
            if (still == 15 || (still > 15 && (still - 15) % 30 == 0)) {
                stalledIn = phase;
                char detail[512] = "";
                if (const HangDetailFn fn = sHangDetail.load(std::memory_order_acquire)) fn(detail, sizeof(detail), false);
                char line[1024];
                std::snprintf(line, sizeof(line),
                              "[hang] %.0f s after start: main thread has not returned to its loop for %d s (%s)%s%s\n", up(),
                              still, phase, detail[0] ? "; " : "", detail);
                // Windows: also last_run.log. While the game is still starting, this is the reason the
                // recovery prompt names if the start is then killed (recovery.h).
                hangLine(line);
#ifndef _WIN32
                // Linux: every thread's stack, once per run, at the first report.
                static bool sDumped = false;
                if (!sDumped && dumpSeconds <= 0) {
                    sDumped = true;
                    dumpAllThreads("main loop stalled for 15 s");
                }
#endif
                char what[160];
                std::snprintf(what, sizeof(what), "no progress for %d s during %s", still, phase);
                recovery::noteHang(what);
            }
            // Only once the main loop has run at all (startup can block longer).
            if (dumpSeconds > 0 && now && still >= dumpSeconds && still < dumpSeconds + 3) {  // three samples
                char reason[96];
                std::snprintf(reason, sizeof(reason), "main loop stalled for %d s (LM_HANG_DUMP)", dumpSeconds);
                dumpAllThreads(reason);
            }
        }
    }).detach();
}

void installCrashHandler() {
#ifdef __ANDROID__
    // Leave SIGABRT and fatal signals to debuggerd (it symbolizes the stack); only name the reason.
    std::set_terminate([] {
        printStack("std::terminate");
        std::abort();
    });
    return;
#endif
    std::signal(SIGABRT, onAbort);
    std::set_terminate([] {
        aurora_pipeline_cache_note_crash();
        recovery::onCrash("std::terminate");
        printStack("std::terminate");
        std::_Exit(3);
    });
#ifdef _WIN32
    SetUnhandledExceptionFilter(onException);
    AddVectoredExceptionHandler(1, onVectored);
    // Route CRT abort() through SIGABRT instead of an immediate fast-fail.
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
#elif !defined(__ANDROID__)
    installSignalHandlers();
#endif
}

}  // namespace port
