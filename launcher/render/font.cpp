#include "font.h"

#include "draw.h"
#include "font_raster.h"
#include "icons.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace render
{
    GlyphAtlas atlas;

    namespace
    {
        constexpr int kAtlasInitial = 1024, kAtlasMax = 4096;
        constexpr int kPad = 1; // empty texels between glyphs: no bleeding under linear filtering

        struct Glyph
        {
            float advance = 0; // pixels
            int16_t left = 0, top = 0, w = 0, h = 0;
            uint16_t ax = 0, ay = 0; // atlas position
        };

        struct Face
        {
            int faces[3] = {-1, -1, -1}; // the font, then fallbacks for characters it lacks
            raster::FaceMetrics metrics{};
        };

        Face g_fonts[kFontCount];
        bool g_brandFound = false;
        std::unordered_map<uint64_t, Glyph> g_glyphs;
        bool g_ready = false;

        int roundSize(float px) { return std::max(1, std::min(511, int(px + 0.5f))); }

        uint64_t glyphKey(Font font, int px, char32_t cp, int sub)
        {
            return uint64_t(font) | (uint64_t(px) << 4) | (uint64_t(sub) << 13) | (uint64_t(cp) << 15);
        }

        void atlasReset(int size)
        {
            atlas.size = size;
            atlas.pixels.assign(size_t(size) * size_t(size), 0);
            atlas.shelfX = atlas.shelfY = atlas.shelfH = 0;
            atlas.generation++;
            atlas.markAllDirty();
        }

        // grows the texture without moving what is in it: texel coordinates stay valid
        bool atlasGrow()
        {
            if (atlas.size >= kAtlasMax)
                return false;
            const int old = atlas.size, size = old * 2;
            std::vector<uint8_t> pixels(size_t(size) * size_t(size), 0);
            for (int y = 0; y < old; ++y)
                std::memcpy(&pixels[size_t(y) * size_t(size)], &atlas.pixels[size_t(y) * size_t(old)], size_t(old));
            atlas.pixels.swap(pixels);
            atlas.size = size;
            atlas.generation++;
            atlas.markAllDirty();
            return true;
        }

        bool atlasPlace(int w, int h, int &x, int &y)
        {
            for (;;)
            {
                if (atlas.shelfX + w + kPad > atlas.size)
                {
                    atlas.shelfY += atlas.shelfH + kPad;
                    atlas.shelfX = 0;
                    atlas.shelfH = 0;
                }
                if (atlas.shelfY + h + kPad <= atlas.size && w + kPad <= atlas.size)
                    break;
                if (!atlasGrow())
                    return false;
            }
            x = atlas.shelfX;
            y = atlas.shelfY;
            atlas.shelfX += w + kPad;
            atlas.shelfH = std::max(atlas.shelfH, h);
            return true;
        }

        // the face of the font that has the character (the font itself, else a fallback)
        std::unordered_map<uint64_t, std::pair<int, uint16_t>> g_indices; // (font, code point) -> face, glyph
        std::unordered_map<uint64_t, float> g_kerning;                     // (face, left, right) -> font units

        int faceFor(Font font, char32_t cp, uint16_t &glyph)
        {
            const uint64_t key = uint64_t(font) | (uint64_t(cp) << 8);
            if (auto it = g_indices.find(key); it != g_indices.end())
            {
                glyph = it->second.second;
                return it->second.first;
            }
            int face = g_fonts[font].faces[0];
            glyph = 0;
            for (int f : g_fonts[font].faces)
            {
                if (f < 0)
                    continue;
                if (const uint16_t g = raster::glyphIndex(f, cp))
                {
                    face = f;
                    glyph = g;
                    break;
                }
            }
            g_indices.emplace(key, std::make_pair(face, glyph));
            return face;
        }

        // a bitmap into the atlas; false when it is full (cleared at the next frame, never mid-frame)
        bool place(const raster::Bitmap &bitmap, Glyph &g)
        {
            if (bitmap.width <= 0 || bitmap.height <= 0)
                return true;
            int x = 0, y = 0;
            if (!atlasPlace(bitmap.width, bitmap.height, x, y))
            {
                atlas.clearPending = true;
                return false;
            }
            for (int row = 0; row < bitmap.height; ++row)
                std::memcpy(&atlas.pixels[size_t(y + row) * size_t(atlas.size) + size_t(x)],
                            &bitmap.coverage[size_t(row) * size_t(bitmap.width)], size_t(bitmap.width));
            atlas.dirtyX0 = std::min(atlas.dirtyX0, x);
            atlas.dirtyY0 = std::min(atlas.dirtyY0, y);
            atlas.dirtyX1 = std::max(atlas.dirtyX1, x + bitmap.width);
            atlas.dirtyY1 = std::max(atlas.dirtyY1, y + bitmap.height);
            g.left = int16_t(bitmap.left);
            g.top = int16_t(bitmap.top);
            g.w = int16_t(bitmap.width);
            g.h = int16_t(bitmap.height);
            g.ax = uint16_t(x);
            g.ay = uint16_t(y);
            return true;
        }

        const Glyph &glyphFor(Font font, int px, char32_t cp, int sub)
        {
            const uint64_t key = glyphKey(font, px, cp, sub);
            if (auto it = g_glyphs.find(key); it != g_glyphs.end())
                return it->second;
            Glyph g;
            raster::Bitmap bitmap;
            if (font == kFontIcons && icons::rasterize(cp, px, bitmap))
            {
                // ours (render/icons.cpp): a square as wide as it advances
                g.advance = float(bitmap.width);
                place(bitmap, g);
                return g_glyphs.emplace(key, g).first->second;
            }
            uint16_t index = 0;
            const int face = faceFor(font, cp, index);
            if (face >= 0)
            {
                const raster::FaceMetrics &m = raster::faceMetrics(face);
                g.advance = raster::advance(face, index) * float(px) / m.unitsPerEm;
                place(raster::rasterize(face, index, float(px), sub), g);
            }
            return g_glyphs.emplace(key, g).first->second;
        }

        float kern(Font font, int px, char32_t a, char32_t b)
        {
            if (!a)
                return 0;
            uint16_t ga = 0, gb = 0;
            const int fa = faceFor(font, a, ga), fb = faceFor(font, b, gb);
            if (fa < 0 || fa != fb || !ga || !gb)
                return 0;
            const uint64_t key = uint64_t(fa) | (uint64_t(ga) << 16) | (uint64_t(gb) << 32);
            auto it = g_kerning.find(key);
            if (it == g_kerning.end())
                it = g_kerning.emplace(key, raster::kerning(fa, ga, gb) / raster::faceMetrics(fa).unitsPerEm).first;
            return it->second * float(px);
        }
    }

    char32_t nextCodepoint(std::string_view s, size_t &at)
    {
        const unsigned char c = static_cast<unsigned char>(s[at++]);
        if (c < 0x80)
            return c;
        int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
        if (extra < 0)
            return 0xFFFD;
        char32_t cp = c & (0x3F >> extra);
        for (; extra > 0; --extra)
        {
            if (at >= s.size() || (static_cast<unsigned char>(s[at]) & 0xC0) != 0x80)
                return 0xFFFD;
            cp = (cp << 6) | (static_cast<unsigned char>(s[at++]) & 0x3F);
        }
        return cp;
    }

    bool fontsInit(std::string &error)
    {
        if (!raster::init(error))
            return false;
        using R = raster::FaceRequest;
#ifdef _WIN32
        const int ui = raster::loadFace({R{"Segoe UI Variable Text", 400}, R{"Segoe UI", 400}, R{"Arial", 400}});
        const int uiSemi = raster::loadFace({R{"Segoe UI Variable Text", 600}, R{"Segoe UI", 600}, R{"Arial", 700}});
        const int display = raster::loadFace({R{"Bahnschrift", 600}, R{"Segoe UI Variable Display", 600}, R{"Segoe UI", 600}});
        const int mono = raster::loadFace({R{"Cascadia Mono", 400}, R{"Consolas", 400}, R{"Courier New", 400}});
        const int symbols = raster::loadFace({R{"Segoe UI Symbol", 400}});
#else
        // what Linux desktops have: the first one installed (fontconfig falls back to the generic family last)
        const int ui = raster::loadFace({R{"Inter", 400}, R{"Cantarell", 400}, R{"Noto Sans", 400}, R{"Ubuntu", 400},
                                         R{"DejaVu Sans", 400}, R{"sans-serif", 400}});
        const int uiSemi = raster::loadFace({R{"Inter", 600}, R{"Cantarell", 600}, R{"Noto Sans", 600}, R{"Ubuntu", 500},
                                             R{"DejaVu Sans", 700}, R{"sans-serif", 600}});
        const int display = raster::loadFace({R{"Bahnschrift", 600}, R{"Barlow Semi Condensed", 600},
                                              R{"Noto Sans Display", 600}, R{"Noto Sans", 600}, R{"Cantarell", 700}});
        const int mono = raster::loadFace({R{"Cascadia Mono", 400}, R{"JetBrains Mono", 400}, R{"Noto Sans Mono", 400},
                                           R{"DejaVu Sans Mono", 400}, R{"monospace", 400}});
        const int symbols = raster::loadFace({R{"Noto Sans Symbols 2", 400}, R{"DejaVu Sans", 400}});
#endif
        const int icons = symbols; // the icons themselves are drawn by render/icons.cpp, not a font
        // Pricedown's licence (Typodermic's desktop EULA) lets each person install it on their own PC, not ship it in
        // software: it is used when installed, never bundled (DECISIONS.md 2026-11-01). VCS_BRAND_FONT: a font file of
        // the player's own instead of an installed one (tests).
        int brand = raster::loadFace({R{"Pricedown", 900}, R{"Pricedown Black", 400}, R{"Pricedown Bl", 400}});
        if (const char *file = std::getenv("VCS_BRAND_FONT"); brand < 0 && file && *file)
            brand = raster::loadFaceFile(file);
        g_brandFound = brand >= 0;
        if (ui < 0)
        {
            error = "no usable system font";
            return false;
        }
        const int faces[kFontCount][3] = {
            {ui, symbols, -1},
            {uiSemi >= 0 ? uiSemi : ui, ui, symbols},
            {display >= 0 ? display : uiSemi, ui, symbols},
            {icons, symbols, ui},
            {mono >= 0 ? mono : ui, ui, symbols},
            {brand >= 0 ? brand : display >= 0 ? display : uiSemi, display >= 0 ? display : ui, ui},
        };
        for (int f = 0; f < kFontCount; ++f)
        {
            std::memcpy(g_fonts[f].faces, faces[f], sizeof(faces[f]));
            const int primary = g_fonts[f].faces[0] >= 0 ? g_fonts[f].faces[0] : ui;
            g_fonts[f].faces[0] = primary;
            g_fonts[f].metrics = raster::faceMetrics(primary);
        }
        atlasReset(kAtlasInitial);
        g_ready = true;
        return true;
    }

    const char *fontRasterizer() { return raster::backendName(); }

    void fontsShutdown()
    {
        g_glyphs.clear();
        raster::shutdown();
        g_ready = false;
    }

    void fontsBeginFrame()
    {
        if (!atlas.clearPending)
            return;
        atlas.clearPending = false;
        g_glyphs.clear();
        atlasReset(atlas.size);
    }

    void fontsClearCache()
    {
        g_glyphs.clear();
        atlasReset(atlas.size ? atlas.size : kAtlasInitial);
    }

    bool fontFound(Font font) { return font != kFontBrand || g_brandFound; }

    FontMetrics metrics(Font font, float px)
    {
        const raster::FaceMetrics &m = g_fonts[font].metrics;
        const float scale = float(roundSize(px)) / m.unitsPerEm;
        return {std::ceil(m.ascent * scale), std::ceil(m.descent * scale),
                std::ceil((m.ascent + m.descent + m.lineGap) * scale), std::ceil(m.capHeight * scale)};
    }

    float measure(Font font, float px, std::string_view text)
    {
        if (!g_ready)
            return 0;
        const int size = roundSize(px);
        float x = 0;
        char32_t previous = 0;
        for (size_t at = 0; at < text.size();)
        {
            const char32_t cp = nextCodepoint(text, at);
            x += kern(font, size, previous, cp) + glyphFor(font, size, cp, 0).advance;
            previous = cp;
        }
        return x;
    }

    float drawText(Font font, float px, float x, float baseline, core::Color color, std::string_view text, float blur)
    {
        if (!g_ready)
            return x;
        const int size = roundSize(px);
        // whole pixels for the baseline; the pen keeps its fraction, as a quarter-pixel glyph variant
        const float base = std::floor(baseline + draw.dy + 0.5f) - draw.dy;
        float pen = x;
        char32_t previous = 0;
        for (size_t at = 0; at < text.size();)
        {
            const char32_t cp = nextCodepoint(text, at);
            pen += kern(font, size, previous, cp);
            previous = cp;
            const float abs = pen + draw.dx;
            float whole = std::floor(abs);
            int sub = int((abs - whole) * 4.0f + 0.5f);
            if (sub == 4)
            {
                sub = 0;
                whole += 1;
            }
            const Glyph &g = glyphFor(font, size, cp, sub);
            if (g.w > 0)
            {
                const float uv[4] = {float(g.ax), float(g.ay), float(g.ax + g.w), float(g.ay + g.h)};
                glyph({whole - draw.dx + float(g.left), base + float(g.top), float(g.w), float(g.h)}, uv, color, blur);
            }
            pen += g.advance;
        }
        return pen;
    }

    void drawIcon(Font font, float px, float cx, float cy, core::Color color, char32_t codepoint)
    {
        if (!g_ready)
            return;
        const int size = roundSize(px);
        const Glyph &g = glyphFor(font, size, codepoint, 0);
        if (g.w <= 0)
            return;
        const float x = std::floor(cx + draw.dx - float(g.w) * 0.5f + 0.5f) - draw.dx;
        const float y = std::floor(cy + draw.dy - float(g.h) * 0.5f + 0.5f) - draw.dy;
        const float uv[4] = {float(g.ax), float(g.ay), float(g.ax + g.w), float(g.ay + g.h)};
        glyph({x, y, float(g.w), float(g.h)}, uv, color);
    }

    std::string ellipsize(Font font, float px, std::string_view text, float width)
    {
        if (measure(font, px, text) <= width)
            return std::string(text);
        // one pass: keep whole characters while they and the ellipsis fit
        const int size = roundSize(px);
        const float room = width - measure(font, px, "\xE2\x80\xA6");
        float x = 0;
        size_t keep = 0;
        char32_t previous = 0;
        for (size_t at = 0; at < text.size();)
        {
            const char32_t cp = nextCodepoint(text, at);
            x += kern(font, size, previous, cp) + glyphFor(font, size, cp, 0).advance;
            previous = cp;
            if (x > room)
                break;
            keep = at;
        }
        std::string cut(text.substr(0, keep));
        while (!cut.empty() && cut.back() == ' ')
            cut.pop_back();
        return cut + "\xE2\x80\xA6";
    }

    std::vector<std::string> wrap(Font font, float px, std::string_view text, float width)
    {
        std::vector<std::string> lines;
        size_t start = 0;
        while (start <= text.size())
        {
            const size_t newline = text.find('\n', start);
            const std::string_view para = text.substr(start, newline == std::string_view::npos ? std::string_view::npos : newline - start);
            std::string line;
            size_t at = 0;
            while (at < para.size())
            {
                size_t end = para.find(' ', at);
                if (end == std::string_view::npos)
                    end = para.size();
                std::string word(para.substr(at, end - at));
                // a word wider than the line (a path) is broken: after a separator where it can, else anywhere
                while (measure(font, px, word) > width && word.size() > 1)
                {
                    if (!line.empty())
                    {
                        lines.push_back(line);
                        line.clear();
                    }
                    size_t cut = 0, lastSeparator = 0;
                    for (size_t i = 0; i < word.size();)
                    {
                        size_t next = i;
                        nextCodepoint(word, next);
                        if (measure(font, px, std::string_view(word).substr(0, next)) > width)
                            break;
                        cut = next;
                        if (word[i] == '\\' || word[i] == '/' || word[i] == '-' || word[i] == '_')
                            lastSeparator = next;
                        i = next;
                    }
                    if (lastSeparator > cut / 2)
                        cut = lastSeparator;
                    if (cut == 0)
                        break;
                    lines.push_back(word.substr(0, cut));
                    word = word.substr(cut);
                }
                const std::string candidate = line.empty() ? word : line + " " + word;
                if (!line.empty() && measure(font, px, candidate) > width)
                {
                    lines.push_back(line);
                    line = word;
                }
                else
                    line = candidate;
                at = end + 1;
            }
            lines.push_back(line);
            if (newline == std::string_view::npos)
                break;
            start = newline + 1;
        }
        return lines;
    }
}
