// The Wayland half of the Linux platform layer (libwayland-client; the protocol code is generated at build time by
// wayland-scanner from the system's wayland-protocols).
//
// - xdg-shell toplevel with client-side decorations: we draw the title bar anyway (xdg-decoration asks for that where
//   the compositor offers server-side ones; GNOME has none). Move, resize and the window menu are the compositor's
//   (xdg_toplevel.move / resize / show_window_menu).
// - Exact scale: fractional-scale-v1 + viewporter (the buffer has physical pixels, shown at the logical size); else
//   the integer scale of the output, as a buffer scale.
// - Pointer shapes from cursor-shape-v1 (the compositor's theme), else libwayland-cursor.
// - Keyboard through xkbcommon, with compose (dead keys) and key repeat on a timerfd the frame loop polls.
// - Files dropped from the file manager (wl_data_device, text/uri-list); raising after the game through
//   xdg-activation (the compositor may only flag the window instead: focus stealing prevention).
// - The window's icon from xdg-toplevel-icon where the compositor has it (KDE); elsewhere (GNOME) the icon comes from
//   the desktop entry matching the app id - the launcher's shortcut writes one.
#include "backend.h"

#include "cursor-shape-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "xdg-activation-v1-client-protocol.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"
#ifdef VCS_HAVE_TOPLEVEL_ICON
#include "launcher_icon.h" // make_icon.py --header: 32, 64, 128 px
#include "xdg-toplevel-icon-v1-client-protocol.h"
#endif

#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon-compose.h>
#include <xkbcommon/xkbcommon.h>

#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <clocale>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <poll.h>
#include <string>
#include <sys/mman.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <vector>

namespace platform::linux_
{
    namespace
    {
        constexpr uint32_t kBtnLeft = 0x110, kBtnRight = 0x111, kBtnMiddle = 0x112; // linux/input-event-codes.h
        constexpr const char *kAppId = "gta-vcs-launcher";                          // the .desktop file's name

        class Wayland final : public Backend
        {
        public:
            const char *name() const override { return "Wayland"; }

            bool init(const char *title, int logicalWidth, int logicalHeight, int minLogicalWidth, int minLogicalHeight,
                      std::string &error) override
            {
                m_display = wl_display_connect(nullptr);
                if (!m_display)
                {
                    error = "no Wayland display";
                    return false;
                }
                m_registry = wl_display_get_registry(m_display);
                wl_registry_add_listener(m_registry, &kRegistry, this);
                wl_display_roundtrip(m_display);
                if (!m_compositor || !m_wmBase || !m_seat)
                {
                    error = "the Wayland compositor lacks xdg-shell";
                    return false;
                }
                wl_display_roundtrip(m_display); // the seat's capabilities, the outputs' scales

                m_xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
                const char *locale = std::setlocale(LC_CTYPE, nullptr);
                for (const char *env : {"LC_ALL", "LC_CTYPE", "LANG"})
                    if (const char *v = std::getenv(env); v && *v)
                    {
                        locale = v;
                        break;
                    }
                if (m_xkb)
                    if (xkb_compose_table *table = xkb_compose_table_new_from_locale(m_xkb, locale ? locale : "C",
                                                                                      XKB_COMPOSE_COMPILE_NO_FLAGS))
                    {
                        m_compose = xkb_compose_state_new(table, XKB_COMPOSE_STATE_NO_FLAGS);
                        xkb_compose_table_unref(table);
                    }
                m_repeatTimer = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);

                m_surface = wl_compositor_create_surface(m_compositor);
                wl_surface_add_listener(m_surface, &kSurface, this);
                if (m_fractionalManager && m_viewporter)
                {
                    m_fractional = wp_fractional_scale_manager_v1_get_fractional_scale(m_fractionalManager, m_surface);
                    wp_fractional_scale_v1_add_listener(m_fractional, &kFractional, this);
                    m_viewport = wp_viewporter_get_viewport(m_viewporter, m_surface);
                }
                m_xdgSurface = xdg_wm_base_get_xdg_surface(m_wmBase, m_surface);
                xdg_surface_add_listener(m_xdgSurface, &kXdgSurface, this);
                m_toplevel = xdg_surface_get_toplevel(m_xdgSurface);
                xdg_toplevel_add_listener(m_toplevel, &kToplevel, this);
                xdg_toplevel_set_title(m_toplevel, title);
                xdg_toplevel_set_app_id(m_toplevel, kAppId);
                xdg_toplevel_set_min_size(m_toplevel, minLogicalWidth, minLogicalHeight);
                setIcon();
                if (m_decorationManager)
                {
                    m_decoration = zxdg_decoration_manager_v1_get_toplevel_decoration(m_decorationManager, m_toplevel);
                    zxdg_toplevel_decoration_v1_set_mode(m_decoration, ZXDG_TOPLEVEL_DECORATION_V1_MODE_CLIENT_SIDE);
                }
                m_logicalW = m_floatW = logicalWidth;
                m_logicalH = m_floatH = logicalHeight;
                wl_surface_commit(m_surface);
                // the first configure comes before anything may be drawn
                for (int i = 0; i < 100 && !m_configured; ++i)
                    if (wl_display_roundtrip(m_display) < 0)
                        break;
                if (!m_configured)
                {
                    error = "the Wayland compositor never configured the window";
                    return false;
                }
                m_native = {NativeSurface::Wayland, m_display, m_surface, 0};
                applySize();
                return true;
            }

            void show() override {} // a Wayland window appears with its first frame, which is already drawn

            void shutdown() override
            {
                if (!m_display)
                    return;
                if (m_repeatTimer >= 0)
                    close(m_repeatTimer);
                if (m_offer)
                    wl_data_offer_destroy(m_offer);
                if (m_dataDevice)
                    wl_data_device_destroy(m_dataDevice);
                if (m_shapeDevice)
                    wp_cursor_shape_device_v1_destroy(m_shapeDevice);
                if (m_cursorSurface)
                    wl_surface_destroy(m_cursorSurface);
                if (m_cursorTheme)
                    wl_cursor_theme_destroy(m_cursorTheme);
                if (m_decoration)
                    zxdg_toplevel_decoration_v1_destroy(m_decoration);
#ifdef VCS_HAVE_TOPLEVEL_ICON
                for (wl_buffer *b : m_iconBuffers)
                    wl_buffer_destroy(b);
                if (m_iconManager)
                    xdg_toplevel_icon_manager_v1_destroy(m_iconManager);
#endif
                if (m_toplevel)
                    xdg_toplevel_destroy(m_toplevel);
                if (m_xdgSurface)
                    xdg_surface_destroy(m_xdgSurface);
                if (m_viewport)
                    wp_viewport_destroy(m_viewport);
                if (m_fractional)
                    wp_fractional_scale_v1_destroy(m_fractional);
                if (m_surface)
                    wl_surface_destroy(m_surface);
                if (m_compose)
                    xkb_compose_state_unref(m_compose);
                if (m_xkbState)
                    xkb_state_unref(m_xkbState);
                if (m_keymap)
                    xkb_keymap_unref(m_keymap);
                if (m_xkb)
                    xkb_context_unref(m_xkb);
                wl_display_disconnect(m_display);
                m_display = nullptr;
            }

            const NativeSurface *surface() override { return &m_native; }
            int fd() override { return wl_display_get_fd(m_display); }

            void prepareWait() override
            {
                while (wl_display_prepare_read(m_display) != 0)
                    wl_display_dispatch_pending(m_display);
                wl_display_flush(m_display);
                m_reading = true;
            }

            void afterWait(bool readable) override
            {
                if (!m_reading)
                    return;
                m_reading = false;
                if (readable)
                    wl_display_read_events(m_display);
                else
                    wl_display_cancel_read(m_display);
            }

            void dispatch() override
            {
                // whatever has arrived, without blocking
                prepareWait();
                pollfd p{fd(), POLLIN, 0};
                afterWait(poll(&p, 1, 0) > 0 && (p.revents & POLLIN));
                wl_display_dispatch_pending(m_display);
                wl_display_flush(m_display);
            }

            int timerFd() override { return m_repeatTimer; }

            void timerFired() override
            {
                if (m_repeatKey && m_xkbState)
                    pressKey(m_repeatKey, true);
            }

            void setShape(Shape shape) override
            {
                m_shape = shape;
                applyShape();
            }

            void startMove() override
            {
                xdg_toplevel_move(m_toplevel, m_seat, m_buttonSerial);
                releaseInput();
            }

            void startResize(Region edge) override
            {
                uint32_t e = XDG_TOPLEVEL_RESIZE_EDGE_NONE;
                switch (edge)
                {
                case Region::Top: e = XDG_TOPLEVEL_RESIZE_EDGE_TOP; break;
                case Region::Bottom: e = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM; break;
                case Region::Left: e = XDG_TOPLEVEL_RESIZE_EDGE_LEFT; break;
                case Region::Right: e = XDG_TOPLEVEL_RESIZE_EDGE_RIGHT; break;
                case Region::TopLeft: e = XDG_TOPLEVEL_RESIZE_EDGE_TOP_LEFT; break;
                case Region::TopRight: e = XDG_TOPLEVEL_RESIZE_EDGE_TOP_RIGHT; break;
                case Region::BottomLeft: e = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_LEFT; break;
                case Region::BottomRight: e = XDG_TOPLEVEL_RESIZE_EDGE_BOTTOM_RIGHT; break;
                default: return;
                }
                xdg_toplevel_resize(m_toplevel, m_seat, m_buttonSerial, e);
                releaseInput();
            }

            void showWindowMenu(float x, float y) override
            {
                xdg_toplevel_show_window_menu(m_toplevel, m_seat, m_buttonSerial, int32_t(x / m_scale), int32_t(y / m_scale));
            }

            void minimize() override { xdg_toplevel_set_minimized(m_toplevel); }

            void toggleMaximize() override
            {
                if (m_maximized)
                    xdg_toplevel_unset_maximized(m_toplevel);
                else
                    xdg_toplevel_set_maximized(m_toplevel);
            }

            void raise() override
            {
                if (!m_activation)
                    return;
                xdg_activation_token_v1 *token = xdg_activation_v1_get_activation_token(m_activation);
                xdg_activation_token_v1_add_listener(token, &kToken, this);
                if (m_lastSerial)
                    xdg_activation_token_v1_set_serial(token, m_lastSerial, m_seat);
                xdg_activation_token_v1_set_surface(token, m_surface);
                xdg_activation_token_v1_set_app_id(token, kAppId);
                xdg_activation_token_v1_commit(token);
            }

        private:
            // ---- the window's icon ---------------------------------------------------------------------------------

            void setIcon()
            {
#ifdef VCS_HAVE_TOPLEVEL_ICON
                if (!m_iconManager || !m_shm)
                    return;
                xdg_toplevel_icon_v1 *icon = xdg_toplevel_icon_manager_v1_create_icon(m_iconManager);
                // g_launcher_icon: width, height, then ARGB pixels (straight alpha) per size; wl_shm wants premultiplied
                for (size_t at = 0; at + 2 < std::size(g_launcher_icon);)
                {
                    const int w = int(g_launcher_icon[at]), h = int(g_launcher_icon[at + 1]);
                    const size_t bytes = size_t(w) * size_t(h) * 4;
                    const int fd = memfd_create("vcs-icon", MFD_CLOEXEC);
                    if (fd < 0 || ftruncate(fd, off_t(bytes)) != 0)
                    {
                        if (fd >= 0)
                            close(fd);
                        break;
                    }
                    void *map = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
                    if (map != MAP_FAILED)
                    {
                        auto *px = static_cast<uint32_t *>(map);
                        for (size_t i = 0; i < size_t(w) * size_t(h); ++i)
                        {
                            const uint32_t c = uint32_t(g_launcher_icon[at + 2 + i]), a = c >> 24;
                            auto mul = [a](uint32_t v) { return (v * a + 127) / 255; };
                            px[i] = a << 24 | mul(c >> 16 & 255) << 16 | mul(c >> 8 & 255) << 8 | mul(c & 255);
                        }
                        munmap(map, bytes);
                        wl_shm_pool *pool = wl_shm_create_pool(m_shm, fd, int32_t(bytes));
                        wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
                        wl_shm_pool_destroy(pool);
                        xdg_toplevel_icon_v1_add_buffer(icon, buffer, 1);
                        m_iconBuffers.push_back(buffer); // kept until the end: the compositor may read them later
                    }
                    close(fd);
                    at += 2 + size_t(w) * size_t(h);
                }
                xdg_toplevel_icon_manager_v1_set_icon(m_iconManager, m_toplevel, icon);
                xdg_toplevel_icon_v1_destroy(icon);
#endif
            }

            // ---- size and scale ------------------------------------------------------------------------------------

            void applySize()
            {
                const int w = std::max(1, int(std::lround(float(m_logicalW) * m_scale)));
                const int h = std::max(1, int(std::lround(float(m_logicalH) * m_scale)));
                if (m_viewport)
                    wp_viewport_set_destination(m_viewport, m_logicalW, m_logicalH);
                else
                    wl_surface_set_buffer_scale(m_surface, int(m_scale));
                scaleChanged(m_scale);
                resized(w, h, m_maximized);
            }

            void setScale(float scale)
            {
                if (scale <= 0 || scale == m_scale)
                    return;
                m_scale = scale;
                if (m_configured)
                    applySize();
                if (m_cursorTheme) // the fallback theme at the new size
                {
                    wl_cursor_theme_destroy(m_cursorTheme);
                    m_cursorTheme = nullptr;
                    applyShape();
                }
            }

            int outputScale() const
            {
                int scale = 1;
                for (const Output &o : m_outputs)
                    if (o.entered)
                        scale = std::max(scale, o.scale);
                return scale;
            }

            // ---- pointer shapes ------------------------------------------------------------------------------------

            void applyShape()
            {
                if (!m_pointer || !m_pointerSerial)
                    return;
                if (m_shapeDevice)
                {
                    static const uint32_t shapes[] = {
                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT,   WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_POINTER,
                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_TEXT,      WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_N_RESIZE,
                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_S_RESIZE,  WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_W_RESIZE,
                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_E_RESIZE,  WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NW_RESIZE,
                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_NE_RESIZE, WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SW_RESIZE,
                        WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_SE_RESIZE,
                    };
                    const int i = std::clamp(int(m_shape), 0, int(std::size(shapes)) - 1);
                    wp_cursor_shape_device_v1_set_shape(m_shapeDevice, m_pointerSerial, shapes[i]);
                    return;
                }
                // no cursor-shape protocol: the theme's images, drawn on a surface of our own
                static const char *names[][2] = {
                    {"default", "left_ptr"},       {"pointer", "hand2"},       {"text", "xterm"},
                    {"n-resize", "top_side"},      {"s-resize", "bottom_side"}, {"w-resize", "left_side"},
                    {"e-resize", "right_side"},    {"nw-resize", "top_left_corner"}, {"ne-resize", "top_right_corner"},
                    {"sw-resize", "bottom_left_corner"}, {"se-resize", "bottom_right_corner"},
                };
                if (!m_shm)
                    return;
                const int scale = std::max(1, int(std::ceil(m_scale)));
                if (!m_cursorTheme)
                {
                    int size = 24;
                    if (const char *s = std::getenv("XCURSOR_SIZE"); s && std::atoi(s) > 0)
                        size = std::atoi(s);
                    m_cursorTheme = wl_cursor_theme_load(std::getenv("XCURSOR_THEME"), size * scale, m_shm);
                    if (!m_cursorTheme)
                        return;
                }
                wl_cursor *cursor = nullptr;
                for (const char *n : names[std::clamp(int(m_shape), 0, 10)])
                    if ((cursor = wl_cursor_theme_get_cursor(m_cursorTheme, n)))
                        break;
                if (!cursor || !cursor->image_count)
                    return;
                wl_cursor_image *image = cursor->images[0];
                if (!m_cursorSurface)
                    m_cursorSurface = wl_compositor_create_surface(m_compositor);
                wl_surface_set_buffer_scale(m_cursorSurface, scale);
                wl_surface_attach(m_cursorSurface, wl_cursor_image_get_buffer(image), 0, 0);
                wl_surface_damage(m_cursorSurface, 0, 0, int32_t(image->width), int32_t(image->height));
                wl_surface_commit(m_cursorSurface);
                wl_pointer_set_cursor(m_pointer, m_pointerSerial, m_cursorSurface, int32_t(image->hotspot_x) / scale,
                                      int32_t(image->hotspot_y) / scale);
            }

            // ---- keyboard ------------------------------------------------------------------------------------------

            void pressKey(uint32_t keycode, bool repeat)
            {
                const xkb_layout_index_t layout = xkb_state_key_get_layout(m_xkbState, keycode);
                const xkb_keysym_t *syms = nullptr;
                // level 0: the unshifted symbol (Ctrl+Shift+L is still L)
                const int n = xkb_keymap_key_get_syms_by_level(m_keymap, keycode, layout, 0, &syms);
                keyEvent(n > 0 ? uint32_t(syms[0]) : 0, true, repeat);
                if (xkb_state_mod_name_is_active(m_xkbState, XKB_MOD_NAME_CTRL, XKB_STATE_MODS_EFFECTIVE) > 0)
                    return; // a Ctrl combination types nothing
                const xkb_keysym_t sym = xkb_state_key_get_one_sym(m_xkbState, keycode);
                char text[64] = {};
                if (m_compose && !repeat)
                {
                    xkb_compose_state_feed(m_compose, sym);
                    switch (xkb_compose_state_get_status(m_compose))
                    {
                    case XKB_COMPOSE_COMPOSING: return; // a dead key, waiting for the next
                    case XKB_COMPOSE_COMPOSED:
                        xkb_compose_state_get_utf8(m_compose, text, sizeof(text));
                        xkb_compose_state_reset(m_compose);
                        textInput(text);
                        return;
                    case XKB_COMPOSE_CANCELLED: xkb_compose_state_reset(m_compose); return;
                    case XKB_COMPOSE_NOTHING: break;
                    }
                }
                if (xkb_state_key_get_utf8(m_xkbState, keycode, text, sizeof(text)) > 0)
                    textInput(text);
            }

            void armRepeat(uint32_t keycode)
            {
                if (m_repeatRate <= 0 || !xkb_keymap_key_repeats(m_keymap, keycode))
                    return;
                m_repeatKey = keycode;
                itimerspec t{};
                t.it_value.tv_sec = m_repeatDelay / 1000;
                t.it_value.tv_nsec = long(m_repeatDelay % 1000) * 1000000L;
                const long interval = 1000000000L / m_repeatRate;
                t.it_interval.tv_sec = interval / 1000000000L;
                t.it_interval.tv_nsec = interval % 1000000000L;
                timerfd_settime(m_repeatTimer, 0, &t, nullptr);
            }

            void stopRepeat()
            {
                m_repeatKey = 0;
                itimerspec t{};
                timerfd_settime(m_repeatTimer, 0, &t, nullptr);
            }

            void updateModifiers()
            {
                auto active = [&](const char *name) {
                    return xkb_state_mod_name_is_active(m_xkbState, name, XKB_STATE_MODS_EFFECTIVE) > 0;
                };
                modifiers(active(XKB_MOD_NAME_CTRL), active(XKB_MOD_NAME_SHIFT), active(XKB_MOD_NAME_ALT));
            }

            // ---- listeners -----------------------------------------------------------------------------------------

            static Wayland *self(void *data) { return static_cast<Wayland *>(data); }

            struct Output
            {
                wl_output *output;
                int scale;
                bool entered;
            };

            static void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
            {
                Wayland *w = self(data);
                auto bind = [&](const wl_interface *iface, uint32_t want) {
                    return wl_registry_bind(registry, name, iface, std::min(version, want));
                };
                if (std::strcmp(interface, wl_compositor_interface.name) == 0)
                    w->m_compositor = static_cast<wl_compositor *>(bind(&wl_compositor_interface, 4));
                else if (std::strcmp(interface, xdg_wm_base_interface.name) == 0)
                {
                    w->m_wmBase = static_cast<xdg_wm_base *>(bind(&xdg_wm_base_interface, 6));
                    xdg_wm_base_add_listener(w->m_wmBase, &kWmBase, w);
                }
                else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !w->m_seat)
                {
                    w->m_seat = static_cast<wl_seat *>(bind(&wl_seat_interface, 5));
                    wl_seat_add_listener(w->m_seat, &kSeat, w);
                }
                else if (std::strcmp(interface, wl_shm_interface.name) == 0)
                    w->m_shm = static_cast<wl_shm *>(bind(&wl_shm_interface, 1));
                else if (std::strcmp(interface, wl_output_interface.name) == 0)
                {
                    auto *output = static_cast<wl_output *>(bind(&wl_output_interface, 2));
                    w->m_outputs.push_back({output, 1, false});
                    wl_output_add_listener(output, &kOutput, w);
                }
                else if (std::strcmp(interface, wl_data_device_manager_interface.name) == 0)
                    w->m_dataManager = static_cast<wl_data_device_manager *>(bind(&wl_data_device_manager_interface, 3));
                else if (std::strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0)
                    w->m_decorationManager =
                        static_cast<zxdg_decoration_manager_v1 *>(bind(&zxdg_decoration_manager_v1_interface, 1));
                else if (std::strcmp(interface, wp_viewporter_interface.name) == 0)
                    w->m_viewporter = static_cast<wp_viewporter *>(bind(&wp_viewporter_interface, 1));
                else if (std::strcmp(interface, wp_fractional_scale_manager_v1_interface.name) == 0)
                    w->m_fractionalManager =
                        static_cast<wp_fractional_scale_manager_v1 *>(bind(&wp_fractional_scale_manager_v1_interface, 1));
                else if (std::strcmp(interface, wp_cursor_shape_manager_v1_interface.name) == 0)
                    w->m_shapeManager = static_cast<wp_cursor_shape_manager_v1 *>(bind(&wp_cursor_shape_manager_v1_interface, 1));
                else if (std::strcmp(interface, xdg_activation_v1_interface.name) == 0)
                    w->m_activation = static_cast<xdg_activation_v1 *>(bind(&xdg_activation_v1_interface, 1));
#ifdef VCS_HAVE_TOPLEVEL_ICON
                else if (std::strcmp(interface, xdg_toplevel_icon_manager_v1_interface.name) == 0)
                {
                    w->m_iconManager = static_cast<xdg_toplevel_icon_manager_v1 *>(
                        bind(&xdg_toplevel_icon_manager_v1_interface, 1));
                    xdg_toplevel_icon_manager_v1_add_listener(w->m_iconManager, &kIconManager, w);
                }
#endif
            }
            static void globalRemove(void *, wl_registry *, uint32_t) {}
            static constexpr wl_registry_listener kRegistry = {global, globalRemove};

#ifdef VCS_HAVE_TOPLEVEL_ICON
            static void iconSize(void *, xdg_toplevel_icon_manager_v1 *, int32_t) {}
            static void iconDone(void *, xdg_toplevel_icon_manager_v1 *) {}
            static constexpr xdg_toplevel_icon_manager_v1_listener kIconManager = {iconSize, iconDone};
#endif

            static void ping(void *, xdg_wm_base *base, uint32_t serial) { xdg_wm_base_pong(base, serial); }
            static constexpr xdg_wm_base_listener kWmBase = {ping};

            static void outputGeometry(void *, wl_output *, int32_t, int32_t, int32_t, int32_t, int32_t, const char *,
                                       const char *, int32_t) {}
            static void outputMode(void *data, wl_output *, uint32_t flags, int32_t, int32_t, int32_t refresh)
            {
                if ((flags & WL_OUTPUT_MODE_CURRENT) && refresh > 1000)
                    self(data)->m_refresh = float(refresh) / 1000.0f;
            }
            static void outputDone(void *data, wl_output *)
            {
                if (self(data)->m_refresh > 1)
                    window.refreshHz = self(data)->m_refresh;
            }
            static void outputScale(void *data, wl_output *output, int32_t factor)
            {
                Wayland *w = self(data);
                for (Output &o : w->m_outputs)
                    if (o.output == output)
                        o.scale = factor;
                if (!w->m_fractional)
                    w->setScale(float(w->outputScale()));
            }
            static constexpr wl_output_listener kOutput = {outputGeometry, outputMode, outputDone, outputScale};

            static void surfaceEnter(void *data, wl_surface *, wl_output *output)
            {
                Wayland *w = self(data);
                for (Output &o : w->m_outputs)
                    if (o.output == output)
                        o.entered = true;
                if (!w->m_fractional)
                    w->setScale(float(w->outputScale()));
            }
            static void surfaceLeave(void *data, wl_surface *, wl_output *output)
            {
                Wayland *w = self(data);
                for (Output &o : w->m_outputs)
                    if (o.output == output)
                        o.entered = false;
                if (!w->m_fractional)
                    w->setScale(float(w->outputScale()));
            }
            static constexpr wl_surface_listener kSurface = {surfaceEnter, surfaceLeave};

            static void preferredScale(void *data, wp_fractional_scale_v1 *, uint32_t scale)
            {
                self(data)->setScale(float(scale) / 120.0f);
            }
            static constexpr wp_fractional_scale_v1_listener kFractional = {preferredScale};

            static void xdgSurfaceConfigure(void *data, xdg_surface *surface, uint32_t serial)
            {
                Wayland *w = self(data);
                xdg_surface_ack_configure(surface, serial);
                if (w->m_pendingW > 0 && w->m_pendingH > 0)
                {
                    w->m_logicalW = w->m_pendingW;
                    w->m_logicalH = w->m_pendingH;
                    if (!w->m_maximized) // the size to come back to
                    {
                        w->m_floatW = w->m_logicalW;
                        w->m_floatH = w->m_logicalH;
                    }
                }
                else
                {
                    // 0 x 0: the size is ours to choose - the floating one (after un-maximising, not the maximised)
                    w->m_logicalW = w->m_floatW;
                    w->m_logicalH = w->m_floatH;
                }
                const bool first = !w->m_configured;
                w->m_configured = true;
                if (!first)
                    w->applySize();
                redraw();
            }
            static constexpr xdg_surface_listener kXdgSurface = {xdgSurfaceConfigure};

            static void toplevelConfigure(void *data, xdg_toplevel *, int32_t width, int32_t height, wl_array *states)
            {
                Wayland *w = self(data);
                w->m_pendingW = width;
                w->m_pendingH = height;
                bool maximized = false, activated = false, suspended = false;
                for (const uint32_t *s = static_cast<const uint32_t *>(states->data);
                     reinterpret_cast<const char *>(s) < static_cast<const char *>(states->data) + states->size; ++s)
                {
                    maximized |= *s == XDG_TOPLEVEL_STATE_MAXIMIZED || *s == XDG_TOPLEVEL_STATE_FULLSCREEN;
                    activated |= *s == XDG_TOPLEVEL_STATE_ACTIVATED;
#ifdef XDG_TOPLEVEL_STATE_SUSPENDED_SINCE_VERSION
                    suspended |= *s == XDG_TOPLEVEL_STATE_SUSPENDED; // minimised or fully covered: no frames
#endif
                }
                w->m_maximized = maximized;
                if (activated != window.focused)
                    focusChanged(activated);
                if (suspended != window.minimized)
                    minimizedChanged(suspended);
            }
            static void toplevelClose(void *, xdg_toplevel *) { closeRequested(); }
            static void toplevelBounds(void *, xdg_toplevel *, int32_t, int32_t) {}
            static void toplevelCapabilities(void *, xdg_toplevel *, wl_array *) {}
            static constexpr xdg_toplevel_listener kToplevel = {toplevelConfigure, toplevelClose, toplevelBounds,
                                                                toplevelCapabilities};

            static void tokenDone(void *data, xdg_activation_token_v1 *token, const char *text)
            {
                Wayland *w = self(data);
                xdg_activation_v1_activate(w->m_activation, text, w->m_surface);
                xdg_activation_token_v1_destroy(token);
            }
            static constexpr xdg_activation_token_v1_listener kToken = {tokenDone};

            static void seatCapabilities(void *data, wl_seat *seat, uint32_t caps)
            {
                Wayland *w = self(data);
                if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w->m_pointer)
                {
                    w->m_pointer = wl_seat_get_pointer(seat);
                    wl_pointer_add_listener(w->m_pointer, &kPointer, w);
                    if (w->m_shapeManager)
                        w->m_shapeDevice = wp_cursor_shape_manager_v1_get_pointer(w->m_shapeManager, w->m_pointer);
                }
                if ((caps & WL_SEAT_CAPABILITY_KEYBOARD) && !w->m_keyboard)
                {
                    w->m_keyboard = wl_seat_get_keyboard(seat);
                    wl_keyboard_add_listener(w->m_keyboard, &kKeyboard, w);
                }
                if (w->m_dataManager && !w->m_dataDevice)
                {
                    w->m_dataDevice = wl_data_device_manager_get_data_device(w->m_dataManager, seat);
                    wl_data_device_add_listener(w->m_dataDevice, &kDataDevice, w);
                }
            }
            static void seatName(void *, wl_seat *, const char *) {}
            static constexpr wl_seat_listener kSeat = {seatCapabilities, seatName};

            static void pointerEnter(void *data, wl_pointer *, uint32_t serial, wl_surface *, wl_fixed_t x, wl_fixed_t y)
            {
                Wayland *w = self(data);
                w->m_pointerSerial = serial;
                w->m_px = float(wl_fixed_to_double(x)) * w->m_scale;
                w->m_py = float(wl_fixed_to_double(y)) * w->m_scale;
                pointerMotion(w->m_px, w->m_py);
                w->applyShape();
            }
            static void pointerLeaveEvent(void *, wl_pointer *, uint32_t, wl_surface *) { pointerLeave(); }
            static void pointerMotionEvent(void *data, wl_pointer *, uint32_t, wl_fixed_t x, wl_fixed_t y)
            {
                Wayland *w = self(data);
                w->m_px = float(wl_fixed_to_double(x)) * w->m_scale;
                w->m_py = float(wl_fixed_to_double(y)) * w->m_scale;
                pointerMotion(w->m_px, w->m_py);
            }
            static void pointerButtonEvent(void *data, wl_pointer *, uint32_t serial, uint32_t, uint32_t button, uint32_t state)
            {
                Wayland *w = self(data);
                w->m_buttonSerial = w->m_lastSerial = serial;
                const int b = button == kBtnLeft ? kMouseLeft : button == kBtnRight ? kMouseRight
                            : button == kBtnMiddle                 ? kMouseMiddle
                                                                   : -1;
                if (b < 0)
                    return;
                const bool down = state == WL_POINTER_BUTTON_STATE_PRESSED;
                if (down && chromePress(b, w->m_px, w->m_py))
                    return;
                clientButton(b, down, w->m_px, w->m_py);
            }
            static void pointerAxis(void *data, wl_pointer *, uint32_t, uint32_t axis, wl_fixed_t value)
            {
                if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
                    self(data)->m_axis += float(wl_fixed_to_double(value));
            }
            static void pointerFrame(void *data, wl_pointer *)
            {
                Wayland *w = self(data);
                // a wheel's notches when it has them, else a touchpad's distance (10 units a notch, as GTK counts)
                if (w->m_axisDiscrete)
                    pointerWheel(-float(w->m_axisDiscrete));
                else if (w->m_axis != 0)
                    pointerWheel(-w->m_axis / 10.0f);
                w->m_axis = 0;
                w->m_axisDiscrete = 0;
            }
            static void pointerAxisSource(void *, wl_pointer *, uint32_t) {}
            static void pointerAxisStop(void *, wl_pointer *, uint32_t, uint32_t) {}
            static void pointerAxisDiscrete(void *data, wl_pointer *, uint32_t axis, int32_t discrete)
            {
                if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
                    self(data)->m_axisDiscrete += discrete;
            }
            static constexpr wl_pointer_listener kPointer = {pointerEnter,     pointerLeaveEvent, pointerMotionEvent,
                                                             pointerButtonEvent, pointerAxis,     pointerFrame,
                                                             pointerAxisSource, pointerAxisStop,  pointerAxisDiscrete};

            static void keymapEvent(void *data, wl_keyboard *, uint32_t format, int32_t fd, uint32_t size)
            {
                Wayland *w = self(data);
                if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || !w->m_xkb)
                {
                    close(fd);
                    return;
                }
                void *map = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
                close(fd);
                if (map == MAP_FAILED)
                    return;
                xkb_keymap *keymap = xkb_keymap_new_from_string(w->m_xkb, static_cast<const char *>(map),
                                                                XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
                munmap(map, size);
                if (!keymap)
                    return;
                if (w->m_xkbState)
                    xkb_state_unref(w->m_xkbState);
                if (w->m_keymap)
                    xkb_keymap_unref(w->m_keymap);
                w->m_keymap = keymap;
                w->m_xkbState = xkb_state_new(keymap);
            }
            static void keyboardEnter(void *data, wl_keyboard *, uint32_t serial, wl_surface *, wl_array *)
            {
                self(data)->m_lastSerial = serial;
            }
            static void keyboardLeave(void *data, wl_keyboard *, uint32_t, wl_surface *)
            {
                self(data)->stopRepeat();
                releaseInput();
            }
            static void keyEventCb(void *data, wl_keyboard *, uint32_t serial, uint32_t, uint32_t key, uint32_t state)
            {
                Wayland *w = self(data);
                if (!w->m_xkbState)
                    return;
                w->m_lastSerial = serial;
                const uint32_t keycode = key + 8; // evdev to xkb
                if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
                {
                    w->pressKey(keycode, false);
                    w->armRepeat(keycode);
                }
                else
                {
                    if (w->m_repeatKey == keycode)
                        w->stopRepeat();
                    const xkb_layout_index_t layout = xkb_state_key_get_layout(w->m_xkbState, keycode);
                    const xkb_keysym_t *syms = nullptr;
                    const int n = xkb_keymap_key_get_syms_by_level(w->m_keymap, keycode, layout, 0, &syms);
                    keyEvent(n > 0 ? uint32_t(syms[0]) : 0, false, false);
                }
            }
            static void modifiersEvent(void *data, wl_keyboard *, uint32_t, uint32_t depressed, uint32_t latched,
                                       uint32_t locked, uint32_t group)
            {
                Wayland *w = self(data);
                if (!w->m_xkbState)
                    return;
                xkb_state_update_mask(w->m_xkbState, depressed, latched, locked, 0, 0, group);
                w->updateModifiers();
            }
            static void repeatInfo(void *data, wl_keyboard *, int32_t rate, int32_t delay)
            {
                self(data)->m_repeatRate = rate;
                self(data)->m_repeatDelay = delay;
            }
            static constexpr wl_keyboard_listener kKeyboard = {keymapEvent, keyboardEnter, keyboardLeave,
                                                               keyEventCb,  modifiersEvent, repeatInfo};

            // ---- drag and drop -------------------------------------------------------------------------------------

            static void offerMime(void *data, wl_data_offer *offer, const char *mime)
            {
                Wayland *w = self(data);
                if (offer == w->m_newOffer && std::strcmp(mime, "text/uri-list") == 0)
                    w->m_newOfferHasUris = true;
            }
            static void offerSourceActions(void *, wl_data_offer *, uint32_t) {}
            static void offerAction(void *, wl_data_offer *, uint32_t) {}
            static constexpr wl_data_offer_listener kOffer = {offerMime, offerSourceActions, offerAction};

            static void dataOffer(void *data, wl_data_device *, wl_data_offer *offer)
            {
                Wayland *w = self(data);
                w->m_newOffer = offer;
                w->m_newOfferHasUris = false;
                wl_data_offer_add_listener(offer, &kOffer, w);
            }
            static void dataEnter(void *data, wl_data_device *, uint32_t serial, wl_surface *, wl_fixed_t x, wl_fixed_t y,
                                  wl_data_offer *offer)
            {
                Wayland *w = self(data);
                if (w->m_offer && w->m_offer != offer)
                    wl_data_offer_destroy(w->m_offer);
                w->m_offer = offer;
                w->m_offerHasUris = offer && offer == w->m_newOffer && w->m_newOfferHasUris;
                if (!offer)
                    return;
                wl_data_offer_accept(offer, serial, w->m_offerHasUris ? "text/uri-list" : nullptr);
                if (wl_data_offer_get_version(offer) >= 3)
                    wl_data_offer_set_actions(offer, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY,
                                              WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);
                if (w->m_offerHasUris)
                    dropEnter(float(wl_fixed_to_double(x)) * w->m_scale, float(wl_fixed_to_double(y)) * w->m_scale);
            }
            static void dataLeave(void *data, wl_data_device *)
            {
                Wayland *w = self(data);
                if (w->m_offer)
                    wl_data_offer_destroy(w->m_offer);
                w->m_offer = nullptr;
                dropLeave();
            }
            static void dataMotion(void *data, wl_data_device *, uint32_t, wl_fixed_t x, wl_fixed_t y)
            {
                Wayland *w = self(data);
                if (w->m_offerHasUris)
                    dropMotion(float(wl_fixed_to_double(x)) * w->m_scale, float(wl_fixed_to_double(y)) * w->m_scale);
            }
            static void dataDrop(void *data, wl_data_device *)
            {
                Wayland *w = self(data);
                if (!w->m_offer || !w->m_offerHasUris)
                {
                    dropLeave();
                    return;
                }
                int fds[2];
                std::string list;
                if (pipe2(fds, O_CLOEXEC) == 0)
                {
                    wl_data_offer_receive(w->m_offer, "text/uri-list", fds[1]);
                    close(fds[1]);
                    wl_display_flush(w->m_display);
                    // the source writes it now (another client: no deadlock); a second at most
                    char buffer[4096];
                    for (int waited = 0; waited < 1000;)
                    {
                        pollfd p{fds[0], POLLIN, 0};
                        const int r = poll(&p, 1, 50);
                        if (r == 0)
                        {
                            waited += 50;
                            continue;
                        }
                        if (r < 0 && errno == EINTR)
                            continue;
                        const ssize_t n = r > 0 ? read(fds[0], buffer, sizeof(buffer)) : -1;
                        if (n <= 0)
                            break;
                        list.append(buffer, size_t(n));
                    }
                    close(fds[0]);
                }
                if (wl_data_offer_get_version(w->m_offer) >= 3)
                    wl_data_offer_finish(w->m_offer);
                wl_data_offer_destroy(w->m_offer);
                w->m_offer = nullptr;
                dropFiles(list);
            }
            static void dataSelection(void *data, wl_data_device *, wl_data_offer *offer)
            {
                // the clipboard: not ours to read; its offer goes
                Wayland *w = self(data);
                if (offer && offer != w->m_offer)
                    wl_data_offer_destroy(offer);
            }
            static constexpr wl_data_device_listener kDataDevice = {dataOffer, dataEnter, dataLeave,
                                                                    dataMotion, dataDrop, dataSelection};

            wl_display *m_display = nullptr;
            wl_registry *m_registry = nullptr;
            wl_compositor *m_compositor = nullptr;
            xdg_wm_base *m_wmBase = nullptr;
            wl_seat *m_seat = nullptr;
            wl_shm *m_shm = nullptr;
            wl_data_device_manager *m_dataManager = nullptr;
            zxdg_decoration_manager_v1 *m_decorationManager = nullptr;
            wp_viewporter *m_viewporter = nullptr;
            wp_fractional_scale_manager_v1 *m_fractionalManager = nullptr;
            wp_cursor_shape_manager_v1 *m_shapeManager = nullptr;
            xdg_activation_v1 *m_activation = nullptr;
#ifdef VCS_HAVE_TOPLEVEL_ICON
            xdg_toplevel_icon_manager_v1 *m_iconManager = nullptr;
            std::vector<wl_buffer *> m_iconBuffers;
#endif
            std::vector<Output> m_outputs;

            wl_surface *m_surface = nullptr;
            xdg_surface *m_xdgSurface = nullptr;
            xdg_toplevel *m_toplevel = nullptr;
            zxdg_toplevel_decoration_v1 *m_decoration = nullptr;
            wp_viewport *m_viewport = nullptr;
            wp_fractional_scale_v1 *m_fractional = nullptr;
            NativeSurface m_native{};
            bool m_configured = false, m_maximized = false, m_reading = false;
            int m_logicalW = 0, m_logicalH = 0, m_pendingW = 0, m_pendingH = 0, m_floatW = 0, m_floatH = 0;
            float m_scale = 1, m_refresh = 0;

            wl_pointer *m_pointer = nullptr;
            wp_cursor_shape_device_v1 *m_shapeDevice = nullptr;
            wl_cursor_theme *m_cursorTheme = nullptr;
            wl_surface *m_cursorSurface = nullptr;
            Shape m_shape = Shape::Arrow;
            uint32_t m_pointerSerial = 0, m_buttonSerial = 0, m_lastSerial = 0;
            float m_px = 0, m_py = 0, m_axis = 0;
            int m_axisDiscrete = 0;

            wl_keyboard *m_keyboard = nullptr;
            xkb_context *m_xkb = nullptr;
            xkb_keymap *m_keymap = nullptr;
            xkb_state *m_xkbState = nullptr;
            xkb_compose_state *m_compose = nullptr;
            int m_repeatTimer = -1, m_repeatRate = 25, m_repeatDelay = 600;
            uint32_t m_repeatKey = 0;

            wl_data_device *m_dataDevice = nullptr;
            wl_data_offer *m_offer = nullptr, *m_newOffer = nullptr;
            bool m_offerHasUris = false, m_newOfferHasUris = false;
        };
    }

    std::unique_ptr<Backend> makeWayland() { return std::make_unique<Wayland>(); }
}
