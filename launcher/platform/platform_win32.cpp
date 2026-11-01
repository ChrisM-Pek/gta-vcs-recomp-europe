// Win32 implementation of the platform layer.
//
// - Per-monitor DPI aware (v2): sized in logical units times the monitor's scale, following WM_DPICHANGED.
// - No GDI redirection surface (WS_EX_NOREDIRECTIONBITMAP): the flip-model swap chain is all that presents.
// - Custom chrome on a normal overlapped window: WM_NCCALCSIZE gives the whole window to the client, WM_NCHITTEST
//   says where the resize edges, the caption and the maximise button are. Snapping, Aero shake, the min/max
//   animations, the system menu and the DWM shadow all stay Windows' own.
// - Win32 runs a modal loop while the window is moved or resized and while a file dialog is up, which starves the
//   main loop: a timer, WM_SIZE and wakes draw frames through the modal frame meanwhile.
#include "platform.h"

#include <windows.h>
#include <dwmapi.h>
#include <oleidl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <timeapi.h>
#include <windowsx.h>

namespace platform
{
    Input input;
    Window window;

    namespace
    {
        std::wstring wide(const char *utf8)
        {
            if (!utf8 || !*utf8)
                return {};
            const int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
            std::wstring out(size_t(n > 0 ? n - 1 : 0), L'\0');
            if (n > 1)
                MultiByteToWideChar(CP_UTF8, 0, utf8, -1, out.data(), n);
            return out;
        }

        std::string utf8(const wchar_t *text)
        {
            if (!text || !*text)
                return {};
            const int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
            std::string out(size_t(n > 0 ? n - 1 : 0), '\0');
            if (n > 1)
                WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), n, nullptr, nullptr);
            return out;
        }

        constexpr UINT kWakeMessage = WM_APP + 1;
        constexpr UINT_PTR kModalTimer = 1;

        HWND g_hwnd = nullptr;
        int g_minWidth = 0, g_minHeight = 0; // logical
        Chrome g_chrome;
        void (*g_modalFrame)() = nullptr;
        bool g_inSizeMove = false, g_inDialog = false, g_inModalFrame = false;
        bool g_trackingMouse = false, g_trackingNonClient = false;
        bool g_maxButtonDown = false; // pressed through the non-client area (HTMAXBUTTON)
        HCURSOR g_cursors[3] = {};
        Cursor g_cursor = Cursor::Arrow;
        wchar_t g_highSurrogate = 0;
        double g_lastClickTime = -1;
        float g_lastClickX = 0, g_lastClickY = 0;
        LARGE_INTEGER g_frequency{}, g_start{};
        IDropTarget *g_dropTarget = nullptr;

        UINT dpi() { return g_hwnd ? std::max(GetDpiForWindow(g_hwnd), 96u) : 96u; }
        int frameThickness() { return GetSystemMetricsForDpi(SM_CXFRAME, dpi()) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi()); }

        float detectRefreshRate()
        {
            MONITORINFOEXW info{};
            info.cbSize = sizeof(info);
            if (!GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &info))
                return 60;
            DEVMODEW mode{};
            mode.dmSize = sizeof(mode);
            if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode) || mode.dmDisplayFrequency <= 1)
                return 60;
            return float(mode.dmDisplayFrequency);
        }

        void readSystemSettings()
        {
            BOOL animations = TRUE;
            SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &animations, 0);
            window.reducedMotion = !animations;
            // Settings > Personalisation > Colours > "Choose your default app mode"; no value (before Windows 10
            // 1809) is light
            DWORD light = 1, size = sizeof(light);
            RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                         L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
            window.darkMode = light == 0;
            window.doubleClickSeconds = float(GetDoubleClickTime()) / 1000.0f;
        }

        void updateClientSize()
        {
            RECT r{};
            GetClientRect(g_hwnd, &r);
            window.width = r.right - r.left;
            window.height = r.bottom - r.top;
            window.maximized = IsZoomed(g_hwnd) != 0;
        }

        void runModalFrame()
        {
            if (!g_modalFrame || g_inModalFrame || window.minimized)
                return;
            g_inModalFrame = true;
            g_modalFrame();
            g_inModalFrame = false;
        }

        void mouseMove(float x, float y)
        {
            input.mouseMoved = input.mouseMoved || x != input.mouseX || y != input.mouseY;
            input.mouseX = x;
            input.mouseY = y;
            input.mouseInside = true;
            window.needsRedraw = true;
        }

        void mouseButton(int button, bool down, float x, float y)
        {
            mouseMove(x, y);
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

        void mouseLeave()
        {
            input.mouseInside = false;
            input.mouseX = input.mouseY = -1e6f;
            input.mouseMoved = true;
            window.needsRedraw = true;
        }

        POINT clientPoint(LPARAM screen)
        {
            POINT p{GET_X_LPARAM(screen), GET_Y_LPARAM(screen)};
            ScreenToClient(g_hwnd, &p);
            return p;
        }

        LRESULT hitTest(POINT p)
        {
            const float x = float(p.x), y = float(p.y);
            if (!window.maximized)
            {
                const int b = frameThickness();
                const int top = std::max(b / 2, 4); // a thin top edge: the caption sits right under it
                const bool left = p.x < b, right = p.x >= window.width - b;
                const bool upper = p.y < top, lower = p.y >= window.height - b;
                if (upper && left) return HTTOPLEFT;
                if (upper && right) return HTTOPRIGHT;
                if (lower && left) return HTBOTTOMLEFT;
                if (lower && right) return HTBOTTOMRIGHT;
                if (upper) return HTTOP;
                if (lower) return HTBOTTOM;
                if (left) return HTLEFT;
                if (right) return HTRIGHT;
            }
            if (g_chrome.maximizeButton.contains(x, y))
                return HTMAXBUTTON;
            for (const core::Rect &r : g_chrome.buttons)
                if (r.contains(x, y))
                    return HTCLIENT;
            if (g_chrome.caption.contains(x, y))
                return HTCAPTION;
            return HTCLIENT;
        }

        // Files dragged from Explorer: where they hover (the drop zone answers) and what was dropped.
        class DropTarget final : public IDropTarget
        {
        public:
            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
            {
                if (riid == IID_IUnknown || riid == IID_IDropTarget)
                {
                    *out = static_cast<IDropTarget *>(this);
                    AddRef();
                    return S_OK;
                }
                *out = nullptr;
                return E_NOINTERFACE;
            }
            ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
            ULONG STDMETHODCALLTYPE Release() override
            {
                const ULONG n = --m_refs;
                if (!n)
                    delete this;
                return n;
            }
            HRESULT STDMETHODCALLTYPE DragEnter(IDataObject *data, DWORD, POINTL pt, DWORD *effect) override
            {
                FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                m_files = data && data->QueryGetData(&format) == S_OK;
                return DragOver(0, pt, effect);
            }
            HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD *effect) override
            {
                *effect = m_files ? DROPEFFECT_COPY : DROPEFFECT_NONE;
                POINT p{pt.x, pt.y};
                ScreenToClient(g_hwnd, &p);
                input.dragOver = m_files;
                input.dragX = float(p.x);
                input.dragY = float(p.y);
                window.needsRedraw = true;
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE DragLeave() override
            {
                input.dragOver = false;
                window.needsRedraw = true;
                return S_OK;
            }
            HRESULT STDMETHODCALLTYPE Drop(IDataObject *data, DWORD, POINTL, DWORD *effect) override
            {
                input.dragOver = false;
                window.needsRedraw = true;
                *effect = DROPEFFECT_NONE;
                FORMATETC format = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
                STGMEDIUM medium{};
                if (!data || data->GetData(&format, &medium) != S_OK)
                    return S_OK;
                if (HDROP drop = static_cast<HDROP>(GlobalLock(medium.hGlobal)))
                {
                    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                    for (UINT i = 0; i < count; ++i)
                    {
                        std::wstring name(DragQueryFileW(drop, i, nullptr, 0) + 1, L'\0');
                        name.resize(DragQueryFileW(drop, i, name.data(), UINT(name.size())));
                        input.dropped.emplace_back(name);
                    }
                    GlobalUnlock(medium.hGlobal);
                    *effect = DROPEFFECT_COPY;
                }
                ReleaseStgMedium(&medium);
                return S_OK;
            }

        private:
            ULONG m_refs = 1;
            bool m_files = false;
        };

        LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
        {
            switch (message)
            {
            case WM_NCCALCSIZE:
                if (wparam)
                {
                    // the whole window is client area; maximised, Windows puts the frame off screen: keep it there
                    if (IsZoomed(hwnd))
                    {
                        auto *params = reinterpret_cast<NCCALCSIZE_PARAMS *>(lparam);
                        const int f = frameThickness();
                        const int fy = GetSystemMetricsForDpi(SM_CYFRAME, dpi()) + GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi());
                        params->rgrc[0].left += f;
                        params->rgrc[0].right -= f;
                        params->rgrc[0].top += fy;
                        params->rgrc[0].bottom -= fy;
                    }
                    return 0;
                }
                break;
            case WM_NCACTIVATE:
                // no non-client area to paint: keep DefWindowProc from drawing the old caption over ours
                window.focused = wparam != 0;
                window.needsRedraw = true;
                return DefWindowProcW(hwnd, message, wparam, -1);
            case WM_NCHITTEST:
            {
                const LRESULT hit = hitTest(clientPoint(lparam));
                return hit;
            }
            case WM_NCMOUSEMOVE:
                if (wparam == HTMAXBUTTON)
                {
                    const POINT p = clientPoint(lparam);
                    mouseMove(float(p.x), float(p.y));
                    if (!g_trackingNonClient)
                    {
                        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE | TME_NONCLIENT, hwnd, 0};
                        TrackMouseEvent(&tme);
                        g_trackingNonClient = true;
                    }
                    return 0;
                }
                if (input.mouseInside && !g_trackingMouse)
                    mouseLeave(); // over the caption: nothing under the mouse is hovered
                break;
            case WM_NCMOUSELEAVE:
                g_trackingNonClient = false;
                if (g_maxButtonDown)
                {
                    g_maxButtonDown = false;
                    input.down[kMouseLeft] = false; // let go elsewhere: no click
                }
                if (!g_trackingMouse)
                    mouseLeave();
                break;
            case WM_NCLBUTTONDOWN:
            case WM_NCLBUTTONDBLCLK:
                if (wparam == HTMAXBUTTON)
                {
                    const POINT p = clientPoint(lparam);
                    g_maxButtonDown = true;
                    mouseButton(kMouseLeft, true, float(p.x), float(p.y));
                    return 0;
                }
                break;
            case WM_NCLBUTTONUP:
                if (wparam == HTMAXBUTTON)
                {
                    const POINT p = clientPoint(lparam);
                    g_maxButtonDown = false;
                    mouseButton(kMouseLeft, false, float(p.x), float(p.y));
                    return 0;
                }
                break;
            case WM_SYSCOMMAND:
                // Alt pressed alone would enter the (absent) menu bar's keyboard mode; Alt+Space still opens the
                // system menu (lparam = the key)
                if ((wparam & 0xFFF0) == SC_KEYMENU && lparam == 0)
                    return 0;
                break;
            case WM_CLOSE:
                window.quitRequested = true;
                window.needsRedraw = true;
                return 0;
            case WM_ERASEBKGND:
                return 1;
            case kWakeMessage:
                window.needsRedraw = true;
                if (g_inDialog)
                    runModalFrame();
                return 0;
            case WM_PAINT:
                ValidateRect(hwnd, nullptr);
                window.needsRedraw = true;
                if (g_inSizeMove || g_inDialog)
                    runModalFrame();
                return 0;
            case WM_SIZE:
                updateClientSize();
                window.minimized = wparam == SIZE_MINIMIZED;
                window.needsRedraw = true;
                if (g_inSizeMove)
                    runModalFrame();
                return 0;
            case WM_ENTERSIZEMOVE:
                SetTimer(hwnd, kModalTimer, UINT(std::max(1000.0f / window.refreshHz, 1.0f)), nullptr);
                g_inSizeMove = true;
                return 0;
            case WM_EXITSIZEMOVE:
                KillTimer(hwnd, kModalTimer);
                g_inSizeMove = false;
                window.refreshHz = detectRefreshRate(); // maybe on another monitor now
                return 0;
            case WM_TIMER:
                if (wparam == kModalTimer)
                {
                    runModalFrame();
                    return 0;
                }
                break;
            case WM_DPICHANGED:
            {
                window.dpiScale = float(LOWORD(wparam)) / 96.0f;
                const RECT *r = reinterpret_cast<const RECT *>(lparam);
                SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
                window.refreshHz = detectRefreshRate();
                window.needsRedraw = true;
                return 0;
            }
            case WM_GETMINMAXINFO:
            {
                auto *info = reinterpret_cast<MINMAXINFO *>(lparam);
                info->ptMinTrackSize.x = LONG(g_minWidth * int(dpi()) / 96);
                info->ptMinTrackSize.y = LONG(g_minHeight * int(dpi()) / 96);
                return 0;
            }
            case WM_SETTINGCHANGE:
                readSystemSettings();
                window.needsRedraw = true;
                break;
            case WM_ACTIVATE:
                window.focused = LOWORD(wparam) != WA_INACTIVE;
                window.needsRedraw = true;
                break;
            case WM_KILLFOCUS:
                releaseInput();
                break;
            case WM_SETCURSOR:
                if (LOWORD(lparam) == HTCLIENT)
                {
                    SetCursor(g_cursors[int(g_cursor)]);
                    return TRUE;
                }
                break;
            case WM_MOUSEMOVE:
                if (!g_trackingMouse)
                {
                    TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, hwnd, 0};
                    TrackMouseEvent(&tme);
                    g_trackingMouse = true;
                }
                mouseMove(float(GET_X_LPARAM(lparam)), float(GET_Y_LPARAM(lparam)));
                return 0;
            case WM_MOUSELEAVE:
                g_trackingMouse = false;
                if (!g_trackingNonClient)
                    mouseLeave();
                return 0;
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
                SetCapture(hwnd);
                mouseButton(message == WM_LBUTTONDOWN ? kMouseLeft : message == WM_RBUTTONDOWN ? kMouseRight : kMouseMiddle,
                            true, float(GET_X_LPARAM(lparam)), float(GET_Y_LPARAM(lparam)));
                return 0;
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
                mouseButton(message == WM_LBUTTONUP ? kMouseLeft : message == WM_RBUTTONUP ? kMouseRight : kMouseMiddle,
                            false, float(GET_X_LPARAM(lparam)), float(GET_Y_LPARAM(lparam)));
                if (!input.down[0] && !input.down[1] && !input.down[2])
                    ReleaseCapture();
                return 0;
            case WM_MOUSEWHEEL:
                input.wheel += float(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
                window.needsRedraw = true;
                return 0;
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
                if (wparam < 256)
                {
                    input.keyDown[wparam] = true;
                    input.keyPressed[wparam] = true;
                }
                window.needsRedraw = true;
                if (message == WM_SYSKEYDOWN)
                    break; // Alt+F4, Alt+Space
                return 0;
            case WM_KEYUP:
            case WM_SYSKEYUP:
                if (wparam < 256)
                    input.keyDown[wparam] = false;
                window.needsRedraw = true;
                if (message == WM_SYSKEYUP)
                    break;
                return 0;
            case WM_CHAR:
            {
                const wchar_t c = wchar_t(wparam);
                if (c >= 0xD800 && c < 0xDC00)
                    g_highSurrogate = c;
                else if (c >= 0xDC00 && c < 0xE000)
                {
                    if (g_highSurrogate)
                        input.text += char32_t(0x10000 + ((g_highSurrogate - 0xD800) << 10) + (c - 0xDC00));
                    g_highSurrogate = 0;
                }
                else if (c >= 32 && c != 127)
                    input.text += char32_t(c);
                window.needsRedraw = true;
                return 0;
            }
            default:
                break;
            }
            return DefWindowProcW(hwnd, message, wparam, lparam);
        }

        // A file dialog of the shell (IFileDialog): modal, with frames drawn meanwhile.
        bool runDialog(IFileDialog *dialog, std::filesystem::path &out)
        {
            g_inDialog = true;
            SetTimer(g_hwnd, kModalTimer, 100, nullptr); // spinners keep turning, at a modest rate
            const HRESULT hr = dialog->Show(g_hwnd);
            KillTimer(g_hwnd, kModalTimer);
            g_inDialog = false;
            releaseInput();
            window.needsRedraw = true;
            if (FAILED(hr))
                return false;
            IShellItem *item = nullptr;
            if (FAILED(dialog->GetResult(&item)))
                return false;
            PWSTR path = nullptr;
            const bool ok = SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path));
            if (ok)
            {
                out = path;
                CoTaskMemFree(path);
            }
            item->Release();
            return ok;
        }

        void setTypes(IFileDialog *dialog, const std::vector<FileType> &types)
        {
            std::vector<std::wstring> text;
            for (const FileType &t : types)
            {
                text.push_back(wide(t.name));
                text.push_back(wide(t.pattern));
            }
            std::vector<COMDLG_FILTERSPEC> specs;
            for (size_t i = 0; i + 1 < text.size(); i += 2)
                specs.push_back({text[i].c_str(), text[i + 1].c_str()});
            if (!specs.empty())
                dialog->SetFileTypes(UINT(specs.size()), specs.data());
        }

        void setFolder(IFileDialog *dialog, const std::filesystem::path &dir)
        {
            std::error_code ec;
            if (dir.empty() || !std::filesystem::is_directory(dir, ec))
                return;
            IShellItem *item = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(dir.c_str(), nullptr, IID_PPV_ARGS(&item))))
            {
                dialog->SetFolder(item);
                item->Release();
            }
        }
    }

    bool init(const char *title, int logicalWidth, int logicalHeight, int minLogicalWidth, int minLogicalHeight,
              std::string &error)
    {
        now();
        if (!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
            SetProcessDPIAware();
        OleInitialize(nullptr); // drag and drop, and the shell's file dialogs
        timeBeginPeriod(1);
        g_minWidth = minLogicalWidth;
        g_minHeight = minLogicalHeight;

        const HINSTANCE instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = windowProc;
        wc.hInstance = instance;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
        wc.hIconSm = wc.hIcon;
        wc.lpszClassName = L"VCSLauncherWindow";
        if (!RegisterClassExW(&wc))
        {
            error = "RegisterClassExW failed";
            return false;
        }
        g_cursors[int(Cursor::Arrow)] = LoadCursorW(nullptr, IDC_ARROW);
        g_cursors[int(Cursor::Hand)] = LoadCursorW(nullptr, IDC_HAND);
        g_cursors[int(Cursor::Text)] = LoadCursorW(nullptr, IDC_IBEAM);

        // hidden at a placeholder size; sized and centred once the monitor's DPI is known
        g_hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_APPWINDOW, wc.lpszClassName, wide(title).c_str(), WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 800, 600, nullptr, nullptr, instance, nullptr);
        if (!g_hwnd)
        {
            error = "CreateWindowExW failed";
            return false;
        }
        window.dpiScale = float(dpi()) / 96.0f;
        window.refreshHz = detectRefreshRate();
        readSystemSettings();

        // on Windows 11 rounded corners; the frame's mode and border follow the palette (setDarkFrame)
        const int round = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(g_hwnd, 33 /* DWMWA_WINDOW_CORNER_PREFERENCE */, &round, sizeof(round));
        setDarkFrame(window.darkMode);
        // a sliver of frame extended into the client area keeps the DWM shadow on a window without a visible frame
        const MARGINS margins = {0, 0, 1, 0};
        DwmExtendFrameIntoClientArea(g_hwnd, &margins);

        int w = int(float(logicalWidth) * window.dpiScale), h = int(float(logicalHeight) * window.dpiScale);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (GetMonitorInfoW(MonitorFromWindow(g_hwnd, MONITOR_DEFAULTTONEAREST), &mi))
        {
            const RECT &work = mi.rcWork;
            w = std::min(w, int(work.right - work.left));
            h = std::min(h, int(work.bottom - work.top));
            SetWindowPos(g_hwnd, nullptr, work.left + (work.right - work.left - w) / 2,
                         work.top + (work.bottom - work.top - h) / 2, w, h, SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        }
        updateClientSize();

        g_dropTarget = new DropTarget();
        RegisterDragDrop(g_hwnd, g_dropTarget);
        return true;
    }

    void show()
    {
        ShowWindow(g_hwnd, SW_SHOW);
        SetForegroundWindow(g_hwnd);
    }

    void shutdown()
    {
        if (g_hwnd)
        {
            RevokeDragDrop(g_hwnd);
            DestroyWindow(g_hwnd);
        }
        if (g_dropTarget)
            g_dropTarget->Release();
        g_dropTarget = nullptr;
        g_hwnd = nullptr;
        timeEndPeriod(1);
        OleUninitialize();
    }

    void *nativeHandle() { return g_hwnd; }

    void setChrome(const Chrome &chrome) { g_chrome = chrome; }

    void pumpEvents()
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            if (msg.message == WM_QUIT)
            {
                window.quitRequested = true;
                continue;
            }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        input.ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        input.shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        input.alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
    }

    void waitForEvents(double timeoutSeconds)
    {
        DWORD ms = INFINITE;
        if (timeoutSeconds >= 0)
            ms = DWORD(std::min(timeoutSeconds * 1000.0 + 1.0, 3600000.0));
        MsgWaitForMultipleObjectsEx(0, nullptr, ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    }

    void wake()
    {
        if (g_hwnd)
            PostMessageW(g_hwnd, kWakeMessage, 0, 0);
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
        if (input.mouseInside)
            SetCursor(g_cursors[int(cursor)]);
    }

    void minimize() { ShowWindow(g_hwnd, SW_MINIMIZE); }
    void toggleMaximize() { ShowWindow(g_hwnd, IsZoomed(g_hwnd) ? SW_RESTORE : SW_MAXIMIZE); }

    void setDarkFrame(bool dark)
    {
        static int current = -1;
        if (!g_hwnd || current == int(dark))
            return;
        current = int(dark);
        const BOOL value = dark;
        DwmSetWindowAttribute(g_hwnd, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &value, sizeof(value));
        // Windows 11's border, in the window's own colour
        const COLORREF border = dark ? RGB(40, 40, 44) : RGB(242, 208, 224);
        DwmSetWindowAttribute(g_hwnd, 34 /* DWMWA_BORDER_COLOR */, &border, sizeof(border));
    }

    void restoreAndRaise()
    {
        if (IsIconic(g_hwnd))
            ShowWindow(g_hwnd, SW_RESTORE);
        SetForegroundWindow(g_hwnd);
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
        if (!g_frequency.QuadPart) // the clock works without a window too (offscreen screenshots)
        {
            QueryPerformanceFrequency(&g_frequency);
            QueryPerformanceCounter(&g_start);
        }
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return double(t.QuadPart - g_start.QuadPart) / double(g_frequency.QuadPart);
    }

    void sleepFor(double seconds)
    {
        if (seconds > 0)
            Sleep(DWORD(seconds * 1000.0));
    }

    bool openFileDialog(std::filesystem::path &out, const char *title, const std::vector<FileType> &types)
    {
        IFileOpenDialog *dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return false;
        dialog->SetTitle(wide(title).c_str());
        setTypes(dialog, types);
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
        const bool ok = runDialog(dialog, out);
        dialog->Release();
        return ok;
    }

    bool saveFileDialog(std::filesystem::path &out, const char *title, const std::filesystem::path &suggested,
                        const std::vector<FileType> &types, const char *defaultExtension)
    {
        IFileSaveDialog *dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileSaveDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return false;
        dialog->SetTitle(wide(title).c_str());
        setTypes(dialog, types);
        if (defaultExtension)
            dialog->SetDefaultExtension(wide(defaultExtension).c_str());
        setFolder(dialog, suggested.parent_path());
        dialog->SetFileName(suggested.filename().c_str());
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_OVERWRITEPROMPT);
        const bool ok = runDialog(dialog, out);
        dialog->Release();
        return ok;
    }

    bool openFolderDialog(std::filesystem::path &out, const char *title, const std::filesystem::path &start)
    {
        IFileOpenDialog *dialog = nullptr;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return false;
        dialog->SetTitle(wide(title).c_str());
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        setFolder(dialog, start);
        const bool ok = runDialog(dialog, out);
        dialog->Release();
        return ok;
    }

    void errorBox(const std::string &text)
    {
        MessageBoxW(nullptr, wide(text.c_str()).c_str(), L"VCS Launcher", MB_ICONERROR);
    }
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    int argc = 0;
    wchar_t **argvW = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i)
        args.push_back(platform::utf8(argvW[i]));
    LocalFree(argvW);
    return launcherMain(args);
}
