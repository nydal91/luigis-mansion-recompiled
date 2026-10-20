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
#include <fcntl.h>
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

std::mutex sFdM;
std::map<std::string, int> sFds;  // content URI -> open descriptor (kept for the process' lifetime)

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
    std::lock_guard lk(sFdM);
    if (auto it = sFds.find(stored); it != sFds.end()) {
        // Still valid? (the descriptor is ours; nothing else closes it)
        if (fcntl(it->second, F_GETFD) != -1) return "/proc/self/fd/" + std::to_string(it->second);
        sFds.erase(it);
    }
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
    sFds[stored] = fd;
    return "/proc/self/fd/" + std::to_string(fd);
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

void relaunch(bool setup) {
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
