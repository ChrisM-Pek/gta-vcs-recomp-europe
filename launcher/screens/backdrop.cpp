// Behind everything: the night sky, soft glows, and on Home a striped sun setting into the sea (the app icon's motif,
// launcher/make_icon.py), drawn with the renderer's own primitives. Still unless the mode changes: an idle window
// draws nothing. (A later option: art from the player's own disc in this slot - docs/LAUNCHER.md.)
#include "shell.h"

#include <cmath>

namespace screens
{
    using namespace render;
    using core::mix;
    using core::withAlpha;
    using core::rgb;

    void drawBackdrop(Rect r, float hero)
    {
        const Color skyTop = theme::kSky, skyBottom = theme::kNight;
        gradient(r, skyTop, skyBottom);
        const float w = r.w, h = r.h;
        const float s = std::min(w, h);

        // glows: a pink one up and to the right, a softer one low on the left
        shadow({w * 0.62f, -h * 0.25f, w * 0.5f, h * 0.55f}, withAlpha(theme::kPink, 0.09f + 0.05f * hero), s * 0.22f, s * 0.2f);
        shadow({-w * 0.2f, h * 0.65f, w * 0.55f, h * 0.5f}, withAlpha(theme::kPinkHi, 0.06f), s * 0.25f, s * 0.2f);

        if (hero <= 0.01f)
            return;
        const float e = core::easeOutCubic(hero);
        const float horizon = std::round(h * 0.70f);
        const float radius = s * 0.24f;
        const float cx = w * 0.73f;
        const float cy = horizon - radius * (0.15f + 0.45f * e); // it rises a little as Home appears
        pushOpacity(hero);

        // the sun's glow over the horizon
        shadow({cx - radius * 1.2f, cy - radius * 1.1f, radius * 2.4f, radius * 2.2f}, withAlpha(theme::kPinkHi, 0.20f), radius * 0.5f, radius);

        // the sun in slices: the gaps between them grow toward the horizon and show the sky behind
        pushClip({0, 0, w, horizon});
        const Rect disc{cx - radius, cy - radius, radius * 2, radius * 2};
        float y = disc.y;
        const float stripesFrom = cy - radius * 0.05f;
        int band = 0;
        while (y < horizon)
        {
            float sliceH = horizon - y, gap = 0;
            if (y >= stripesFrom)
            {
                sliceH = radius * (0.17f - 0.025f * float(band));
                gap = radius * (0.03f + 0.024f * float(band));
                ++band;
                if (sliceH < radius * 0.03f)
                    break;
            }
            else
                sliceH = stripesFrom - y;
            pushClip({0, y, w, sliceH});
            gradient(disc, theme::kPinkHi, theme::kPink, radius);
            popClip();
            y += sliceH + gap;
        }
        popClip();

        // the sea: darker below the horizon, lines closing in toward it, and the sun's reflection
        gradient({0, horizon, w, h - horizon}, theme::kSea, theme::kNight);
        rect({0, horizon, w, std::max(1.0f, px(1))}, withAlpha(theme::kPink, 0.55f));
        for (int i = 1; i < 9; ++i)
        {
            const float t = float(i) / 9.0f;
            const float ly = horizon + (h - horizon) * t * t;
            rect({0, std::round(ly), w, std::max(1.0f, px(1))}, withAlpha(theme::kPink, 0.12f * (1 - t) + 0.03f));
        }
        for (int i = 0; i < 6; ++i)
        {
            const float ly = horizon + px(8) + float(i) * px(10) * (1 + float(i) * 0.4f);
            const float rw = radius * (1.4f - 0.18f * float(i));
            rect({cx - rw * 0.5f, std::round(ly), rw, px(2.5f)}, withAlpha(mix(theme::kPinkHi, theme::kPink, float(i) / 6.0f), 0.30f - 0.045f * float(i)), px(1.25f));
        }
        popOpacity();
    }
}
