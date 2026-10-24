#pragma once
// Custom texture packs (docs/TEXTURE_PACKS.md): Dolphin-style replacement textures
// (tex1_<w>x<h>[_m]_<hash>[_<tlut hash>]_<format>.png/.dds) loaded from <data>/textures through
// Aurora's texture replacement (aurora/texture.hpp). F1 > Visuals > Texture pack.
#include <filesystem>
#include <string>

namespace recomp::texture_pack {

// Folder the packs live in (any layout below it: the files are found recursively).
std::filesystem::path folder();

// Loads the folder's textures if the setting is on (after aurora_initialize).
void init();
// Applies the setting / picks up changed files: unloads, then loads again if enabled.
void reload();
// Textures registered now.
size_t loadedCount();

// The F1 menu section (Visuals tab). Returns true when a setting changed.
bool drawMenu();

}  // namespace recomp::texture_pack
