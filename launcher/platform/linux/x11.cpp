// The X11 half of the Linux platform layer (Xlib): an undecorated window whose title bar we draw, moved and resized
// by the window manager through _NET_WM_MOVERESIZE (so its snapping and tiling stay), text through the input method,
// files dropped through XDND, the scale from Xft.dpi and the desktop's style from XSETTINGS (XFCE, MATE, Cinnamon
// keep their theme, animation and double-click settings there).
#include "backend.h"

#include "launcher_icon.h" // make_icon.py --header: 32, 64 and 128 px

#include <X11/Xatom.h>
#include <X11/XKBlib.h>
#include <X11/Xcursor/Xcursor.h>
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include <algorithm>
#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>
#include <vector>

namespace platform::linux_
{
    namespace
    {
        enum MoveResize : long // _NET_WM_MOVERESIZE directions
        {
            kSizeTopLeft = 0,
            kSizeTop = 1,
            kSizeTopRight = 2,
            kSizeRight = 3,
            kSizeBottomRight = 4,
            kSizeBottom = 5,
            kSizeBottomLeft = 6,
            kSizeLeft = 7,
            kMove = 8,
        };

        class X11 final : public Backend
        {
        public:
            const char *name() const override { return "X11"; }

            bool init(const char *title, int logicalWidth, int logicalHeight, int minLogicalWidth, int minLogicalHeight,
                      std::string &error) override
            {
                std::setlocale(LC_CTYPE, ""); // the input method works in the locale's encoding (UTF-8 everywhere now)
                XSetLocaleModifiers("");
                m_display = XOpenDisplay(nullptr);
                if (!m_display)
                {
                    error = "no X display";
                    return false;
                }
                m_screen = DefaultScreen(m_display);
                m_root = RootWindow(m_display, m_screen);
                Bool detectable = False;
                XkbSetDetectableAutoRepeat(m_display, True, &detectable); // repeats as presses only, as on Windows
                internAtoms();

                const float scale = readScale();
                window.dpiScale = scale;
                readXSettings();

                // centred on the primary monitor
                int monX = 0, monY = 0, monW = DisplayWidth(m_display, m_screen), monH = DisplayHeight(m_display, m_screen);
                primaryMonitor(monX, monY, monW, monH);
                const int w = std::min(int(std::lround(float(logicalWidth) * scale)), monW);
                const int h = std::min(int(std::lround(float(logicalHeight) * scale)), monH);
                const int x = monX + (monW - w) / 2, y = monY + (monH - h) / 2;

                XSetWindowAttributes attrs{};
                attrs.event_mask = ExposureMask | StructureNotifyMask | KeyPressMask | KeyReleaseMask | ButtonPressMask |
                                   ButtonReleaseMask | PointerMotionMask | EnterWindowMask | LeaveWindowMask |
                                   FocusChangeMask | PropertyChangeMask;
                attrs.background_pixmap = None; // nothing painted under our frames (no flash of a colour on resize)
                attrs.bit_gravity = NorthWestGravity;
                m_window = XCreateWindow(m_display, m_root, x, y, unsigned(w), unsigned(h), 0, CopyFromParent, InputOutput,
                                         CopyFromParent, CWEventMask | CWBackPixmap | CWBitGravity, &attrs);
                if (!m_window)
                {
                    error = "XCreateWindow failed";
                    return false;
                }

                // names, class (the .desktop file's StartupWMClass), protocols
                XClassHint cls{const_cast<char *>("VCSLauncher"), const_cast<char *>("VCSLauncher")};
                XSetClassHint(m_display, m_window, &cls);
                XStoreName(m_display, m_window, title);
                XChangeProperty(m_display, m_window, m_atoms.netWmName, m_atoms.utf8String, 8, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(title), int(std::strlen(title)));
                Atom protocols[] = {m_atoms.wmDelete, m_atoms.netWmPing};
                XSetWMProtocols(m_display, m_window, protocols, 2);
                const long pid = long(getpid());
                XChangeProperty(m_display, m_window, m_atoms.netWmPid, XA_CARDINAL, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(&pid), 1);
                Atom netWmIcon = XInternAtom(m_display, "_NET_WM_ICON", False);
                XChangeProperty(m_display, m_window, netWmIcon, XA_CARDINAL, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(g_launcher_icon), int(std::size(g_launcher_icon)));
                Atom type = m_atoms.netWmWindowTypeNormal;
                XChangeProperty(m_display, m_window, m_atoms.netWmWindowType, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(&type), 1);
                // no decorations: the title bar is ours (Motif hints: flags = decorations, decorations = 0)
                const long motif[5] = {2, 0, 0, 0, 0};
                XChangeProperty(m_display, m_window, m_atoms.motifHints, m_atoms.motifHints, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(motif), 5);
                XSizeHints *hints = XAllocSizeHints();
                hints->flags = PMinSize | PPosition | PSize;
                hints->min_width = int(std::lround(float(minLogicalWidth) * scale));
                hints->min_height = int(std::lround(float(minLogicalHeight) * scale));
                hints->x = x;
                hints->y = y;
                hints->width = w;
                hints->height = h;
                XSetWMNormalHints(m_display, m_window, hints);
                XFree(hints);
                XWMHints *wm = XAllocWMHints();
                wm->flags = InputHint | StateHint;
                wm->input = True;
                wm->initial_state = NormalState;
                XSetWMHints(m_display, m_window, wm);
                XFree(wm);
                // files dragged from the file manager (XDND version 5)
                const long version = 5;
                XChangeProperty(m_display, m_window, m_atoms.xdndAware, XA_ATOM, 32, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(&version), 1);

                // text through the input method (dead keys, compose, IMEs)
                m_im = XOpenIM(m_display, nullptr, nullptr, nullptr);
                if (m_im)
                    m_ic = XCreateIC(m_im, XNInputStyle, XIMPreeditNothing | XIMStatusNothing, XNClientWindow, m_window,
                                     XNFocusWindow, m_window, nullptr);

                m_surface = {NativeSurface::X11, m_display, nullptr, m_window};
                window.width = w;
                window.height = h;
                window.refreshHz = refreshRate();
                return true;
            }

            void show() override
            {
                XMapRaised(m_display, m_window);
                XFlush(m_display);
                m_shown = true;
            }

            void shutdown() override
            {
                if (!m_display)
                    return;
                for (::Cursor &c : m_cursors)
                    if (c)
                        XFreeCursor(m_display, c);
                if (m_ic)
                    XDestroyIC(m_ic);
                if (m_im)
                    XCloseIM(m_im);
                if (m_window)
                    XDestroyWindow(m_display, m_window);
                XCloseDisplay(m_display);
                m_display = nullptr;
            }

            const NativeSurface *surface() override { return &m_surface; }
            int fd() override { return ConnectionNumber(m_display); }

            void dispatch() override
            {
                while (XPending(m_display))
                {
                    XEvent e;
                    XNextEvent(m_display, &e);
                    if (XFilterEvent(&e, None)) // the input method's own (compose sequences)
                        continue;
                    handle(e);
                }
                XFlush(m_display);
            }

            void setShape(Shape shape) override
            {
                const int i = int(shape);
                if (i < 0 || i >= kShapes)
                    return;
                if (!m_cursors[i])
                    m_cursors[i] = loadCursor(shape);
                XDefineCursor(m_display, m_window, m_cursors[i]);
                XFlush(m_display);
            }

            void startMove() override { moveResize(kMove); }

            void startResize(Region edge) override
            {
                long direction = kSizeBottomRight;
                switch (edge)
                {
                case Region::Top: direction = kSizeTop; break;
                case Region::Bottom: direction = kSizeBottom; break;
                case Region::Left: direction = kSizeLeft; break;
                case Region::Right: direction = kSizeRight; break;
                case Region::TopLeft: direction = kSizeTopLeft; break;
                case Region::TopRight: direction = kSizeTopRight; break;
                case Region::BottomLeft: direction = kSizeBottomLeft; break;
                case Region::BottomRight: direction = kSizeBottomRight; break;
                default: return;
                }
                moveResize(direction);
            }

            void minimize() override
            {
                XIconifyWindow(m_display, m_window, m_screen);
                XFlush(m_display);
            }

            void toggleMaximize() override
            {
                sendState(2 /* toggle */, m_atoms.netWmStateMaxVert, m_atoms.netWmStateMaxHorz);
            }

            void raise() override
            {
                if (window.minimized)
                    XMapRaised(m_display, m_window);
                XEvent e{};
                e.xclient.type = ClientMessage;
                e.xclient.window = m_window;
                e.xclient.message_type = m_atoms.netActiveWindow;
                e.xclient.format = 32;
                e.xclient.data.l[0] = 1; // from an application
                e.xclient.data.l[1] = long(m_lastTime);
                XSendEvent(m_display, m_root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
                XFlush(m_display);
            }

            std::string parentHandle() override
            {
                char text[32];
                std::snprintf(text, sizeof(text), "x11:%lx", static_cast<unsigned long>(m_window));
                return text;
            }

        private:
            static constexpr int kShapes = 11;

            struct Atoms
            {
                Atom wmDelete, wmProtocols, netWmPing, netWmName, utf8String, netWmPid, netWmWindowType,
                    netWmWindowTypeNormal, motifHints, netWmState, netWmStateMaxVert, netWmStateMaxHorz, netWmStateHidden,
                    netWmMoveResize, netActiveWindow, xdndAware, xdndEnter, xdndPosition, xdndStatus, xdndLeave, xdndDrop,
                    xdndFinished, xdndSelection, xdndActionCopy, uriList, dropProperty, xsettingsSelection,
                    xsettingsSettings;
            };

            void internAtoms()
            {
                char selection[32];
                std::snprintf(selection, sizeof(selection), "_XSETTINGS_S%d", m_screen);
                const char *names[] = {"WM_DELETE_WINDOW", "WM_PROTOCOLS", "_NET_WM_PING", "_NET_WM_NAME", "UTF8_STRING",
                                       "_NET_WM_PID", "_NET_WM_WINDOW_TYPE", "_NET_WM_WINDOW_TYPE_NORMAL",
                                       "_MOTIF_WM_HINTS", "_NET_WM_STATE", "_NET_WM_STATE_MAXIMIZED_VERT",
                                       "_NET_WM_STATE_MAXIMIZED_HORZ", "_NET_WM_STATE_HIDDEN", "_NET_WM_MOVERESIZE",
                                       "_NET_ACTIVE_WINDOW", "XdndAware", "XdndEnter", "XdndPosition", "XdndStatus",
                                       "XdndLeave", "XdndDrop", "XdndFinished", "XdndSelection", "XdndActionCopy",
                                       "text/uri-list", "VCS_DROP", selection, "_XSETTINGS_SETTINGS"};
                static_assert(sizeof(Atoms) / sizeof(Atom) == sizeof(names) / sizeof(names[0]));
                XInternAtoms(m_display, const_cast<char **>(names), int(std::size(names)), False,
                             reinterpret_cast<Atom *>(&m_atoms));
            }

            // Xft.dpi (what the desktop's scale setting writes; GNOME, XFCE, KDE all do), else 96
            float readScale()
            {
                float dpi = 0;
                if (const char *resources = XResourceManagerString(m_display))
                {
                    XrmInitialize();
                    if (XrmDatabase db = XrmGetStringDatabase(resources))
                    {
                        char *type = nullptr;
                        XrmValue value{};
                        if (XrmGetResource(db, "Xft.dpi", "Xft.Dpi", &type, &value) && value.addr)
                            dpi = float(std::atof(value.addr));
                        XrmDestroyDatabase(db);
                    }
                }
                if (const char *env = std::getenv("VCS_LAUNCHER_SCALE"); env && std::atof(env) > 0)
                    return float(std::atof(env));
                return dpi > 0 ? std::clamp(dpi / 96.0f, 0.5f, 4.0f) : 1.0f;
            }

            void primaryMonitor(int &x, int &y, int &w, int &h)
            {
                XRRScreenResources *res = XRRGetScreenResourcesCurrent(m_display, m_root);
                if (!res)
                    return;
                RROutput primary = XRRGetOutputPrimary(m_display, m_root);
                for (int i = 0; i < res->noutput; ++i)
                {
                    const RROutput output = primary ? primary : res->outputs[i];
                    XRROutputInfo *info = XRRGetOutputInfo(m_display, res, output);
                    if (info && info->crtc && info->connection == RR_Connected)
                        if (XRRCrtcInfo *crtc = XRRGetCrtcInfo(m_display, res, info->crtc))
                        {
                            x = crtc->x;
                            y = crtc->y;
                            w = int(crtc->width);
                            h = int(crtc->height);
                            m_mode = crtc->mode;
                            XRRFreeCrtcInfo(crtc);
                            XRRFreeOutputInfo(info);
                            break;
                        }
                    if (info)
                        XRRFreeOutputInfo(info);
                    if (primary)
                        break;
                }
                XRRFreeScreenResources(res);
            }

            float refreshRate()
            {
                float hz = 60;
                XRRScreenResources *res = XRRGetScreenResourcesCurrent(m_display, m_root);
                if (!res)
                    return hz;
                for (int i = 0; i < res->nmode; ++i)
                {
                    const XRRModeInfo &m = res->modes[i];
                    if (m.id == m_mode && m.hTotal && m.vTotal)
                        hz = float(double(m.dotClock) / (double(m.hTotal) * double(m.vTotal)));
                }
                XRRFreeScreenResources(res);
                return hz > 1 ? hz : 60;
            }

            // XSETTINGS: the GTK theme's name (dark or not), animations, double-click time, scale - and a
            // notification when they change
            void readXSettings()
            {
                m_settingsOwner = XGetSelectionOwner(m_display, m_atoms.xsettingsSelection);
                if (!m_settingsOwner)
                    return;
                XSelectInput(m_display, m_settingsOwner, PropertyChangeMask | StructureNotifyMask);
                Atom type = None;
                int format = 0;
                unsigned long count = 0, after = 0;
                unsigned char *data = nullptr;
                if (XGetWindowProperty(m_display, m_settingsOwner, m_atoms.xsettingsSettings, 0, 1 << 20, False,
                                       m_atoms.xsettingsSettings, &type, &format, &count, &after, &data) != Success ||
                    !data)
                    return;
                parseXSettings(data, size_t(count));
                XFree(data);
            }

            void parseXSettings(const unsigned char *p, size_t size)
            {
                if (size < 12)
                    return;
                const bool msb = p[0] == 1;
                auto u16 = [&](size_t at) -> uint32_t { return msb ? (p[at] << 8) | p[at + 1] : p[at] | (p[at + 1] << 8); };
                auto u32 = [&](size_t at) -> uint32_t {
                    return msb ? (uint32_t(p[at]) << 24) | (p[at + 1] << 16) | (p[at + 2] << 8) | p[at + 3]
                               : p[at] | (p[at + 1] << 8) | (p[at + 2] << 16) | (uint32_t(p[at + 3]) << 24);
                };
                auto pad4 = [](size_t n) { return (n + 3) & ~size_t(3); };
                const uint32_t n = u32(8);
                size_t at = 12;
                for (uint32_t i = 0; i < n && at + 8 <= size; ++i)
                {
                    const unsigned type = p[at];
                    const size_t nameLen = u16(at + 2);
                    if (at + 4 + pad4(nameLen) + 4 > size)
                        return;
                    const std::string name(reinterpret_cast<const char *>(p + at + 4), nameLen);
                    at += 4 + pad4(nameLen) + 4; // the name, then the last-change serial
                    if (type == 0) // integer
                    {
                        if (at + 4 > size)
                            return;
                        const int32_t value = int32_t(u32(at));
                        at += 4;
                        if (name == "Gtk/EnableAnimations")
                            window.reducedMotion = value == 0;
                        else if (name == "Net/DoubleClickTime" && value > 0)
                            window.doubleClickSeconds = float(value) / 1000.0f;
                    }
                    else if (type == 1) // string
                    {
                        if (at + 4 > size)
                            return;
                        const size_t len = u32(at);
                        if (at + 4 + pad4(len) > size)
                            return;
                        std::string value(reinterpret_cast<const char *>(p + at + 4), len);
                        at += 4 + pad4(len);
                        if (name == "Net/ThemeName" && !value.empty() && !portalHasColorScheme())
                        {
                            std::transform(value.begin(), value.end(), value.begin(),
                                           [](unsigned char c) { return char(std::tolower(c)); });
                            window.darkMode = value.find("dark") != std::string::npos;
                        }
                    }
                    else if (type == 2) // colour
                        at += 8;
                    else
                        return;
                }
                window.needsRedraw = true;
            }

            ::Cursor loadCursor(Shape shape)
            {
                static const char *names[kShapes][2] = {
                    {"default", "left_ptr"},       {"pointer", "hand2"},       {"text", "xterm"},
                    {"n-resize", "top_side"},      {"s-resize", "bottom_side"}, {"w-resize", "left_side"},
                    {"e-resize", "right_side"},    {"nw-resize", "top_left_corner"}, {"ne-resize", "top_right_corner"},
                    {"sw-resize", "bottom_left_corner"}, {"se-resize", "bottom_right_corner"},
                };
                for (const char *n : names[int(shape)])
                    if (::Cursor c = XcursorLibraryLoadCursor(m_display, n))
                        return c;
                return XCreateFontCursor(m_display, 68 /* XC_left_ptr */);
            }

            void moveResize(long direction)
            {
                // the press's implicit grab goes first, or the window manager can't take the pointer
                XUngrabPointer(m_display, CurrentTime);
                XEvent e{};
                e.xclient.type = ClientMessage;
                e.xclient.window = m_window;
                e.xclient.message_type = m_atoms.netWmMoveResize;
                e.xclient.format = 32;
                e.xclient.data.l[0] = m_pressRootX;
                e.xclient.data.l[1] = m_pressRootY;
                e.xclient.data.l[2] = direction;
                e.xclient.data.l[3] = Button1;
                e.xclient.data.l[4] = 1; // from an application
                XSendEvent(m_display, m_root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
                XFlush(m_display);
                // the button's release goes to the window manager: nothing is held as far as we know
                releaseInput();
            }

            void sendState(long action, Atom a, Atom b)
            {
                XEvent e{};
                e.xclient.type = ClientMessage;
                e.xclient.window = m_window;
                e.xclient.message_type = m_atoms.netWmState;
                e.xclient.format = 32;
                e.xclient.data.l[0] = action;
                e.xclient.data.l[1] = long(a);
                e.xclient.data.l[2] = long(b);
                e.xclient.data.l[3] = 1;
                XSendEvent(m_display, m_root, False, SubstructureRedirectMask | SubstructureNotifyMask, &e);
                XFlush(m_display);
            }

            void readState()
            {
                Atom type = None;
                int format = 0;
                unsigned long count = 0, after = 0;
                unsigned char *data = nullptr;
                bool vert = false, horz = false, hidden = false;
                if (XGetWindowProperty(m_display, m_window, m_atoms.netWmState, 0, 64, False, XA_ATOM, &type, &format,
                                       &count, &after, &data) == Success && data)
                {
                    const Atom *atoms = reinterpret_cast<const Atom *>(data);
                    for (unsigned long i = 0; i < count; ++i)
                    {
                        vert |= atoms[i] == m_atoms.netWmStateMaxVert;
                        horz |= atoms[i] == m_atoms.netWmStateMaxHorz;
                        hidden |= atoms[i] == m_atoms.netWmStateHidden;
                    }
                    XFree(data);
                }
                resized(window.width, window.height, vert && horz);
                if (m_shown)
                    minimizedChanged(hidden);
            }

            void xdndStatus(::Window source, bool accept)
            {
                XEvent e{};
                e.xclient.type = ClientMessage;
                e.xclient.window = source;
                e.xclient.message_type = m_atoms.xdndStatus;
                e.xclient.format = 32;
                e.xclient.data.l[0] = long(m_window);
                e.xclient.data.l[1] = accept ? 1 : 0;
                e.xclient.data.l[4] = accept ? long(m_atoms.xdndActionCopy) : None;
                XSendEvent(m_display, source, False, NoEventMask, &e);
            }

            void xdndFinished(::Window source, bool accepted)
            {
                XEvent e{};
                e.xclient.type = ClientMessage;
                e.xclient.window = source;
                e.xclient.message_type = m_atoms.xdndFinished;
                e.xclient.format = 32;
                e.xclient.data.l[0] = long(m_window);
                e.xclient.data.l[1] = accepted ? 1 : 0;
                e.xclient.data.l[2] = accepted ? long(m_atoms.xdndActionCopy) : None;
                XSendEvent(m_display, source, False, NoEventMask, &e);
            }

            void toWindow(long packed, float &x, float &y)
            {
                // XdndPosition carries root coordinates
                int wx = 0, wy = 0;
                ::Window child;
                XTranslateCoordinates(m_display, m_root, m_window, int(packed >> 16), int(packed & 0xFFFF), &wx, &wy, &child);
                x = float(wx);
                y = float(wy);
            }

            void clientMessage(const XClientMessageEvent &e)
            {
                if (e.message_type == m_atoms.wmProtocols)
                {
                    const Atom protocol = Atom(e.data.l[0]);
                    if (protocol == m_atoms.wmDelete)
                        closeRequested();
                    else if (protocol == m_atoms.netWmPing)
                    {
                        XEvent reply{};
                        reply.xclient = e;
                        reply.xclient.window = m_root;
                        XSendEvent(m_display, m_root, False, SubstructureRedirectMask | SubstructureNotifyMask, &reply);
                    }
                    return;
                }
                if (e.message_type == m_atoms.xdndEnter)
                {
                    m_dragSource = ::Window(e.data.l[0]);
                    m_dragVersion = int(e.data.l[1] >> 24);
                    m_dragAccept = false;
                    if (e.data.l[1] & 1) // more than 3 types: the list is on the source
                    {
                        Atom type = None;
                        int format = 0;
                        unsigned long count = 0, after = 0;
                        unsigned char *data = nullptr;
                        Atom typeList = XInternAtom(m_display, "XdndTypeList", False);
                        if (XGetWindowProperty(m_display, m_dragSource, typeList, 0, 1024, False, XA_ATOM, &type, &format,
                                               &count, &after, &data) == Success && data)
                        {
                            const Atom *atoms = reinterpret_cast<const Atom *>(data);
                            for (unsigned long i = 0; i < count; ++i)
                                m_dragAccept |= atoms[i] == m_atoms.uriList;
                            XFree(data);
                        }
                    }
                    else
                        for (int i = 2; i < 5; ++i)
                            m_dragAccept |= Atom(e.data.l[i]) == m_atoms.uriList;
                    if (m_dragAccept)
                        dropEnter(-1e6f, -1e6f);
                    return;
                }
                if (e.message_type == m_atoms.xdndPosition)
                {
                    float x, y;
                    toWindow(e.data.l[2], x, y);
                    if (m_dragAccept)
                        dropMotion(x, y);
                    if (m_dragVersion >= 1)
                        m_dropTime = Time(e.data.l[3]);
                    xdndStatus(::Window(e.data.l[0]), m_dragAccept);
                    return;
                }
                if (e.message_type == m_atoms.xdndLeave)
                {
                    dropLeave();
                    m_dragSource = 0;
                    return;
                }
                if (e.message_type == m_atoms.xdndDrop)
                {
                    if (!m_dragAccept)
                    {
                        xdndFinished(::Window(e.data.l[0]), false);
                        dropLeave();
                        return;
                    }
                    // the URIs come as a selection, answered by SelectionNotify
                    XConvertSelection(m_display, m_atoms.xdndSelection, m_atoms.uriList, m_atoms.dropProperty, m_window,
                                      m_dragVersion >= 1 ? Time(e.data.l[2]) : CurrentTime);
                }
            }

            void selectionNotify(const XSelectionEvent &e)
            {
                if (e.property != m_atoms.dropProperty)
                    return;
                Atom type = None;
                int format = 0;
                unsigned long count = 0, after = 0;
                unsigned char *data = nullptr;
                std::string list;
                if (XGetWindowProperty(m_display, m_window, m_atoms.dropProperty, 0, 1 << 20, True, AnyPropertyType, &type,
                                       &format, &count, &after, &data) == Success && data)
                {
                    list.assign(reinterpret_cast<const char *>(data), count);
                    XFree(data);
                }
                dropFiles(list);
                if (m_dragSource)
                    xdndFinished(m_dragSource, !list.empty());
                m_dragSource = 0;
            }

            void key(XKeyEvent &e, bool down)
            {
                m_lastTime = e.time;
                modifiers((e.state & ControlMask) != 0, (e.state & ShiftMask) != 0, (e.state & Mod1Mask) != 0);
                const KeySym sym = XLookupKeysym(&e, 0); // unshifted: Ctrl+Shift+L is still L
                keyEvent(uint32_t(sym), down, false);
                if (!down)
                    return;
                char text[64];
                KeySym ignored = 0;
                int n = 0;
                if (m_ic)
                {
                    Status status = 0;
                    n = Xutf8LookupString(m_ic, &e, text, int(sizeof(text)) - 1, &ignored, &status);
                    if (status != XLookupChars && status != XLookupBoth)
                        n = 0;
                }
                else
                    n = XLookupString(&e, text, int(sizeof(text)) - 1, &ignored, nullptr);
                // a Ctrl combination types nothing
                if (n > 0 && !(e.state & ControlMask))
                {
                    text[n] = 0;
                    textInput(text);
                }
            }

            void handle(XEvent &e)
            {
                switch (e.type)
                {
                case Expose:
                    redraw();
                    break;
                case ConfigureNotify:
                    if (e.xconfigure.window == m_window &&
                        (e.xconfigure.width != window.width || e.xconfigure.height != window.height))
                        resized(e.xconfigure.width, e.xconfigure.height, window.maximized);
                    break;
                case MapNotify:
                    if (e.xmap.window == m_window)
                        minimizedChanged(false);
                    break;
                case UnmapNotify:
                    if (e.xunmap.window == m_window && m_shown)
                        minimizedChanged(true);
                    break;
                case PropertyNotify:
                    if (e.xproperty.window == m_window && e.xproperty.atom == m_atoms.netWmState)
                        readState();
                    else if (e.xproperty.window == m_settingsOwner && e.xproperty.atom == m_atoms.xsettingsSettings)
                        readXSettings();
                    break;
                case DestroyNotify:
                    if (e.xdestroywindow.window == m_settingsOwner)
                        m_settingsOwner = 0; // the settings daemon went away
                    break;
                case FocusIn:
                    if (m_ic)
                        XSetICFocus(m_ic);
                    focusChanged(true);
                    break;
                case FocusOut:
                    if (m_ic)
                        XUnsetICFocus(m_ic);
                    focusChanged(false);
                    break;
                case EnterNotify:
                case MotionNotify:
                {
                    const int x = e.type == MotionNotify ? e.xmotion.x : e.xcrossing.x;
                    const int y = e.type == MotionNotify ? e.xmotion.y : e.xcrossing.y;
                    pointerMotion(float(x), float(y));
                    break;
                }
                case LeaveNotify:
                    if (e.xcrossing.mode == NotifyNormal)
                        pointerLeave();
                    break;
                case ButtonPress:
                case ButtonRelease:
                {
                    const XButtonEvent &b = e.xbutton;
                    m_lastTime = b.time;
                    modifiers((b.state & ControlMask) != 0, (b.state & ShiftMask) != 0, (b.state & Mod1Mask) != 0);
                    if (b.button == Button4 || b.button == Button5)
                    {
                        if (e.type == ButtonPress)
                            pointerWheel(b.button == Button4 ? 1.0f : -1.0f);
                        break;
                    }
                    const int button = b.button == Button1 ? kMouseLeft : b.button == Button3 ? kMouseRight
                                     : b.button == Button2                    ? kMouseMiddle
                                                                              : -1;
                    if (button < 0)
                        break;
                    if (e.type == ButtonPress)
                    {
                        m_pressRootX = b.x_root;
                        m_pressRootY = b.y_root;
                        if (chromePress(button, float(b.x), float(b.y)))
                            break;
                    }
                    clientButton(button, e.type == ButtonPress, float(b.x), float(b.y));
                    break;
                }
                case KeyPress:
                    key(e.xkey, true);
                    break;
                case KeyRelease:
                    key(e.xkey, false);
                    break;
                case ClientMessage:
                    clientMessage(e.xclient);
                    break;
                case SelectionNotify:
                    selectionNotify(e.xselection);
                    break;
                default:
                    break;
                }
            }

            Display *m_display = nullptr;
            int m_screen = 0;
            ::Window m_root = 0, m_window = 0, m_settingsOwner = 0;
            Atoms m_atoms{};
            XIM m_im = nullptr;
            XIC m_ic = nullptr;
            NativeSurface m_surface{};
            ::Cursor m_cursors[kShapes] = {};
            RRMode m_mode = 0;
            bool m_shown = false;
            int m_pressRootX = 0, m_pressRootY = 0;
            Time m_lastTime = CurrentTime, m_dropTime = CurrentTime;
            ::Window m_dragSource = 0;
            int m_dragVersion = 0;
            bool m_dragAccept = false;
        };
    }

    std::unique_ptr<Backend> makeX11() { return std::make_unique<X11>(); }
}
