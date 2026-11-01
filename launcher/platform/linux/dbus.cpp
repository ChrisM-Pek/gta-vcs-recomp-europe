#include "dbus.h"

#include <dbus/dbus.h>

#include <cstdio>
#include <cstring>
#include <random>
#include <unistd.h>

namespace dbus
{
    namespace
    {
        DBusConnection *bus(DBusBusType type)
        {
            DBusError err;
            dbus_error_init(&err);
            DBusConnection *c = dbus_bus_get(type, &err);
            dbus_error_free(&err);
            if (c)
                dbus_connection_set_exit_on_disconnect(c, false);
            return c;
        }

        DBusMessage *call(DBusConnection *c, DBusMessage *m, int timeoutMs)
        {
            DBusError err;
            dbus_error_init(&err);
            DBusMessage *reply = dbus_connection_send_with_reply_and_block(c, m, timeoutMs, &err);
            dbus_error_free(&err);
            dbus_message_unref(m);
            return reply;
        }

        void dictString(DBusMessageIter *dict, const char *key, const char *value)
        {
            DBusMessageIter entry, variant;
            dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "s", &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_STRING, &value);
            dbus_message_iter_close_container(&entry, &variant);
            dbus_message_iter_close_container(dict, &entry);
        }

        void dictBool(DBusMessageIter *dict, const char *key, bool value)
        {
            DBusMessageIter entry, variant;
            dbus_bool_t b = value;
            dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "b", &variant);
            dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &b);
            dbus_message_iter_close_container(&entry, &variant);
            dbus_message_iter_close_container(dict, &entry);
        }

        // a path as the portal wants it: bytes with the terminating NUL
        void dictBytes(DBusMessageIter *dict, const char *key, const std::string &value)
        {
            DBusMessageIter entry, variant, array;
            dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "ay", &variant);
            dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "y", &array);
            const char *data = value.c_str();
            dbus_message_iter_append_fixed_array(&array, DBUS_TYPE_BYTE, &data, int(value.size() + 1));
            dbus_message_iter_close_container(&variant, &array);
            dbus_message_iter_close_container(&entry, &variant);
            dbus_message_iter_close_container(dict, &entry);
        }

        // filters: a(sa(us)) - a name and (0 = glob, pattern) pairs
        void dictFilters(DBusMessageIter *dict, const std::vector<FileFilter> &filters)
        {
            const char *key = "filters";
            DBusMessageIter entry, variant, list;
            dbus_message_iter_open_container(dict, DBUS_TYPE_DICT_ENTRY, nullptr, &entry);
            dbus_message_iter_append_basic(&entry, DBUS_TYPE_STRING, &key);
            dbus_message_iter_open_container(&entry, DBUS_TYPE_VARIANT, "a(sa(us))", &variant);
            dbus_message_iter_open_container(&variant, DBUS_TYPE_ARRAY, "(sa(us))", &list);
            for (const FileFilter &f : filters)
            {
                DBusMessageIter filter, patterns;
                dbus_message_iter_open_container(&list, DBUS_TYPE_STRUCT, nullptr, &filter);
                const char *name = f.name.c_str();
                dbus_message_iter_append_basic(&filter, DBUS_TYPE_STRING, &name);
                dbus_message_iter_open_container(&filter, DBUS_TYPE_ARRAY, "(us)", &patterns);
                for (const std::string &g : f.globs)
                {
                    DBusMessageIter pair;
                    dbus_uint32_t glob = 0;
                    const char *pattern = g.c_str();
                    dbus_message_iter_open_container(&patterns, DBUS_TYPE_STRUCT, nullptr, &pair);
                    dbus_message_iter_append_basic(&pair, DBUS_TYPE_UINT32, &glob);
                    dbus_message_iter_append_basic(&pair, DBUS_TYPE_STRING, &pattern);
                    dbus_message_iter_close_container(&patterns, &pair);
                }
                dbus_message_iter_close_container(&filter, &patterns);
                dbus_message_iter_close_container(&list, &filter);
            }
            dbus_message_iter_close_container(&variant, &list);
            dbus_message_iter_close_container(&entry, &variant);
            dbus_message_iter_close_container(dict, &entry);
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

        std::string pathFromUri(const std::string &uri)
        {
            std::string s = uri.rfind("file://", 0) == 0 ? uri.substr(7) : uri;
            if (!s.empty() && s[0] != '/') // file://host/path
                s = s.find('/') == std::string::npos ? std::string() : s.substr(s.find('/'));
            std::string out;
            for (size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0)
                {
                    out += char(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
                    i += 2;
                }
                else
                    out += s[i];
            }
            return out;
        }

        // a variant's value, through any number of nested variants (Settings.Read wraps it twice)
        Setting variantValue(DBusMessageIter *it)
        {
            while (dbus_message_iter_get_arg_type(it) == DBUS_TYPE_VARIANT)
            {
                DBusMessageIter inner;
                dbus_message_iter_recurse(it, &inner);
                *it = inner;
            }
            Setting out;
            DBusBasicValue v{};
            switch (dbus_message_iter_get_arg_type(it))
            {
            case DBUS_TYPE_STRING:
                dbus_message_iter_get_basic(it, &v);
                out.kind = Setting::Text;
                out.text = v.str ? v.str : "";
                break;
            case DBUS_TYPE_BOOLEAN:
                dbus_message_iter_get_basic(it, &v);
                out.kind = Setting::Boolean;
                out.number = v.bool_val ? 1 : 0;
                break;
            case DBUS_TYPE_UINT32:
                dbus_message_iter_get_basic(it, &v);
                out.kind = Setting::Number;
                out.number = v.u32;
                break;
            case DBUS_TYPE_INT32:
                dbus_message_iter_get_basic(it, &v);
                out.kind = Setting::Number;
                out.number = v.i32;
                break;
            default:
                break;
            }
            return out;
        }

        bool g_noPortal = false; // asked once and nobody answered: don't wait again
    }

    bool showItems(const char *const *uris, int count)
    {
        DBusConnection *c = bus(DBUS_BUS_SESSION);
        if (!c)
            return false;
        DBusMessage *m = dbus_message_new_method_call("org.freedesktop.FileManager1", "/org/freedesktop/FileManager1",
                                                      "org.freedesktop.FileManager1", "ShowItems");
        DBusMessageIter args, array;
        dbus_message_iter_init_append(m, &args);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "s", &array);
        for (int i = 0; i < count; ++i)
            dbus_message_iter_append_basic(&array, DBUS_TYPE_STRING, &uris[i]);
        dbus_message_iter_close_container(&args, &array);
        const char *startup = "";
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &startup);
        DBusMessage *reply = call(c, m, 3000); // may start the file manager first
        const bool ok = reply && dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN;
        if (reply)
            dbus_message_unref(reply);
        dbus_connection_unref(c);
        return ok;
    }

    int inhibitSleep(const char *who, const char *why)
    {
        DBusConnection *c = bus(DBUS_BUS_SYSTEM);
        if (!c)
            return -1;
        DBusMessage *m = dbus_message_new_method_call("org.freedesktop.login1", "/org/freedesktop/login1",
                                                      "org.freedesktop.login1.Manager", "Inhibit");
        const char *what = "sleep:idle", *mode = "block";
        dbus_message_append_args(m, DBUS_TYPE_STRING, &what, DBUS_TYPE_STRING, &who, DBUS_TYPE_STRING, &why,
                                 DBUS_TYPE_STRING, &mode, DBUS_TYPE_INVALID);
        DBusMessage *reply = call(c, m, 1000);
        int fd = -1;
        if (reply)
        {
            DBusError err;
            dbus_error_init(&err);
            if (!dbus_message_get_args(reply, &err, DBUS_TYPE_UNIX_FD, &fd, DBUS_TYPE_INVALID))
                fd = -1;
            dbus_error_free(&err);
            dbus_message_unref(reply);
        }
        dbus_connection_unref(c);
        return fd;
    }

    Setting portalSetting(const char *ns, const char *key)
    {
        Setting result;
        if (g_noPortal)
            return result;
        DBusConnection *c = bus(DBUS_BUS_SESSION);
        if (!c)
            return result;
        bool answered = false;
        for (const char *method : {"ReadOne", "Read"}) // ReadOne: portal 2; Read (deprecated) everywhere
        {
            DBusMessage *m = dbus_message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                          "org.freedesktop.portal.Settings", method);
            dbus_message_append_args(m, DBUS_TYPE_STRING, &ns, DBUS_TYPE_STRING, &key, DBUS_TYPE_INVALID);
            DBusMessage *reply = call(c, m, 500);
            if (!reply)
                continue;
            answered = true;
            DBusMessageIter it;
            if (dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_METHOD_RETURN && dbus_message_iter_init(reply, &it))
                result = variantValue(&it);
            const bool unknownMethod = dbus_message_get_type(reply) == DBUS_MESSAGE_TYPE_ERROR &&
                                       dbus_message_is_error(reply, DBUS_ERROR_UNKNOWN_METHOD);
            dbus_message_unref(reply);
            if (result.kind != Setting::None || !unknownMethod)
                break;
        }
        if (!answered)
            g_noPortal = true;
        dbus_connection_unref(c);
        return result;
    }

    int fileChooser(ChooserKind kind, const std::string &parent, const std::string &title,
                    const std::vector<FileFilter> &filters, const std::string &currentFolder,
                    const std::string &currentName, std::vector<std::string> &paths,
                    const std::function<void()> &idle)
    {
        paths.clear();
        DBusConnection *c = bus(DBUS_BUS_SESSION);
        if (!c)
            return -1;

        // the Response signal comes on a request object whose path we know in advance: listen before asking, so it
        // can't be missed (the portal's documented pattern)
        char token[32];
        std::snprintf(token, sizeof(token), "vcs%u", unsigned(std::random_device{}()));
        std::string sender = dbus_bus_get_unique_name(c) ? dbus_bus_get_unique_name(c) + 1 : "";
        for (char &ch : sender)
            if (ch == '.')
                ch = '_';
        const std::string requestPath = "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
        const std::string rule = "type='signal',interface='org.freedesktop.portal.Request',member='Response',path='" +
                                 requestPath + "'";
        dbus_bus_add_match(c, rule.c_str(), nullptr);

        DBusMessage *m = dbus_message_new_method_call("org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
                                                      "org.freedesktop.portal.FileChooser",
                                                      kind == ChooserKind::Save ? "SaveFile" : "OpenFile");
        DBusMessageIter args, dict;
        dbus_message_iter_init_append(m, &args);
        const char *parentText = parent.c_str(), *titleText = title.c_str();
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &parentText);
        dbus_message_iter_append_basic(&args, DBUS_TYPE_STRING, &titleText);
        dbus_message_iter_open_container(&args, DBUS_TYPE_ARRAY, "{sv}", &dict);
        dictString(&dict, "handle_token", token);
        dictBool(&dict, "modal", true);
        if (kind == ChooserKind::Folder)
            dictBool(&dict, "directory", true);
        if (!filters.empty() && kind != ChooserKind::Folder)
            dictFilters(&dict, filters);
        if (!currentFolder.empty())
            dictBytes(&dict, "current_folder", currentFolder);
        if (kind == ChooserKind::Save && !currentName.empty())
            dictString(&dict, "current_name", currentName.c_str());
        dbus_message_iter_close_container(&args, &dict);

        DBusMessage *reply = call(c, m, 5000);
        if (!reply || dbus_message_get_type(reply) != DBUS_MESSAGE_TYPE_METHOD_RETURN)
        {
            if (reply)
                dbus_message_unref(reply);
            dbus_bus_remove_match(c, rule.c_str(), nullptr);
            dbus_connection_unref(c);
            return -1;
        }
        // an old portal answers with a path of its own choosing
        const char *handle = nullptr;
        std::string path = requestPath;
        if (dbus_message_get_args(reply, nullptr, DBUS_TYPE_OBJECT_PATH, &handle, DBUS_TYPE_INVALID) && handle)
            path = handle;
        dbus_message_unref(reply);

        int result = -2;
        while (result == -2)
        {
            if (!dbus_connection_read_write(c, 30))
            {
                result = -1; // the bus went away
                break;
            }
            while (DBusMessage *msg = dbus_connection_pop_message(c))
            {
                if (dbus_message_is_signal(msg, "org.freedesktop.portal.Request", "Response") &&
                    dbus_message_get_path(msg) && path == dbus_message_get_path(msg))
                {
                    DBusMessageIter it;
                    dbus_uint32_t response = 2;
                    if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_UINT32)
                    {
                        dbus_message_iter_get_basic(&it, &response);
                        dbus_message_iter_next(&it);
                    }
                    result = 0;
                    if (response == 0 && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY)
                    {
                        DBusMessageIter results;
                        dbus_message_iter_recurse(&it, &results);
                        for (; dbus_message_iter_get_arg_type(&results) == DBUS_TYPE_DICT_ENTRY;
                             dbus_message_iter_next(&results))
                        {
                            DBusMessageIter entry, value;
                            const char *key = nullptr;
                            dbus_message_iter_recurse(&results, &entry);
                            dbus_message_iter_get_basic(&entry, &key);
                            dbus_message_iter_next(&entry);
                            if (!key || std::strcmp(key, "uris") != 0)
                                continue;
                            dbus_message_iter_recurse(&entry, &value); // the variant
                            if (dbus_message_iter_get_arg_type(&value) != DBUS_TYPE_ARRAY)
                                continue;
                            DBusMessageIter uris;
                            dbus_message_iter_recurse(&value, &uris);
                            for (; dbus_message_iter_get_arg_type(&uris) == DBUS_TYPE_STRING; dbus_message_iter_next(&uris))
                            {
                                const char *uri = nullptr;
                                dbus_message_iter_get_basic(&uris, &uri);
                                if (uri)
                                    paths.push_back(pathFromUri(uri));
                            }
                        }
                        result = paths.empty() ? 0 : 1;
                    }
                }
                dbus_message_unref(msg);
            }
            if (result == -2 && idle)
                idle();
        }
        dbus_bus_remove_match(c, rule.c_str(), nullptr);
        dbus_connection_unref(c);
        return result;
    }
}
