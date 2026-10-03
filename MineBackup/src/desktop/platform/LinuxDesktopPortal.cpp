#include "LinuxDesktopPortal.h"

#include "text_to_text.h"

#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <set>
#include <unistd.h>
#include <utility>

using namespace std;

namespace {

constexpr const char* PortalBus = "org.freedesktop.portal.Desktop";
constexpr const char* PortalPath = "/org/freedesktop/portal/desktop";

GDBusConnection* g_connection = nullptr;
map<string, CapabilityStatus> g_probeCache;
string g_shortcutSession;
guint g_shortcutSignal = 0;
map<string, int> g_shortcutIds;
function<void(int)> g_shortcutCallback;
atomic_uint64_t g_tokenCounter{0};

wstring ErrorText(const char* prefix, GError* error) {
    wstring message = utf8_to_wstring(prefix);
    if (error && error->message) message += L": " + utf8_to_wstring(error->message);
    return message;
}

GDBusConnection* Connection(wstring& error) {
    if (g_connection) return g_connection;
    GError* nativeError = nullptr;
    g_connection = g_bus_get_sync(G_BUS_TYPE_SESSION, nullptr, &nativeError);
    if (!g_connection) {
        error = ErrorText("The D-Bus session bus is unavailable", nativeError);
        g_clear_error(&nativeError);
    }
    return g_connection;
}

string NextToken(const char* prefix) {
    return string(prefix) + "_" + to_string(getpid()) + "_"
        + to_string(++g_tokenCounter);
}

string RequestPath(GDBusConnection* connection, const string& token) {
    string sender = g_dbus_connection_get_unique_name(connection);
    if (!sender.empty() && sender.front() == ':') sender.erase(sender.begin());
    replace(sender.begin(), sender.end(), '.', '_');
    return "/org/freedesktop/portal/desktop/request/" + sender + "/" + token;
}

struct PortalRequestState {
    GMainLoop* loop = nullptr;
    guint32 response = 2;
    GVariant* results = nullptr;
    bool received = false;
    bool timedOut = false;
};

void PortalResponse(
    GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
    GVariant* parameters, gpointer userData) {
    auto* state = static_cast<PortalRequestState*>(userData);
    if (state->received) return;
    g_variant_get(parameters, "(u@a{sv})", &state->response, &state->results);
    state->received = true;
    g_main_loop_quit(state->loop);
}

gboolean PortalRequestTimeout(gpointer userData) {
    auto* state = static_cast<PortalRequestState*>(userData);
    state->timedOut = true;
    g_main_loop_quit(state->loop);
    return G_SOURCE_REMOVE;
}

struct PortalRequestResult {
    guint32 response = 2;
    GVariant* results = nullptr;
    wstring error;
    bool timedOut = false;
};

PortalRequestResult RunPortalRequest(
    const char* interfaceName,
    const char* methodName,
    const function<GVariant*(const string&)>& parameters,
    GUnixFDList* fileDescriptors = nullptr) {
    PortalRequestResult result;
    wstring connectionError;
    auto* connection = Connection(connectionError);
    if (!connection) {
        result.error = std::move(connectionError);
        return result;
    }

    const string token = NextToken("minebackup");
    string requestPath = RequestPath(connection, token);
    PortalRequestState state;
    state.loop = g_main_loop_new(nullptr, FALSE);
    guint subscription = g_dbus_connection_signal_subscribe(connection, PortalBus,
        "org.freedesktop.portal.Request", "Response", requestPath.c_str(), nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, PortalResponse, &state, nullptr);

    GError* nativeError = nullptr;
    GVariant* reply = nullptr;
    if (fileDescriptors) {
        GUnixFDList* returnedDescriptors = nullptr;
        reply = g_dbus_connection_call_with_unix_fd_list_sync(
            connection, PortalBus, PortalPath, interfaceName, methodName,
            parameters(token), G_VARIANT_TYPE("(o)"), G_DBUS_CALL_FLAGS_NONE, 5000,
            fileDescriptors, &returnedDescriptors, nullptr, &nativeError);
        if (returnedDescriptors) g_object_unref(returnedDescriptors);
    }
    else {
        reply = g_dbus_connection_call_sync(connection, PortalBus, PortalPath,
            interfaceName, methodName, parameters(token), G_VARIANT_TYPE("(o)"),
            G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &nativeError);
    }
    if (!reply) {
        result.error = ErrorText("The desktop portal request failed", nativeError);
        g_clear_error(&nativeError);
        g_dbus_connection_signal_unsubscribe(connection, subscription);
        g_main_loop_unref(state.loop);
        return result;
    }

    const char* returnedPath = nullptr;
    g_variant_get(reply, "(&o)", &returnedPath);
    if (returnedPath && requestPath != returnedPath) {
        g_dbus_connection_signal_unsubscribe(connection, subscription);
        requestPath = returnedPath;
        subscription = g_dbus_connection_signal_subscribe(connection, PortalBus,
            "org.freedesktop.portal.Request", "Response", requestPath.c_str(), nullptr,
            G_DBUS_SIGNAL_FLAGS_NONE, PortalResponse, &state, nullptr);
    }
    g_variant_unref(reply);

    const guint timeout = g_timeout_add_seconds(300, PortalRequestTimeout, &state);
    if (!state.received) g_main_loop_run(state.loop);
    if (!state.timedOut) g_source_remove(timeout);
    else {
        GVariant* closed = g_dbus_connection_call_sync(
            connection, PortalBus, requestPath.c_str(),
            "org.freedesktop.portal.Request", "Close", nullptr, nullptr,
            G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
        if (closed) g_variant_unref(closed);
    }
    g_dbus_connection_signal_unsubscribe(connection, subscription);
    g_main_loop_unref(state.loop);

    result.response = state.response;
    result.results = state.results;
    result.timedOut = state.timedOut;
    if (state.timedOut) result.error = L"The desktop portal request timed out.";
    return result;
}

CapabilityStatus RequestStatus(const PortalRequestResult& result, const wchar_t* cancelled) {
    if (!result.error.empty()) return CapabilityStatus::Failed(result.error);
    if (result.response == 0) return CapabilityStatus::Ready();
    if (result.response == 1) return CapabilityStatus::PermissionRequired(cancelled);
    return CapabilityStatus::Failed(L"The desktop portal rejected the request.");
}

CapabilityStatus ShortcutRequestStatus(
    const PortalRequestResult& result, const wchar_t* denied) {
    if (!result.error.empty()) return CapabilityStatus::Failed(result.error);
    if (result.response == 0) return CapabilityStatus::Ready();
    return CapabilityStatus::PermissionRequired(denied);
}

void ClosePortalSession(const string& sessionPath) {
    if (!g_connection || sessionPath.empty()) return;
    GVariant* closed = g_dbus_connection_call_sync(
        g_connection, PortalBus, sessionPath.c_str(),
        "org.freedesktop.portal.Session", "Close", nullptr, nullptr,
        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, nullptr);
    if (closed) g_variant_unref(closed);
}

void CloseShortcutSession() {
    if (g_shortcutSignal != 0) {
        if (g_connection) {
            g_dbus_connection_signal_unsubscribe(g_connection, g_shortcutSignal);
        }
        g_shortcutSignal = 0;
    }
    if (!g_shortcutSession.empty()) {
        ClosePortalSession(g_shortcutSession);
        g_shortcutSession.clear();
    }
    g_shortcutIds.clear();
    g_shortcutCallback = {};
}

void ShortcutActivated(
    GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*,
    GVariant* parameters, gpointer) {
    const char* session = nullptr;
    const char* shortcutId = nullptr;
    guint64 timestamp = 0;
    GVariant* options = nullptr;
    g_variant_get(parameters, "(&o&st@a{sv})", &session, &shortcutId, &timestamp, &options);
    (void)timestamp;
    if (options) g_variant_unref(options);
    if (!session || g_shortcutSession != session || !shortcutId) return;
    const auto found = g_shortcutIds.find(shortcutId);
    if (found != g_shortcutIds.end() && g_shortcutCallback) {
        g_shortcutCallback(found->second);
    }
}

} // namespace

CapabilityStatus ProbeLinuxPortalInterface(const char* interfaceName) {
    if (!interfaceName || !*interfaceName) {
        return CapabilityStatus::Failed(L"A portal interface name is required.");
    }
    if (const auto cached = g_probeCache.find(interfaceName); cached != g_probeCache.end()) {
        return cached->second;
    }

    wstring connectionError;
    auto* connection = Connection(connectionError);
    if (!connection) return CapabilityStatus::Unavailable(std::move(connectionError));

    GError* nativeError = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(connection, PortalBus, PortalPath,
        "org.freedesktop.DBus.Properties", "Get",
        g_variant_new("(ss)", interfaceName, "version"), G_VARIANT_TYPE("(v)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &nativeError);
    CapabilityStatus status;
    if (reply) {
        g_variant_unref(reply);
        status = CapabilityStatus::Ready(
            L"The XDG Desktop Portal interface is available.");
    }
    else {
        status = CapabilityStatus::Unavailable(
            ErrorText("The required XDG Desktop Portal interface is unavailable", nativeError));
        g_clear_error(&nativeError);
    }
    g_probeCache.emplace(interfaceName, status);
    return status;
}

CapabilityStatus ProbeLinuxStatusNotifierHost() {
    static CapabilityStatus cached;
    static chrono::steady_clock::time_point checkedAt{};
    const auto now = chrono::steady_clock::now();
    if (!cached.diagnostic.empty() && now - checkedAt < chrono::seconds(2)) return cached;
    checkedAt = now;

    wstring connectionError;
    auto* connection = Connection(connectionError);
    if (!connection) {
        cached = CapabilityStatus::Unavailable(std::move(connectionError));
        return cached;
    }
    GError* nativeError = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(connection, "org.freedesktop.DBus",
        "/org/freedesktop/DBus", "org.freedesktop.DBus", "NameHasOwner",
        g_variant_new("(s)", "org.kde.StatusNotifierWatcher"), G_VARIANT_TYPE("(b)"),
        G_DBUS_CALL_FLAGS_NONE, 2000, nullptr, &nativeError);
    if (!reply) {
        cached = CapabilityStatus::Failed(
            ErrorText("The system tray host could not be queried", nativeError));
        g_clear_error(&nativeError);
        return cached;
    }
    gboolean hasOwner = FALSE;
    g_variant_get(reply, "(b)", &hasOwner);
    g_variant_unref(reply);
    cached = hasOwner
        ? CapabilityStatus::Ready(L"A StatusNotifier tray host is available.")
        : CapabilityStatus::Unavailable(
            L"This desktop session does not provide a StatusNotifier tray host.");
    return cached;
}

CapabilityStatus OpenUriWithLinuxPortal(const wstring& uri) {
    const auto capability = ProbeLinuxPortalInterface("org.freedesktop.portal.OpenURI");
    if (!capability.IsAvailable()) return capability;
    if (uri.empty()) return CapabilityStatus::Failed(L"The URI must not be empty.");
    const string utf8Uri = wstring_to_utf8(uri);
    auto request = RunPortalRequest("org.freedesktop.portal.OpenURI", "OpenURI",
        [&](const string& token) {
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&options, "{sv}", "handle_token",
                g_variant_new_string(token.c_str()));
            return g_variant_new("(ss@a{sv})", "", utf8Uri.c_str(),
                g_variant_builder_end(&options));
        });
    const auto status = RequestStatus(request, L"Opening the URI was cancelled or denied.");
    if (request.results) g_variant_unref(request.results);
    return status;
}

CapabilityStatus OpenPathWithLinuxPortal(const filesystem::path& path, bool revealInFolder) {
    const auto capability = ProbeLinuxPortalInterface("org.freedesktop.portal.OpenURI");
    if (!capability.IsAvailable()) return capability;
    if (path.empty()) return CapabilityStatus::Failed(L"The local path must not be empty.");

    const int descriptor = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return CapabilityStatus::Failed(
            L"The local path could not be opened for the desktop portal.");
    }
    GError* nativeError = nullptr;
    GUnixFDList* descriptors = g_unix_fd_list_new();
    const int descriptorIndex = g_unix_fd_list_append(descriptors, descriptor, &nativeError);
    close(descriptor);
    if (descriptorIndex < 0) {
        auto status = CapabilityStatus::Failed(
            ErrorText("The local file descriptor could not be passed to the desktop portal", nativeError));
        g_clear_error(&nativeError);
        g_object_unref(descriptors);
        return status;
    }

    const char* method = revealInFolder ? "OpenDirectory" : "OpenFile";
    auto request = RunPortalRequest("org.freedesktop.portal.OpenURI", method,
        [&](const string& token) {
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&options, "{sv}", "handle_token",
                g_variant_new_string(token.c_str()));
            return g_variant_new("(sh@a{sv})", "", descriptorIndex,
                g_variant_builder_end(&options));
        }, descriptors);
    g_object_unref(descriptors);
    const auto status = RequestStatus(
        request, L"Opening the local path was cancelled or denied.");
    if (request.results) g_variant_unref(request.results);
    return status;
}

CapabilityStatus NotifyWithLinuxPortal(const wstring& title, const wstring& message) {
    const auto capability = ProbeLinuxPortalInterface("org.freedesktop.portal.Notification");
    if (!capability.IsAvailable()) return capability;
    wstring connectionError;
    auto* connection = Connection(connectionError);
    if (!connection) return CapabilityStatus::Unavailable(std::move(connectionError));

    GVariantBuilder notification;
    g_variant_builder_init(&notification, G_VARIANT_TYPE_VARDICT);
    g_variant_builder_add(&notification, "{sv}", "title",
        g_variant_new_string(wstring_to_utf8(title).c_str()));
    g_variant_builder_add(&notification, "{sv}", "body",
        g_variant_new_string(wstring_to_utf8(message).c_str()));
    const string id = NextToken("notification");
    GError* nativeError = nullptr;
    GVariant* reply = g_dbus_connection_call_sync(connection, PortalBus, PortalPath,
        "org.freedesktop.portal.Notification", "AddNotification",
        g_variant_new("(s@a{sv})", id.c_str(), g_variant_builder_end(&notification)),
        G_VARIANT_TYPE("()"), G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &nativeError);
    if (!reply) {
        auto status = CapabilityStatus::Failed(
            ErrorText("The desktop notification could not be sent", nativeError));
        g_clear_error(&nativeError);
        return status;
    }
    g_variant_unref(reply);
    return CapabilityStatus::Ready();
}

LinuxPortalShortcutResult ConfigureLinuxPortalShortcuts(
    const vector<LinuxPortalShortcutBinding>& bindings,
    function<void(int)> activatedCallback) {
    LinuxPortalShortcutResult output;
    if (bindings.empty()) {
        CloseShortcutSession();
        output.status = CapabilityStatus::Ready();
        return output;
    }
    if (!activatedCallback) {
        output.status = CapabilityStatus::Failed(
            L"A GlobalShortcuts activation callback is required.");
        return output;
    }
    set<string> shortcutIds;
    set<int> hotkeyIds;
    for (const auto& binding : bindings) {
        if (binding.hotkeyId <= 0 || binding.shortcutId.empty()
            || binding.description.empty() || binding.preferredTrigger.empty()
            || !shortcutIds.insert(binding.shortcutId).second
            || !hotkeyIds.insert(binding.hotkeyId).second) {
            output.status = CapabilityStatus::Failed(
                L"Global shortcut bindings must be complete and uniquely identified.");
            return output;
        }
    }
    const auto capability = ProbeLinuxPortalInterface(
        "org.freedesktop.portal.GlobalShortcuts");
    if (!capability.IsAvailable()) {
        output.status = capability;
        return output;
    }

    auto create = RunPortalRequest("org.freedesktop.portal.GlobalShortcuts", "CreateSession",
        [&](const string& token) {
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&options, "{sv}", "handle_token",
                g_variant_new_string(token.c_str()));
            const string sessionToken = NextToken("shortcut_session");
            g_variant_builder_add(&options, "{sv}", "session_handle_token",
                g_variant_new_string(sessionToken.c_str()));
            return g_variant_new("(@a{sv})", g_variant_builder_end(&options));
        });
    output.status = ShortcutRequestStatus(
        create, L"Global shortcut access was cancelled, denied or rejected.");
    if (!output.status.IsAvailable()) {
        if (create.results) g_variant_unref(create.results);
        return output;
    }
    if (!create.results) {
        output.status = CapabilityStatus::Failed(
            L"The GlobalShortcuts portal returned an empty session response.");
        return output;
    }
    // The portal specification intentionally keeps this object path encoded
    // as a string for compatibility with the original implementation.
    GVariant* sessionValue = g_variant_lookup_value(
        create.results, "session_handle", G_VARIANT_TYPE_STRING);
    if (!sessionValue) {
        output.status = CapabilityStatus::Failed(
            L"The GlobalShortcuts portal did not return a session handle.");
        if (create.results) g_variant_unref(create.results);
        return output;
    }
    const string newSession = g_variant_get_string(sessionValue, nullptr);
    g_variant_unref(sessionValue);
    g_variant_unref(create.results);
    if (!g_variant_is_object_path(newSession.c_str())) {
        output.status = CapabilityStatus::Failed(
            L"The GlobalShortcuts portal returned an invalid session handle.");
        return output;
    }

    map<string, int> newShortcutIds;
    map<int, wstring> shortcutDescriptions;
    for (const auto& binding : bindings) {
        newShortcutIds[binding.shortcutId] = binding.hotkeyId;
        shortcutDescriptions[binding.hotkeyId] = binding.description;
    }

    auto bind = RunPortalRequest("org.freedesktop.portal.GlobalShortcuts", "BindShortcuts",
        [&](const string& token) {
            GVariantBuilder shortcuts;
            g_variant_builder_init(&shortcuts, G_VARIANT_TYPE("a(sa{sv})"));
            for (const auto& binding : bindings) {
                GVariantBuilder properties;
                g_variant_builder_init(&properties, G_VARIANT_TYPE_VARDICT);
                g_variant_builder_add(&properties, "{sv}", "description",
                    g_variant_new_string(wstring_to_utf8(binding.description).c_str()));
                g_variant_builder_add(&properties, "{sv}", "preferred_trigger",
                    g_variant_new_string(binding.preferredTrigger.c_str()));
                g_variant_builder_add(&shortcuts, "(s@a{sv})", binding.shortcutId.c_str(),
                    g_variant_builder_end(&properties));
            }
            GVariantBuilder options;
            g_variant_builder_init(&options, G_VARIANT_TYPE_VARDICT);
            g_variant_builder_add(&options, "{sv}", "handle_token",
                g_variant_new_string(token.c_str()));
            return g_variant_new("(o@a(sa{sv})s@a{sv})", newSession.c_str(),
                g_variant_builder_end(&shortcuts), "", g_variant_builder_end(&options));
        });
    output.status = ShortcutRequestStatus(
        bind, L"Global shortcut access was cancelled, denied or rejected.");
    if (!output.status.IsAvailable()) {
        if (bind.results) g_variant_unref(bind.results);
        ClosePortalSession(newSession);
        return output;
    }
    if (!bind.results) {
        output.status = CapabilityStatus::Failed(
            L"The GlobalShortcuts portal returned an empty binding response.");
        ClosePortalSession(newSession);
        return output;
    }

    GVariant* returned = g_variant_lookup_value(
        bind.results, "shortcuts", G_VARIANT_TYPE("a(sa{sv})"));
    if (returned) {
        GVariantIter iterator;
        g_variant_iter_init(&iterator, returned);
        while (GVariant* item = g_variant_iter_next_value(&iterator)) {
            const char* shortcutId = nullptr;
            GVariant* properties = nullptr;
            g_variant_get(item, "(&s@a{sv})", &shortcutId, &properties);
            GVariant* trigger = g_variant_lookup_value(
                properties, "trigger_description", G_VARIANT_TYPE_STRING);
            if (shortcutId && trigger) {
                const auto found = newShortcutIds.find(shortcutId);
                if (found != newShortcutIds.end()) {
                    output.actualTriggers[found->second] = utf8_to_wstring(
                        g_variant_get_string(trigger, nullptr));
                }
            }
            if (trigger) g_variant_unref(trigger);
            g_variant_unref(properties);
            g_variant_unref(item);
        }
        g_variant_unref(returned);
    }
    g_variant_unref(bind.results);

    if (output.actualTriggers.empty()) {
        output.status = CapabilityStatus::PermissionRequired(
            L"The GlobalShortcuts portal did not bind any shortcut.");
        ClosePortalSession(newSession);
        return output;
    }
    wstring diagnostic = L"Portal shortcuts: ";
    bool first = true;
    for (const auto& [hotkeyId, trigger] : output.actualTriggers) {
        if (!first) diagnostic += L"; ";
        first = false;
        const auto description = shortcutDescriptions.find(hotkeyId);
        diagnostic += (description == shortcutDescriptions.end()
            ? L"Shortcut " + to_wstring(hotkeyId)
            : description->second) + L" = " + trigger;
    }
    output.status = CapabilityStatus::Ready(std::move(diagnostic));

    // A failed reconfiguration leaves the previous session alive. Only swap
    // the process-wide session after the portal accepted at least one binding.
    const guint newSignal = g_dbus_connection_signal_subscribe(g_connection, PortalBus,
        "org.freedesktop.portal.GlobalShortcuts", "Activated", PortalPath, nullptr,
        G_DBUS_SIGNAL_FLAGS_NONE, ShortcutActivated, nullptr, nullptr);
    if (newSignal == 0) {
        ClosePortalSession(newSession);
        output.status = CapabilityStatus::Failed(
            L"The GlobalShortcuts activation signal could not be subscribed.");
        output.actualTriggers.clear();
        return output;
    }
    CloseShortcutSession();
    g_shortcutSession = newSession;
    g_shortcutIds = std::move(newShortcutIds);
    g_shortcutCallback = std::move(activatedCallback);
    g_shortcutSignal = newSignal;
    return output;
}

void ShutdownLinuxDesktopPortal() {
    CloseShortcutSession();
    g_probeCache.clear();
    if (g_connection) {
        g_object_unref(g_connection);
        g_connection = nullptr;
    }
}
