// Custom texture packs (texture_pack.h, docs/TEXTURE_PACKS.md).
//
// Aurora already replaces textures by the Dolphin naming convention: the name of each file is the
// texture's size, an xxHash64 of its GX data (and of the palette range it uses) and its GX
// format, so a pack made for Dolphin's "Load Custom Textures" works unchanged. This file only
// points Aurora at <data>/textures, keeps the registrations so the pack can be switched off or
// reloaded, and on Android imports a pack from a .zip or a folder the player picks (the app
// cannot read shared storage by path; LMActivity copies the texture files into its own folder).
#include "recomp/texture_pack.h"

#include "port/install.h"
#include "port/settings.h"
#include "recomp/menu.h"
#ifdef __ANDROID__
#include "port/android.h"
#endif

#include <dolphin/gx/GXStruct.h>
#include <aurora/texture.hpp>
#include <SDL3/SDL_misc.h>
#include <imgui.h>

#include <cstdio>
#include <mutex>
#include <system_error>

namespace recomp::texture_pack {
namespace {

std::mutex sM;
aurora::texture::ReplacementGroup sGroup;
bool sLoaded = false;

// Number of texture files below the folder (shown before they are loaded).
size_t countFiles(const std::filesystem::path& dir) {
    size_t n = 0;
    std::error_code ec;
    for (auto it = std::filesystem::recursive_directory_iterator(
             dir, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        std::error_code e;
        if (!it->is_regular_file(e)) continue;
        const auto name = it->path().filename().string();
        if (name.rfind("tex1_", 0) == 0) n++;
    }
    return n;
}

void unloadLocked() {
    if (!sLoaded) return;
    aurora::texture::unregister_replacements(sGroup);
    sGroup = {};
    sLoaded = false;
}

void loadLocked() {
    unloadLocked();
    if (!port::settings().rendering.customTextures) return;
    const auto dir = folder();
    sGroup = aurora::texture::load_replacement_directory(dir);
    sLoaded = true;
    std::printf("[textures] %zu replacement textures from %s\n", sGroup.registrations.size(),
                port::displayPath(dir).c_str());
    std::fflush(stdout);
}

#ifdef __ANDROID__
// Import state, polled from the menu (LMActivity copies on its own thread).
bool sImportWatching = false;
#endif

}  // namespace

std::filesystem::path folder() { return port::dataDir() / "textures"; }

void init() {
    std::error_code ec;
    std::filesystem::create_directories(folder(), ec);
    std::lock_guard lk(sM);
    loadLocked();
}

void reload() {
    std::lock_guard lk(sM);
    loadLocked();
}

size_t loadedCount() {
    std::lock_guard lk(sM);
    return sLoaded ? sGroup.registrations.size() : 0;
}

bool drawMenu() {
    bool changed = false;
    auto& r = port::settings().rendering;
    ImGui::SeparatorText("Texture pack");
    if (recomp::menu::Checkbox("Use custom textures", &r.customTextures)) {
        changed = true;
        reload();
    }
    ImGui::SetItemTooltip(
        "Replacement textures made for Dolphin's \"Load Custom Textures\" (files named\n"
        "tex1_<size>_<hash>_<format>.png or .dds, e.g. a 4K texture pack) are used instead of the\n"
        "game's own. Put the pack's texture folder (for Luigi's Mansion usually \"GLM\") into the\n"
        "textures folder below; any folder layout works. Packs made from the US version mostly\n"
        "match the European one too.");
    static size_t sFiles = size_t(-1);
    if (sFiles == size_t(-1)) sFiles = countFiles(folder());
    const size_t loaded = loadedCount();
    if (!r.customTextures)
        ImGui::TextDisabled("Off (%zu texture files in the folder)", sFiles);
    else if (sFiles == 0)
        ImGui::TextDisabled("No texture pack installed");
    else
        ImGui::Text("%zu replacement textures loaded", loaded);
#ifdef __ANDROID__
    // Phones: the pack is copied into the app (it cannot read a folder in shared storage).
    if (recomp::menu::Button("Import .zip")) {
        port::android::pickTexturePack(false, folder().string());
        sImportWatching = true;
    }
    ImGui::SetItemTooltip("Choose the texture pack's .zip. Its texture files are copied into the app (the rest of\n"
                          "the archive is skipped). Use the pack's 1080p version on a phone: the 4K one needs\n"
                          "several GB of storage and much more memory. A .7z or .rar must be unpacked first\n"
                          "(e.g. with a file manager app); then use Import folder.");
    ImGui::SameLine();
    if (recomp::menu::Button("Import folder")) {
        port::android::pickTexturePack(true, folder().string());
        sImportWatching = true;
    }
    ImGui::SetItemTooltip("Choose an unpacked texture pack folder (e.g. \"GLM\"). Its texture files, also in\n"
                          "subfolders, are copied into the app.");
    if (sImportWatching) {
        const auto st = port::android::texturePackImportStatus();
        if (st.state == port::android::ImportState::Running) {
            ImGui::Text("Importing... %d files", st.count);
        } else if (st.state == port::android::ImportState::Done) {
            sImportWatching = false;
            sFiles = countFiles(folder());
            reload();
            std::printf("[textures] imported %d files\n", st.count);
        } else if (st.state == port::android::ImportState::Failed) {
            sImportWatching = false;
            sFiles = countFiles(folder());
            std::printf("[textures] import failed: %s\n", st.message.c_str());
        }
    }
    if (const auto st = port::android::texturePackImportStatus();
        !sImportWatching && !st.message.empty() && st.state != port::android::ImportState::Running)
        ImGui::TextWrapped("%s", st.message.c_str());
    if (sFiles > 0) {
        if (recomp::menu::Button("Remove texture packs")) {
            {
                std::lock_guard lk(sM);
                unloadLocked();
            }
            std::error_code ec;
            std::filesystem::remove_all(folder(), ec);
            std::filesystem::create_directories(folder(), ec);
            sFiles = 0;
            std::printf("[textures] removed the imported texture packs\n");
        }
        ImGui::SetItemTooltip("Deletes the imported texture files from the app.");
    }
#else
    ImGui::TextDisabled("Folder: %s", port::displayPath(folder()).c_str());
    if (recomp::menu::Button("Open textures folder")) {
        std::error_code ec;
        std::filesystem::create_directories(folder(), ec);
        const auto url = "file:///" + port::displayPath(folder());
        SDL_OpenURL(url.c_str());
    }
    ImGui::SameLine();
#endif
#ifndef __ANDROID__
    if (recomp::menu::Button("Reload textures")) {
        sFiles = countFiles(folder());
        reload();
    }
    ImGui::SetItemTooltip("Picks up texture files added, changed or removed since the game started.");
#endif
    return changed;
}

}  // namespace recomp::texture_pack
