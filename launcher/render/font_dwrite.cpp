// Glyphs rasterised by DirectWrite: Windows' own rendering (natural symmetric, grayscale antialiasing), from the
// fonts every Windows has. No font library, nothing bundled.
#include "font_raster.h"

#include <windows.h>
#include <dwrite_3.h>

#include <cmath>

namespace render::raster
{
    namespace
    {
        IDWriteFactory2 *g_factory = nullptr;
        IDWriteFontCollection *g_collection = nullptr;

        struct LoadedFace
        {
            IDWriteFontFace *face = nullptr;
            IDWriteFontFace1 *face1 = nullptr; // kerning (Windows 8+)
            FaceMetrics metrics{};
            bool kerning = false;
        };
        std::vector<LoadedFace> g_faces;

        std::wstring wide(const char *utf8)
        {
            const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
            std::wstring out(size_t(n > 0 ? n - 1 : 0), L'\0');
            if (n > 1)
                MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
            return out;
        }

        template <typename T> void release(T *&p)
        {
            if (p)
                p->Release();
            p = nullptr;
        }

        // metrics and kerning of a face just made
        void finish(LoadedFace &loaded)
        {
            DWRITE_FONT_METRICS m{};
            loaded.face->GetMetrics(&m);
            loaded.metrics = {float(m.designUnitsPerEm), float(m.ascent), float(m.descent), float(m.lineGap), float(m.capHeight)};
            if (SUCCEEDED(loaded.face->QueryInterface(__uuidof(IDWriteFontFace1), reinterpret_cast<void **>(&loaded.face1))))
                loaded.kerning = loaded.face1->HasKerningPairs() != FALSE;
        }
    }

    bool init(std::string &error)
    {
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory2),
                                       reinterpret_cast<IUnknown **>(&g_factory))))
        {
            error = "DirectWrite is not available";
            return false;
        }
        if (FAILED(g_factory->GetSystemFontCollection(&g_collection, FALSE)))
        {
            error = "the system's fonts could not be read";
            return false;
        }
        return true;
    }

    void shutdown()
    {
        for (LoadedFace &f : g_faces)
        {
            release(f.face1);
            release(f.face);
        }
        g_faces.clear();
        release(g_collection);
        release(g_factory);
    }

    int loadFace(const std::vector<FaceRequest> &candidates)
    {
        for (const FaceRequest &request : candidates)
        {
            UINT32 index = 0;
            BOOL exists = FALSE;
            if (FAILED(g_collection->FindFamilyName(wide(request.family).c_str(), &index, &exists)) || !exists)
                continue;
            IDWriteFontFamily *family = nullptr;
            IDWriteFont *font = nullptr;
            LoadedFace loaded;
            if (SUCCEEDED(g_collection->GetFontFamily(index, &family)) &&
                SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT(request.weight), DWRITE_FONT_STRETCH_NORMAL,
                                                       DWRITE_FONT_STYLE_NORMAL, &font)) &&
                SUCCEEDED(font->CreateFontFace(&loaded.face)))
                finish(loaded);
            release(font);
            release(family);
            if (loaded.face)
            {
                g_faces.push_back(loaded);
                return int(g_faces.size()) - 1;
            }
        }
        return -1;
    }

    const char *backendName() { return "DirectWrite"; }

    int loadFaceFile(const std::string &utf8Path)
    {
        const std::wstring path = wide(utf8Path.c_str());
        IDWriteFontFile *file = nullptr;
        LoadedFace loaded;
        BOOL supported = FALSE;
        DWRITE_FONT_FILE_TYPE fileType{};
        DWRITE_FONT_FACE_TYPE faceType{};
        UINT32 faces = 0;
        if (SUCCEEDED(g_factory->CreateFontFileReference(path.c_str(), nullptr, &file)) &&
            SUCCEEDED(file->Analyze(&supported, &fileType, &faceType, &faces)) && supported && faces > 0 &&
            SUCCEEDED(g_factory->CreateFontFace(faceType, 1, &file, 0, DWRITE_FONT_SIMULATIONS_NONE, &loaded.face)))
            finish(loaded);
        release(file);
        if (!loaded.face)
            return -1;
        g_faces.push_back(loaded);
        return int(g_faces.size()) - 1;
    }

    FaceMetrics faceMetrics(int face) { return g_faces[size_t(face)].metrics; }

    uint16_t glyphIndex(int face, char32_t codepoint)
    {
        UINT16 glyph = 0;
        const UINT32 cp = codepoint;
        g_faces[size_t(face)].face->GetGlyphIndices(&cp, 1, &glyph);
        return glyph;
    }

    float advance(int face, uint16_t glyph)
    {
        DWRITE_GLYPH_METRICS m{};
        g_faces[size_t(face)].face->GetDesignGlyphMetrics(&glyph, 1, &m, FALSE);
        return float(m.advanceWidth);
    }

    float kerning(int face, uint16_t left, uint16_t right)
    {
        const LoadedFace &f = g_faces[size_t(face)];
        if (!f.kerning)
            return 0;
        const UINT16 pair[2] = {left, right};
        INT32 adjust[2] = {};
        if (FAILED(f.face1->GetKerningPairAdjustments(2, pair, adjust)))
            return 0;
        return float(adjust[0]);
    }

    Bitmap rasterize(int face, uint16_t glyph, float px, int subpixel)
    {
        Bitmap out;
        const FLOAT advance = 0;
        const DWRITE_GLYPH_OFFSET offset = {};
        DWRITE_GLYPH_RUN run{};
        run.fontFace = g_faces[size_t(face)].face;
        run.fontEmSize = px;
        run.glyphCount = 1;
        run.glyphIndices = &glyph;
        run.glyphAdvances = &advance;
        run.glyphOffsets = &offset;
        IDWriteGlyphRunAnalysis *analysis = nullptr;
        // grayscale coverage, vertically grid-fitted at text sizes (crisp baselines and x-heights)
        const DWRITE_GRID_FIT_MODE grid = px <= 28 ? DWRITE_GRID_FIT_MODE_ENABLED : DWRITE_GRID_FIT_MODE_DISABLED;
        if (FAILED(g_factory->CreateGlyphRunAnalysis(&run, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                                                     DWRITE_MEASURING_MODE_NATURAL, grid, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
                                                     float(subpixel) * 0.25f, 0.0f, &analysis)))
            return out;
        RECT bounds{};
        if (SUCCEEDED(analysis->GetAlphaTextureBounds(DWRITE_TEXTURE_ALIASED_1x1, &bounds)) && bounds.right > bounds.left &&
            bounds.bottom > bounds.top)
        {
            out.width = bounds.right - bounds.left;
            out.height = bounds.bottom - bounds.top;
            out.left = bounds.left;
            out.top = bounds.top;
            out.coverage.resize(size_t(out.width) * size_t(out.height));
            if (FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds, out.coverage.data(),
                                                    UINT32(out.coverage.size()))))
                out = Bitmap();
        }
        analysis->Release();
        return out;
    }
}
