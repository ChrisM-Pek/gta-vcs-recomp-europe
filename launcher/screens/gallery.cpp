// --screen gallery: every widget in its states on one page, for screenshots and for looking at a change to one.
#include "shell.h"

#include <deque>

namespace screens
{
    using namespace render;
    using core::withAlpha;
    using ui::ButtonStyle;

    void drawGallery(Rect r)
    {
        Rect area = r.inset(px(40), px(28));
        float y = area.y;
        const float x = area.x;
        auto title = [&](std::string_view t) {
            ui::eyebrow(x, y, theme::kFaint, t);
            y += px(24);
        };

        title("BUTTONS");
        float bx = x;
        const ButtonStyle styles[] = {ButtonStyle::Primary, ButtonStyle::Secondary, ButtonStyle::Ghost, ButtonStyle::Danger};
        const char *names[] = {"Play", "Secondary", "Ghost", "Danger"};
        const char32_t icons[] = {theme::icon::kPlay, theme::icon::kFolder, theme::icon::kRefresh, theme::icon::kStop};
        for (int i = 0; i < 4; ++i)
        {
            const float w = ui::buttonWidth(names[i], icons[i]);
            ui::button(names[i], {bx, y, w, px(40)}, styles[i], icons[i]);
            bx += w + px(10);
        }
        ui::button("Disabled", {bx, y, ui::buttonWidth("Disabled"), px(40)}, ButtonStyle::Primary, 0, false);
        bx += ui::buttonWidth("Disabled") + px(10);
        ui::iconButton("#g-icon", theme::icon::kSettings, {bx, y + px(3), px(34), px(34)}, "An icon button");
        // the icons the screens use
        {
            namespace I = theme::icon;
            const char32_t all[] = {I::kPlay, I::kSettings, I::kHelp, I::kSave, I::kFolder, I::kDisc, I::kDownload, I::kRefresh,
                                    I::kCheck, I::kCancel, I::kWarning, I::kInfo, I::kChevronRight, I::kDocument, I::kRepair,
                                    I::kChip, I::kClock, I::kSync, I::kLink, I::kUndo, I::kBug, I::kDrive, I::kOpen, I::kList,
                                    I::kGame, I::kStop, I::kOpenFile};
            y += px(52);
            float ix = x + px(8);
            for (char32_t c : all)
            {
                drawIcon(kFontIcons, px(15), ix, y + px(20), theme::kDim, c);
                ix += px(24);
            }
        }
        y += px(62);

        title("CONTROLS");
        static int seg = 0;
        static bool on = true, off = false;
        static int choice = 0;
        ui::segmented("#g-seg", {x, y, px(420), px(40)}, {"Standard", "Quick to build", "Fastest game"}, seg);
        ui::toggle("#g-on", {x + px(450), y + px(5), px(44), px(30)}, on);
        ui::toggle("#g-off", {x + px(510), y + px(5), px(44), px(30)}, off);
        y += px(56);
        for (int i = 0; i < 2; ++i)
            if (ui::choiceRow(i ? "#g-c1" : "#g-c0", {x - px(10), y + float(i) * px(58), px(440), px(54)}, i ? "Close the launcher" : "Minimize the launcher",
                              i ? "" : "It comes back when the game ends", choice == i))
                choice = i;
        y += px(128);

        title("STATUS");
        const ui::Status statuses[] = {ui::Status::Checking, ui::Status::Running, ui::Status::Good, ui::Status::Warning, ui::Status::Missing, ui::Status::Failed};
        const char *labels[] = {"Checking", "Running", "Good", "Warning", "Missing", "Failed"};
        for (int i = 0; i < 6; ++i)
        {
            const float cx = x + px(24) + float(i) * px(96);
            ui::statusBadge(ui::id("#g-badge", i), cx, y + px(18), px(14), statuses[i], 3);
            ui::label(kFontBody, px(12), {cx - px(44), y + px(40), px(88), px(18)}, theme::kDim, labels[i], ui::Align::Centre);
        }
        for (int i = 0; i < 4; ++i)
        {
            const char *tags[] = {"VERIFIED", "NEEDS AN UPDATE", "CHOSEN", "NOT FOUND"};
            const Color colors[] = {theme::kTeal, theme::kAmber, theme::kPink, theme::kRed};
            const float pxs = x + px(600) + float(i % 2) * px(150);
            ui::pill({pxs, y + float(i / 2) * px(30), ui::pillWidth(tags[i]), px(22)}, tags[i], colors[i]);
        }
        y += px(78);

        title("PROGRESS");
        ui::progressBar(ui::id("#g-bar"), {x, y, px(420), px(8)}, 0.62f, theme::kPink, theme::kPinkHi);
        ui::progressBar(ui::id("#g-bar2"), {x + px(460), y, px(300), px(8)}, -1, theme::kPink, theme::kPinkHi);
        y += px(26);
        const int pct = int(ui::state.time * 7) % 100;
        ui::rollingNumber(ui::id("#g-num"), kFontDisplay, px(48), x, y + px(44), theme::kText, pct);
        ui::spinner(x + px(160), y + px(28), px(12), theme::kPink, px(2.4f));
        y += px(72);

        title("CARD AND LOG");
        ui::card({x, y, px(360), px(120)});
        ui::label(kFontSemibold, px(theme::kBody), {x + px(24), y + px(20), px(300), px(22)}, theme::kText, "A card");
        ui::paragraph(kFontBody, px(theme::kSmall), {x + px(24), y + px(48), px(310), 0}, theme::kDim,
                      "Raised from the background by a soft shadow, with a hairline edge catching the light.");
        static std::deque<std::string> log;
        if (log.empty())
            for (int i = 0; i < 80; ++i)
                log.push_back("[build] [" + std::to_string(i * 40) + "/3400] Building CXX object generated/ps2_recomp_" + std::to_string(i) + ".cpp.obj");
        ui::logView(ui::id("#g-log"), {x + px(390), y, std::max(px(200), area.right() - x - px(390)), px(120)}, log);
    }
}
