// On top of everything: dialogs (they scale up and fade in over a dim, and block the rest of the window) and toasts
// (short messages that rise at the bottom and leave on their own).
#include "shell.h"

#include "model/os.h"
#include "model/system.h"
#include "platform/platform.h"

#include <algorithm>
#include <cmath>

namespace fs = std::filesystem;

namespace screens
{
    using namespace render;
    using core::alphaOf;
    using core::mix;
    using core::withAlpha;
    using core::rgb;
    using ui::ButtonStyle;

    namespace
    {
        struct DialogButton
        {
            std::string label;
            ButtonStyle style;
            int result;
        };

        // a dialog's frame and buttons; returns the result of the button pressed, or -1
        int dialogBox(Rect window, float t, std::string_view title, const std::vector<std::pair<std::string, Color>> &paragraphs,
                      const std::vector<DialogButton> &buttons)
        {
            const float width = std::min(px(480), window.w - px(48));
            const float textW = width - px(64);
            float h = px(32) + px(32);
            for (const auto &p : paragraphs)
                h += ui::paragraphHeight(kFontBody, px(15), textW, p.first) + px(10);
            h += px(28) + px(40) + px(28);
            const float e = core::easeOutCubic(t);
            const Rect box = core::centred(window, width, h).offset(0, (1 - e) * px(12)).scaled(0.96f + 0.04f * core::easeOutBack(t));
            ui::blockInput(box);
            render::pushOpacity(core::clamp01(t * 1.4f));
            shadow(box.offset(0, px(16)), theme::shade(0.9f), px(34), px(16));
            rectEx(box, theme::kSurfaceHi, theme::kSurface, px(16), theme::ink(0.08f), ui::hairline());
            float y = box.y + px(30);
            drawText(kFontSemibold, px(21), box.x + px(32), y + metrics(kFontSemibold, px(21)).ascent, theme::kText, title);
            y += px(42);
            for (const auto &p : paragraphs)
                y += ui::paragraph(kFontBody, px(15), {box.x + px(32), y, textW, 0}, p.second ? p.second : theme::kDim, p.first) + px(10);
            Rect row{box.x + px(32), box.bottom() - px(28) - px(40), textW, px(40)};
            int result = -1;
            for (const DialogButton &b : buttons)
            {
                const float w = ui::buttonWidth(b.label);
                if (ui::button(b.label + "##dialog", core::cutRight(row, w), b.style))
                    result = b.result;
                core::cutRight(row, px(10));
            }
            // Enter: the first button (the one on the right, the dialog's own action)
            if (result < 0 && !buttons.empty() && platform::input.key(platform::kKeyEnter) && !ui::state.focusVisible && t > 0.5f)
                result = buttons.front().result;
            render::popOpacity();
            return result;
        }

        void buildDirDialog(App &app, Rect window, float t)
        {
            const launcher::Json &c = app.buildDirCheck;
            const bool ok = c["ok"].boolean();
            const bool current = c["current"].boolean();
            const fs::path target = launcher::pathFrom(c["path"].str());
            std::vector<std::pair<std::string, Color>> text = {{c["path"].str(), theme::kText}};
            if (!ok)
                text.push_back({"Because " + c["problem"].str() + ".", theme::kRed});
            else
            {
                text.push_back({std::string(c["default"].boolean() ? "The default, next to the source. " : "") +
                                    (c["reused"].boolean() ? "A build folder already there is used again. " : "") +
                                    gigabytes(c["free_bytes"].num()) + " free on that drive.",
                                0});
                for (size_t i = 0; i < c["warnings"].size(); ++i)
                    text.push_back({c["warnings"][i].str(), theme::kAmber});
                if (!current)
                {
                    std::error_code ec;
                    const bool built = fs::is_regular_file(app.build / launcher::os::kGameExe, ec);
                    text.push_back({std::string("The game is built again there (10 to 60 minutes, once); your game settings and disc choice come along.") +
                                        (built ? " The current build stays in " + launcher::pathText(app.build) + " until you delete it." : ""),
                                    0});
                }
            }
            std::vector<DialogButton> buttons;
            if (ok && !current)
                buttons.push_back({"Build here", ButtonStyle::Primary, 1});
            buttons.push_back({"Choose another", ButtonStyle::Secondary, 2});
            buttons.push_back({ok && current ? "OK" : "Cancel", ButtonStyle::Ghost, 0});
            const int r = dialogBox(window, t, !ok ? "Can't build there" : current ? "Already building there" : "Build here?", text, buttons);
            if (r == 1)
                app.startBuildDirSet(target);
            if (r == 2)
                chooseBuildDir(target.parent_path());
            if (r >= 0)
                shell.dialog = Dialog::None;
        }
    }

    void drawDialogs(Rect window)
    {
        App &app = *shell.app;
        if (shell.dialog != Dialog::None)
            shell.dialogShown = shell.dialog;
        if (shell.dialog == Dialog::None && shell.dialogT <= 0.001f)
        {
            shell.dialogShown = Dialog::None;
            return;
        }
        // a quick, slightly springy entrance; a quicker exit
        ui::animate(shell.dialogT, shell.dialog != Dialog::None ? 1.0f : 0.0f, shell.dialog != Dialog::None ? 14.0f : 22.0f);
        const float t = shell.dialogT;
        const Layer previous = setLayer(Layer::Overlay);
        ui::beginLayer(kLayerDialog, shell.dialog != Dialog::None);
        rect(window, withAlpha(theme::kScrim, alphaOf(theme::kScrim) * t));
        if (ui::interact(ui::id("#dialogscrim"), window, false).clicked)
            shell.dialog = Dialog::None;
        const bool leaving = shell.dialog == Dialog::None;
        if (leaving)
            ui::pushDisabled(); // drawn, not touchable
        switch (shell.dialogShown)
        {
        case Dialog::Restore:
        {
            const int r = dialogBox(window, t, "Restore this backup?",
                                    {{shell.restoreName + " replaces your memory card. The card as it is now is backed up first.", 0}},
                                    {{"Restore", ButtonStyle::Primary, 1}, {"Cancel", ButtonStyle::Ghost, 0}});
            if (r == 1)
            {
                app.startRestore(shell.restoreName);
                shell.backupsAsked = false;
                shell.sheet = Sheet::None;
            }
            if (r >= 0)
                shell.dialog = Dialog::None;
            break;
        }
        case Dialog::Quit:
        {
            const std::string what = app.task ? app.task->title : std::string("A task");
            const int r = dialogBox(window, t, "Stop and close?",
                                    {{what + " is still running. Closing stops it; starting it again later continues where it stopped.", 0}},
                                    {{"Stop and close", ButtonStyle::Danger, 1}, {"Keep it running", ButtonStyle::Ghost, 0}});
            if (r == 1)
            {
                app.cancelTask();
                shell.quitNow = true;
            }
            if (r >= 0)
                shell.dialog = Dialog::None;
            break;
        }
        case Dialog::CancelTask:
        {
            const std::string what = app.task ? app.task->title : std::string("The task");
            const int r = dialogBox(window, t, "Stop now?",
                                    {{what + " stops now. Starting it again continues where it stopped.", 0}},
                                    {{"Stop", ButtonStyle::Danger, 1}, {"Keep going", ButtonStyle::Ghost, 0}});
            if (r == 1)
                app.cancelTask();
            if (r >= 0)
                shell.dialog = Dialog::None;
            break;
        }
        case Dialog::BuildDir:
            buildDirDialog(app, window, t);
            break;
        default:
            break;
        }
        if (leaving)
            ui::popDisabled();
        ui::endLayer();
        setLayer(previous);
    }

    void drawToasts(Rect window)
    {
        constexpr double kLife = 6.0;
        const double now = ui::state.wall;
        shell.toasts.erase(std::remove_if(shell.toasts.begin(), shell.toasts.end(), [&](const Toast &t) { return now - t.since > kLife; }),
                           shell.toasts.end());
        if (shell.toasts.empty())
            return;
        const Layer previous = setLayer(Layer::Overlay);
        float y = window.bottom() - px(28);
        for (auto it = shell.toasts.rbegin(); it != shell.toasts.rend(); ++it)
        {
            const Toast &toast = *it;
            const float age = float(now - toast.since);
            const float in = ui::state.motion > 0 ? core::easeOutBack(age / 0.35f) : 1.0f;
            const float out = ui::state.motion > 0 ? core::clamp01(float(kLife - age) / 0.35f) : 1.0f;
            if (age < 0.35f || kLife - age < 0.35f)
                ui::keepAnimating();
            ui::wakeAt(toast.since + kLife - 0.35);
            const float size = px(theme::kBody);
            const std::string text = ellipsize(kFontBody, size, toast.text, std::min(px(640), window.w - px(120)));
            const float w = measure(kFontBody, size, text) + px(62);
            const float h = px(42);
            const Rect box{window.cx() - w * 0.5f, y - h + (1 - in) * px(24) - (1 - out) * px(8), w, h};
            render::pushOpacity(core::clamp01(in) * out);
            shadow(box.offset(0, px(8)), theme::shade(0.9f), px(16), h * 0.5f);
            rectEx(box, theme::kSurfaceTop, theme::kSurfaceHi, h * 0.5f, theme::ink(0.09f), ui::hairline());
            circle(box.x + px(22), box.cy(), px(4), toast.color);
            drawText(kFontBody, size, box.x + px(38), box.cy() + px(5), theme::kText, text);
            render::popOpacity();
            y -= (h + px(10)) * out;
        }
        setLayer(previous);
    }
}
