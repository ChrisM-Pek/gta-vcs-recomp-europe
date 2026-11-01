#include "widgets.h"

#include "render/draw.h"
#include "ui/theme.h"

#include <algorithm>
#include <cmath>
#include <unordered_map>

namespace ui
{
    using namespace render;
    using core::fade;
    using core::mix;
    using core::rgb;
    using core::withAlpha;

    namespace
    {
        // the label's baseline that centres the text's cap height in r
        float centredBaseline(Font font, float size, const Rect &r)
        {
            const FontMetrics m = metrics(font, size);
            return std::floor(r.cy() + (m.ascent - m.descent) * 0.5f + 0.5f);
        }

        void focusRing(const Rect &r, float amount, float radius)
        {
            if (amount <= 0.01f)
                return;
            const float g = px(3);
            outline({r.x - g, r.y - g, r.w + 2 * g, r.h + 2 * g}, withAlpha(theme::kPink, 0.85f * amount), px(2),
                    radius + g);
        }

        struct ScrollState
        {
            float pos = 0, target = 0, content = 0;
            Rect view;
            float bar = 0;          // scrollbar visibility 0..1
            double lastScroll = -10;
            bool dragging = false;
            float dragMouse = 0, dragPos = 0;
            bool stick = true;      // at the end: a log follows it
            bool toEnd = false;
            uint64_t frame = 0;
        };
        std::unordered_map<Id, ScrollState> g_scrolls;
    }

    // ---- text -----------------------------------------------------------------------------------------------------

    void label(Font font, float size, Rect r, Color color, std::string_view text, Align align)
    {
        const std::string shown = ellipsize(font, size, text, r.w);
        const float w = measure(font, size, shown);
        const float x = align == Align::Left ? r.x : align == Align::Centre ? r.cx() - w * 0.5f : r.right() - w;
        drawText(font, size, x, centredBaseline(font, size, r), color, shown);
    }

    float paragraphHeight(Font font, float size, float width, std::string_view text, float lineHeight)
    {
        if (text.empty())
            return 0;
        return float(wrap(font, size, text, width).size()) * std::ceil(size * lineHeight);
    }

    float paragraph(Font font, float size, Rect r, Color color, std::string_view text, float lineHeight)
    {
        if (text.empty())
            return 0;
        const float step = std::ceil(size * lineHeight);
        const FontMetrics m = metrics(font, size);
        float y = r.y;
        for (const std::string &line : wrap(font, size, text, r.w))
        {
            drawText(font, size, r.x, y + std::floor((step + m.ascent - m.descent) * 0.5f), color, line);
            y += step;
        }
        return y - r.y;
    }

    void eyebrow(float x, float y, Color color, std::string_view text)
    {
        // small capitals with a little tracking: one glyph at a time
        const float size = px(theme::kEyebrow);
        float pen = x;
        for (size_t at = 0; at < text.size();)
        {
            const size_t start = at;
            nextCodepoint(text, at);
            pen = drawText(kFontSemibold, size, pen, y + metrics(kFontSemibold, size).ascent, color,
                           text.substr(start, at - start)) + px(1.1f);
        }
    }

    // ---- controls -------------------------------------------------------------------------------------------------

    // what a label shows: the text before "##" (what follows only tells widgets with the same text apart)
    std::string_view shownText(std::string_view label) { return label.substr(0, label.find("##")); }

    namespace
    {
        // a button's badge: small capitals on solid pink
        constexpr float kBadgeText = 10.5f;
        float badgeWidth(std::string_view badge)
        {
            return badge.empty() ? 0 : std::ceil(measure(kFontSemibold, px(kBadgeText), badge) + px(14));
        }
        void drawBadge(float x, float cy, std::string_view badge)
        {
            const Rect r{x, std::round(cy - px(9)), badgeWidth(badge), px(18)};
            shadow(r.offset(0, px(2)), withAlpha(theme::kPink, 0.35f), px(4), r.h * 0.5f);
            gradient(r, theme::kPinkHi, theme::kPink, r.h * 0.5f);
            drawText(kFontSemibold, px(kBadgeText), r.x + px(7), centredBaseline(kFontSemibold, px(kBadgeText), r), theme::kOnPink, badge);
        }
    }

    float buttonWidth(std::string_view label, char32_t icon, float size, Font font, std::string_view badge)
    {
        const std::string_view text = shownText(label);
        const float s = size > 0 ? size : px(theme::kBody);
        return std::ceil(measure(font, s, text) + px(36) + (icon ? s + px(10) : 0) + (badge.empty() ? 0 : badgeWidth(badge) + px(10)));
    }

    bool button(std::string_view label, Rect r, ButtonStyle style, char32_t icon, bool enabled, float size, Font font,
                std::string_view badge)
    {
        const Interaction in = interact(id(label), r, true, enabled);
        const std::string_view text = shownText(label);
        Widget &w = *in.w;
        const float s = size > 0 ? size : px(theme::kBody);
        const float radius = std::min(px(theme::kRadiusSmall + 2), r.h * 0.5f);
        // hovering lifts it a little; pressing sinks it back and shrinks it slightly
        const float lift = (w.hover - w.press) * px(1);
        Rect box = r.scaled(1.0f - 0.025f * w.press).offset(0, -lift);
        render::pushOpacity(enabled ? 1.0f : 0.45f);
        Color textColor = theme::kText;
        switch (style)
        {
        case ButtonStyle::Primary:
            shadow(box.offset(0, px(6)), withAlpha(theme::kPink, 0.22f + 0.30f * w.hover), px(14 + 6 * w.hover), radius);
            gradient(box, mix(theme::kPink, rgb(0xFFFFFF), 0.10f * w.hover), mix(theme::kPinkHi, rgb(0xFFFFFF), 0.10f * w.hover),
                     radius, Gradient::Horizontal);
            // a highlight along the top edge: light from above
            gradient({box.x, box.y, box.w, box.h * 0.5f}, rgb(0xFFFFFF, 40), rgb(0xFFFFFF, 0), {radius, radius, 0, 0});
            textColor = theme::kOnPink;
            break;
        case ButtonStyle::Secondary:
            rectEx(box, mix(theme::kSurfaceHi, theme::kSurfaceTop, w.hover), mix(theme::kSurfaceHi, theme::kSurfaceTop, w.hover * 0.6f),
                   radius, theme::ink(0.07f + 0.06f * w.hover), hairline());
            break;
        case ButtonStyle::Ghost:
            rect(box, theme::ink(0.07f * w.hover + 0.04f * w.press), radius);
            textColor = mix(theme::kDim, theme::kText, w.hover);
            break;
        case ButtonStyle::Danger:
            rectEx(box, mix(theme::kDangerFill, theme::kDangerFillHi, w.hover), mix(theme::kDangerFill, theme::kDangerFillHi, w.hover), radius,
                   withAlpha(theme::kRed, 0.35f), hairline());
            textColor = mix(theme::kDangerText, theme::kText, 0.6f * w.hover);
            break;
        }
        const float textW = measure(font, s, text);
        const float iconW = icon ? s + px(10) : 0;
        const float badgeW = badge.empty() ? 0 : badgeWidth(badge) + px(10);
        float x = std::floor(box.cx() - (textW + iconW + badgeW) * 0.5f);
        if (icon)
        {
            drawIcon(kFontIcons, s * 0.95f, x + s * 0.5f, box.cy(), textColor, icon);
            x += iconW;
        }
        drawText(font, s, x, centredBaseline(font, s, box), textColor, text);
        if (!badge.empty())
            drawBadge(x + textW + px(10), box.cy(), badge);
        render::popOpacity();
        focusRing(r, w.focus, radius);
        return in.clicked && enabled;
    }

    bool iconButton(std::string_view idLabel, char32_t icon, Rect r, std::string_view tip, bool enabled, float iconSize)
    {
        const Interaction in = interact(id(idLabel), r, true, enabled);
        Widget &w = *in.w;
        const float radius = std::min(px(theme::kRadiusSmall), r.h * 0.5f);
        rect(r.scaled(1.0f - 0.06f * w.press), theme::ink(0.08f * w.hover + 0.05f * w.press), radius);
        const Color c = enabled ? mix(theme::kDim, theme::kText, w.hover) : theme::kFaint;
        drawIcon(kFontIcons, iconSize > 0 ? iconSize : px(15), r.cx(), r.cy(), c, icon);
        focusRing(r, w.focus, radius);
        tooltip(id(idLabel), r, tip);
        return in.clicked && enabled;
    }

    bool segmented(std::string_view idLabel, Rect r, const std::vector<std::string> &options, int &selected, bool enabled)
    {
        pushId(idLabel);
        Widget &w = widget(id("#track"));
        const int n = int(options.size());
        const float seg = r.w / float(std::max(n, 1));
        if (w.fresh)
            w.spring.snap(float(selected));
        spring(w.spring, float(selected), 420, 0.82f);
        const float radius = px(theme::kRadiusSmall + 1);
        render::pushOpacity(enabled ? 1.0f : 0.5f);
        rectEx(r, theme::kBase, theme::kBase, radius, theme::ink(0.06f), hairline());
        // the thumb slides on a spring to the chosen segment
        const Rect thumb = Rect{r.x + w.spring.value * seg, r.y, seg, r.h}.inset(px(3));
        shadow(thumb.offset(0, px(2)), theme::shade(0.67f), px(6), radius - px(3));
        rectEx(thumb, theme::kSurfaceTop, theme::kSurfaceHi, radius - px(3), theme::ink(0.08f), hairline());
        bool changed = false;
        for (int i = 0; i < n; ++i)
        {
            const Rect cell{r.x + float(i) * seg, r.y, seg, r.h};
            const Interaction in = interact(id(options[size_t(i)]), cell, true, enabled);
            if (in.clicked && selected != i)
            {
                selected = i;
                changed = true;
            }
            const float near = core::clamp01(1.0f - std::fabs(w.spring.value - float(i)));
            const Color c = mix(mix(theme::kFaint, theme::kDim, in.w->hover), theme::kText, near);
            label(kFontSemibold, px(theme::kSmall), cell.inset(px(8), 0), c, options[size_t(i)], Align::Centre);
            focusRing(cell.inset(px(2)), in.w->focus, radius);
        }
        render::popOpacity();
        popId();
        return changed;
    }

    bool toggle(std::string_view idLabel, Rect r, bool &value, bool enabled)
    {
        const Interaction in = interact(id(idLabel), r, true, enabled);
        Widget &w = *in.w;
        if (in.clicked)
            value = !value;
        if (w.fresh)
            w.spring.snap(value ? 1.0f : 0.0f);
        spring(w.spring, value ? 1.0f : 0.0f, 500, 0.7f);
        const float t = core::clamp01(w.spring.value);
        const Rect track = centred(r, px(38), px(22)).offset(r.w * 0.5f - px(19), 0);
        rectEx(track, mix(theme::kSurfaceTop, theme::kPink, t), mix(theme::kSurfaceTop, theme::kPinkHi, t), track.h * 0.5f,
               theme::ink(0.08f * (1 - t)), hairline());
        const float knob = px(16) * (1.0f + 0.12f * w.press);
        const float kx = track.x + px(3) + knob * 0.5f + w.spring.value * (track.w - px(6) - knob);
        shadow({kx - knob * 0.5f, track.cy() - knob * 0.5f + px(1), knob, knob}, theme::shade(0.67f), px(3), knob * 0.5f);
        circle(kx, track.cy(), knob * 0.5f, mix(theme::kDim, rgb(0xFFFFFF), std::max(t, w.hover * 0.5f)));
        focusRing(track, w.focus, track.h * 0.5f);
        return in.clicked;
    }

    bool choiceRow(std::string_view idLabel, Rect r, std::string_view title, std::string_view detail, bool selected,
                   bool enabled)
    {
        const Interaction in = interact(id(idLabel), r, true, enabled);
        Widget &w = *in.w;
        if (w.fresh)
            w.spring.snap(selected ? 1.0f : 0.0f);
        spring(w.spring, selected ? 1.0f : 0.0f, 520, 0.6f);
        rect(r, theme::ink(0.035f * w.hover + 0.03f * core::clamp01(w.spring.value)), px(10));
        const float cx = r.x + px(22), cy = r.y + px(detail.empty() ? 0 : 0) + r.h * 0.5f;
        const float ring = px(9);
        outline({cx - ring, cy - ring, ring * 2, ring * 2}, mix(theme::kFaint, theme::kPink, core::clamp01(w.spring.value)),
                px(1.6f), ring);
        const float dot = px(4.5f) * std::max(0.0f, w.spring.value);
        if (dot > 0.2f)
            circle(cx, cy, dot, theme::kPink);
        const float tx = r.x + px(44);
        if (detail.empty())
            label(kFontBody, px(theme::kBody), {tx, r.y, r.right() - tx - px(12), r.h}, theme::kText, title);
        else
        {
            label(kFontSemibold, px(theme::kBody), {tx, r.y + r.h * 0.5f - px(19), r.right() - tx - px(12), px(20)},
                  theme::kText, title);
            label(kFontBody, px(theme::kSmall), {tx, r.y + r.h * 0.5f + px(1), r.right() - tx - px(12), px(18)},
                  theme::kDim, detail);
        }
        focusRing(r, w.focus, px(10));
        return in.clicked && enabled;
    }

    // ---- surfaces and marks ---------------------------------------------------------------------------------------

    void card(Rect r, Color fill, Color edge)
    {
        const float radius = px(theme::kRadius);
        if (!fill)
            fill = theme::kSurface;
        shadow(r.offset(0, px(8)), theme::shade(0.55f), px(18), radius);
        rectEx(r, mix(fill, rgb(0xFFFFFF), 0.025f), fill, radius, theme::ink(0.06f), hairline());
        if (edge)
        {
            pushClip(r);
            rect({r.x, r.y, px(3), r.h}, edge, {radius, 0, 0, radius});
            popClip();
        }
    }

    float pillWidth(std::string_view text) { return std::ceil(measure(kFontSemibold, px(11.5f), text) + px(18)); }

    void pill(Rect r, std::string_view text, Color color)
    {
        rect(r, withAlpha(color, 0.16f), r.h * 0.5f);
        label(kFontSemibold, px(11.5f), r, color, text, Align::Centre);
    }

    void spinner(float cx, float cy, float radius, Color color, float thickness)
    {
        // a head that chases its tail: the arc grows and shrinks while it turns
        const float t = float(std::fmod(state.time, 1000.0));
        const float sweep = core::kPi * (0.35f + 0.55f * (0.5f + 0.5f * std::sin(t * 2.6f)));
        const float start = t * 5.2f + 0.6f * std::sin(t * 2.6f);
        arc(cx, cy, radius, thickness, start, sweep, color);
        keepAnimating();
    }

    void checkMark(float cx, float cy, float size, Color color, float thickness, float progress)
    {
        // two strokes drawn in order: the short one down, then the long one up
        const float ax = cx - size * 0.42f, ay = cy + size * 0.02f;
        const float bx = cx - size * 0.12f, by = cy + size * 0.30f;
        const float ex = cx + size * 0.44f, ey = cy - size * 0.30f;
        const float l1 = std::hypot(bx - ax, by - ay), l2 = std::hypot(ex - bx, ey - by);
        const float drawn = core::clamp01(progress) * (l1 + l2);
        if (drawn <= 0)
            return;
        const float f1 = std::min(drawn / l1, 1.0f);
        line(ax, ay, ax + (bx - ax) * f1, ay + (by - ay) * f1, thickness, color);
        if (drawn > l1)
        {
            const float f2 = std::min((drawn - l1) / l2, 1.0f);
            line(bx, by, bx + (ex - bx) * f2, by + (ey - by) * f2, thickness, color);
        }
    }

    void crossMark(float cx, float cy, float size, Color color, float thickness, float progress)
    {
        const float h = size * 0.32f;
        const float p1 = core::clamp01(progress * 2), p2 = core::clamp01(progress * 2 - 1);
        line(cx - h, cy - h, cx - h + 2 * h * p1, cy - h + 2 * h * p1, thickness, color);
        if (p2 > 0)
            line(cx + h, cy - h, cx + h - 2 * h * p2, cy - h + 2 * h * p2, thickness, color);
    }

    namespace
    {
        void drawStatus(Status status, float cx, float cy, float r, float appear, int number)
        {
            if (appear <= 0.01f)
                return;
            const float pop = core::easeOutBack(appear);
            const float rr = r * (0.6f + 0.4f * pop);
            render::pushOpacity(core::clamp01(appear * 1.6f));
            switch (status)
            {
            case Status::Checking:
                arc(cx, cy, rr - px(1), px(1.5f), 0, 2 * core::kPi, withAlpha(theme::kFaint, 0.35f));
                spinner(cx, cy, rr - px(1), theme::kDim, px(1.8f));
                break;
            case Status::Running:
                circle(cx, cy, rr, withAlpha(theme::kPink, 0.14f));
                spinner(cx, cy, rr - px(1), theme::kPink, px(2));
                break;
            case Status::Good:
                circle(cx, cy, rr, withAlpha(theme::kTeal, 0.16f));
                checkMark(cx, cy, rr * 1.15f, theme::kTeal, px(2), core::clamp01((appear - 0.15f) / 0.85f));
                break;
            case Status::Warning:
                circle(cx, cy, rr, withAlpha(theme::kAmber, 0.16f));
                line(cx, cy - rr * 0.42f, cx, cy + rr * 0.08f, px(2.2f), theme::kAmber);
                circle(cx, cy + rr * 0.40f, px(1.4f), theme::kAmber);
                break;
            case Status::Failed:
                circle(cx, cy, rr, withAlpha(theme::kRed, 0.16f));
                crossMark(cx, cy, rr * 1.2f, theme::kRed, px(2), core::clamp01((appear - 0.15f) / 0.85f));
                break;
            case Status::Missing:
            {
                arc(cx, cy, rr - px(1), px(1.5f), 0, 2 * core::kPi, theme::kFaint);
                if (number > 0)
                {
                    const std::string n = std::to_string(number);
                    const float s = r * 0.95f;
                    const float w = measure(kFontSemibold, s, n);
                    const FontMetrics m = metrics(kFontSemibold, s);
                    drawText(kFontSemibold, s, cx - w * 0.5f, std::floor(cy + (m.ascent - m.descent) * 0.5f), theme::kDim, n);
                }
                break;
            }
            }
            render::popOpacity();
        }
    }

    void statusBadge(Id wid, float cx, float cy, float radius, Status status, int number)
    {
        Widget &w = widget(wid);
        const int s = int(status);
        if (w.fresh || w.value < 0)
        {
            w.value = s;
            w.previous = -1;
            w.t = 1;
        }
        else if (w.value != s)
        {
            w.previous = w.value;
            w.value = s;
            w.t = 0;
        }
        // linear time for the morph, eased inside drawStatus
        if (w.t < 1)
        {
            w.t = state.motion > 0 ? std::min(1.0f, w.t + state.dt / 0.45f) : 1.0f;
            keepAnimating();
        }
        if (w.previous >= 0 && w.t < 1)
            drawStatus(Status(w.previous), cx, cy, radius, 1.0f - core::clamp01(w.t * 2.2f), number);
        drawStatus(status, cx, cy, radius, w.previous >= 0 ? w.t : 1.0f, number);
    }

    void progressBar(Id wid, Rect r, float value, Color from, Color to)
    {
        Widget &w = widget(wid);
        const float radius = r.h * 0.5f;
        rect(r, theme::ink(0.06f), radius);
        const float t = float(state.time);
        if (value < 0)
        {
            // indeterminate: a band slides across and back
            const float phase = float(std::fmod(t / 1.5, 1.0));
            const float e = core::easeInOutCubic(phase);
            const float bw = r.w * 0.32f;
            const float x = r.x - bw + e * (r.w + bw);
            pushClip(r);
            gradient({x, r.y, bw, r.h}, withAlpha(from, 0.0f), from, radius, Gradient::Horizontal);
            gradient({x + bw * 0.5f, r.y, bw * 0.5f, r.h}, from, to, radius, Gradient::Horizontal);
            popClip();
            keepAnimating();
            w.t = 0;
            return;
        }
        if (w.fresh)
            w.t = value;
        animate(w.t, core::clamp01(value), 6);
        const float fw = std::max(r.h, r.w * w.t);
        if (w.t <= 0.0005f)
            return;
        const Rect fill{r.x, r.y, fw, r.h};
        shadow(fill.offset(0, px(1)), withAlpha(from, 0.35f), px(6), radius);
        gradient(fill, from, to, radius, Gradient::Horizontal);
        // a sheen travels along the filled part every couple of seconds
        const float period = 2.4f;
        const float phase = float(std::fmod(t, double(period))) / period;
        const float sw = std::max(px(60), fw * 0.25f);
        const float sx = fill.x - sw + phase * 1.6f * (fw + sw);
        pushClip(fill);
        gradient({sx, r.y, sw * 0.5f, r.h}, rgb(0xFFFFFF, 0), rgb(0xFFFFFF, 70), 0, Gradient::Horizontal);
        gradient({sx + sw * 0.5f, r.y, sw * 0.5f, r.h}, rgb(0xFFFFFF, 70), rgb(0xFFFFFF, 0), 0, Gradient::Horizontal);
        popClip();
        keepAnimating();
    }

    float rollingNumber(Id wid, Font font, float size, float x, float baseline, Color color, int value)
    {
        Widget &w = widget(wid);
        if (w.fresh)
            w.t = float(value);
        animate(w.t, float(value), 4.5f); // unhurried: a change of one takes about half a second to settle
        const float v = std::max(0.0f, w.t);
        const int shownMax = std::max(value, int(std::ceil(v)));
        int digits = 1;
        for (int n = shownMax; n >= 10; n /= 10)
            ++digits;
        const FontMetrics m = metrics(font, size);
        // digits are cap height tall: the strip steps a little more than that, and the clip hides the rest
        const float cap = m.capHeight > 0 ? m.capHeight : m.ascent * 0.7f;
        const float step = std::ceil(cap * 1.35f);
        float digitW = 0;
        for (char c = '0'; c <= '9'; ++c)
            digitW = std::max(digitW, measure(font, size, std::string(1, c)));
        // A digit leaving rises, blurs and fades; the one arriving comes up from below out of a blur into focus. The
        // fades do the edges; the clip (with room for the blur) only keeps the strip's far digits out.
        const float maxBlur = cap * 0.16f;
        pushClip({x - maxBlur - px(2), baseline - cap - cap * 0.5f, digitW * float(digits) + 2 * maxBlur + px(4), cap * 2.4f});
        for (int i = 0; i < digits; ++i)
        {
            const float place = std::pow(10.0f, float(digits - 1 - i));
            const float base = std::floor(v / place);
            const float rem = v - base * place;
            // an odometer: a digit turns only while the ones below it roll over from 9
            const float carry = rem > place - 1 ? rem - (place - 1) : 0;
            const float pos = std::fmod(base, 10.0f) + carry;
            const int d0 = int(std::floor(pos)) % 10;
            const float frac = pos - std::floor(pos);
            const float dx = x + float(i) * digitW;
            const bool leading = i < digits - 1 && v < place; // a digit the number has not reached yet
            const float alpha = leading ? core::clamp01(v / place) : 1.0f;
            const std::string a(1, char('0' + d0)), b(1, char('0' + (d0 + 1) % 10));
            const float ca = (digitW - measure(font, size, a)) * 0.5f, cb = (digitW - measure(font, size, b)) * 0.5f;
            // smooth in, smooth out: the blur and the fade lead a little at both ends of the move
            const float s = frac * frac * (3 - 2 * frac);
            // the one leaving is gone before the motion's slow tail, so no ghost lingers above
            drawText(font, size, dx + ca, baseline - frac * step, fade(color, alpha * (1 - s) * (1 - s)), a, maxBlur * std::sqrt(frac));
            if (frac > 0.001f)
                drawText(font, size, dx + cb, baseline + (1 - frac) * step, fade(color, alpha * s), b, maxBlur * std::sqrt(1 - frac));
        }
        popClip();
        return digitW * float(digits);
    }

    // ---- scrolling ------------------------------------------------------------------------------------------------

    float beginScroll(Id wid, Rect view, bool followEnd)
    {
        ScrollState &s = g_scrolls[wid];
        if (s.frame + 1 < state.frame)
        {
            s = ScrollState();
        }
        s.frame = state.frame;
        s.view = view;
        const float maxScroll = std::max(0.0f, s.content - view.h);
        const platform::Input &in = platform::input;
        if (in.wheel != 0 && mouseOver(view) && maxScroll > 0)
        {
            s.target -= in.wheel * px(72);
            s.lastScroll = state.wall;
        }
        if (s.toEnd)
        {
            s.target = maxScroll;
            s.toEnd = false;
        }
        if (followEnd && s.stick && !s.dragging && in.wheel == 0)
            s.target = maxScroll; // following the end while it is there
        s.target = std::clamp(s.target, 0.0f, maxScroll);
        if (state.motion <= 0)
            s.pos = s.target;
        else
            animate(s.pos, s.target, 18);
        s.stick = followEnd && s.target >= maxScroll - px(2);
        pushClip(view);
        pushOffset(0, -std::round(s.pos));
        return s.pos;
    }

    void endScroll(Id wid, float contentBottom)
    {
        ScrollState &s = g_scrolls[wid];
        popOffset();
        popClip();
        s.content = contentBottom - s.view.y;
        const float maxScroll = std::max(0.0f, s.content - s.view.h);
        if (maxScroll <= 0)
            return;
        // the scrollbar: shown while hovering the area or just after scrolling
        const float thumbH = std::max(px(28), s.view.h * s.view.h / s.content);
        const float y = s.view.y + (s.pos / maxScroll) * (s.view.h - thumbH);
        const Rect thumb{s.view.right() - px(7), y, px(4), thumbH};
        const Interaction in = interact(wid ^ 0x5C5C5C5Cull, thumb.inset(-px(4), 0), false);
        if (in.pressed)
        {
            s.dragging = true;
            s.dragMouse = platform::input.mouseY;
            s.dragPos = s.target;
        }
        if (!platform::input.down[platform::kMouseLeft])
            s.dragging = false;
        if (s.dragging)
        {
            s.target = std::clamp(s.dragPos + (platform::input.mouseY - s.dragMouse) * maxScroll / (s.view.h - thumbH), 0.0f, maxScroll);
            s.pos = s.target;
            s.lastScroll = state.wall;
        }
        const bool show = mouseOver(s.view) || s.dragging || state.wall - s.lastScroll < 1.0;
        if (state.wall - s.lastScroll < 1.0)
            wakeAt(s.lastScroll + 1.0);
        animate(s.bar, show ? 1.0f : 0.0f, show ? 16.0f : 5.0f);
        if (s.bar > 0.01f)
            rect(thumb.inset(-in.w->hover * px(1), 0), theme::ink((0.16f + 0.14f * in.w->hover) * s.bar), px(2) + in.w->hover * px(1));
    }

    void scrollToEnd(Id wid) { g_scrolls[wid].toEnd = true; }

    bool scrolledToEnd(Id wid)
    {
        auto it = g_scrolls.find(wid);
        return it == g_scrolls.end() || it->second.stick;
    }

    void logView(Id wid, Rect r, const std::deque<std::string> &lines)
    {
        rectEx(r, theme::kLogFill, theme::kLogFill, px(10), theme::ink(0.05f), hairline());
        const Rect view = r.inset(px(14), px(10));
        const float size = px(12.5f);
        const float step = std::ceil(px(19));
        const float pos = beginScroll(wid, view, true);
        const FontMetrics m = metrics(kFontMono, size);
        const size_t first = size_t(std::max(0.0f, std::floor(pos / step)));
        const size_t last = std::min(lines.size(), first + size_t(view.h / step) + 2);
        for (size_t i = first; i < last; ++i)
        {
            const float y = view.y + float(i) * step;
            const std::string &line = lines[i];
            // the tools' own tags a little brighter, errors red
            const bool error = line.find("error") != std::string::npos || line.find("ERROR") != std::string::npos ||
                               line.find("FAILED") != std::string::npos;
            const Color c = error ? theme::kRed : line.rfind("[", 0) == 0 ? theme::kDim : theme::kFaint;
            // monospace: only the characters that fit are drawn (the clip takes the last one's overhang)
            const size_t fits = size_t(view.w / std::max(1.0f, measure(kFontMono, size, "M"))) + 2;
            drawText(kFontMono, size, view.x, y + std::floor((step + m.ascent - m.descent) * 0.5f), c,
                     std::string_view(line).substr(0, std::min(line.size(), fits * 2)));
        }
        endScroll(wid, view.y + float(lines.size()) * step);
    }
}
