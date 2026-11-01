// What font.cpp needs from the OS's rasteriser (font_dwrite.cpp on Windows). Internal to render/.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace render::raster
{
    struct FaceMetrics
    {
        float unitsPerEm, ascent, descent, lineGap, capHeight; // font units
    };

    struct Bitmap
    {
        int width = 0, height = 0;
        int left = 0, top = 0; // ink offset from the pen position on the baseline (top is negative above it)
        std::vector<uint8_t> coverage;
    };

    struct FaceRequest
    {
        const wchar_t *family;
        int weight; // 400 regular, 600 semibold, 700 bold
    };

    bool init(std::string &error);
    void shutdown();
    // the first family of the list that exists; -1 when none does
    int loadFace(const std::vector<FaceRequest> &candidates);
    FaceMetrics faceMetrics(int face);
    uint16_t glyphIndex(int face, char32_t codepoint); // 0: the face has no glyph for it
    float advance(int face, uint16_t glyph);           // font units
    float kerning(int face, uint16_t left, uint16_t right); // font units
    // the glyph at `px` with the pen at x = subpixel / 4 on the baseline
    Bitmap rasterize(int face, uint16_t glyph, float px, int subpixel);
}
