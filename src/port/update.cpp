// Self-updater: see update.h and docs/SETUP.md ("Updates").
//
// Contract with the release pipeline (tools/release_windows.ps1, tools/release_linux.sh):
//   - releases are GitHub releases of kRepo (rob646464/luigis-mansion-recompiled; the old name
//     luigis-mansion-pc-port redirects) tagged v<version> (e.g. v1.1, v1.0.1); drafts and
//     prereleases are ignored;
//   - assets: LuigisMansionRecompiled-<version>-windows-x64.zip and
//     LuigisMansionRecompiled-<version>-linux-x86_64.tar.gz (from v1.1; v1.0/v1.0.1 used the prefix
//     LuigisMansionPC-, still accepted),
//     each holding one folder (or the files at the top) with the install layout of docs/SETUP.md;
//   - integrity: the API's asset "digest" ("sha256:<hex>"), else the release body's download table
//     row "| `<asset name>` | ... | `<sha256 hex>` |".
// Transport: WinHTTP + BCrypt on Windows; libcurl loaded at run time (dlopen) on Linux, and the
// system tar for extraction on both (Windows 10 1803+ ships bsdtar as System32\tar.exe).
#include "port/update.h"

#include "port/install.h"
#include "port/settings.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>
#include <winhttp.h>
#else
#include <dlfcn.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

#ifndef LM_VERSION
#define LM_VERSION "0.0"
#endif

namespace fs = std::filesystem;

namespace port::update {
namespace {

constexpr const char* kRepo = "rob646464/luigis-mansion-recompiled";
constexpr const char* kApiLatest = "https://api.github.com/repos/rob646464/luigis-mansion-recompiled/releases/latest";
// Release downloads of the repository (new name, and the old one GitHub redirects).
constexpr const char* kAssetPrefixes[] = {"https://github.com/rob646464/luigis-mansion-recompiled/releases/download/",
                                          "https://github.com/rob646464/luigis-mansion-pc-port/releases/download/"};
// Package names: "<prefix><version><kAssetSuffix>".
constexpr const char* kPackagePrefixes[] = {"LuigisMansionRecompiled-", "LuigisMansionPC-"};
// Hosts a release download may redirect to (GitHub's asset storage).
constexpr const char* kAssetHosts[] = {"github.com", "objects.githubusercontent.com",
                                       "release-assets.githubusercontent.com"};
#ifdef _WIN32
constexpr const char* kAssetSuffix = "-windows-x64.zip";
constexpr const char* kExeName = "LuigisMansionRecomp.exe";
const char* const kExpected[] = {"LuigisMansionRecomp.exe", "pipelines.bin", "llvm/bin/clang.exe",
                                 "llvm/bin/lld-link.exe"};
#else
constexpr const char* kAssetSuffix = "-linux-x86_64.tar.gz";
constexpr const char* kExeName = "LuigisMansionRecomp";
const char* const kExpected[] = {"LuigisMansionRecomp", "pipelines.bin", "llvm/bin/clang", "llvm/bin/ld.lld"};
constexpr const char* kLauncher = "LuigisMansionRecompiled.sh";
constexpr const char* kOldLauncher = "LuigisMansionPC.sh";  // 1.0 / 1.0.1 (same file in both)
constexpr const char* kOldLauncherSha = "efbcb131f2018dcbeb49fc937a5fafa1ec425e55bc159cf4ced3445a7b12dc36";
#endif
// Inside the install folder. Short, and the package's top folder is flattened into it, so the
// deepest staged file is only 4 characters longer than its installed path (docs/SETUP.md "Long
// paths"; up to 1.1.1 "update.tmp\new\<package>\" added 74 and failed past MAX_PATH).
constexpr const char* kStagingName = "upd";
constexpr const char* kOldStagingName = "update.tmp";  // up to 1.1.1: removed when found
#ifdef _WIN32
// tar.exe is not long-path aware: its working directory (the staging folder) must stay below
// MAX_PATH. 247 = CreateDirectory's MAX_PATH - 12, with room to spare.
constexpr size_t kMaxStagingPath = 247;
#endif
constexpr const char* kCleanupList = "update-cleanup.txt";  // *.old files to delete at the next start

void ulog(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::fputs("[update] ", stdout);
    std::vfprintf(stdout, fmt, ap);
    va_end(ap);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

// UTF-8 for logs and messages, without the \\?\ prefix (port::longPath).
std::string utf8(const fs::path& p) { return port::displayPath(p); }
fs::path fromUtf8(const std::string& s) { return fs::path(std::u8string(s.begin(), s.end())); }

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// ---- JSON (just enough for the GitHub release API) -----------------------------------------
struct Json {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Json> a;
    std::vector<std::string> keys;  // object members (two vectors: a pair of an incomplete Json
    std::vector<Json> vals;         // is not allowed in a member vector with libstdc++)

    const Json& operator[](const char* key) const {
        static const Json kNull;
        if (type == Object)
            for (size_t i = 0; i < keys.size(); i++)
                if (keys[i] == key) return vals[i];
        return kNull;
    }
    std::string str() const { return type == String ? s : std::string(); }
    bool truthy() const { return type == Bool ? b : false; }
};

struct JsonParser {
    const char* p;
    const char* end;
    int depth = 0;
    bool ok = true;

    void ws() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    }
    bool lit(const char* t) {
        const size_t n = std::strlen(t);
        if (size_t(end - p) < n || std::memcmp(p, t, n) != 0) return ok = false;
        p += n;
        return true;
    }
    static void putUtf8(std::string& out, unsigned cp) {
        if (cp < 0x80) out += char(cp);
        else if (cp < 0x800) {
            out += char(0xC0 | cp >> 6);
            out += char(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += char(0xE0 | cp >> 12);
            out += char(0x80 | (cp >> 6 & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        } else {
            out += char(0xF0 | cp >> 18);
            out += char(0x80 | (cp >> 12 & 0x3F));
            out += char(0x80 | (cp >> 6 & 0x3F));
            out += char(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(unsigned& v) {
        if (end - p < 4) return ok = false;
        v = 0;
        for (int i = 0; i < 4; i++) {
            const char c = *p++;
            v <<= 4;
            if (c >= '0' && c <= '9') v |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') v |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= unsigned(c - 'A' + 10);
            else return ok = false;
        }
        return true;
    }
    bool string(std::string& out) {
        if (p >= end || *p != '"') return ok = false;
        p++;
        while (p < end && *p != '"') {
            if (*p != '\\') {
                out += *p++;
                continue;
            }
            if (++p >= end) return ok = false;
            const char c = *p++;
            switch (c) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned cp = 0;
                if (!hex4(cp)) return false;
                if (cp >= 0xD800 && cp < 0xDC00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                    p += 2;
                    unsigned lo = 0;
                    if (!hex4(lo)) return false;
                    if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else cp = 0xFFFD;
                } else if (cp >= 0xD800 && cp < 0xE000) {
                    cp = 0xFFFD;
                }
                putUtf8(out, cp);
                break;
            }
            default: return ok = false;
            }
        }
        if (p >= end) return ok = false;
        p++;
        return true;
    }
    Json value() {
        Json v;
        ws();
        if (p >= end || ++depth > 64) {
            ok = false;
            return v;
        }
        const char c = *p;
        if (c == '{') {
            v.type = Json::Object;
            p++;
            ws();
            if (p < end && *p == '}') p++;
            else
                while (ok) {
                    ws();
                    std::string k;
                    if (!string(k)) break;
                    ws();
                    if (p >= end || *p != ':') {
                        ok = false;
                        break;
                    }
                    p++;
                    Json item = value();
                    v.keys.push_back(std::move(k));
                    v.vals.push_back(std::move(item));
                    ws();
                    if (p < end && *p == ',') {
                        p++;
                        continue;
                    }
                    if (p < end && *p == '}') {
                        p++;
                        break;
                    }
                    ok = false;
                }
        } else if (c == '[') {
            v.type = Json::Array;
            p++;
            ws();
            if (p < end && *p == ']') p++;
            else
                while (ok) {
                    v.a.push_back(value());
                    ws();
                    if (p < end && *p == ',') {
                        p++;
                        continue;
                    }
                    if (p < end && *p == ']') {
                        p++;
                        break;
                    }
                    ok = false;
                }
        } else if (c == '"') {
            v.type = Json::String;
            string(v.s);
        } else if (c == 't') {
            v.type = Json::Bool;
            v.b = lit("true");
        } else if (c == 'f') {
            v.type = Json::Bool;
            lit("false");
        } else if (c == 'n') {
            lit("null");
        } else {
            v.type = Json::Number;
            const char* s = p;
            while (p < end && (std::isdigit(static_cast<unsigned char>(*p)) || *p == '-' || *p == '+' || *p == '.' ||
                               *p == 'e' || *p == 'E'))
                p++;
            if (p == s) ok = false;
            else v.n = std::strtod(std::string(s, p).c_str(), nullptr);
        }
        depth--;
        return v;
    }
};

bool parseJson(const std::string& text, Json& out) {
    JsonParser jp{text.data(), text.data() + text.size()};
    out = jp.value();
    jp.ws();
    return jp.ok && jp.p == jp.end;
}

// ---- SHA-256 -------------------------------------------------------------------------------
#ifdef _WIN32
// BCrypt (Windows' own implementation).
bool sha256File(const fs::path& file, std::string& hex, const std::atomic<bool>& cancel) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return false;
    bool ok = BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0) == 0;
    std::ifstream in(file, std::ios::binary);
    std::vector<char> buf(1 << 20);
    while (ok && in && !cancel) {
        in.read(buf.data(), std::streamsize(buf.size()));
        const auto n = in.gcount();
        if (n > 0) ok = BCryptHashData(h, reinterpret_cast<PUCHAR>(buf.data()), ULONG(n), 0) == 0;
    }
    unsigned char d[32];
    ok = ok && !cancel && BCryptFinishHash(h, d, 32, 0) == 0;
    if (h) BCryptDestroyHash(h);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!ok) return false;
    static const char* kHex = "0123456789abcdef";
    hex.clear();
    for (unsigned char c : d) {
        hex += kHex[c >> 4];
        hex += kHex[c & 15];
    }
    return true;
}
#else
// FIPS 180-4 SHA-256 (written from the specification).
struct Sha256 {
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                          0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    unsigned char block[64];
    size_t fill = 0;
    std::uint64_t bits = 0;

    static std::uint32_t rotr(std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
    void compress(const unsigned char* b) {
        static const std::uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        std::uint32_t w[64];
        for (int i = 0; i < 16; i++)
            w[i] = std::uint32_t(b[i * 4]) << 24 | std::uint32_t(b[i * 4 + 1]) << 16 | std::uint32_t(b[i * 4 + 2]) << 8 |
                   b[i * 4 + 3];
        for (int i = 16; i < 64; i++) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], bb = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; i++) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t mj = (a & bb) ^ (a & c) ^ (bb & c);
            const std::uint32_t t2 = S0 + mj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = bb;
            bb = a;
            a = t1 + t2;
        }
        h[0] += a; h[1] += bb; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    void update(const unsigned char* data, size_t n) {
        bits += std::uint64_t(n) * 8;
        while (n) {
            const size_t take = std::min(n, 64 - fill);
            std::memcpy(block + fill, data, take);
            fill += take;
            data += take;
            n -= take;
            if (fill == 64) {
                compress(block);
                fill = 0;
            }
        }
    }
    void finish(unsigned char out[32]) {
        const std::uint64_t total = bits;
        const unsigned char one = 0x80, zero = 0;
        update(&one, 1);
        while (fill != 56) update(&zero, 1);
        unsigned char len[8];
        for (int i = 0; i < 8; i++) len[i] = (unsigned char)(total >> (56 - 8 * i));
        update(len, 8);
        for (int i = 0; i < 8; i++)
            for (int j = 0; j < 4; j++) out[i * 4 + j] = (unsigned char)(h[i] >> (24 - 8 * j));
    }
};

bool sha256File(const fs::path& file, std::string& hex, const std::atomic<bool>& cancel) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    Sha256 s;
    std::vector<char> buf(1 << 20);
    while (in && !cancel) {
        in.read(buf.data(), std::streamsize(buf.size()));
        if (in.gcount() > 0) s.update(reinterpret_cast<const unsigned char*>(buf.data()), size_t(in.gcount()));
    }
    if (cancel) return false;
    unsigned char d[32];
    s.finish(d);
    static const char* kHex = "0123456789abcdef";
    hex.clear();
    for (unsigned char c : d) {
        hex += kHex[c >> 4];
        hex += kHex[c & 15];
    }
    return true;
}
#endif

// ---- HTTP GET (one request, redirects not followed: the caller checks each hop) -----------
struct Url {
    std::string scheme, host, path;  // path includes the query
    int port = 0;
};

bool parseUrl(const std::string& url, Url& u) {
    const auto se = url.find("://");
    if (se == std::string::npos) return false;
    u.scheme = lower(url.substr(0, se));
    const auto hs = se + 3;
    auto pe = url.find_first_of("/?#", hs);
    std::string hostport = url.substr(hs, pe == std::string::npos ? std::string::npos : pe - hs);
    if (hostport.empty() || hostport.find('@') != std::string::npos) return false;  // no credentials
    u.path = pe == std::string::npos ? "/" : url.substr(pe);
    if (const auto h = u.path.find('#'); h != std::string::npos) u.path.resize(h);
    if (u.path.empty() || u.path[0] != '/') u.path = "/" + u.path;
    u.port = u.scheme == "https" ? 443 : 80;
    if (const auto c = hostport.rfind(':'); c != std::string::npos && hostport.find(']') == std::string::npos) {
        u.port = std::atoi(hostport.c_str() + c + 1);
        hostport.resize(c);
    }
    u.host = lower(hostport);
    return (u.scheme == "https" || u.scheme == "http") && !u.host.empty() && u.port > 0 && u.port < 65536;
}

struct Response {
    int status = 0;
    std::string error;  // transport failure (no HTTP answer); empty otherwise
    bool cancelled = false;
    std::map<std::string, std::string> headers;  // lower-case names
    std::string header(const char* name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
};

// sink(data, size) returns false to stop (cancel). timeoutMs: per connect/receive step; the
// request also stops after totalMs (0 = no limit).
using Sink = std::function<bool(const char*, size_t)>;

std::string userAgent() { return std::string("LuigisMansionRecompiled/") + buildVersion() + " (updater)"; }

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring w(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), w.data(), n);
    return w;
}
std::string narrow(const wchar_t* w, size_t n) {
    if (!n) return {};
    const int len = WideCharToMultiByte(CP_UTF8, 0, w, int(n), nullptr, 0, nullptr, nullptr);
    std::string s(size_t(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, int(n), s.data(), len, nullptr, nullptr);
    return s;
}

std::string winHttpError(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_NAME_NOT_RESOLVED: return "could not resolve the server name (offline?)";
    case ERROR_WINHTTP_CANNOT_CONNECT: return "could not connect (offline or blocked?)";
    case ERROR_WINHTTP_TIMEOUT: return "the connection timed out";
    case ERROR_WINHTTP_CONNECTION_ERROR: return "the connection was reset";
    case ERROR_WINHTTP_SECURE_FAILURE: return "the secure connection failed (certificate)";
    default: {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "network error %lu", e);
        return buf;
    }
    }
}

bool httpGet(const std::string& url, const std::vector<std::string>& headers, int timeoutMs, int totalMs,
             const Sink& sink, Response& r) {
    Url u;
    if (!parseUrl(url, u)) {
        r.error = "bad URL";
        return false;
    }
    const auto start = std::chrono::steady_clock::now();
    HINTERNET s = WinHttpOpen(widen(userAgent()).c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                              WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s)  // before Windows 8.1
        s = WinHttpOpen(widen(userAgent()).c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!s) {
        r.error = winHttpError(GetLastError());
        return false;
    }
    WinHttpSetTimeouts(s, timeoutMs, timeoutMs, timeoutMs, timeoutMs);
    HINTERNET c = WinHttpConnect(s, widen(u.host).c_str(), INTERNET_PORT(u.port), 0);
    HINTERNET q = c ? WinHttpOpenRequest(c, L"GET", widen(u.path).c_str(), nullptr, WINHTTP_NO_REFERER,
                                         WINHTTP_DEFAULT_ACCEPT_TYPES, u.scheme == "https" ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    bool ok = q != nullptr;
    if (ok) {
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        WinHttpSetOption(q, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof(policy));
        std::string all;
        for (auto& h : headers) all += h + "\r\n";
        if (!all.empty()) WinHttpAddRequestHeaders(q, widen(all).c_str(), DWORD(-1), WINHTTP_ADDREQ_FLAG_ADD);
        ok = WinHttpSendRequest(q, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
             WinHttpReceiveResponse(q, nullptr);
    }
    if (!ok) r.error = winHttpError(GetLastError());
    if (ok) {
        DWORD code = 0, len = sizeof(code);
        WinHttpQueryHeaders(q, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                            &code, &len, WINHTTP_NO_HEADER_INDEX);
        r.status = int(code);
        // All headers: "Name: value\r\n" lines.
        len = 0;
        WinHttpQueryHeaders(q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, nullptr, &len,
                            WINHTTP_NO_HEADER_INDEX);
        if (len) {
            std::wstring raw(len / sizeof(wchar_t) + 1, L'\0');
            if (WinHttpQueryHeaders(q, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX, raw.data(), &len,
                                    WINHTTP_NO_HEADER_INDEX)) {
                const std::string text = narrow(raw.data(), len / sizeof(wchar_t));
                size_t pos = 0;
                while (pos < text.size()) {
                    size_t e = text.find("\r\n", pos);
                    if (e == std::string::npos) e = text.size();
                    const std::string line = text.substr(pos, e - pos);
                    pos = e + 2;
                    const auto colon = line.find(':');
                    if (colon == std::string::npos) continue;
                    std::string v = line.substr(colon + 1);
                    while (!v.empty() && v.front() == ' ') v.erase(v.begin());
                    r.headers[lower(line.substr(0, colon))] = v;
                }
            }
        }
        std::vector<char> buf(256 * 1024);
        for (;;) {
            if (totalMs > 0 && std::chrono::steady_clock::now() - start > std::chrono::milliseconds(totalMs)) {
                r.error = "the connection timed out";
                ok = false;
                break;
            }
            DWORD got = 0;
            if (!WinHttpReadData(q, buf.data(), DWORD(buf.size()), &got)) {
                r.error = winHttpError(GetLastError());
                ok = false;
                break;
            }
            if (got == 0) break;
            if (!sink(buf.data(), got)) {
                r.cancelled = true;
                ok = false;
                break;
            }
        }
    }
    if (q) WinHttpCloseHandle(q);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return ok;
}
#else
// libcurl, loaded at run time: no build dependency, and a system without it can still play
// (the menu then offers the download page). Option numbers are libcurl's stable ABI values.
struct CurlApi {
    void* lib = nullptr;
    int (*global_init)(long) = nullptr;
    void* (*easy_init)() = nullptr;
    int (*easy_setopt)(void*, int, ...) = nullptr;
    int (*easy_perform)(void*) = nullptr;
    int (*easy_getinfo)(void*, int, ...) = nullptr;
    void (*easy_cleanup)(void*) = nullptr;
    void* (*slist_append)(void*, const char*) = nullptr;
    void (*slist_free_all)(void*) = nullptr;
    const char* (*easy_strerror)(int) = nullptr;
};
enum : int {
    kOptWriteData = 10001, kOptUrl = 10002, kOptUserAgent = 10018, kOptHttpHeader = 10023, kOptHeaderData = 10029,
    kOptWriteFunction = 20011, kOptHeaderFunction = 20079, kOptLowSpeedLimit = 19, kOptLowSpeedTime = 20,
    kOptNoProgress = 43, kOptFollowLocation = 52, kOptNoSignal = 99, kOptTimeoutMs = 155, kOptConnectTimeoutMs = 156,
    kInfoResponseCode = 0x200000 + 2,
    kErrResolve = 6, kErrConnect = 7, kErrWrite = 23, kErrTimeout = 28, kErrSsl = 35, kErrPeerCert = 60,
};

const CurlApi* curl() {
    static CurlApi api;
    static std::once_flag once;
    std::call_once(once, [] {
        for (const char* name : {"libcurl.so.4", "libcurl-gnutls.so.4", "libcurl-nss.so.4", "libcurl.so"}) {
            api.lib = dlopen(name, RTLD_NOW | RTLD_LOCAL);
            if (api.lib) {
                ulog("using %s", name);
                break;
            }
        }
        if (!api.lib) return;
        auto sym = [](const char* n) { return dlsym(api.lib, n); };
        api.global_init = reinterpret_cast<int (*)(long)>(sym("curl_global_init"));
        api.easy_init = reinterpret_cast<void* (*)()>(sym("curl_easy_init"));
        api.easy_setopt = reinterpret_cast<int (*)(void*, int, ...)>(sym("curl_easy_setopt"));
        api.easy_perform = reinterpret_cast<int (*)(void*)>(sym("curl_easy_perform"));
        api.easy_getinfo = reinterpret_cast<int (*)(void*, int, ...)>(sym("curl_easy_getinfo"));
        api.easy_cleanup = reinterpret_cast<void (*)(void*)>(sym("curl_easy_cleanup"));
        api.slist_append = reinterpret_cast<void* (*)(void*, const char*)>(sym("curl_slist_append"));
        api.slist_free_all = reinterpret_cast<void (*)(void*)>(sym("curl_slist_free_all"));
        api.easy_strerror = reinterpret_cast<const char* (*)(int)>(sym("curl_easy_strerror"));
        if (!api.global_init || !api.easy_init || !api.easy_setopt || !api.easy_perform || !api.easy_getinfo ||
            !api.easy_cleanup || !api.slist_append || !api.slist_free_all || !api.easy_strerror) {
            dlclose(api.lib);
            api.lib = nullptr;
            return;
        }
        api.global_init(3);  // CURL_GLOBAL_DEFAULT
    });
    return api.lib ? &api : nullptr;
}

struct CurlCtx {
    const Sink* sink;
    Response* r;
};
size_t curlWrite(char* data, size_t size, size_t n, void* user) {
    auto* c = static_cast<CurlCtx*>(user);
    if (!(*c->sink)(data, size * n)) {
        c->r->cancelled = true;
        return 0;
    }
    return size * n;
}
size_t curlHeader(char* data, size_t size, size_t n, void* user) {
    auto* c = static_cast<CurlCtx*>(user);
    std::string line(data, size * n);
    while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
    const auto colon = line.find(':');
    if (colon != std::string::npos) {
        std::string v = line.substr(colon + 1);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin());
        c->r->headers[lower(line.substr(0, colon))] = v;
    }
    return size * n;
}

bool httpGet(const std::string& url, const std::vector<std::string>& headers, int timeoutMs, int totalMs,
             const Sink& sink, Response& r) {
    const CurlApi* api = curl();
    if (!api) {
        r.error = "libcurl was not found (install the libcurl4 package)";
        return false;
    }
    void* h = api->easy_init();
    if (!h) {
        r.error = "libcurl could not start";
        return false;
    }
    CurlCtx ctx{&sink, &r};
    void* list = nullptr;
    for (auto& hd : headers) list = api->slist_append(list, hd.c_str());
    const std::string ua = userAgent();
    api->easy_setopt(h, kOptUrl, url.c_str());
    api->easy_setopt(h, kOptUserAgent, ua.c_str());
    if (list) api->easy_setopt(h, kOptHttpHeader, list);
    api->easy_setopt(h, kOptFollowLocation, 0L);
    api->easy_setopt(h, kOptNoSignal, 1L);
    api->easy_setopt(h, kOptNoProgress, 1L);
    api->easy_setopt(h, kOptConnectTimeoutMs, long(timeoutMs));
    if (totalMs > 0) api->easy_setopt(h, kOptTimeoutMs, long(totalMs));
    api->easy_setopt(h, kOptLowSpeedLimit, 1L);
    api->easy_setopt(h, kOptLowSpeedTime, long(std::max(1, timeoutMs / 1000)));
    api->easy_setopt(h, kOptWriteFunction, &curlWrite);
    api->easy_setopt(h, kOptWriteData, &ctx);
    api->easy_setopt(h, kOptHeaderFunction, &curlHeader);
    api->easy_setopt(h, kOptHeaderData, &ctx);
    const int rc = api->easy_perform(h);
    long code = 0;
    api->easy_getinfo(h, kInfoResponseCode, &code);
    r.status = int(code);
    api->easy_cleanup(h);
    if (list) api->slist_free_all(list);
    if (rc == 0) return true;
    if (rc == kErrWrite && r.cancelled) return false;
    switch (rc) {
    case kErrResolve: r.error = "could not resolve the server name (offline?)"; break;
    case kErrConnect: r.error = "could not connect (offline or blocked?)"; break;
    case kErrTimeout: r.error = "the connection timed out"; break;
    case kErrSsl:
    case kErrPeerCert: r.error = std::string("the secure connection failed: ") + api->easy_strerror(rc); break;
    default: r.error = api->easy_strerror(rc); break;
    }
    return false;
}
#endif

bool hostAllowed(const std::string& host) {
    for (const char* h : kAssetHosts)
        if (host == h) return true;
    return false;
}

// ---- Feed ----------------------------------------------------------------------------------
struct Candidate {
    Info info;
    std::string assetUrl;
    std::string expectedSha;  // lower-case hex, "" if the release gives none
    std::string shaSource;    // "digest" / "release notes"
};

const std::string& feedOverride() {
    static const std::string s = [] {
        const char* v = std::getenv("LM_UPDATE_FEED");
        return std::string(v ? v : "");
    }();
    return s;
}

bool isHex64(const std::string& s) {
    if (s.size() != 64) return false;
    for (char c : s)
        if (!std::isxdigit(static_cast<unsigned char>(c))) return false;
    return true;
}

// The download table of the release notes: "| `<name>` | <platform> | `<sha256>` |".
std::string shaFromBody(const std::string& body, const std::string& name) {
    size_t pos = 0;
    while (pos < body.size()) {
        size_t e = body.find('\n', pos);
        if (e == std::string::npos) e = body.size();
        const std::string line = body.substr(pos, e - pos);
        pos = e + 1;
        if (line.find("`" + name + "`") == std::string::npos && line.find("| " + name + " ") == std::string::npos)
            continue;
        for (size_t i = 0; i < line.size();) {
            if (!std::isxdigit(static_cast<unsigned char>(line[i]))) {
                i++;
                continue;
            }
            size_t j = i;
            while (j < line.size() && std::isxdigit(static_cast<unsigned char>(line[j]))) j++;
            if (j - i == 64) return lower(line.substr(i, 64));
            i = j;
        }
    }
    return {};
}

// Release body (GitHub markdown) as readable plain text lines.
std::string plainLine(std::string s) {
    // Typographic UTF-8 to ASCII; other characters beyond Latin-1 (emoji) dropped (menu font).
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        if (c < 0x80) {
            out += char(c);
            i++;
            continue;
        }
        const int n = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
        if (n == 0 || i + size_t(n) >= s.size()) {
            i++;  // stray continuation byte or truncated sequence
            continue;
        }
        unsigned cp = n == 3 ? c & 7u : n == 2 ? c & 15u : c & 31u;
        for (int k = 1; k <= n; k++) cp = cp << 6 | (static_cast<unsigned char>(s[i + size_t(k)]) & 63u);
        i += size_t(n) + 1;
        switch (cp) {
        case 0x2018: case 0x2019: out += '\''; break;
        case 0x201C: case 0x201D: out += '"'; break;
        case 0x2013: case 0x2014: out += '-'; break;
        case 0x2026: out += "..."; break;
        case 0x2022: out += '*'; break;
        case 0x2192: out += "->"; break;
        case 0x00A0: out += ' '; break;
        default:
            if (cp >= 0xA0 && cp < 0x100) JsonParser::putUtf8(out, cp);
            break;
        }
    }
    s.swap(out);
    // Inline markup: **bold**, __bold__, `code`, [text](url) -> text.
    out.clear();
    for (size_t i = 0; i < s.size(); i++) {
        if ((s[i] == '*' || s[i] == '_') && i + 1 < s.size() && s[i + 1] == s[i]) {
            i++;
            continue;
        }
        if (s[i] == '`') continue;
        if (s[i] == '[') {
            const auto close = s.find("](", i);
            const auto paren = close == std::string::npos ? std::string::npos : s.find(')', close);
            if (paren != std::string::npos) {
                out += s.substr(i + 1, close - i - 1);
                i = paren;
                continue;
            }
        }
        out += s[i];
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '\t')) out.pop_back();
    return out;
}

std::string notesToText(const std::string& body, std::vector<NoteLine>& lines) {
    std::string text;
    size_t pos = 0;
    bool comment = false;
    while (pos <= body.size()) {
        size_t e = body.find('\n', pos);
        if (e == std::string::npos) e = body.size();
        std::string line = body.substr(pos, e - pos);
        pos = e + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (comment || line.find("<!--") != std::string::npos) {
            comment = line.find("-->") == std::string::npos;
            continue;
        }
        size_t lead = 0;
        while (lead < line.size() && line[lead] == ' ') lead++;
        std::string t = line.substr(lead);
        NoteLine nl;
        if (t.empty()) {
            if (!lines.empty() && lines.back().kind != NoteLine::Blank) lines.push_back({NoteLine::Blank, {}, 0});
            continue;
        }
        if (t[0] == '#') {
            size_t h = 0;
            while (h < t.size() && t[h] == '#') h++;
            nl.kind = NoteLine::Heading;
            nl.text = plainLine(t.substr(h));
            while (!nl.text.empty() && nl.text.front() == ' ') nl.text.erase(nl.text.begin());
        } else if (t[0] == '|') {
            // Table row; the |---|---| separator is dropped.
            if (t.find_first_not_of("|-: ") == std::string::npos) continue;
            std::string row;
            size_t c = 1;
            while (c < t.size()) {
                size_t n = t.find('|', c);
                if (n == std::string::npos) n = t.size();
                std::string cell = plainLine(t.substr(c, n - c));
                while (!cell.empty() && cell.front() == ' ') cell.erase(cell.begin());
                if (!cell.empty()) row += (row.empty() ? "" : "   ") + cell;
                c = n + 1;
            }
            nl.kind = NoteLine::Text;
            nl.text = row;
        } else if ((t[0] == '-' || t[0] == '*' || t[0] == '+') && t.size() > 1 && t[1] == ' ') {
            nl.kind = NoteLine::Bullet;
            nl.indent = int(lead / 2);
            nl.text = plainLine(t.substr(2));
        } else if (t == "---" || t == "***") {
            continue;
        } else {
            nl.kind = NoteLine::Text;
            nl.text = plainLine(t);
        }
        if (nl.text.empty()) continue;
        text += (nl.kind == NoteLine::Bullet ? "- " : "") + nl.text + "\n";
        lines.push_back(std::move(nl));
    }
    while (!lines.empty() && lines.back().kind == NoteLine::Blank) lines.pop_back();
    return text;
}

bool releaseFromJson(const Json& r, Candidate& c) {
    if (r.type != Json::Object) return false;
    if (r["draft"].truthy() || r["prerelease"].truthy()) return false;
    std::string tag = r["tag_name"].str();
    if (tag.size() < 2 || (tag[0] != 'v' && tag[0] != 'V') || !std::isdigit(static_cast<unsigned char>(tag[1])))
        return false;
    c.info.tag = tag;
    c.info.version = tag.substr(1);
    c.info.title = plainLine(r["name"].str());
    c.info.page = r["html_url"].str();
    if (c.info.page.rfind("https://github.com/", 0) != 0)
        c.info.page = std::string("https://github.com/") + kRepo + "/releases/tag/" + tag;
    c.info.published = r["published_at"].str().substr(0, 10);
    const std::string body = r["body"].str();
    c.info.notes = notesToText(body, c.info.lines);
    for (const Json& a : r["assets"].a) {
        const std::string name = a["name"].str();
        bool prefixOk = false;
        for (const char* pre : kPackagePrefixes) prefixOk |= name.rfind(pre, 0) == 0;
        if (!prefixOk || name.size() <= std::strlen(kAssetSuffix) ||
            name.compare(name.size() - std::strlen(kAssetSuffix), std::string::npos, kAssetSuffix) != 0)
            continue;
        c.info.assetName = name;
        c.info.assetSize = a["size"].type == Json::Number ? std::uint64_t(a["size"].n) : 0;
        c.assetUrl = a["browser_download_url"].str();
        const std::string digest = lower(a["digest"].str());
        if (digest.rfind("sha256:", 0) == 0 && isHex64(digest.substr(7))) {
            c.expectedSha = digest.substr(7);
            c.shaSource = "the release's asset digest";
        } else if (std::string s = shaFromBody(body, name); !s.empty()) {
            c.expectedSha = s;
            c.shaSource = "the release notes";
        }
        break;
    }
    return true;
}

// ---- State ---------------------------------------------------------------------------------
std::mutex sM;
State sState;
Candidate sCand;
bool sLater = false;
bool sBusy = false;  // a worker runs (check / download)
std::atomic<bool> sCancel{false};
fs::path sStagedRoot;  // extracted package folder (Ready)

fs::path sExePath = [] {
#ifdef _WIN32
    wchar_t buf[MAX_PATH * 4];
    const DWORD n = GetModuleFileNameW(nullptr, buf, DWORD(std::size(buf)));
    return port::plainPath(fs::path(std::wstring(buf, n)));  // \\?\... when started by that name
#else
    std::error_code ec;
    return fs::read_symlink("/proc/self/exe", ec);
#endif
}();
// Extended-length (port::longPath): every file operation of the updater works past MAX_PATH, also
// when Windows' long path policy is off. Package-relative names are joined with under().
fs::path installDir() { return port::longPath(sExePath.parent_path()); }
fs::path under(const fs::path& dir, const std::string& rel) {
    fs::path p = dir / fromUtf8(rel);
    p.make_preferred();  // "llvm/bin/clang.exe": an extended path takes no forward slashes
    return p;
}

bool isPackaged() {
    std::error_code ec;
    const fs::path d = installDir();
#ifdef _WIN32
    return fs::exists(d / "llvm" / "bin" / "clang.exe", ec) && fs::exists(d / "pipelines.bin", ec);
#else
    return fs::exists(d / "llvm" / "bin" / "clang", ec) && fs::exists(d / "pipelines.bin", ec);
#endif
}

bool canWriteInstall() {
    const fs::path probe = installDir() / ".update-write-test";
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

void setPhase(Phase p, const std::string& msg) {
    std::lock_guard lk(sM);
    sState.phase = p;
    sState.message = msg;
}

std::string clockTime(long long epoch) {
    const std::time_t t = std::time_t(epoch);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M", &tm);
    return buf;
}

bool readFile(const fs::path& p, std::string& out) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return false;
    out.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

// Release JSON from GitHub (or the LM_UPDATE_FEED test feed). Returns "" and fills `body` on
// success, else a message for the player.
std::string fetchFeed(std::string& body) {
    std::string url = feedOverride().empty() ? std::string(kApiLatest) : feedOverride();
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        fs::path p = url.rfind("file://", 0) == 0 ? fromUtf8(url.substr(7)) : fromUtf8(url);
#ifdef _WIN32
        // file:///C:/x -> C:/x
        if (url.rfind("file:///", 0) == 0 && url.size() > 10 && url[9] == ':') p = fromUtf8(url.substr(8));
#endif
        ulog("reading test feed %s", utf8(p).c_str());
        return readFile(p, body) ? "" : "could not read the test feed " + utf8(p);
    }
    for (int hop = 0; hop < 4; hop++) {
        Url u;
        if (!parseUrl(url, u)) return "bad update URL";
        const bool local = u.host == "localhost" || u.host == "127.0.0.1";
        // Only HTTPS to GitHub's API (a test feed may be plain HTTP on this machine).
        if (!(u.scheme == "https" && (u.host == "api.github.com" || !feedOverride().empty())) &&
            !(local && !feedOverride().empty()))
            return "refusing update feed " + url;
        ulog("GET %s", url.c_str());
        Response r;
        body.clear();
        const bool ok = httpGet(url, {"Accept: application/vnd.github+json", "X-GitHub-Api-Version: 2022-11-28"},
                                8000, 15000,
                                [&](const char* d, size_t n) {
                                    if (body.size() + n > 4u << 20) return false;  // a release is a few KB
                                    body.append(d, n);
                                    return true;
                                },
                                r);
        if (!ok && r.status == 0) {
            ulog("no answer: %s", r.error.c_str());
            return "Could not reach GitHub: " + (r.error.empty() ? std::string("no answer") : r.error) + ".";
        }
        ulog("HTTP %d (rate limit remaining %s)", r.status, r.header("x-ratelimit-remaining").c_str());
        if ((r.status == 301 || r.status == 302 || r.status == 307 || r.status == 308) && !r.header("location").empty()) {
            url = r.header("location");
            continue;
        }
        if (r.status == 200 && ok) return "";
        if (r.status == 429 || (r.status == 403 && r.header("x-ratelimit-remaining") == "0")) {
            std::string when;
            if (const std::string reset = r.header("x-ratelimit-reset"); !reset.empty())
                when = " after " + clockTime(std::atoll(reset.c_str()));
            else if (const std::string ra = r.header("retry-after"); !ra.empty())
                when = " in " + ra + " s";
            return "GitHub's limit for update checks from this network was reached; try again" + when + ".";
        }
        if (r.status == 404) return "No published release was found.";
        return "GitHub answered HTTP " + std::to_string(r.status) + ".";
    }
    return "too many redirects";
}

void checkWorker(bool manual) {
    ulog("checking for updates (running %s%s, %s)", currentVersion(),
         std::strcmp(currentVersion(), buildVersion()) ? " via LM_UPDATE_VERSION" : "", manual ? "menu" : "startup");
    std::string body;
    std::string err = fetchFeed(body);
    Json j;
    Candidate best;
    bool found = false;
    if (err.empty()) {
        if (!parseJson(body, j)) err = "GitHub's answer could not be read.";
        else if (j.type == Json::Array) {
            for (const Json& r : j.a) {
                Candidate c;
                if (releaseFromJson(r, c) && (!found || compareVersions(c.info.version, best.info.version) > 0)) {
                    best = std::move(c);
                    found = true;
                }
            }
        } else {
            found = releaseFromJson(j, best);
        }
        if (err.empty() && !found) err = "No published release was found.";
    }
    std::lock_guard lk(sM);
    sBusy = false;
    sState.manual = manual;
    if (!err.empty()) {
        ulog("check failed: %s", err.c_str());
        sState.phase = Phase::CheckFailed;
        sState.message = err;
        return;
    }
    const int cmp = compareVersions(best.info.version, currentVersion());
    ulog("latest release %s (%s), asset %s, sha256 %s", best.info.tag.c_str(), best.info.published.c_str(),
         best.info.assetName.empty() ? "none for this platform" : best.info.assetName.c_str(),
         best.expectedSha.empty() ? "not given" : best.shaSource.c_str());
    if (cmp <= 0) {
        sState.phase = Phase::UpToDate;
        sState.message = std::string("You have the latest version (") + currentVersion() + ").";
        ulog("up to date (%s)", currentVersion());
        return;
    }
    sCand = best;
    sState.info = best.info;
    sState.phase = Phase::Available;
    sState.message.clear();
    if (best.info.assetName.empty())
        sState.message = "This release has no package for this system; use the download page.";
    const bool skipped = port::settings().updates.skipVersion == best.info.tag;
    ulog("update available: %s%s", best.info.tag.c_str(), skipped ? " (skipped by the player)" : "");
}

bool startWorker(std::function<void()> fn) {
    {
        std::lock_guard lk(sM);
        if (sBusy) return false;
        sBusy = true;
    }
    sCancel = false;
    std::thread(std::move(fn)).detach();
    return true;
}

// Runs tar in `cwd` (a plain path below MAX_PATH: tar is not long-path aware); its standard
// output goes to `out` when given. Returns the exit code, -1 when it could not start.
#ifdef _WIN32
int runTool(const std::wstring& exe, const std::wstring& args, const fs::path& cwd, const fs::path& out = {}) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{sizeof(si)};
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!out.empty()) {
        SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
        h = CreateFileW(out.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &sa, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) return -1;
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = h;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
        si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    PROCESS_INFORMATION pi{};
    const std::wstring dir = port::plainPath(cwd).wstring();
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, h != INVALID_HANDLE_VALUE,
                                   CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS, nullptr, dir.c_str(), &si, &pi);
    if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    if (!ok) return -1;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}
#else
int runTool(const std::vector<std::string>& args, const fs::path& cwd, const fs::path& out = {}) {
    std::vector<char*> argv;
    std::vector<std::string> store = args;
    for (auto& a : store) argv.push_back(a.data());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    if (!out.empty()) posix_spawn_file_actions_addopen(&fa, 1, out.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    // posix_spawn has no working directory before glibc 2.29's _np call: tar's -C does it.
    pid_t pid = -1;
    const int err = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    (void)cwd;
    if (err != 0) return -1;
    int status = 0;
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
#endif

// The single top folder every member of the package is in ("" when the files are at the top
// or there are several), from tar's list of member names.
std::string commonTopFolder(const std::string& list) {
    std::string top;
    bool any = false;
    size_t pos = 0;
    while (pos < list.size()) {
        size_t e = list.find('\n', pos);
        if (e == std::string::npos) e = list.size();
        std::string name = list.substr(pos, e - pos);
        pos = e + 1;
        if (!name.empty() && name.back() == '\r') name.pop_back();
        while (name.rfind("./", 0) == 0) name.erase(0, 2);
        if (name.empty()) continue;
        const size_t sep = name.find_first_of("/\\");
        if (sep == std::string::npos || sep == 0) return {};  // a file at the top
        const std::string first = name.substr(0, sep);
        if (any && first != top) return {};
        top = first;
        any = true;
    }
    return top;
}

// Unpacks `archive` (inside `dir`) into `dir`, flattening a single top folder into it.
std::string extract(const fs::path& archive, const fs::path& dir) {
    std::error_code ec;
    const fs::path list = dir / "list.txt";
#ifdef _WIN32
    // bsdtar (Windows 10 1803+) reads zip; run in the staging folder with relative names, so a
    // user name with characters outside the code page cannot confuse it. It refuses ".." and
    // absolute member names, and writes members deeper than MAX_PATH itself (\\?\ names) as long
    // as its working directory is below it (checked before the download).
    wchar_t sys[MAX_PATH];
    const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tar = std::wstring(sys, n) + L"\\tar.exe";
    if (!fs::exists(tar, ec)) return "this Windows has no tar.exe (Windows 10 1803 or newer is needed)";
    const std::wstring name = L"\"" + archive.filename().wstring() + L"\"";
    int rc = runTool(tar, L"-tf " + name, dir, list);
#else
    int rc = runTool({"tar", "-tzf", archive.string()}, dir, list);
#endif
    if (rc != 0) return "listing the package failed (tar exit " + std::to_string(rc) + ")";
    std::string names;
    readFile(list, names);
    fs::remove(list, ec);
    const std::string top = commonTopFolder(names);
    ulog("package top folder: %s", top.empty() ? "(none)" : top.c_str());
#ifdef _WIN32
    rc = runTool(tar, L"-xf " + name + (top.empty() ? L"" : L" --strip-components 1"), dir);
#else
    std::vector<std::string> args = {"tar", "-xzf", archive.string(), "-C", dir.string(), "--no-same-owner"};
    if (!top.empty()) args.push_back("--strip-components=1");
    rc = runTool(args, dir);
#endif
    if (rc != 0) return "unpacking failed (tar exit " + std::to_string(rc) + ")";
    // No links in a release package: refuse any (they could point outside the install).
    for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_symlink(ec)) return "the package contains a link (" + utf8(it->path().filename()) + "); refused";
    return {};
}

// The folder of the unpacked package holding the program: the top or its single subfolder.
fs::path packageRoot(const fs::path& dir) {
    std::error_code ec;
    if (fs::exists(dir / kExeName, ec)) return dir;
    fs::path found;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (e.is_directory(ec) && fs::exists(e.path() / kExeName, ec)) {
            if (!found.empty()) return {};
            found = e.path();
        }
    }
    return found;
}

void downloadWorker() {
    Candidate c;
    {
        std::lock_guard lk(sM);
        c = sCand;
    }
    const fs::path staging = installDir() / kStagingName;
    auto fail = [&](const std::string& msg) {
        ulog("download failed: %s", msg.c_str());
        std::error_code ec;
        fs::remove_all(staging, ec);
        std::lock_guard lk(sM);
        sBusy = false;
        sState.phase = Phase::Failed;
        sState.message = msg;
    };
    auto cancelled = [&] {
        ulog("cancelled");
        std::error_code ec;
        fs::remove_all(staging, ec);
        std::lock_guard lk(sM);
        sBusy = false;
        sState.phase = Phase::Available;
        sState.message = "Download cancelled.";
    };
    if (c.info.assetName.empty() || c.assetUrl.empty()) return fail("This release has no package for this system.");
    bool fromRepo = false;
    for (const char* pre : kAssetPrefixes) fromRepo |= c.assetUrl.rfind(pre, 0) == 0;
    if (!fromRepo ||
        c.assetUrl.find("/" + c.info.tag + "/" + c.info.assetName) == std::string::npos)
        return fail("The package is not a download of this game's GitHub release; refused.");
    if (c.expectedSha.empty())
        return fail("The release lists no SHA-256 for " + c.info.assetName +
                    ", so it cannot be verified; not installed. Use the download page.");
    std::error_code ec;
    fs::remove_all(staging, ec);
    fs::remove_all(installDir() / kOldStagingName, ec);
    fs::create_directories(staging, ec);
    if (!fs::is_directory(staging, ec)) return fail("Could not create " + utf8(staging) + ".");
    const fs::path part = staging / (c.info.assetName + ".part");
    const fs::path archive = staging / c.info.assetName;
    ulog("downloading %s (%llu bytes) to %s", c.assetUrl.c_str(), (unsigned long long)c.info.assetSize,
         utf8(staging).c_str());
    std::string url = c.assetUrl;
    bool done = false;
    for (int hop = 0; hop < 6 && !done; hop++) {
        Url u;
        if (!parseUrl(url, u) || u.scheme != "https" || !hostAllowed(u.host))
            return fail("The download was redirected to " + (u.host.empty() ? url : u.host) + ", which is not GitHub; refused.");
        std::FILE* f = nullptr;
#ifdef _WIN32
        _wfopen_s(&f, part.wstring().c_str(), L"wb");
#else
        f = std::fopen(part.c_str(), "wb");
#endif
        if (!f) return fail("Could not write " + utf8(part) + ".");
        std::uint64_t got = 0;
        bool writeErr = false;
        Response r;
        const bool ok = httpGet(url, {"Accept: application/octet-stream"}, 30000, 0,
                                [&](const char* d, size_t n) {
                                    if (sCancel) return false;
                                    if (std::fwrite(d, 1, n, f) != n) {
                                        writeErr = true;
                                        return false;
                                    }
                                    got += n;
                                    std::lock_guard lk(sM);
                                    sState.got = got;
                                    return true;
                                },
                                r);
        std::fclose(f);
        if (sCancel) return cancelled();
        if (writeErr) return fail("Writing the download failed (disk full?).");
        if (r.status >= 300 && r.status < 400) {
            const std::string loc = r.header("location");
            if (loc.empty()) return fail("GitHub sent a redirect without a target.");
            ulog("redirect %d -> host %s", r.status, [&] {
                Url t;
                return parseUrl(loc, t) ? t.host : std::string("?");
            }().c_str());
            url = loc;
            continue;
        }
        if (!ok) return fail("The download failed: " + (r.error.empty() ? "connection lost" : r.error) + ".");
        if (r.status != 200) return fail("The download failed: HTTP " + std::to_string(r.status) + ".");
        done = true;
        ulog("downloaded %llu bytes", (unsigned long long)got);
        if (c.info.assetSize && got != c.info.assetSize)
            return fail("The download is incomplete (" + std::to_string(got) + " of " +
                        std::to_string(c.info.assetSize) + " bytes).");
    }
    if (!done) return fail("Too many redirects.");

    setPhase(Phase::Verifying, "Checking the download...");
    std::string sha;
    if (!sha256File(part, sha, sCancel)) return sCancel ? cancelled() : fail("Could not read the download.");
    ulog("sha256 %s, expected %s (from %s)", sha.c_str(), c.expectedSha.c_str(), c.shaSource.c_str());
    if (sha != c.expectedSha)
        return fail("The download does not match the SHA-256 the release lists (damaged or tampered with); "
                    "it was deleted and nothing was installed.");
    fs::rename(part, archive, ec);
    if (ec) return fail("Could not rename the download: " + ec.message());

    setPhase(Phase::Extracting, "Unpacking...");
    if (std::string e = extract(archive, staging); !e.empty()) return fail("The package could not be unpacked: " + e + ".");
    fs::remove(archive, ec);
    if (fs::exists(archive, ec)) return fail("Could not remove the unpacked download " + utf8(archive) + ".");
    const fs::path root = packageRoot(staging);
    if (root.empty()) {
        ulog("no %s found in %s", kExeName, utf8(staging).c_str());
        return fail("The package does not contain the game program.");
    }
    for (const char* f : kExpected) {
        const fs::path p = under(root, f);
        if (!fs::is_regular_file(p, ec) || fs::file_size(p, ec) == 0)
            return fail(std::string("The package is missing ") + f + ".");
    }
    ulog("staged %s in %s", c.info.tag.c_str(), utf8(root).c_str());
    std::lock_guard lk(sM);
    sBusy = false;
    sStagedRoot = root;
    sState.phase = Phase::Ready;
    sState.message.clear();
}

#ifndef _WIN32
// The launcher was LuigisMansionPC.sh up to 1.0.1 (LuigisMansionRecompiled.sh since 1.1). Desktop
// entries and shortcuts may still start the old one, so an unmodified old launcher becomes a
// forwarder to the new one; one the player edited is left alone (it still starts the game).
void forwardOldLauncher(const fs::path& dir) {
    std::error_code ec;
    const fs::path oldSh = dir / kOldLauncher, newSh = dir / kLauncher;
    if (!fs::is_regular_file(oldSh, ec) || !fs::is_regular_file(newSh, ec)) return;
    const std::string forwarder = std::string("#!/bin/sh\n"
                                              "# The launcher's name up to 1.0.1; kept so existing shortcuts keep working.\n"
                                              "exec \"$(dirname \"$(readlink -f \"$0\")\")/") +
                                  kLauncher + "\" \"$@\"\n";
    {
        std::ifstream in(oldSh, std::ios::binary);
        const std::string have((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        if (have == forwarder) return;  // done before
    }
    std::string sha;
    const std::atomic<bool> noCancel{false};
    if (!sha256File(oldSh, sha, noCancel)) return;
    if (sha != kOldLauncherSha) {
        ulog("%s was changed by hand: left as it is (%s starts the game too)", kOldLauncher, kLauncher);
        return;
    }
    // Written next to it and renamed over it: a shell that is still running the old script
    // keeps reading its own (now unlinked) file.
    const fs::path tmp = dir / (std::string(kOldLauncher) + ".new");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out << forwarder;
        if (!out) {
            out.close();
            fs::remove(tmp, ec);
            return;
        }
    }
    fs::permissions(tmp, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                             fs::perms::others_read | fs::perms::others_exec,
                    ec);
    fs::rename(tmp, oldSh, ec);
    if (ec) {
        ulog("could not replace %s with a forwarder: %s", kOldLauncher, ec.message().c_str());
        fs::remove(tmp, ec);
        return;
    }
    ulog("%s (1.0.x launcher) now starts %s", kOldLauncher, kLauncher);
}
#endif

// Lexically: p (built as dir / a relative name) stays inside dir. Links are refused when the
// package is unpacked, so no component can lead elsewhere. (weakly_canonical drops the \\?\
// prefix of an extended path, and fails past MAX_PATH without the long path policy.)
bool sameOrInside(const fs::path& p, const fs::path& dir) {
    const fs::path a = p.lexically_normal(), b = dir.lexically_normal();
    auto it = a.begin();
    for (const auto& part : b) {
        if (part.empty()) continue;  // trailing separator of dir
        if (it == a.end()) return false;
#ifdef _WIN32
        if (_wcsicmp(it->c_str(), part.c_str()) != 0) return false;
#else
        if (*it != part) return false;
#endif
        ++it;
    }
    for (; it != a.end(); ++it)
        if (*it == "..") return false;
    return true;
}

}  // namespace

const char* buildVersion() { return LM_VERSION; }

const char* currentVersion() {
    static const std::string v = [] {
        const char* o = std::getenv("LM_UPDATE_VERSION");
        std::string s = o && *o ? o : LM_VERSION;
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(s.begin());
        return s;
    }();
    return v.c_str();
}

int compareVersions(const std::string& a, const std::string& b) {
    auto parts = [](std::string s) {
        if (!s.empty() && (s[0] == 'v' || s[0] == 'V')) s.erase(s.begin());
        if (const auto dash = s.find_first_of("-+ "); dash != std::string::npos) s.resize(dash);
        std::vector<long> v;
        size_t pos = 0;
        while (pos <= s.size()) {
            size_t e = s.find('.', pos);
            if (e == std::string::npos) e = s.size();
            v.push_back(std::atol(s.substr(pos, e - pos).c_str()));
            pos = e + 1;
        }
        while (!v.empty() && v.back() == 0) v.pop_back();
        return v;
    };
    const auto x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        const long p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p < q ? -1 : 1;
    }
    return 0;
}

State state() {
    std::lock_guard lk(sM);
    State s = sState;
    s.packaged = isPackaged();
    return s;
}

void startupCheck() {
#ifdef __ANDROID__
    ulog("Android: no updater (new versions come as a new APK; docs/ANDROID.md)");
    return;
#endif
    if (!port::settings().updates.checkAtStartup) {
        ulog("startup check off (F1 > Game)");
        return;
    }
    if (const char* v = std::getenv("LM_UPDATE_CHECK"); v && *v == '0') return;
    if (!isPackaged() && feedOverride().empty()) {
        ulog("not a release folder (%s): no startup check", utf8(installDir()).c_str());
        return;
    }
    {
        std::lock_guard lk(sM);
        sState.phase = Phase::Checking;
    }
    startWorker([] { checkWorker(false); });
}

void checkNow() {
    {
        std::lock_guard lk(sM);
        if (sBusy) return;
        sState.phase = Phase::Checking;
        sState.message = "Checking...";
        sLater = false;
    }
    startWorker([] { checkWorker(true); });
}

void download() {
    {
        std::lock_guard lk(sM);
        if (sBusy || (sState.phase != Phase::Available && sState.phase != Phase::Failed) || sCand.info.tag.empty()) return;
    }
    if (!isPackaged()) {
        setPhase(Phase::Failed, "This copy is not a release folder (a build tree?); updates install only into a "
                                "folder unpacked from a release. Use the download page.");
        return;
    }
    if (!canWriteInstall()) {
        ulog("install folder %s is not writable", utf8(installDir()).c_str());
        std::lock_guard lk(sM);
        sState.installWritable = false;
        sState.phase = Phase::Failed;
        sState.message = "The game folder (" + utf8(installDir()) +
                         ") cannot be written (e.g. Program Files). Download the new version from the release page "
                         "and unpack it yourself, or move the game to a folder you own.";
        return;
    }
#ifdef _WIN32
    // The package is unpacked by tar.exe in <install>\upd, which must stay below MAX_PATH.
    if (const std::string st = utf8(installDir() / kStagingName); st.size() > kMaxStagingPath) {
        const size_t maxInstall = kMaxStagingPath - 1 - std::strlen(kStagingName);
        ulog("install folder path too long for the updater (%zu characters, at most %zu)", st.size() - 4, maxInstall);
        std::lock_guard lk(sM);
        sState.installWritable = false;
        sState.phase = Phase::Failed;
        sState.message = "The game folder's path is too long for the updater (" + std::to_string(st.size() - 4) +
                         " characters, at most " + std::to_string(maxInstall) +
                         "). Move the game to a shorter folder (e.g. C:\\Games\\Luigi's Mansion Recompiled) and update "
                         "from there, or download the new version from the release page and unpack it yourself.";
        return;
    }
#endif
    {
        std::lock_guard lk(sM);
        sState.installWritable = true;
        sState.phase = Phase::Downloading;
        sState.got = 0;
        sState.total = sCand.info.assetSize;
        sState.message.clear();
    }
    startWorker(downloadWorker);
}

void cancel() { sCancel = true; }

void installAndRestart() {
    fs::path root;
    std::string tag;
    {
        std::lock_guard lk(sM);
        if (sState.phase != Phase::Ready || sBusy) return;
        root = sStagedRoot;
        tag = sState.info.tag;
        sState.phase = Phase::Installing;
    }
    const fs::path dir = installDir();
    ulog("installing %s from %s into %s", tag.c_str(), utf8(root).c_str(), utf8(dir).c_str());
    std::error_code ec;
    std::vector<fs::path> files;
    for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator();
         it.increment(ec))
        if (it->is_regular_file(ec)) files.push_back(it->path().lexically_relative(root));
    if (files.empty()) {
        setPhase(Phase::Failed, "The staged update is empty.");
        return;
    }
    struct Moved {
        fs::path target, old;  // old empty: the file was new
    };
    std::vector<Moved> moved;
    std::string err;
    for (const fs::path& rel : files) {
        const fs::path target = dir / rel;
        if (port::isPortableDataPath(rel)) {  // portable mode's data is the player's, never the package's
            ulog("skipped %s (portable data)", utf8(rel).c_str());
            continue;
        }
        if (!sameOrInside(target, dir)) {
            err = "bad path in the package";
            break;
        }
        fs::create_directories(target.parent_path(), ec);
        Moved m{target, {}};
        if (fs::exists(target, ec)) {
            // A running program or a loaded DLL cannot be overwritten on Windows, but it can be
            // renamed: move it out of the way (deleted at the next start).
            fs::path old = target;
            old += ".old";
            for (int n = 2; fs::exists(old, ec); n++) {
                fs::remove(old, ec);
                if (!fs::exists(old, ec)) break;
                old = target;
                old += ".old" + std::to_string(n);
            }
            fs::rename(target, old, ec);
            if (ec) {
                err = "could not move " + utf8(rel) + " aside: " + ec.message();
                break;
            }
            m.old = old;
        }
        fs::rename(root / rel, target, ec);
        if (ec) {
            if (!m.old.empty()) {
                std::error_code e2;
                fs::rename(m.old, target, e2);
            }
            err = "could not install " + utf8(rel) + ": " + ec.message();
            break;
        }
        moved.push_back(m);
    }
    if (!err.empty()) {
        ulog("install failed: %s; rolling back %zu files", err.c_str(), moved.size());
        for (auto it = moved.rbegin(); it != moved.rend(); ++it) {
            std::error_code e2;
            fs::remove(it->target, e2);
            if (!it->old.empty()) fs::rename(it->old, it->target, e2);
            if (e2) ulog("rollback of %s failed: %s", utf8(it->target).c_str(), e2.message().c_str());
        }
        setPhase(Phase::Failed, "Installing failed (" + err + "); the old version was kept.");
        return;
    }
#ifndef _WIN32
    // Keep the executable bits (tar keeps them; a repacked archive might not).
    for (const char* f : {"LuigisMansionRecomp", kLauncher, "llvm/bin/clang", "llvm/bin/ld.lld"}) {
        const fs::path p = dir / f;
        if (fs::exists(p, ec))
            fs::permissions(p, fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec |
                                   fs::perms::others_read | fs::perms::others_exec,
                            ec);
    }
#endif
    // The old files: Linux can delete them now (a running program's file stays readable to it);
    // Windows deletes them at the next start (cleanupAfterUpdate).
    std::vector<fs::path> leftOver;
    for (auto& m : moved) {
        if (m.old.empty()) continue;
#ifndef _WIN32
        fs::remove(m.old, ec);
        if (!ec) continue;
#endif
        leftOver.push_back(m.old);
    }
    if (!leftOver.empty()) {
        std::ofstream list(dir / kCleanupList, std::ios::binary | std::ios::app);
        for (auto& p : leftOver) list << utf8(p.lexically_relative(dir)) << '\n';
    }
    fs::remove_all(dir / kStagingName, ec);  // only empty folders are left in it
    ulog("installed %zu files; restarting", moved.size());
    port::saveSettings();
    std::fflush(stdout);
    port::relaunch(false, sExePath);
    setPhase(Phase::Failed, "The new version is installed but could not be started; start the game again.");
}

bool shouldNotify() {
    std::lock_guard lk(sM);
    if (sLater) return false;
    if (sState.phase != Phase::Available && sState.phase != Phase::Downloading && sState.phase != Phase::Verifying &&
        sState.phase != Phase::Extracting && sState.phase != Phase::Ready)
        return false;
    return sState.manual || port::settings().updates.skipVersion != sState.info.tag;
}

void skipThisVersion() {
    std::string tag;
    {
        std::lock_guard lk(sM);
        tag = sState.info.tag;
        sState.manual = false;
    }
    if (tag.empty()) return;
    ulog("player skipped %s", tag.c_str());
    port::settings().updates.skipVersion = tag;
    port::saveSettings();
}

void later() {
    std::lock_guard lk(sM);
    sLater = true;
    ulog("player chose later for %s", sState.info.tag.c_str());
}

bool dismissed() {
    std::lock_guard lk(sM);
    return sLater;
}

void cleanupAfterUpdate() {
    const fs::path dir = installDir();
    std::error_code ec;
#ifndef _WIN32
    // At every start, not only after an update: a 1.1+ package unpacked over a 1.0.x folder
    // leaves the old launcher next to the new one too. The updater's restart (execv) runs this
    // right after an update.
    if (isPackaged()) forwardOldLauncher(dir);
#endif
    const fs::path listFile = dir / kCleanupList;
    const bool haveList = fs::exists(listFile, ec);
    const bool haveStaging = fs::exists(dir / kStagingName, ec) || fs::exists(dir / kOldStagingName, ec);
    if (!haveList && !haveStaging) return;
    std::thread([dir, listFile] {
        std::vector<fs::path> left;
        {
            std::ifstream in(listFile, std::ios::binary);
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.empty()) continue;
                const fs::path p = under(dir, line);
                const std::string name = utf8(p.filename());
                // Only files this updater renamed (<name>.old / .oldN) inside the install folder.
                if (name.find(".old") == std::string::npos || !sameOrInside(p, dir)) continue;
                left.push_back(p);
            }
        }
        // The previous process may still be exiting (its exe and DLLs are in use until then).
        for (int attempt = 0; attempt < 40 && !left.empty(); attempt++) {
            if (attempt) std::this_thread::sleep_for(std::chrono::milliseconds(250));
            std::vector<fs::path> still;
            for (auto& p : left) {
                std::error_code ec;
                fs::remove(p, ec);
                if (ec && fs::exists(p, ec)) still.push_back(p);
            }
            left.swap(still);
        }
        std::error_code ec;
        fs::remove_all(dir / kStagingName, ec);
        fs::remove_all(dir / kOldStagingName, ec);
        if (left.empty()) {
            fs::remove(listFile, ec);
            ulog("removed the previous version's files");
        } else {
            std::ofstream out(listFile, std::ios::binary);
            for (auto& p : left) out << utf8(p.lexically_relative(dir)) << '\n';
            ulog("%zu old files still in use; deleted at a later start", left.size());
        }
    }).detach();
}

}  // namespace port::update
