// The launcher's icons, drawn by our own code on both OSes (DECISIONS.md 2026-11-01: no icon font). Each is a few
// shapes on a 24-unit grid (strokes, arcs, discs, rounded rectangles, polygons) whose signed distances are turned into
// coverage at the exact pixel size asked for, then cached in the glyph atlas like a character: one glyph instance to
// draw, overlapping strokes never darker where they meet, crisp at every scale. The code points are the Segoe Fluent
// Icons ones theme::icon names (they are only identifiers now). Internal to render/.
#pragma once

#include "font_raster.h"

namespace render::icons
{
    // the icon at `px` (the size its 20-unit design box is drawn at), centred in a square bitmap; false for a code
    // point that is no icon of ours
    bool rasterize(char32_t codepoint, int px, raster::Bitmap &out);
}
