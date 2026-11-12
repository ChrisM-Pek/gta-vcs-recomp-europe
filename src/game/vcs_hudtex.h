// HD textures for the 2D layer (vcs_hudtex.cpp): the HUD, text, radar and menus are drawn over the picture at the
// window's size (docs/NATIVE_RENDERER.md "HUD layer"), but sampled from the PS2's textures - the font atlas is 256x256,
// the money / clock digits 128x64, the radar's map tiles and the icons 128x128 or less. This puts a sharper image behind
// those draws: a PCSX2 texture-replacement pack's PNG (textures/<serial>/replacements/<TEX0Hash>-<CLUTHash>-<bits>.png,
// named as PCSX2 names it: runtime/gs/gs_cpu_backend.cpp's Pcsx2TextureName), else the PS2 textures as they are.
#pragma once

#include <filesystem>

namespace vcs::hudtex
{
    // Graphics > Image quality: "HD textures (HUD)" (off by default: the PS2's own art)
    void registerSettings();
    // The GS stage's texture source (gsHwSetTexture2DSource) and the pack folders under `exeDir` (textures/) and
    // `dataDir` (textures/): call once the runtime is up.
    void install(const std::filesystem::path &exeDir, const std::filesystem::path &dataDir);
    // Replacements drawn from so far, and the textures the source was asked about (the check).
    unsigned long long replaced();
    unsigned long long asked();
    // The pack's files found (0: no pack), and its folder (the check and the log).
    unsigned long long packFiles();
    const char *packFolder();
}
