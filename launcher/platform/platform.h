// The platform layer: one window, its input, and the OS services the UI needs (time, waiting, cursors, file dialogs,
// drag and drop). The OS-independent state lives here; platform_win32.cpp implements it for Windows. Another OS is
// another implementation file (docs/LAUNCHER.md, "Other platforms").
//
// Sizes are physical pixels; `window.dpiScale` converts logical units. Keys are Windows virtual-key values on every
// platform (others translate to them).
#pragma once

#include "core/math.h"

#include <filesystem>
#include <string>
#include <vector>

namespace platform
{
    enum Key : int
    {
        kKeyBackspace = 0x08,
        kKeyTab = 0x09,
        kKeyEnter = 0x0D,
        kKeyShift = 0x10,
        kKeyControl = 0x11,
        kKeyAlt = 0x12,
        kKeyEscape = 0x1B,
        kKeySpace = 0x20,
        kKeyPageUp = 0x21,
        kKeyPageDown = 0x22,
        kKeyEnd = 0x23,
        kKeyHome = 0x24,
        kKeyLeft = 0x25,
        kKeyUp = 0x26,
        kKeyRight = 0x27,
        kKeyDown = 0x28,
        kKeyF1 = 0x70,
        kKeyF12 = 0x7B,
    };

    enum MouseButton
    {
        kMouseLeft,
        kMouseRight,
        kMouseMiddle,
    };

    enum class Cursor
    {
        Arrow,
        Hand,
        Text,
    };

    struct Input
    {
        float mouseX = -1e6f, mouseY = -1e6f;
        bool mouseInside = false;
        bool mouseMoved = false;           // since last frame
        bool down[3] = {}, pressed[3] = {}, released[3] = {};
        int clicks = 0;                    // on a left press: 1, or 2 for a double click
        float wheel = 0;                   // notches, positive away from the user
        bool keyDown[256] = {}, keyPressed[256] = {}; // pressed: this frame, auto-repeat included
        std::u32string text;               // typed characters
        bool ctrl = false, shift = false, alt = false;

        // files dragged from Explorer: over the window, and dropped this frame
        bool dragOver = false;
        float dragX = 0, dragY = 0;
        std::vector<std::filesystem::path> dropped;

        bool key(int vk) const { return vk >= 0 && vk < 256 && keyPressed[vk]; }
    };

    struct Window
    {
        int width = 0, height = 0;   // client area, pixels
        float dpiScale = 1;
        float refreshHz = 60;
        bool minimized = false, maximized = false, focused = true;
        bool quitRequested = false;  // the close button, Alt+F4: the UI decides
        bool needsRedraw = true;     // input or an OS event since the last frame
        bool reducedMotion = false;  // Windows' "Show animations in Windows" is off
        bool darkMode = true;        // Windows' app mode is dark
        float doubleClickSeconds = 0.5f;
    };

    extern Input input;
    extern Window window;

    // The title bar the UI draws (client pixels): dragging the caption moves the window (and double-click maximises,
    // right-click opens the system menu); the button rects inside it stay clickable; the maximise button is reported
    // as one to Windows 11 (its snap layouts flyout). Set every frame.
    struct Chrome
    {
        core::Rect caption;
        core::Rect maximizeButton;
        std::vector<core::Rect> buttons;
    };
    void setChrome(const Chrome &chrome);

    // A window of logical size, centred on the monitor it lands on, created hidden: show() once the first frame is
    // drawn, so it never flashes empty.
    bool init(const wchar_t *title, int logicalWidth, int logicalHeight, int minLogicalWidth, int minLogicalHeight,
              std::string &error);
    void show();
    void shutdown();
    void *nativeHandle();

    void pumpEvents();                       // every pending OS message, without blocking
    void waitForEvents(double timeoutSeconds); // sleep until input, a wake() or the timeout (negative: none)
    void wake();                             // from any thread
    void endFrame();                         // clears this frame's presses, wheel, text and drops

    // Frames while the OS runs a modal loop of its own (the window being moved or resized, a file dialog): the main
    // loop is starved then, so the platform calls this to keep drawing. Never inside a frame.
    void setModalFrame(void (*frame)());

    void setCursor(Cursor cursor);
    void minimize();
    void toggleMaximize();
    void restoreAndRaise();  // back from the game
    void setDarkFrame(bool dark); // the window's frame, border and menus to match the palette
    void releaseInput();     // everything held is let go (a dialog took the keyboard and mouse)

    double now();            // seconds, monotonic
    void sleepFor(double seconds);

    // File dialogs. They block until closed (frames keep coming through the modal frame), so call them between
    // frames, never from inside one. False when cancelled.
    struct FileType
    {
        const wchar_t *name;
        const wchar_t *pattern; // "*.iso"
    };
    bool openFileDialog(std::filesystem::path &out, const wchar_t *title, const std::vector<FileType> &types);
    bool saveFileDialog(std::filesystem::path &out, const wchar_t *title, const std::filesystem::path &suggested,
                        const std::vector<FileType> &types, const wchar_t *defaultExtension);
    bool openFolderDialog(std::filesystem::path &out, const wchar_t *title, const std::filesystem::path &start);
}
