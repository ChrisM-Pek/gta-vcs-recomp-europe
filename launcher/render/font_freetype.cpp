// Glyphs rasterised by FreeType from the fonts fontconfig finds: the Linux desktop's own fonts and rendering
// (grayscale antialiasing, light hinting - vertical only, so the quarter-pixel horizontal positions font.cpp asks for
// stay true, as with DirectWrite's natural symmetric mode). Both libraries are the system's; nothing is bundled.
#include "font_raster.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_TRUETYPE_TABLES_H
#include <fontconfig/fontconfig.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <strings.h>

namespace render::raster
{
    namespace
    {
        FT_Library g_library = nullptr;

        struct LoadedFace
        {
            FT_Face face = nullptr;
            FaceMetrics metrics{};
            float sizePx = -1; // the size FT_Set_Char_Size was last given
        };
        std::vector<LoadedFace> g_faces;

        bool generic(const char *family)
        {
            for (const char *g : {"sans-serif", "serif", "monospace", "sans", "mono"})
                if (strcasecmp(family, g) == 0)
                    return true;
            return false;
        }

        int addFace(FT_Face face)
        {
            LoadedFace loaded;
            loaded.face = face;
            const float upem = face->units_per_EM ? float(face->units_per_EM) : 1000.0f;
            const float ascent = float(face->ascender), descent = float(-face->descender);
            float capHeight = ascent * 0.7f;
            if (const auto *os2 = static_cast<const TT_OS2 *>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
                os2 && os2->version >= 2 && os2->sCapHeight > 0)
                capHeight = float(os2->sCapHeight);
            loaded.metrics = {upem, ascent, descent, std::max(0.0f, float(face->height) - ascent - descent), capHeight};
            g_faces.push_back(loaded);
            return int(g_faces.size()) - 1;
        }
    }

    bool init(std::string &error)
    {
        if (FT_Init_FreeType(&g_library) != 0)
        {
            error = "FreeType could not start";
            return false;
        }
        if (!FcInit())
        {
            error = "fontconfig could not read the system's fonts";
            return false;
        }
        return true;
    }

    void shutdown()
    {
        for (LoadedFace &f : g_faces)
            FT_Done_Face(f.face);
        g_faces.clear();
        if (g_library)
            FT_Done_FreeType(g_library);
        g_library = nullptr;
    }

    const char *backendName() { return "FreeType"; }

    int loadFace(const std::vector<FaceRequest> &candidates)
    {
        for (const FaceRequest &request : candidates)
        {
            FcPattern *pattern = FcPatternCreate();
            FcPatternAddString(pattern, FC_FAMILY, reinterpret_cast<const FcChar8 *>(request.family));
            FcPatternAddInteger(pattern, FC_WEIGHT, FcWeightFromOpenType(request.weight));
            FcPatternAddInteger(pattern, FC_SLANT, FC_SLANT_ROMAN);
            FcPatternAddBool(pattern, FC_SCALABLE, FcTrue);
            FcConfigSubstitute(nullptr, pattern, FcMatchPattern);
            FcDefaultSubstitute(pattern);
            FcResult result = FcResultNoMatch;
            FcPattern *match = FcFontMatch(nullptr, pattern, &result);
            FcPatternDestroy(pattern);
            if (!match)
                continue;
            // fontconfig always answers with something: only the family asked for counts (any for a generic name)
            bool found = generic(request.family);
            FcChar8 *family = nullptr;
            for (int i = 0; !found && FcPatternGetString(match, FC_FAMILY, i, &family) == FcResultMatch; ++i)
                found = strcasecmp(reinterpret_cast<const char *>(family), request.family) == 0;
            FcChar8 *file = nullptr;
            int index = 0;
            int face = -1;
            if (found && FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
            {
                FcPatternGetInteger(match, FC_INDEX, 0, &index); // a variable font's named instance in the high bits
                FT_Face ft = nullptr;
                if (FT_New_Face(g_library, reinterpret_cast<const char *>(file), index, &ft) == 0)
                    face = addFace(ft);
            }
            FcPatternDestroy(match);
            if (face >= 0)
                return face;
        }
        return -1;
    }

    int loadFaceFile(const std::string &utf8Path)
    {
        FT_Face ft = nullptr;
        if (FT_New_Face(g_library, utf8Path.c_str(), 0, &ft) != 0)
            return -1;
        return addFace(ft);
    }

    FaceMetrics faceMetrics(int face) { return g_faces[size_t(face)].metrics; }

    uint16_t glyphIndex(int face, char32_t codepoint)
    {
        return uint16_t(FT_Get_Char_Index(g_faces[size_t(face)].face, FT_ULong(codepoint)));
    }

    float advance(int face, uint16_t glyph)
    {
        FT_Face f = g_faces[size_t(face)].face;
        if (FT_Load_Glyph(f, glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_HINTING) != 0)
            return 0;
        return float(f->glyph->advance.x);
    }

    float kerning(int face, uint16_t left, uint16_t right)
    {
        FT_Face f = g_faces[size_t(face)].face;
        if (!FT_HAS_KERNING(f))
            return 0;
        FT_Vector k{};
        if (FT_Get_Kerning(f, left, right, FT_KERNING_UNSCALED, &k) != 0)
            return 0;
        return float(k.x);
    }

    Bitmap rasterize(int face, uint16_t glyph, float px, int subpixel)
    {
        Bitmap out;
        LoadedFace &loaded = g_faces[size_t(face)];
        FT_Face f = loaded.face;
        if (loaded.sizePx != px)
        {
            FT_Set_Char_Size(f, 0, FT_F26Dot6(std::lround(px * 64.0f)), 72, 72); // 72 dpi: points are pixels
            loaded.sizePx = px;
        }
        if (FT_Load_Glyph(f, glyph, FT_LOAD_TARGET_LIGHT | FT_LOAD_NO_BITMAP) != 0)
            return out;
        if (f->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
            FT_Outline_Translate(&f->glyph->outline, FT_Pos(subpixel * 16), 0); // quarter pixels in 26.6
        if (FT_Render_Glyph(f->glyph, FT_RENDER_MODE_LIGHT) != 0)
            return out;
        const FT_Bitmap &b = f->glyph->bitmap;
        if (b.pixel_mode != FT_PIXEL_MODE_GRAY || b.width == 0 || b.rows == 0)
            return out;
        out.width = int(b.width);
        out.height = int(b.rows);
        out.left = f->glyph->bitmap_left;
        out.top = -f->glyph->bitmap_top;
        out.coverage.resize(size_t(out.width) * size_t(out.height));
        for (int y = 0; y < out.height; ++y)
        {
            const unsigned char *row = b.pitch >= 0 ? b.buffer + size_t(y) * size_t(b.pitch)
                                                    : b.buffer + size_t(out.height - 1 - y) * size_t(-b.pitch);
            std::memcpy(&out.coverage[size_t(y) * size_t(out.width)], row, size_t(out.width));
        }
        return out;
    }
}
