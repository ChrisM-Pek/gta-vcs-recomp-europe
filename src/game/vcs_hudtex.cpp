// See vcs_hudtex.h. The 2D layer's textures are the PS2's own (the font atlas 256x256, the digits 128x64, the radar's
// map tiles and the icons 128x128 or less), drawn over a modern picture at its full size - so the HUD is the last part
// of the game that stays at 640x448. A texture-replacement pack drawn at a higher resolution is what removes that:
// PCSX2's packs are named "<TEX0Hash>-<CLUTHash>-<bits>.png" (or "<TEX0Hash>-<bits>.png" for formats without a palette),
// with the two hashes of the texture's data and its CLUT and the TEX0 / TEXA bits in the name; the renderer reproduces
// those hashes from its own GS memory (runtime/gs/gs_cpu_backend.cpp's Pcsx2TextureName) and this finds the file.
//
// Where packs are looked for (any depth): <exe>/textures/ and <data>/textures/ - a PCSX2 pack unzipped as it comes
// (textures/<serial>/replacements/*.png) works as it is. VCS_HUDTEX_DIR=<dir> adds a folder for one run.
//
// The setting is Graphics > Image quality > "HD textures (HUD)", off by default (the PS2's own art): authentic unless
// asked for. VCS_HUDTEX=0|1 / VCS_HUDTEX_LOG=1 override it for a run; the log's "[hudtex]" lines name every texture the
// 2D layer drew and whether a pack file was found for it.
#include "game/vcs_hudtex.h"

#include "platform/platform.h"
#include "runtime/gs/gs_hw.h"
#include "settings/settings.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vcs::hudtex
{
    namespace
    {
        namespace fs = std::filesystem;

        std::atomic<bool> g_enabled{false}; // the setting (and the env override), read by the source (the GS stage)
        bool g_log = false;     // VCS_HUDTEX_LOG
        std::string g_dumpDir;  // VCS_HUDTEX_DUMP: the PS2 textures the 2D layer draws, as pack authors need them
        std::unordered_set<std::string> g_dumped;
        std::mutex g_mutex;     // the pack's map, the dumps and its lazy load
        bool g_scanned = false;
        std::unordered_map<std::string, fs::path> g_pack; // the file's stem (lowercase) -> its path
        std::string g_packFolder;
        std::vector<fs::path> g_roots;
        std::unordered_map<std::string, bool> g_logged; // texture name -> logged (diagnosis)

        bool onTexture2DThunk(const GSHwTexture2D &texture, GSHwTexture2DImage &out);
        // The renderer only does the work (a read-back, a decode, the source) while the source is installed: nullptr
        // takes the PS2's textures back. A dump run installs it too (the setting may stay off).
        void applyEnabled() { gsHwSetTexture2DSource(g_enabled.load() || !g_dumpDir.empty() ? &onTexture2DThunk : nullptr); }

        // [gta-recomp] A pack's file name as PCSX2 writes it: no leading zeros, lowercase, the bits field 8 hex digits.
        // Bit 14 of the bits field held TCC in older dumps and PCSX2 clears it when it reads a name (RemoveUnusedBits):
        // the same here, or a pack dumped by 1.6 never matches.
        constexpr uint32_t kBitsUnused = 0x4000u;
        std::string nameOf(uint64_t tex0Hash, uint64_t clutHash, uint32_t bits, bool withClut)
        {
            char text[64];
            bits &= ~kBitsUnused;
            if (withClut)
                std::snprintf(text, sizeof(text), "%llx-%llx-%08x", (unsigned long long)tex0Hash,
                              (unsigned long long)clutHash, bits);
            else
                std::snprintf(text, sizeof(text), "%llx-%08x", (unsigned long long)tex0Hash, bits);
            return text;
        }

        // A file's stem as the table's key and as nameOf writes it (bit 14 cleared, no leading zeros), or false.
        bool canonicalName(const std::string &stem, std::string &out)
        {
            unsigned long long a = 0, b = 0;
            unsigned int c = 0;
            if (std::sscanf(stem.c_str(), "%llx-%llx-%8x", &a, &b, &c) == 3 &&
                stem.find('-', stem.find('-') + 1) == stem.rfind('-'))
            {
                out = nameOf(a, b, c, true);
                return true;
            }
            if (std::sscanf(stem.c_str(), "%llx-%8x", &a, &c) == 2 && stem.find('-') == stem.rfind('-'))
            {
                out = nameOf(a, 0u, c, false);
                return true;
            }
            return false;
        }

        void scan()
        {
            if (g_scanned)
                return;
            g_scanned = true;
            size_t count = 0;
            for (const fs::path &root : g_roots)
            {
                std::error_code error;
                if (!fs::is_directory(root, error))
                    continue;
                if (g_packFolder.empty())
                    g_packFolder = root.string();
                // a pack unzipped as PCSX2 keeps it (textures/<serial>/replacements/, textures/<serial>/dumps/)
                for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, error), end;
                     it != end && !error; it.increment(error))
                {
                    if (!it->is_regular_file(error))
                        continue;
                    std::string ext = it->path().extension().string();
                    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                    if (ext != ".png" && ext != ".dds")
                        continue;
                    std::string stem = it->path().stem().string();
                    std::transform(stem.begin(), stem.end(), stem.begin(), [](unsigned char ch) { return char(std::tolower(ch)); });
                    std::string key;
                    if (!canonicalName(stem, key))
                        continue;
                    g_pack.emplace(std::move(key), it->path());
                    ++count;
                }
            }
            std::fprintf(stderr, "[hudtex] pack: %llu files%s%s\n", (unsigned long long)count,
                         count ? " in " : "", count ? g_packFolder.c_str() : " (no texture replacement pack found)");
        }

        void ensureScanned()
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            scan();
        }

        // A pack's image (RGBA8, rows top-down, straight alpha) prepared for the renderer: RGBA8 with R in the low byte,
        // which is how the GS stage's textures are stored. The replacement keeps the PS2 texture's alpha scale (0..128:
        // these packs are dumped from it), so nothing is rescaled. Larger than 8x the PS2 texture or than 2048 a side is
        // downscaled by an area average (a 4096x4096 font atlas would cost 64 MB for pixels the HUD cannot show).
        bool prepare(const uint8_t *rgba, int w, int h, uint32_t ps2W, uint32_t ps2H, GSHwTexture2DImage &out)
        {
            const uint32_t capW = std::min<uint32_t>(2048u, ps2W * 8u), capH = std::min<uint32_t>(2048u, ps2H * 8u);
            const float scale = std::min({1.0f, static_cast<float>(capW) / static_cast<float>(w),
                                          static_cast<float>(capH) / static_cast<float>(h)});
            const int tw = std::max(1, static_cast<int>(static_cast<float>(w) * scale + 0.5f));
            const int th = std::max(1, static_cast<int>(static_cast<float>(h) * scale + 0.5f));
            auto pixels = std::make_shared<std::vector<uint32_t>>(static_cast<size_t>(tw) * static_cast<size_t>(th));
            for (int y = 0; y < th; ++y)
            {
                const int y0 = static_cast<int>(static_cast<int64_t>(y) * h / th);
                const int y1 = std::max(y0 + 1, static_cast<int>(static_cast<int64_t>(y + 1) * h / th));
                for (int x = 0; x < tw; ++x)
                {
                    const int x0 = static_cast<int>(static_cast<int64_t>(x) * w / tw);
                    const int x1 = std::max(x0 + 1, static_cast<int>(static_cast<int64_t>(x + 1) * w / tw));
                    uint64_t r = 0, g = 0, b = 0, a = 0;
                    uint32_t n = 0;
                    for (int sy = y0; sy < y1; ++sy)
                        for (int sx = x0; sx < x1; ++sx)
                        {
                            const uint8_t *p = rgba + (static_cast<size_t>(sy) * w + sx) * 4u;
                            const uint32_t alpha = p[3];
                            r += uint64_t(p[0]) * alpha;
                            g += uint64_t(p[1]) * alpha;
                            b += uint64_t(p[2]) * alpha;
                            a += alpha;
                            ++n;
                        }
                    const uint32_t alpha = static_cast<uint32_t>(a / n);
                    const uint32_t rr = a ? static_cast<uint32_t>(r / a) : 0u;
                    const uint32_t gg = a ? static_cast<uint32_t>(g / a) : 0u;
                    const uint32_t bb = a ? static_cast<uint32_t>(b / a) : 0u;
                    (*pixels)[static_cast<size_t>(y) * tw + x] = rr | (gg << 8) | (bb << 16) | (alpha << 24);
                }
            }
            out.width = static_cast<uint32_t>(tw);
            out.height = static_cast<uint32_t>(th);
            out.pixels = std::move(pixels);
            return true;
        }

        // The GS stage's source (gsHwSetTexture2DSource): PCSX2's name for the texture -> the pack's file for it.
        bool onTexture2D(const GSHwTexture2D &texture, GSHwTexture2DImage &out)
        {
            if (!g_enabled.load() && g_dumpDir.empty())
                return false;
            ensureScanned();
            std::lock_guard<std::mutex> lock(g_mutex);
            const bool withClut = texture.clutHash != 0u;
            const std::string named = nameOf(texture.tex0Hash, texture.clutHash, texture.bits, withClut);
            if (!g_dumpDir.empty() && g_dumped.insert(named).second) // pack authors: the PS2 texture as PCSX2 names it
            {
                std::vector<uint8_t> bytes(static_cast<size_t>(texture.width) * texture.height * 4u);
                for (size_t i = 0, n = bytes.size() / 4u; i < n; ++i)
                {
                    const uint32_t texel = texture.pixels[i];
                    bytes[i * 4u] = static_cast<uint8_t>(texel & 0xFFu);
                    bytes[i * 4u + 1u] = static_cast<uint8_t>((texel >> 8) & 0xFFu);
                    bytes[i * 4u + 2u] = static_cast<uint8_t>((texel >> 16) & 0xFFu);
                    bytes[i * 4u + 3u] = static_cast<uint8_t>((texel >> 24) & 0xFFu);
                }
                const std::string path = g_dumpDir + "/" + named + ".png";
                platform::writePngRgba(path.c_str(), bytes.data(), static_cast<int>(texture.width),
                                       static_cast<int>(texture.height));
            }
            if (!g_enabled.load())
                return false;
            std::string name = named;
            auto found = g_pack.find(name);
            if (found == g_pack.end() && withClut)
            {
                // a pack may name a paletted texture without its CLUT (PCSX2's "<hash>-<bits>" form): the same art for
                // every palette of it
                name = nameOf(texture.tex0Hash, 0u, texture.bits, false);
                found = g_pack.find(name);
            }
            bool loaded = false;
            if (found != g_pack.end())
            {
                int w = 0, h = 0;
                std::vector<uint8_t> rgba;
                const std::string ext = found->second.extension().string();
                const bool png = ext == ".png" || ext == ".PNG";
                if ((png ? platform::readPng(found->second.string().c_str(), w, h, rgba)
                         : platform::readDds(found->second.string().c_str(), w, h, rgba)) &&
                    !rgba.empty())
                    loaded = prepare(rgba.data(), w, h, texture.width, texture.height, out);
            }
            if (g_log && !g_logged[named])
            {
                g_logged[named] = true;
                std::string what = "no file";
                if (found != g_pack.end())
                    what = loaded ? found->second.filename().string() + " -> " + std::to_string(out.width) + "x" +
                                        std::to_string(out.height)
                                  : "file not loadable";
                std::fprintf(stderr, "[hudtex] %ux%u psm %u: %s %s\n", (unsigned)texture.width, (unsigned)texture.height,
                             (unsigned)(texture.bits & 0x3Fu), named.c_str(), what.c_str());
            }
            return loaded;
        }

        bool onTexture2DThunk(const GSHwTexture2D &texture, GSHwTexture2DImage &out) { return onTexture2D(texture, out); }
    }

    void registerSettings()
    {
        using settings::Type;
        settings::Spec s;
        s.key = "display.hud_textures";
        s.page = "Graphics";
        s.section = "Image quality";
        s.label = "HD textures (HUD)";
        s.help = "Draws the HUD, texts, radar and menus from a high-resolution texture pack (PCSX2's texture replacements, "
                 "in a textures folder next to the game) when one is installed. Off is the PS2's own art.";
        s.type = Type::Choice;
        s.choices = {{"PS2 (original)", 0}, {"HD pack", 1}};
        s.defaultValue = 0;
        s.env = "VCS_HUDTEX";
        s.onChange = [](int v) { g_enabled = v != 0; applyEnabled(); };
        settings::add(s);
    }

    void install(const fs::path &exeDir, const fs::path &dataDir)
    {
        g_log = std::getenv("VCS_HUDTEX_LOG") != nullptr;
        if (const char *dir = std::getenv("VCS_HUDTEX_DUMP"); dir && *dir) // pack authors: the PS2 textures, named as PCSX2 names them
        {
            std::error_code error;
            fs::create_directories(dir, error);
            g_dumpDir = dir;
        }
        g_roots.clear();
        g_roots.push_back(exeDir / "textures");
        if (dataDir != exeDir)
            g_roots.push_back(dataDir / "textures");
        if (const char *dir = std::getenv("VCS_HUDTEX_DIR"); dir && *dir)
            g_roots.insert(g_roots.begin(), fs::path(dir));
        applyEnabled();
    }

    unsigned long long replaced() { return gsHwTexture2DReplaced(); }
    unsigned long long asked() { return gsHwTexture2DAsked(); }
    unsigned long long packFiles()
    {
        ensureScanned();
        std::lock_guard<std::mutex> lock(g_mutex);
        return g_pack.size();
    }
    const char *packFolder() { return g_packFolder.c_str(); }
}
