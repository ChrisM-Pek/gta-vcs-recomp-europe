// The platform layer on Linux: what X11 and Wayland share (platform/linux/backend.h has the split).
//
// - The frame loop sleeps in poll() on the display connection, an eventfd that wake() writes from any thread (a tool's
//   output, a process ending) and the key-repeat timer: no polling, no CPU while idle.
// - Custom chrome as on Windows: the UI draws the title bar and says where it is (setChrome); a press on the caption
//   or on a resize edge is handed to the window manager / compositor (move, resize, double-click maximise), so
//   snapping and tiling stay the desktop's.
// - File dialogs: the desktop portal (the desktop's own dialog, right on Wayland and in sandboxes), else zenity, else
//   kdialog. They block the caller like Windows' do, with the launcher still drawing behind them.
// - Desktop settings: the portal's colour scheme and animation setting; on X11 also XSETTINGS (XFCE, MATE, Cinnamon).
#include "platform.h"

#include "platform/linux/backend.h"
#include "platform/linux/dbus.h"

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

namespace platform
{
    Input input;
    Window window;

    namespace
    {
        std::unique_ptr<linux_::Backend> g_backend;
        int g_wakeFd = -1;
        Chrome g_chrome;
        Cursor g_cursor = Cursor::Arrow;
        linux_::Shape g_shape = linux_::Shape::Arrow;
        void (*g_modalFrame)() = nullptr;
        bool g_inModalFrame = false, g_inDialog = false;
        double g_lastClickTime = -1, g_lastCaptionClick = -1, g_lastSettingsRead = -1;
        float g_pointerX = 0, g_pointerY = 0; // where the pointer is, chrome included
        bool g_pointerIn = false;
        float g_lastClickX = 0, g_lastClickY = 0;

        int ensureWakeFd()
        {
            if (g_wakeFd < 0)
                g_wakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
            return g_wakeFd;
        }

        void drainWake()
        {
            uint64_t value = 0;
            if (g_wakeFd >= 0 && read(g_wakeFd, &value, sizeof(value)) == sizeof(value))
                window.needsRedraw = true;
        }

        void runModalFrame()
        {
            if (!g_modalFrame || g_inModalFrame || window.minimized)
                return;
            g_inModalFrame = true;
            g_modalFrame();
            g_inModalFrame = false;
        }

        // the Windows virtual-key value of an X / xkb keysym (platform.h: keys are VK values everywhere)
        int virtualKey(uint32_t sym)
        {
            if (sym >= 'a' && sym <= 'z')
                return int(sym - 'a' + 'A');
            if ((sym >= 'A' && sym <= 'Z') || (sym >= '0' && sym <= '9') || sym == ' ')
                return int(sym);
            if (sym >= 0xffbe && sym <= 0xffc9) // XK_F1..XK_F12
                return kKeyF1 + int(sym - 0xffbe);
            switch (sym)
            {
            case 0xff08: return kKeyBackspace;       // BackSpace
            case 0xff09:                             // Tab
            case 0xfe20: return kKeyTab;             // ISO_Left_Tab (Shift+Tab)
            case 0xff0d:                             // Return
            case 0xff8d: return kKeyEnter;           // KP_Enter
            case 0xff1b: return kKeyEscape;          // Escape
            case 0xff55: case 0xff9a: return kKeyPageUp;   // Prior, KP_Prior
            case 0xff56: case 0xff9b: return kKeyPageDown; // Next, KP_Next
            case 0xff57: case 0xff9c: return kKeyEnd;      // End, KP_End
            case 0xff50: case 0xff95: return kKeyHome;     // Home, KP_Home
            case 0xff51: case 0xff96: return kKeyLeft;
            case 0xff52: case 0xff97: return kKeyUp;
            case 0xff53: case 0xff98: return kKeyRight;
            case 0xff54: case 0xff99: return kKeyDown;
            case 0xffe1: case 0xffe2: return kKeyShift;
            case 0xffe3: case 0xffe4: return kKeyControl;
            case 0xffe9: case 0xffea: case 0xfe03: return kKeyAlt; // Alt_L, Alt_R, ISO_Level3_Shift (AltGr)
            case 0xffff: return 0x2E;                // Delete (VK_DELETE)
            case 0xff63: return 0x2D;                // Insert (VK_INSERT)
            default: return 0;
            }
        }

        int hexValue(char c)
        {
            if (c >= '0' && c <= '9')
                return c - '0';
            if (c >= 'a' && c <= 'f')
                return c - 'a' + 10;
            if (c >= 'A' && c <= 'F')
                return c - 'A' + 10;
            return -1;
        }

        // a program's standard output, with frames drawn while it runs (zenity, kdialog). -1 when it can't start
        int runCapture(const std::vector<std::string> &argv, std::string &output, const std::function<void()> &idle)
        {
            output.clear();
            int out[2];
            if (pipe2(out, O_CLOEXEC) != 0)
                return -1;
            std::vector<char *> args;
            for (const auto &a : argv)
                args.push_back(const_cast<char *>(a.c_str()));
            args.push_back(nullptr);
            const pid_t pid = fork();
            if (pid == 0)
            {
                dup2(out[1], 1);
                const int nul = open("/dev/null", O_RDWR);
                if (nul >= 0)
                {
                    dup2(nul, 0);
                    dup2(nul, 2);
                }
                execvp(args[0], args.data());
                _exit(127);
            }
            close(out[1]);
            if (pid < 0)
            {
                close(out[0]);
                return -1;
            }
            char buffer[4096];
            for (;;)
            {
                pollfd p{out[0], POLLIN, 0};
                const int r = poll(&p, 1, 30);
                if (r < 0 && errno == EINTR)
                    continue;
                if (r > 0)
                {
                    const ssize_t n = read(out[0], buffer, sizeof(buffer));
                    if (n <= 0)
                        break;
                    output.append(buffer, size_t(n));
                }
                if (idle)
                    idle();
            }
            close(out[0]);
            int status = 0;
            while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
            {
            }
            while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
                output.pop_back();
            if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
                return -1;
            return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        }

        // while a dialog is up: events handled (the window repaints, the compositor sees us alive), input to the
        // launcher ignored, a frame now and then (spinners keep turning, at a modest rate)
        void dialogIdle()
        {
            static double lastFrame = -1;
            pumpEvents();
            releaseInput();
            endFrame();
            const double t = now();
            if (t - lastFrame >= 0.1 || window.needsRedraw)
            {
                lastFrame = t;
                runModalFrame();
            }
        }

        enum class DialogKind
        {
            Open,
            Save,
            Folder
        };

        bool fileDialog(DialogKind kind, std::filesystem::path &out, const char *title, const std::vector<FileType> &types,
                        const std::filesystem::path &start, const char *defaultExtension)
        {
            g_inDialog = true;
            std::vector<dbus::FileFilter> filters;
            for (const FileType &t : types)
                filters.push_back({t.name, {t.pattern}});
            const std::string folder = kind == DialogKind::Save ? start.parent_path().string() : start.string();
            const std::string name = kind == DialogKind::Save ? start.filename().string() : std::string();
            std::vector<std::string> picked;
            const dbus::ChooserKind chooser = kind == DialogKind::Open   ? dbus::ChooserKind::Open
                                              : kind == DialogKind::Save ? dbus::ChooserKind::Save
                                                                         : dbus::ChooserKind::Folder;
            int result = dbus::fileChooser(chooser, g_backend ? g_backend->parentHandle() : std::string(), title, filters,
                                           folder, name, picked, dialogIdle);
            if (result < 0)
            {
                // no portal: zenity (GTK), else kdialog (KDE)
                std::string output;
                std::vector<std::string> z = {"zenity", "--file-selection", std::string("--title=") + title};
                if (kind == DialogKind::Save)
                    z.insert(z.end(), {"--save", "--confirm-overwrite"});
                if (kind == DialogKind::Folder)
                    z.push_back("--directory");
                if (!start.empty())
                    z.push_back("--filename=" + start.string() + (kind == DialogKind::Save ? "" : "/"));
                for (const FileType &t : types)
                    z.push_back(std::string("--file-filter=") + t.name + " | " + t.pattern);
                result = runCapture(z, output, dialogIdle);
                if (result < 0)
                {
                    std::string filter;
                    for (const FileType &t : types)
                        filter += (filter.empty() ? "" : "\n") + std::string(t.pattern) + "|" + t.name;
                    std::vector<std::string> k = {"kdialog", "--title", title};
                    if (kind == DialogKind::Open)
                        k.insert(k.end(), {"--getopenfilename", start.empty() ? "." : start.string(), filter});
                    else if (kind == DialogKind::Save)
                        k.insert(k.end(), {"--getsavefilename", start.string(), filter});
                    else
                        k.insert(k.end(), {"--getexistingdirectory", start.empty() ? "." : start.string()});
                    result = runCapture(k, output, dialogIdle);
                }
                if (result < 0)
                    std::fprintf(stderr, "[platform] no file dialog: neither the desktop portal nor zenity / kdialog\n");
                else if (result == 0 && !output.empty())
                {
                    picked = {output};
                    result = 1;
                }
                else
                    result = 0;
            }
            g_inDialog = false;
            releaseInput();
            window.needsRedraw = true;
            if (result != 1 || picked.empty())
                return false;
            out = std::filesystem::path(picked[0]);
            if (kind == DialogKind::Save && defaultExtension && *defaultExtension && !out.has_extension())
                out += std::string(".") + defaultExtension;
            return true;
        }
    }

    namespace linux_
    {
        float edgeWidth() { return std::round(6.0f * window.dpiScale); }

        Region regionAt(float x, float y)
        {
            if (!window.maximized)
            {
                const float b = edgeWidth(), top = std::max(std::round(b / 2), 3.0f);
                const bool left = x < b, right = x >= float(window.width) - b;
                const bool upper = y < top, lower = y >= float(window.height) - b;
                if (upper && left) return Region::TopLeft;
                if (upper && right) return Region::TopRight;
                if (lower && left) return Region::BottomLeft;
                if (lower && right) return Region::BottomRight;
                if (upper) return Region::Top;
                if (lower) return Region::Bottom;
                if (left) return Region::Left;
                if (right) return Region::Right;
            }
            if (g_chrome.maximizeButton.contains(x, y))
                return Region::Client;
            for (const core::Rect &r : g_chrome.buttons)
                if (r.contains(x, y))
                    return Region::Client;
            if (g_chrome.caption.contains(x, y))
                return Region::Caption;
            return Region::Client;
        }

        Shape shapeFor(Region region)
        {
            switch (region)
            {
            case Region::Top: return Shape::ResizeN;
            case Region::Bottom: return Shape::ResizeS;
            case Region::Left: return Shape::ResizeW;
            case Region::Right: return Shape::ResizeE;
            case Region::TopLeft: return Shape::ResizeNW;
            case Region::TopRight: return Shape::ResizeNE;
            case Region::BottomLeft: return Shape::ResizeSW;
            case Region::BottomRight: return Shape::ResizeSE;
            case Region::Caption: return Shape::Arrow;
            case Region::Client: break;
            }
            return g_cursor == Cursor::Hand ? Shape::Hand : g_cursor == Cursor::Text ? Shape::Text : Shape::Arrow;
        }

        void updateShape()
        {
            if (!g_backend || !g_pointerIn)
                return;
            const Shape shape = shapeFor(regionAt(g_pointerX, g_pointerY));
            if (shape != g_shape)
            {
                g_shape = shape;
                g_backend->setShape(shape);
            }
        }

        void pointerMotion(float x, float y)
        {
            g_pointerX = x;
            g_pointerY = y;
            g_pointerIn = true;
            const bool held = input.down[0] || input.down[1] || input.down[2];
            if (!held && regionAt(x, y) != Region::Client)
            {
                // over the chrome: nothing in the UI is hovered (Windows' non-client area)
                if (input.mouseInside)
                {
                    input.mouseInside = false;
                    input.mouseX = input.mouseY = -1e6f;
                    input.mouseMoved = true;
                    window.needsRedraw = true;
                }
                updateShape();
                return;
            }
            input.mouseMoved = input.mouseMoved || x != input.mouseX || y != input.mouseY;
            input.mouseX = x;
            input.mouseY = y;
            input.mouseInside = true;
            window.needsRedraw = true;
            updateShape();
        }

        void pointerLeave()
        {
            g_pointerIn = false;
            input.mouseInside = false;
            input.mouseX = input.mouseY = -1e6f;
            input.mouseMoved = true;
            window.needsRedraw = true;
            g_shape = Shape(-1); // whatever the pointer shows on its return is set again
        }

        void pointerWheel(float notches)
        {
            input.wheel += notches;
            window.needsRedraw = true;
        }

        void keyEvent(uint32_t keysym, bool down, bool repeat)
        {
            (void)repeat; // a repeat is a press again, as Windows reports it
            const int vk = virtualKey(keysym);
            if (vk > 0 && vk < 256)
            {
                input.keyDown[vk] = down;
                if (down)
                    input.keyPressed[vk] = true;
            }
            window.needsRedraw = true;
        }

        void textInput(const char *utf8)
        {
            for (size_t at = 0; utf8[at];)
            {
                const unsigned char c = static_cast<unsigned char>(utf8[at++]);
                char32_t cp = c;
                int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : 0;
                if (c >= 0x80 && extra == 0)
                    continue;
                cp = extra ? c & (0x3F >> extra) : c;
                for (; extra > 0 && (static_cast<unsigned char>(utf8[at]) & 0xC0) == 0x80; --extra)
                    cp = (cp << 6) | (static_cast<unsigned char>(utf8[at++]) & 0x3F);
                if (extra == 0 && cp >= 32 && cp != 127)
                    input.text += cp;
            }
            window.needsRedraw = true;
        }

        void modifiers(bool ctrl, bool shift, bool alt)
        {
            input.ctrl = ctrl;
            input.shift = shift;
            input.alt = alt;
        }

        void focusChanged(bool focused)
        {
            window.focused = focused;
            window.needsRedraw = true;
            if (!focused)
                releaseInput();
            else if (now() - g_lastSettingsRead > 2.0)
                readPortalSettings(); // the desktop's style may have changed meanwhile
        }

        void dropEnter(float x, float y)
        {
            input.dragOver = true;
            dropMotion(x, y);
        }

        void dropMotion(float x, float y)
        {
            input.dragX = x;
            input.dragY = y;
            window.needsRedraw = true;
        }

        void dropLeave()
        {
            input.dragOver = false;
            window.needsRedraw = true;
        }

        void dropFiles(const std::string &uriList)
        {
            input.dragOver = false;
            window.needsRedraw = true;
            size_t at = 0;
            while (at < uriList.size())
            {
                size_t end = uriList.find('\n', at);
                if (end == std::string::npos)
                    end = uriList.size();
                std::string line = uriList.substr(at, end - at);
                at = end + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\0'))
                    line.pop_back();
                if (line.empty() || line[0] == '#' || line.rfind("file://", 0) != 0)
                    continue;
                std::string s = line.substr(7);
                if (!s.empty() && s[0] != '/') // file://host/path
                    s = s.find('/') == std::string::npos ? std::string() : s.substr(s.find('/'));
                std::string path;
                for (size_t i = 0; i < s.size(); ++i)
                {
                    if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0)
                    {
                        path += char(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
                        i += 2;
                    }
                    else
                        path += s[i];
                }
                if (!path.empty())
                    input.dropped.emplace_back(path);
            }
        }

        void resized(int width, int height, bool maximized)
        {
            window.width = width;
            window.height = height;
            window.maximized = maximized;
            window.needsRedraw = true;
            if (g_inDialog)
                runModalFrame();
        }

        void scaleChanged(float scale)
        {
            if (scale > 0 && scale != window.dpiScale)
            {
                window.dpiScale = scale;
                window.needsRedraw = true;
            }
        }

        void minimizedChanged(bool minimized)
        {
            window.minimized = minimized;
            window.needsRedraw = true;
        }

        void closeRequested()
        {
            window.quitRequested = true;
            window.needsRedraw = true;
        }

        void redraw() { window.needsRedraw = true; }

        bool g_portalScheme = false;
        bool portalHasColorScheme() { return g_portalScheme; }

        void readPortalSettings()
        {
            g_lastSettingsRead = now();
            const dbus::Setting scheme = dbus::portalSetting("org.freedesktop.appearance", "color-scheme");
            g_portalScheme = scheme.kind == dbus::Setting::Number && scheme.number != 0;
            if (g_portalScheme)
                window.darkMode = scheme.number == 1;
            else
            {
                // no preference: the GTK theme's name says it (Adwaita-dark, Mint-Y-Dark...)
                const dbus::Setting theme = dbus::portalSetting("org.gnome.desktop.interface", "gtk-theme");
                if (theme.kind == dbus::Setting::Text && !theme.text.empty())
                {
                    std::string lower = theme.text;
                    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                    window.darkMode = lower.find("dark") != std::string::npos;
                }
            }
            const dbus::Setting animations = dbus::portalSetting("org.gnome.desktop.interface", "enable-animations");
            if (animations.kind == dbus::Setting::Boolean)
                window.reducedMotion = animations.number == 0;
            window.needsRedraw = true;
        }
    }

    bool init(const char *title, int logicalWidth, int logicalHeight, int minLogicalWidth, int minLogicalHeight,
              std::string &error)
    {
        now();
        ensureWakeFd();
        window.darkMode = true; // until the desktop says otherwise
        linux_::readPortalSettings();
        const char *forced = std::getenv("VCS_LAUNCHER_VIDEO");
        const std::string choice = forced ? forced : "";
        const bool waylandSession = std::getenv("WAYLAND_DISPLAY") && *std::getenv("WAYLAND_DISPLAY");
        std::vector<std::unique_ptr<linux_::Backend> (*)()> order;
        if (choice == "x11")
            order = {linux_::makeX11};
        else if (choice == "wayland")
            order = {linux_::makeWayland};
        else if (waylandSession)
            order = {linux_::makeWayland, linux_::makeX11};
        else
            order = {linux_::makeX11, linux_::makeWayland};
        std::string errors;
        for (auto make : order)
        {
            std::unique_ptr<linux_::Backend> backend = make();
            std::string e;
            if (backend && backend->init(title, logicalWidth, logicalHeight, minLogicalWidth, minLogicalHeight, e))
            {
                g_backend = std::move(backend);
                return true;
            }
            if (backend)
                backend->shutdown();
            errors += (errors.empty() ? "" : "; ") + e;
        }
        error = "could not open a window (" + errors + ")";
        return false;
    }

    void show()
    {
        if (g_backend)
            g_backend->show();
    }

    void shutdown()
    {
        if (g_backend)
            g_backend->shutdown();
        g_backend.reset();
    }

    void *nativeHandle() { return g_backend ? const_cast<NativeSurface *>(g_backend->surface()) : nullptr; }

    void setChrome(const Chrome &chrome)
    {
        g_chrome = chrome;
    }

    void pumpEvents()
    {
        drainWake();
        if (!g_backend)
            return;
        g_backend->dispatch();
        if (const int t = g_backend->timerFd(); t >= 0)
        {
            uint64_t expirations = 0;
            if (read(t, &expirations, sizeof(expirations)) == sizeof(expirations) && expirations)
                g_backend->timerFired();
        }
    }

    void waitForEvents(double timeoutSeconds)
    {
        int ms = -1;
        if (timeoutSeconds >= 0)
            ms = int(std::min(timeoutSeconds * 1000.0 + 1.0, 3600000.0));
        pollfd fds[3];
        int n = 0;
        fds[n++] = {ensureWakeFd(), POLLIN, 0};
        int display = -1, timer = -1;
        if (g_backend)
        {
            g_backend->prepareWait();
            display = n;
            fds[n++] = {g_backend->fd(), POLLIN, 0};
            if (const int t = g_backend->timerFd(); t >= 0)
            {
                timer = n;
                fds[n++] = {t, POLLIN, 0};
            }
        }
        const int r = poll(fds, nfds_t(n), ms);
        if (g_backend)
            g_backend->afterWait(r > 0 && display >= 0 && (fds[display].revents & POLLIN));
        (void)timer;
        pumpEvents();
    }

    void wake()
    {
        const uint64_t one = 1;
        if (ensureWakeFd() >= 0)
            [[maybe_unused]] ssize_t n = write(g_wakeFd, &one, sizeof(one));
    }

    void endFrame()
    {
        for (int i = 0; i < 3; ++i)
            input.pressed[i] = input.released[i] = false;
        input.clicks = 0;
        input.wheel = 0;
        input.mouseMoved = false;
        std::fill(std::begin(input.keyPressed), std::end(input.keyPressed), false);
        input.text.clear();
        input.dropped.clear();
    }

    void setModalFrame(void (*frame)()) { g_modalFrame = frame; }

    void setCursor(Cursor cursor)
    {
        if (cursor == g_cursor)
            return;
        g_cursor = cursor;
        linux_::updateShape();
    }

    void minimize()
    {
        if (g_backend)
            g_backend->minimize();
    }

    void toggleMaximize()
    {
        if (g_backend)
            g_backend->toggleMaximize();
    }

    void setDarkFrame(bool) {} // we draw the whole frame ourselves; no border of the desktop's to colour

    void restoreAndRaise()
    {
        if (g_backend)
            g_backend->raise();
    }

    void releaseInput()
    {
        for (int i = 0; i < 3; ++i)
            input.down[i] = false;
        std::fill(std::begin(input.keyDown), std::end(input.keyDown), false);
        window.needsRedraw = true;
    }

    double now()
    {
        static timespec start{};
        timespec t{};
        clock_gettime(CLOCK_MONOTONIC, &t);
        if (start.tv_sec == 0 && start.tv_nsec == 0)
            start = t;
        return double(t.tv_sec - start.tv_sec) + double(t.tv_nsec - start.tv_nsec) * 1e-9;
    }

    void sleepFor(double seconds)
    {
        if (seconds <= 0)
            return;
        timespec t{time_t(seconds), long((seconds - std::floor(seconds)) * 1e9)};
        while (nanosleep(&t, &t) != 0 && errno == EINTR)
        {
        }
    }

    bool openFileDialog(std::filesystem::path &out, const char *title, const std::vector<FileType> &types)
    {
        return fileDialog(DialogKind::Open, out, title, types, {}, nullptr);
    }

    bool saveFileDialog(std::filesystem::path &out, const char *title, const std::filesystem::path &suggested,
                        const std::vector<FileType> &types, const char *defaultExtension)
    {
        return fileDialog(DialogKind::Save, out, title, types, suggested, defaultExtension);
    }

    bool openFolderDialog(std::filesystem::path &out, const char *title, const std::filesystem::path &start)
    {
        return fileDialog(DialogKind::Folder, out, title, {}, start, nullptr);
    }

    void errorBox(const std::string &text)
    {
        std::fprintf(stderr, "VCS Launcher: %s\n", text.c_str());
        if (isatty(2))
            return;
        // started from the desktop: nobody sees stderr
        std::string ignored;
        if (runCapture({"zenity", "--error", "--title=VCS Launcher", "--no-markup", "--text=" + text}, ignored, nullptr) < 0)
            runCapture({"kdialog", "--title", "VCS Launcher", "--error", text}, ignored, nullptr);
    }
}

namespace platform::linux_
{
    // the press handling that turns a click on the chrome into a move / resize / maximise; called by the backends
    // before they report a press, with the press position. True when the chrome took it.
    bool chromePress(int button, float x, float y)
    {
        if (!g_backend)
            return false;
        const Region region = regionAt(x, y);
        if (region == Region::Client)
            return false;
        if (button == kMouseRight && region == Region::Caption)
        {
            g_backend->showWindowMenu(x, y);
            return true;
        }
        if (button != kMouseLeft)
            return true;
        if (region == Region::Caption)
        {
            const double t = now();
            if (t - g_lastCaptionClick <= window.doubleClickSeconds)
            {
                g_lastCaptionClick = -1;
                g_backend->toggleMaximize();
            }
            else
            {
                g_lastCaptionClick = t;
                g_backend->startMove();
            }
            return true;
        }
        g_backend->startResize(region);
        return true;
    }

    // a press or release in the UI: Input's state, the double click
    void clientButton(int button, bool down, float x, float y)
    {
        pointerMotion(x, y);
        if (down && !input.down[button])
        {
            input.pressed[button] = true;
            if (button == kMouseLeft)
            {
                const double t = now();
                const float slop = 4.0f * window.dpiScale;
                const bool second = t - g_lastClickTime <= window.doubleClickSeconds &&
                                    std::fabs(x - g_lastClickX) <= slop && std::fabs(y - g_lastClickY) <= slop;
                input.clicks = second ? 2 : 1;
                g_lastClickTime = second ? -1 : t;
                g_lastClickX = x;
                g_lastClickY = y;
            }
        }
        if (!down && input.down[button])
            input.released[button] = true;
        input.down[button] = down;
        window.needsRedraw = true;
    }
}

int main(int argc, char **argv)
{
    signal(SIGPIPE, SIG_IGN); // a tool that exits while we write to it is not our end
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(argv[i]);
    return launcherMain(args);
}
