// The few D-Bus calls the launcher makes on Linux (libdbus-1, the system's): the file manager, logind's inhibitor,
// the desktop portal (file dialogs, the colour scheme). Every call has a short timeout and fails quietly when the
// service isn't there: the callers have a fallback.
#pragma once

#include <functional>
#include <string>
#include <vector>

namespace dbus
{
    // org.freedesktop.FileManager1.ShowItems: the file manager with these file:// URIs selected
    bool showItems(const char *const *uris, int count);

    // org.freedesktop.login1.Manager.Inhibit("sleep:idle", ...): a file descriptor that holds the inhibitor until
    // closed; -1 when logind refused or isn't there
    int inhibitSleep(const char *who, const char *why);

    // org.freedesktop.portal.Settings.ReadOne (Read on older portals): a desktop setting such as
    // org.freedesktop.appearance color-scheme (0 no preference, 1 dark, 2 light), org.gnome.desktop.interface
    // gtk-theme or enable-animations. Kind None without a portal or that setting.
    struct Setting
    {
        enum Kind
        {
            None,
            Text,
            Number,
            Boolean
        } kind = None;
        std::string text;
        long long number = 0;
    };
    Setting portalSetting(const char *ns, const char *key);

    // org.freedesktop.portal.FileChooser. `parent` is "x11:<hex xid>" or "wayland:<handle>" (or empty). Blocks until
    // the dialog closes, calling `idle` while it waits (the launcher keeps drawing). Returns 1 with the paths picked,
    // 0 when cancelled, -1 when there is no portal (the caller tries zenity / kdialog).
    struct FileFilter
    {
        std::string name;
        std::vector<std::string> globs; // "*.iso"
    };
    enum class ChooserKind
    {
        Open,
        Save,
        Folder
    };
    int fileChooser(ChooserKind kind, const std::string &parent, const std::string &title,
                    const std::vector<FileFilter> &filters, const std::string &currentFolder,
                    const std::string &currentName, std::vector<std::string> &paths,
                    const std::function<void()> &idle);
}
