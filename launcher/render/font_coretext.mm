// Glyphs rasterised by Core Text from the Mac's own fonts (macOS, docs/LAUNCHER.md "macOS"): grayscale antialiasing
// without font smoothing (no stem darkening), at the quarter-pixel horizontal positions font.cpp asks for - as
// DirectWrite's natural symmetric mode and FreeType's light hinting. Nothing is bundled. Objective-C++ with ARC.
#include "font_raster.h"

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <strings.h>

namespace render::raster
{
    namespace
    {
        struct LoadedFace
        {
            CTFontRef font = nullptr;  // at its em size (font units = points)
            CGFontRef cgFont = nullptr;
            CTFontRef sized = nullptr; // at sizePx, for rasterising
            float sizePx = -1;
            FaceMetrics metrics{};
            // the 'kern' table's format 0 pairs (left << 16 | right -> value), as FreeType's FT_Get_Kerning reads
            std::vector<std::pair<uint32_t, int16_t>> kern;
        };
        std::vector<LoadedFace> g_faces;

        // the weight trait Core Text uses (-1..1) for an OpenType weight
        CGFloat weightTrait(int weight)
        {
            if (weight >= 800)
                return 0.56;
            if (weight >= 700)
                return 0.4;
            if (weight >= 600)
                return 0.3;
            if (weight >= 500)
                return 0.23;
            return 0.0;
        }

        uint16_t be16(const uint8_t *p) { return uint16_t((p[0] << 8) | p[1]); }

        void readKern(LoadedFace &f)
        {
            CFDataRef table = CTFontCopyTable(f.font, kCTFontTableKern, kCTFontTableOptionNoOptions);
            if (!table)
                return;
            const uint8_t *d = CFDataGetBytePtr(table);
            const size_t n = size_t(CFDataGetLength(table));
            // version 0 (OpenType): uint16 version, uint16 nTables; subtables: version, length, coverage
            if (n >= 4 && be16(d) == 0)
            {
                size_t at = 4;
                for (uint16_t t = 0, count = be16(d + 2); t < count && at + 6 <= n; ++t)
                {
                    const uint16_t length = be16(d + at + 2), coverage = be16(d + at + 4);
                    const bool horizontalFormat0 = (coverage & 0xFF00u) == 0 && (coverage & 1u);
                    if (horizontalFormat0 && at + 14 <= n)
                    {
                        const uint16_t pairs = be16(d + at + 6);
                        for (uint16_t i = 0; i < pairs && at + 14 + size_t(i + 1) * 6 <= n; ++i)
                        {
                            const uint8_t *p = d + at + 14 + size_t(i) * 6;
                            f.kern.push_back({(uint32_t(be16(p)) << 16) | be16(p + 2), int16_t(be16(p + 4))});
                        }
                    }
                    if (!length)
                        break;
                    at += length;
                }
                std::sort(f.kern.begin(), f.kern.end());
            }
            CFRelease(table);
        }

        int addFace(CTFontRef font)
        {
            LoadedFace f;
            f.cgFont = CTFontCopyGraphicsFont(font, nullptr);
            const float upem = float(std::max<unsigned>(CGFontGetUnitsPerEm(f.cgFont), 1u));
            f.font = CTFontCreateCopyWithAttributes(font, upem, nullptr, nullptr); // one point = one font unit
            CFRelease(font);
            const float ascent = float(CGFontGetAscent(f.cgFont)), descent = float(-CGFontGetDescent(f.cgFont));
            float capHeight = float(CGFontGetCapHeight(f.cgFont));
            if (capHeight <= 0)
                capHeight = ascent * 0.7f;
            f.metrics = {upem, ascent, descent, std::max(0.0f, float(CGFontGetLeading(f.cgFont))), capHeight};
            readKern(f);
            g_faces.push_back(f);
            return int(g_faces.size()) - 1;
        }

        bool sameFamily(CTFontRef font, const char *wanted)
        {
            CFStringRef family = CTFontCopyFamilyName(font);
            if (!family)
                return false;
            char name[256] = {};
            CFStringGetCString(family, name, sizeof(name), kCFStringEncodingUTF8);
            CFRelease(family);
            return strcasecmp(name, wanted) == 0;
        }
    }

    bool init(std::string &) { return true; }

    void shutdown()
    {
        for (LoadedFace &f : g_faces)
        {
            if (f.sized)
                CFRelease(f.sized);
            if (f.font)
                CFRelease(f.font);
            if (f.cgFont)
                CGFontRelease(f.cgFont);
        }
        g_faces.clear();
    }

    const char *backendName() { return "Core Text"; }

    int loadFace(const std::vector<FaceRequest> &candidates)
    {
        @autoreleasepool
        {
            for (const FaceRequest &request : candidates)
            {
                // "system-ui": the Mac's own interface font (San Francisco), which has no family name to ask for
                if (strcasecmp(request.family, "system-ui") == 0)
                {
                    CTFontRef ui = CTFontCreateUIFontForLanguage(kCTFontUIFontSystem, 16.0, nullptr);
                    if (!ui)
                        continue;
                    NSDictionary *traits = @{(__bridge id)kCTFontWeightTrait : @(weightTrait(request.weight))};
                    NSDictionary *attrs = @{(__bridge id)kCTFontTraitsAttribute : traits};
                    CTFontDescriptorRef base = CTFontCopyFontDescriptor(ui);
                    CTFontDescriptorRef weighted = CTFontDescriptorCreateCopyWithAttributes(base, (__bridge CFDictionaryRef)attrs);
                    CFRelease(base);
                    CTFontRef font = CTFontCreateWithFontDescriptor(weighted, 16.0, nullptr);
                    CFRelease(weighted);
                    CFRelease(ui);
                    if (font)
                        return addFace(font);
                    continue;
                }
                NSDictionary *traits = @{(__bridge id)kCTFontWeightTrait : @(weightTrait(request.weight))};
                NSDictionary *attrs = @{(__bridge id)kCTFontFamilyNameAttribute : [NSString stringWithUTF8String:request.family],
                                        (__bridge id)kCTFontTraitsAttribute : traits};
                CTFontDescriptorRef desc = CTFontDescriptorCreateWithAttributes((__bridge CFDictionaryRef)attrs);
                CTFontRef font = CTFontCreateWithFontDescriptor(desc, 16.0, nullptr);
                CFRelease(desc);
                // Core Text always answers with something (Helvetica): only the family asked for counts
                if (font && sameFamily(font, request.family))
                    return addFace(font);
                if (font)
                    CFRelease(font);
            }
            return -1;
        }
    }

    int loadFaceFile(const std::string &utf8Path)
    {
        @autoreleasepool
        {
            NSURL *url = [NSURL fileURLWithPath:[NSString stringWithUTF8String:utf8Path.c_str()]];
            CFArrayRef descs = CTFontManagerCreateFontDescriptorsFromURL((__bridge CFURLRef)url);
            if (!descs || CFArrayGetCount(descs) == 0)
            {
                if (descs)
                    CFRelease(descs);
                return -1;
            }
            CTFontRef font = CTFontCreateWithFontDescriptor((CTFontDescriptorRef)CFArrayGetValueAtIndex(descs, 0), 16.0, nullptr);
            CFRelease(descs);
            return font ? addFace(font) : -1;
        }
    }

    FaceMetrics faceMetrics(int face) { return g_faces[size_t(face)].metrics; }

    uint16_t glyphIndex(int face, char32_t codepoint)
    {
        UniChar chars[2];
        CFIndex n = 1;
        if (codepoint >= 0x10000)
        {
            const char32_t v = codepoint - 0x10000;
            chars[0] = UniChar(0xD800 + (v >> 10));
            chars[1] = UniChar(0xDC00 + (v & 0x3FF));
            n = 2;
        }
        else
            chars[0] = UniChar(codepoint);
        CGGlyph glyphs[2] = {0, 0};
        if (!CTFontGetGlyphsForCharacters(g_faces[size_t(face)].font, chars, glyphs, n))
            return 0;
        return uint16_t(glyphs[0]);
    }

    float advance(int face, uint16_t glyph)
    {
        int advance = 0;
        CGGlyph g = glyph;
        if (!CGFontGetGlyphAdvances(g_faces[size_t(face)].cgFont, &g, 1, &advance))
            return 0;
        return float(advance);
    }

    float kerning(int face, uint16_t left, uint16_t right)
    {
        const auto &kern = g_faces[size_t(face)].kern;
        const uint32_t key = (uint32_t(left) << 16) | right;
        auto it = std::lower_bound(kern.begin(), kern.end(), std::make_pair(key, int16_t(-32768)));
        return it != kern.end() && it->first == key ? float(it->second) : 0.0f;
    }

    Bitmap rasterize(int face, uint16_t glyph, float px, int subpixel)
    {
        Bitmap out;
        LoadedFace &f = g_faces[size_t(face)];
        if (f.sizePx != px || !f.sized)
        {
            if (f.sized)
                CFRelease(f.sized);
            f.sized = CTFontCreateCopyWithAttributes(f.font, px, nullptr, nullptr);
            f.sizePx = px;
        }
        const CGGlyph g = glyph;
        const CGFloat penX = CGFloat(subpixel) / 4.0;
        const CGRect r = CTFontGetBoundingRectsForGlyphs(f.sized, kCTFontOrientationHorizontal, &g, nullptr, 1);
        if (r.size.width <= 0 || r.size.height <= 0)
            return out;
        // the ink's pixels with a pixel to spare (antialiasing), baseline-up coordinates
        const int x0 = int(std::floor(r.origin.x + penX)) - 1, x1 = int(std::ceil(r.origin.x + r.size.width + penX)) + 1;
        const int y0 = int(std::floor(r.origin.y)) - 1, y1 = int(std::ceil(r.origin.y + r.size.height)) + 1;
        out.width = x1 - x0;
        out.height = y1 - y0;
        out.left = x0;
        out.top = -y1;
        out.coverage.assign(size_t(out.width) * size_t(out.height), 0);
        CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
        CGContextRef ctx = CGBitmapContextCreate(out.coverage.data(), size_t(out.width), size_t(out.height), 8, size_t(out.width),
                                                 gray, kCGImageAlphaNone);
        CGColorSpaceRelease(gray);
        if (!ctx)
        {
            out = Bitmap();
            return out;
        }
        CGContextSetGrayFillColor(ctx, 1.0, 1.0);
        CGContextSetShouldAntialias(ctx, true);
        CGContextSetShouldSmoothFonts(ctx, false); // no stem darkening
        CGContextSetAllowsFontSubpixelPositioning(ctx, true);
        CGContextSetShouldSubpixelPositionFonts(ctx, true);
        CGContextSetShouldSubpixelQuantizeFonts(ctx, false);
        const CGPoint at = CGPointMake(penX - x0, -y0); // the bitmap's bottom-left is (x0, y0)
        CTFontDrawGlyphs(f.sized, &g, &at, 1, ctx);
        CGContextRelease(ctx); // rows top-down in memory, as Bitmap wants
        return out;
    }
}
