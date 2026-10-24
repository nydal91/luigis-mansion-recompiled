# Texture packs

The port loads replacement textures made for Dolphin's "Load Custom Textures": files named
`tex1_<width>x<height>[_m]_<texture hash>[_<palette hash>]_<format>.png` (or `.dds`), for
example Henriko Magnifico's Luigi's Mansion 4K pack. Download packs from their authors; none are
included.

## Installing

- **Windows / Linux:** copy the pack's texture folder (for Luigi's Mansion usually `GLM`, found in
  Dolphin packs under `Load/Textures/`) into the `textures` folder of the game's data folder.
  F1 > Visuals > Texture pack shows the folder and opens it ("Open textures folder"). Any layout
  works: files are found in subfolders too. "Reload textures" picks up changes without a restart.
- **Android:** F1 > Visuals > Texture pack > "Import .zip" (the pack's archive) or "Import
  folder" (an unpacked pack). Only the texture files are copied into the app. A `.7z`/`.rar`
  must be unpacked first (e.g. with a file manager app), then imported as a folder. Use a pack's
  1080p version on phones: 4K textures take several GB of storage and much more memory.

"Use custom textures" switches the pack off and on at once.

## How it works

Aurora (`extern/aurora`, `lib/gfx/texture_replacement.cpp`) hashes each texture the game loads
the way Dolphin names them: xxHash64 (seed 0) of the GX texture data and, for palette formats, of
the palette range the texture uses. A matching file replaces the texture (mipmaps from the file
or its `_mipN` sidecars). `src/recomp/texture_pack.cpp` points it at `<data>/textures`
(`port::dataDir()`), keeps the registrations so the setting can unload them, and draws the menu.
On Android, `LMActivity` copies `tex1_*.png/.dds` files from the picked `.zip` or folder into
`files/textures/<pack name>/` (flat: only file names are used, never archive paths).

Packs made from the US game mostly match the European game too (most textures are the same
data); textures with region-specific content (text) stay the original ones.
