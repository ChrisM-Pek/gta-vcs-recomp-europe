// Text. Glyphs are rasterised on demand at the exact pixel size they are drawn at (so small text is as crisp as the
// system's own) into one shared R8 atlas, packed in shelves. They are cached per font, pixel size, code point and
// horizontal subpixel position (quarter pixels), so text keeps the font's true advances while every glyph lands on
// whole pixels, and drawing cached text costs one hash lookup per character.
//
// The atlas, the cache and the layout are platform independent (font.cpp); rasterising is the OS's (font_dwrite.cpp:
// DirectWrite on Windows; font_freetype.cpp: FreeType + fontconfig on Linux). Icons are no font: render/icons.cpp draws
// them from the renderer's own shapes. Sizes are physical pixels.
#pragma once

#include "core/math.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace render
{
    enum Font : uint8_t
    {
        kFontBody,     // Segoe UI Variable Text / Segoe UI
        kFontSemibold,
        kFontDisplay,  // Bahnschrift: the wordmark, large numbers
        kFontIcons,    // Segoe Fluent Icons / Segoe MDL2 Assets
        kFontMono,     // Cascadia Mono / Consolas: the log
        kFontBrand,    // Pricedown (GTA's), when the player has it installed; else the display font. Sparingly.
        kFontCount
    };

    struct GlyphAtlas
    {
        int size = 0;
        std::vector<uint8_t> pixels;
        uint32_t generation = 0; // the texture is recreated when it changes (it grew or was cleared)
        int shelfX = 0, shelfY = 0, shelfH = 0;
        int dirtyX0 = 0, dirtyY0 = 0, dirtyX1 = 0, dirtyY1 = 0; // changed since the last upload (x0 >= x1: clean)
        bool clearPending = false;

        void markAllDirty()
        {
            dirtyX0 = dirtyY0 = 0;
            dirtyX1 = dirtyY1 = size;
        }
    };
    extern GlyphAtlas atlas;

    struct FontMetrics
    {
        float ascent, descent, lineHeight, capHeight; // pixels, rounded up
    };

    bool fontsInit(std::string &error);
    const char *fontRasterizer(); // "DirectWrite", "FreeType"
    void fontsShutdown();
    void fontsBeginFrame();  // a full atlas is cleared here, never mid-frame
    void fontsClearCache();  // the DPI changed: everything rasterises again at the new sizes

    // false when the font fell back to another (kFontBrand without Pricedown installed)
    bool fontFound(Font font);

    FontMetrics metrics(Font font, float px);
    float measure(Font font, float px, std::string_view utf8);
    // draws with the baseline at `baseline`; returns the pen position after the last glyph. blur: a Gaussian radius in
    // pixels (0: sharp)
    float drawText(Font font, float px, float x, float baseline, core::Color color, std::string_view utf8, float blur = 0);
    // one glyph, its ink centred on (cx, cy): icons
    void drawIcon(Font font, float px, float cx, float cy, core::Color color, char32_t codepoint);

    // `text` cut to `width` with an ellipsis (whole characters)
    std::string ellipsize(Font font, float px, std::string_view text, float width);
    // `text` broken into lines no wider than `width` at spaces (explicit newlines kept)
    std::vector<std::string> wrap(Font font, float px, std::string_view text, float width);

    char32_t nextCodepoint(std::string_view text, size_t &at); // UTF-8 decoding; U+FFFD for bad bytes
}
