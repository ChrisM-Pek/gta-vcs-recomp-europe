// The window-system half of the Linux platform layer: X11 (x11.cpp) or Wayland (wayland.cpp), picked at startup by
// platform_linux.cpp, which keeps everything the two share - the frame loop's waiting, input state, the title bar's
// behaviour, key mapping, file dialogs. Internal to platform/.
#pragma once

#include "platform/platform.h"

#include <cstdint>
#include <memory>
#include <string>

namespace platform::linux_
{
    // what the pointer is over: the UI, the title bar, or one of the resize edges (custom chrome: we draw it, so we
    // say where it is)
    enum class Region
    {
        Client,
        Caption,
        Top,
        Bottom,
        Left,
        Right,
        TopLeft,
        TopRight,
        BottomLeft,
        BottomRight,
    };

    enum class Shape // pointer shapes: the UI's three, and the resize ones
    {
        Arrow,
        Hand,
        Text,
        ResizeN,
        ResizeS,
        ResizeW,
        ResizeE,
        ResizeNW,
        ResizeNE,
        ResizeSW,
        ResizeSE,
    };

    class Backend
    {
    public:
        virtual ~Backend() = default;
        virtual const char *name() const = 0;
        virtual bool init(const char *title, int logicalWidth, int logicalHeight, int minLogicalWidth,
                          int minLogicalHeight, std::string &error) = 0;
        virtual void show() = 0;
        virtual void shutdown() = 0;
        virtual const NativeSurface *surface() = 0;

        virtual int fd() = 0;                  // the connection, for poll()
        virtual void prepareWait() {}          // before poll (Wayland: prepare_read + flush)
        virtual void afterWait(bool readable) { (void)readable; } // after it (Wayland: read_events or cancel)
        virtual void dispatch() = 0;           // handle everything pending, without blocking
        virtual int timerFd() { return -1; }   // key repeat (Wayland), -1: none
        virtual void timerFired() {}

        virtual void setShape(Shape shape) = 0;
        virtual void startMove() = 0;          // the press that started it is the current one
        virtual void startResize(Region edge) = 0;
        virtual void showWindowMenu(float x, float y) { (void)x, (void)y; }
        virtual void minimize() = 0;
        virtual void toggleMaximize() = 0;
        virtual void raise() = 0;
        virtual std::string parentHandle() { return {}; } // for the portal's dialogs: "x11:<hex>" / "wayland:<handle>"
    };

    std::unique_ptr<Backend> makeX11();
    std::unique_ptr<Backend> makeWayland();

    // What the backends report (platform_linux.cpp turns it into Input / Window and the title bar's behaviour).
    // Coordinates are physical pixels in the window.
    void pointerMotion(float x, float y);
    void pointerLeave();
    // a press: chromePress first (true: the title bar or an edge took it - a move, resize, maximise); otherwise, and
    // for every release, clientButton. Buttons: kMouseLeft / kMouseRight / kMouseMiddle
    bool chromePress(int button, float x, float y);
    void clientButton(int button, bool down, float x, float y);
    void pointerWheel(float notches);          // positive away from the user
    void keyEvent(uint32_t keysym, bool down, bool repeat); // an xkb / X keysym, unshifted (letters lower case)
    void textInput(const char *utf8);
    void modifiers(bool ctrl, bool shift, bool alt);
    void focusChanged(bool focused);
    void dropEnter(float x, float y);
    void dropMotion(float x, float y);
    void dropLeave();
    void dropFiles(const std::string &uriList); // text/uri-list
    void resized(int width, int height, bool maximized);
    void scaleChanged(float scale);
    void minimizedChanged(bool minimized);
    void closeRequested();
    void redraw();

    Region regionAt(float x, float y); // where a press at (x, y) lands, from the chrome the UI set
    Shape shapeFor(Region region);     // the pointer shape there (the UI's own over the client area)
    float edgeWidth();                 // resize edges, physical pixels

    // desktop settings both backends start from (the portal); X11 refines them from XSETTINGS
    void readPortalSettings();
    bool portalHasColorScheme(); // the portal said light or dark (then XSETTINGS' theme name doesn't decide)
}
