// Android platform helpers (docs/ANDROID.md, port/android.h).
#ifdef __ANDROID__
#include "port/android.h"

#include <SDL3/SDL_system.h>
#include <android/log.h>
#include <jni.h>

#include <cerrno>
#include <cstdlib>
#include <thread>
#include <cstdio>
#include <map>
#include <mutex>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <vector>
#include <unistd.h>

namespace port::android {
namespace {

// LMActivity exports its folders as environment variables before libmain.so starts
// (android.system.Os.setenv in onCreate); SDL's own paths are the fallback.
std::filesystem::path envDir(const char* var, const char* fallback) {
    std::filesystem::path p;
    if (const char* v = std::getenv(var); v && v[0] == '/') p = v;
    else if (fallback) p = fallback;
    else p = ".";
    std::error_code ec;
    std::filesystem::create_directories(p, ec);
    return p;
}

// Calls a static method of the running activity's class (LMActivity). SDL hands out the
// activity as a local reference; using its class avoids FindClass, which only sees system
// classes on threads that were not started from Java.
template <typename F>
auto withActivity(F&& f) -> decltype(f(static_cast<JNIEnv*>(nullptr), jclass{})) {
    auto* env = static_cast<JNIEnv*>(SDL_GetAndroidJNIEnv());
    auto activity = static_cast<jobject>(SDL_GetAndroidActivity());
    if (!env || !activity) return decltype(f(env, jclass{})){};
    jclass cls = env->GetObjectClass(activity);
    auto r = f(env, cls);
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    env->DeleteLocalRef(cls);
    env->DeleteLocalRef(activity);
    return r;
}


}  // namespace

void redirectOutput() {
    static bool sDone = false;
    if (sDone) return;
    sDone = true;
    int fds[2];
    if (pipe(fds) != 0) return;
    const auto dir = filesDir();
    std::error_code ec;
    std::filesystem::rename(dir / "last_run.log", dir / "previous_run.log", ec);
    // Removed by sessionEnded() on a normal exit; still there after a crash, so LMActivity copies
    // the log to Downloads on the next start.
    std::fclose(std::fopen((dir / "session_active").c_str(), "w"));
    FILE* file = std::fopen((dir / "last_run.log").c_str(), "w");
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::setvbuf(stderr, nullptr, _IONBF, 0);
    std::thread([rd = fds[0], file] {
        std::string line;
        char buf[4096];
        for (;;) {
            const ssize_t n = read(rd, buf, sizeof(buf));
            if (n <= 0) {
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            if (file) {
                std::fwrite(buf, 1, size_t(n), file);
                std::fflush(file);
            }
            line.append(buf, size_t(n));
            for (size_t nl; (nl = line.find('\n')) != std::string::npos;) {
                __android_log_write(ANDROID_LOG_INFO, "LuigisMansion", line.substr(0, nl).c_str());
                line.erase(0, nl + 1);
            }
        }
    }).detach();
}

std::filesystem::path filesDir() { return envDir("LM_ANDROID_FILES", SDL_GetAndroidInternalStoragePath()); }
std::filesystem::path cacheDir() { return envDir("LM_ANDROID_CACHE", SDL_GetAndroidCachePath()); }
std::filesystem::path resourceDir() { return envDir("LM_ANDROID_RES", SDL_GetAndroidInternalStoragePath()); }
std::filesystem::path nativeLibDir() { return envDir("LM_ANDROID_LIBDIR", nullptr); }

std::string openDiscPath(const std::string& stored, std::string* error) {
    if (stored.rfind("content://", 0) != 0) return stored;
    // Opening once through LMActivity checks the file is still there and makes the read
    // permission persistent. The URI itself is then what gets opened: SDL_IOFromFile opens
    // content:// URIs through the ContentResolver and reads that descriptor directly
    // (re-opening /proc/self/fd/<n> fails for files the app can't see by path).
    const int fd = withActivity([&](JNIEnv* env, jclass cls) -> int {
        jmethodID m = env->GetStaticMethodID(cls, "openContentFd", "(Ljava/lang/String;)I");
        if (!m) return -1;
        jstring s = env->NewStringUTF(stored.c_str());
        const int r = env->CallStaticIntMethod(cls, m, s);
        env->DeleteLocalRef(s);
        return r;
    });
    if (fd < 0) {
        if (error) *error = "The disc image can no longer be opened (was it moved or deleted?). Choose it again.";
        return {};
    }
    close(fd);
    return stored;
}

std::string displayName(const std::string& stored) {
    if (stored.rfind("content://", 0) != 0) return stored;
    std::string name = withActivity([&](JNIEnv* env, jclass cls) -> std::string {
        jmethodID m = env->GetStaticMethodID(cls, "contentDisplayName", "(Ljava/lang/String;)Ljava/lang/String;");
        if (!m) return {};
        jstring s = env->NewStringUTF(stored.c_str());
        auto r = static_cast<jstring>(env->CallStaticObjectMethod(cls, m, s));
        env->DeleteLocalRef(s);
        if (!r) return {};
        const char* c = env->GetStringUTFChars(r, nullptr);
        std::string out = c ? c : "";
        env->ReleaseStringUTFChars(r, c);
        env->DeleteLocalRef(r);
        return out;
    });
    return name.empty() ? stored : name;
}

namespace {
struct ThreadTimes {
    std::string name;
    unsigned long long ticks = 0;
};

// utime + stime (clock ticks) and comm of a /proc/.../stat file.
bool readStat(const std::string& path, ThreadTimes& out) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[1024];
    const size_t n = std::fread(buf, 1, sizeof(buf) - 1, f);
    std::fclose(f);
    buf[n] = 0;
    const char* open = std::strchr(buf, '(');
    const char* close = std::strrchr(buf, ')');
    if (!open || !close) return false;
    out.name.assign(open + 1, close);
    // Fields after ")": state(3) ... utime(14) stime(15).
    const char* p = close + 2;
    unsigned long long v[13] = {};
    char state = 0;
    if (std::sscanf(p, "%c %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %llu", &state, &v[0], &v[1], &v[2],
                    &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11]) < 13)
        return false;
    out.ticks = v[10] + v[11];
    return true;
}
}  // namespace

void startPerfLog() {
    std::thread([] {
        const double hz = double(sysconf(_SC_CLK_TCK));
        std::map<std::string, unsigned long long> last;  // tid -> ticks
        unsigned long long lastProc = 0;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            ThreadTimes proc;
            if (!readStat("/proc/self/stat", proc)) continue;
            std::vector<std::pair<double, std::string>> busy;
            std::map<std::string, unsigned long long> now;
            if (DIR* d = opendir("/proc/self/task")) {
                while (dirent* e = readdir(d)) {
                    if (e->d_name[0] == '.') continue;
                    ThreadTimes t;
                    if (!readStat(std::string("/proc/self/task/") + e->d_name + "/stat", t)) continue;
                    now[e->d_name] = t.ticks;
                    if (auto it = last.find(e->d_name); it != last.end() && t.ticks > it->second)
                        busy.emplace_back(double(t.ticks - it->second) / hz / 10.0 * 100.0, t.name);
                }
                closedir(d);
            }
            last = std::move(now);
            std::sort(busy.begin(), busy.end(), [](auto& a, auto& b) { return a.first > b.first; });
            const int thermal = withActivity([](JNIEnv* env, jclass cls) -> int {
                jmethodID m = env->GetStaticMethodID(cls, "thermalStatus", "()I");
                return m ? env->CallStaticIntMethod(cls, m) : -1;
            });
            static const char* kThermal[] = {"none", "light", "moderate", "severe", "critical", "emergency", "shutdown"};
            std::string top;
            for (size_t i = 0; i < busy.size() && i < 5; i++) {
                char b[96];
                std::snprintf(b, sizeof(b), "%s%s %.0f%%", i ? ", " : "", busy[i].second.c_str(), busy[i].first);
                top += b;
            }
            if (lastProc)
                std::printf("[perf] cpu %.0f%% (100%% = one core); busiest threads: %s; thermal %s\n",
                            double(proc.ticks - lastProc) / hz / 10.0 * 100.0, top.c_str(),
                            thermal >= 0 && thermal <= 6 ? kThermal[thermal] : "unknown");
            std::fflush(stdout);
            lastProc = proc.ticks;
        }
    }).detach();
}

void sessionEnded() {
    std::fflush(nullptr);
    std::error_code ec;
    std::filesystem::remove(filesDir() / "session_active", ec);
}

void shareLog() {
    std::fflush(stdout);
    withActivity([&](JNIEnv* env, jclass cls) -> int {
        if (jmethodID m = env->GetStaticMethodID(cls, "shareLog", "()V")) env->CallStaticVoidMethod(cls, m);
        return 0;
    });
}

void pickTexturePack(bool folder, const std::string& destDir) {
    withActivity([&](JNIEnv* env, jclass cls) -> int {
        jmethodID m = env->GetStaticMethodID(cls, "pickTexturePack", "(ZLjava/lang/String;)V");
        if (!m) return 0;
        jstring s = env->NewStringUTF(destDir.c_str());
        env->CallStaticVoidMethod(cls, m, jboolean(folder), s);
        env->DeleteLocalRef(s);
        return 0;
    });
}

ImportStatus texturePackImportStatus() {
    ImportStatus st;
    const std::string s = withActivity([](JNIEnv* env, jclass cls) -> std::string {
        jmethodID m = env->GetStaticMethodID(cls, "texturePackStatus", "()Ljava/lang/String;");
        if (!m) return {};
        auto r = static_cast<jstring>(env->CallStaticObjectMethod(cls, m));
        if (!r) return {};
        const char* c = env->GetStringUTFChars(r, nullptr);
        std::string out = c ? c : "";
        env->ReleaseStringUTFChars(r, c);
        env->DeleteLocalRef(r);
        return out;
    });
    // "<state>|<count>|<message>"
    const auto a = s.find('|'), b = a == std::string::npos ? a : s.find('|', a + 1);
    if (b == std::string::npos) return st;
    st.state = ImportState(std::atoi(s.substr(0, a).c_str()));
    st.count = std::atoi(s.substr(a + 1, b - a - 1).c_str());
    st.message = s.substr(b + 1);
    return st;
}

void relaunch(bool setup) {
    sessionEnded();
    withActivity([&](JNIEnv* env, jclass cls) -> int {
        if (jmethodID m = env->GetStaticMethodID(cls, "relaunch", "(Z)V"))
            env->CallStaticVoidMethod(cls, m, jboolean(setup));
        return 0;
    });
    std::fflush(nullptr);
    std::_Exit(0);
}

}  // namespace port::android
#endif
