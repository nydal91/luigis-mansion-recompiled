#include "setup/disc.h"

#include "port/dol.h"
#include "port/install.h"

#include <nod.h>

#include <cctype>
#include <cstring>
#ifdef __ANDROID__
#include <SDL3/SDL_iostream.h>
#include <SDL3/SDL_error.h>
#endif

namespace setup {

std::string hex(const std::uint8_t* data, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; i++) {
        s += d[data[i] >> 4];
        s += d[data[i] & 15];
    }
    return s;
}

Disc::~Disc() {
    nod_free(part_);
    nod_free(disc_);
}

bool Disc::open(const std::string& utf8Path, std::string& error) {
    {
        // Archives are not disc images: say so instead of nod's "unknown format".
        std::string name = port::discDisplayName(utf8Path);
        for (auto& ch : name) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        for (const char* ext : {".7z", ".zip", ".rar", ".gz", ".xz"})
            if (name.size() > std::strlen(ext) && name.compare(name.size() - std::strlen(ext), std::string::npos, ext) == 0) {
                error = "This is a compressed archive, not a disc image. Extract it first (to an .iso, .ciso, .rvz, ...) "
                        "and choose the extracted file.";
                return false;
            }
    }
    const std::string openPath = port::discOpenPath(utf8Path, &error);  // Android: content:// URIs
    if (openPath.empty()) return false;
#ifdef __ANDROID__
    // Picked files are content:// URIs, which only SDL_IOFromFile can open (through the
    // ContentResolver), so nod reads through an SDL stream, like Aurora's DVD code does.
    {
        SDL_IOStream* io = SDL_IOFromFile(openPath.c_str(), "rb");
        if (!io) {
            error = std::string("Could not read the disc image: ") + SDL_GetError();
            return false;
        }
        const NodDiscStream stream{
            .user_data = io,
            .read_at = [](void* ud, uint64_t off, void* out, size_t len) -> int64_t {
                auto* s = static_cast<SDL_IOStream*>(ud);
                if (SDL_SeekIO(s, Sint64(off), SDL_IO_SEEK_SET) < 0) return -1;
                size_t done = 0;
                while (done < len) {
                    const size_t n = SDL_ReadIO(s, static_cast<char*>(out) + done, len - done);
                    if (n == 0) break;
                    done += n;
                }
                return int64_t(done);
            },
            .stream_len = [](void* ud) -> int64_t { return SDL_GetIOSize(static_cast<SDL_IOStream*>(ud)); },
            .close = [](void* ud) { SDL_CloseIO(static_cast<SDL_IOStream*>(ud)); },
        };
        const NodDiscOptions options{.preloader_threads = 1};
        if (nod_disc_open_stream(&stream, &options, &disc_) != NOD_RESULT_OK) {
            const char* msg = nod_error_message();
            error = std::string("Could not read the disc image") + (msg ? std::string(": ") + msg : ".");
            return false;
        }
    }
    if (false) {
#else
    if (nod_disc_open(openPath.c_str(), nullptr, &disc_) != NOD_RESULT_OK) {
#endif
        const char* msg = nod_error_message();
        error = std::string("Could not read the disc image") + (msg ? std::string(": ") + msg : ".");
        return false;
    }
    if (nod_disc_open_partition_kind(disc_, NOD_PARTITION_KIND_DATA, nullptr, &part_) != NOD_RESULT_OK) {
        error = "The disc image has no game partition.";
        return false;
    }
    NodPartitionMeta meta{};
    if (nod_partition_meta(part_, &meta) != NOD_RESULT_OK || !meta.raw_dol.data) {
        error = "The disc image has no main.dol.";
        return false;
    }
    dol_.assign(meta.raw_dol.data, meta.raw_dol.data + meta.raw_dol.size);
    switch (port::dol::identify(dol_.data(), dol_.size())) {
    case port::dol::Version::GLME01: version_ = port::GameVersion::USA; return true;
    case port::dol::Version::GLMP01: version_ = port::GameVersion::PAL; return true;
    default: break;
    }
    std::string id;
    if (meta.raw_boot.data && meta.raw_boot.size >= 6) id.assign(reinterpret_cast<const char*>(meta.raw_boot.data), 6);
    if (id.rfind("GLM", 0) == 0)
        error = "This Luigi's Mansion disc (" + id + ") is not a supported release. Supported: USA (GLME01) and "
                "Europe (GLMP01), first printing.";
    else
        error = "This is not a Luigi's Mansion disc" + (id.empty() ? std::string(".") : " (game ID " + id + ").");
    return false;
}

std::string Disc::dolSha1() const {
    const auto d = port::dol::sha1(dol_.data(), dol_.size());
    return hex(d.data(), d.size());
}

bool Disc::readFile(const char* path, std::vector<std::uint8_t>& out) const {
    NodNodeKind kind{};
    uint32_t length = 0;
    const uint32_t index = nod_partition_find_file(part_, path, &kind, &length);
    if (index == NOD_FST_STOP) return false;
    NodHandle* file = nullptr;
    if (nod_partition_open_file(part_, index, &file) != NOD_RESULT_OK) return false;
    out.resize(length);
    size_t done = 0;
    while (done < length) {
        const int64_t n = nod_read(file, out.data() + done, length - done);
        if (n <= 0) break;
        done += size_t(n);
    }
    nod_free(file);
    return done == length;
}

bool probeDisc(const std::string& utf8Path, port::GameVersion& version, std::string& error) {
    Disc d;
    if (!d.open(utf8Path, error)) return false;
    version = d.version();
    return true;
}

}  // namespace setup
