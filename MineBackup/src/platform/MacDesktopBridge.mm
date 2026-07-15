#include "MacDesktopBridge.h"

#include "text_to_text.h"

#import <AppKit/AppKit.h>
#import <Carbon/Carbon.h>
#import <CoreFoundation/CoreFoundation.h>
#import <ServiceManagement/ServiceManagement.h>
#import <UserNotifications/UserNotifications.h>

#include <atomic>
#include <mutex>

using namespace std;
namespace fs = std::filesystem;

@interface MBNotificationDelegate : NSObject <UNUserNotificationCenterDelegate>
@end

@implementation MBNotificationDelegate
- (void)userNotificationCenter:(UNUserNotificationCenter*)center
       willPresentNotification:(UNNotification*)notification
         withCompletionHandler:(void (^)(UNNotificationPresentationOptions options))completionHandler {
    (void)center;
    (void)notification;
    completionHandler(UNNotificationPresentationOptionBanner
        | UNNotificationPresentationOptionList
        | UNNotificationPresentationOptionSound);
}
@end

namespace {

NSString* NativeString(const wstring& value) {
    const string utf8 = wstring_to_utf8(value);
    return [[NSString alloc] initWithBytes:utf8.data()
        length:utf8.size() encoding:NSUTF8StringEncoding];
}

NSString* NativeString(const string& value) {
    return [[NSString alloc] initWithBytes:value.data()
        length:value.size() encoding:NSUTF8StringEncoding];
}

NSString* NativePath(const fs::path& value) {
    const string& native = value.native();
    return [[NSString alloc] initWithBytes:native.data()
        length:native.size() encoding:NSUTF8StringEncoding];
}

fs::path FileSystemPath(NSString* value) {
    if (!value) return {};
    const char* native = value.fileSystemRepresentation;
    return native ? fs::path(native) : fs::path();
}

wstring WideString(NSString* value) {
    return value ? utf8_to_wstring(value.UTF8String ?: "") : wstring();
}

bool IsPackagedApplication() {
    NSBundle* bundle = [NSBundle mainBundle];
    return bundle.bundleIdentifier.length > 0
        && [bundle.bundlePath.pathExtension caseInsensitiveCompare:@"app"] == NSOrderedSame;
}

bool IsInApplicationsFolder() {
    NSString* bundlePath = [[NSBundle mainBundle].bundlePath stringByStandardizingPath];
    NSString* userApplications = [NSHomeDirectory()
        stringByAppendingPathComponent:@"Applications"].stringByStandardizingPath;
    return [bundlePath hasPrefix:@"/Applications/"]
        || [bundlePath isEqualToString:@"/Applications"]
        || [bundlePath hasPrefix:[userApplications stringByAppendingString:@"/"]]
        || [bundlePath isEqualToString:userApplications];
}

wstring NativeError(NSError* error, const wchar_t* fallback) {
    if (!error) return fallback;
    const wstring detail = WideString(error.localizedDescription);
    return detail.empty() ? wstring(fallback) : detail;
}

template <typename Function>
auto OnMainThread(Function&& function) -> decltype(function()) {
    using Result = decltype(function());
    if ([NSThread isMainThread]) return function();
    __block Result result{};
    dispatch_sync(dispatch_get_main_queue(), ^{
        @autoreleasepool {
            result = function();
        }
    });
    return result;
}

DesktopPathResult OpenPanel(bool folders) {
    return OnMainThread([folders]() {
        @autoreleasepool {
            [NSApplication sharedApplication];
            NSOpenPanel* panel = [NSOpenPanel openPanel];
            panel.canChooseFiles = folders ? NO : YES;
            panel.canChooseDirectories = folders ? YES : NO;
            panel.allowsMultipleSelection = NO;
            panel.resolvesAliases = YES;
            panel.canCreateDirectories = folders ? YES : NO;
            [NSApp activateIgnoringOtherApps:YES];
            if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
                return DesktopPathResult{CapabilityStatus::Ready(), {}, true};
            }
            return DesktopPathResult{CapabilityStatus::Ready(
                L"The selection was provided by the native macOS Open Panel."),
                FileSystemPath(panel.URL.path), false};
        }
    });
}

enum class NotificationState {
    Unknown,
    Querying,
    NotDetermined,
    Authorized,
    Denied,
    Failed
};

mutex g_notificationMutex;
NotificationState g_notificationState = NotificationState::Unknown;
wstring g_notificationDiagnostic;

id<UNUserNotificationCenterDelegate> g_notificationDelegate = nil;

void SetNotificationState(NotificationState state, wstring diagnostic = {}) {
    lock_guard lock(g_notificationMutex);
    g_notificationState = state;
    g_notificationDiagnostic = std::move(diagnostic);
}

void InstallNotificationDelegate() {
    void (^install)(void) = ^{
        if (!g_notificationDelegate) {
            g_notificationDelegate = [[MBNotificationDelegate alloc] init];
            [UNUserNotificationCenter currentNotificationCenter].delegate = g_notificationDelegate;
        }
    };
    if ([NSThread isMainThread]) install();
    else dispatch_async(dispatch_get_main_queue(), install);
}

void ApplyNotificationSettings(UNNotificationSettings* settings) {
    switch (settings.authorizationStatus) {
    case UNAuthorizationStatusAuthorized:
    case UNAuthorizationStatusProvisional:
        SetNotificationState(NotificationState::Authorized,
            L"Native macOS notifications are authorized.");
        break;
    case UNAuthorizationStatusDenied:
        SetNotificationState(NotificationState::Denied,
            L"Notifications are disabled for MineBackup in System Settings.");
        break;
    case UNAuthorizationStatusNotDetermined:
        SetNotificationState(NotificationState::NotDetermined,
            L"macOS will request notification permission when MineBackup first needs it.");
        break;
    default:
        SetNotificationState(NotificationState::Failed,
            L"macOS returned an unknown notification authorization state.");
        break;
    }
}

void RefreshNotificationSettings() {
    {
        lock_guard lock(g_notificationMutex);
        if (g_notificationState != NotificationState::Unknown) return;
        g_notificationState = NotificationState::Querying;
        g_notificationDiagnostic = L"Checking native notification permission.";
    }
    InstallNotificationDelegate();
    [[UNUserNotificationCenter currentNotificationCenter]
        getNotificationSettingsWithCompletionHandler:^(UNNotificationSettings* settings) {
            ApplyNotificationSettings(settings);
        }];
}

CapabilityStatus CurrentNotificationStatus() {
    lock_guard lock(g_notificationMutex);
    switch (g_notificationState) {
    case NotificationState::Authorized:
        return CapabilityStatus::Ready(g_notificationDiagnostic);
    case NotificationState::Denied:
    case NotificationState::NotDetermined:
    case NotificationState::Querying:
        return CapabilityStatus::PermissionRequired(g_notificationDiagnostic);
    case NotificationState::Failed:
        return CapabilityStatus::Failed(g_notificationDiagnostic);
    case NotificationState::Unknown:
        return CapabilityStatus::PermissionRequired(
            L"Checking native notification permission.");
    }
    return CapabilityStatus::Failed(L"Unknown notification capability state.");
}

void QueueNotification(const wstring& title, const wstring& message) {
    UNMutableNotificationContent* content = [[UNMutableNotificationContent alloc] init];
    content.title = NativeString(title);
    content.body = NativeString(message);
    content.sound = [UNNotificationSound defaultSound];
    content.threadIdentifier = @"MineBackup";
    NSString* identifier = [@"MineBackup-" stringByAppendingString:[NSUUID UUID].UUIDString];
    UNNotificationRequest* request = [UNNotificationRequest
        requestWithIdentifier:identifier content:content trigger:nil];
    [[UNUserNotificationCenter currentNotificationCenter]
        addNotificationRequest:request withCompletionHandler:^(NSError* error) {
            if (error) {
                SetNotificationState(NotificationState::Failed,
                    NativeError(error, L"macOS rejected the notification request."));
            }
        }];
}

CapabilityStatus LoginItemStatus() {
    if (!IsPackagedApplication()) {
        return CapabilityStatus::Unavailable(
            L"Login-item integration requires a packaged MineBackup.app bundle.");
    }
    if (!IsInApplicationsFolder()) {
        return CapabilityStatus::PermissionRequired(
            L"Move MineBackup.app to /Applications or ~/Applications before enabling launch at login.");
    }
    switch ([SMAppService mainAppService].status) {
    case SMAppServiceStatusEnabled:
        return CapabilityStatus::Ready(L"MineBackup is enabled in Login Items.");
    case SMAppServiceStatusNotRegistered:
        return CapabilityStatus::Ready(L"MineBackup can be registered in Login Items.");
    case SMAppServiceStatusRequiresApproval:
        return CapabilityStatus::PermissionRequired(
            L"MineBackup is registered but requires approval in System Settings > General > Login Items.");
    case SMAppServiceStatusNotFound:
        return CapabilityStatus::Failed(
            L"ServiceManagement could not find the main application login item.");
    }
    return CapabilityStatus::Failed(L"ServiceManagement returned an unknown login-item state.");
}

} // namespace

DesktopPathResult MacSelectFile() {
    return OpenPanel(false);
}

DesktopPathResult MacSelectFolder() {
    return OpenPanel(true);
}

DesktopPathResult MacSelectSaveFile(const wstring& defaultFileName, const wstring& filter) {
    (void)filter;
    return OnMainThread([&defaultFileName]() {
        @autoreleasepool {
            [NSApplication sharedApplication];
            NSSavePanel* panel = [NSSavePanel savePanel];
            panel.canCreateDirectories = YES;
            if (!defaultFileName.empty()) panel.nameFieldStringValue = NativeString(defaultFileName);
            [NSApp activateIgnoringOtherApps:YES];
            if ([panel runModal] != NSModalResponseOK || panel.URL == nil) {
                return DesktopPathResult{CapabilityStatus::Ready(), {}, true};
            }
            return DesktopPathResult{CapabilityStatus::Ready(
                L"The destination was provided by the native macOS Save Panel."),
                FileSystemPath(panel.URL.path), false};
        }
    });
}

CapabilityStatus MacOpenUri(const wstring& uri) {
    if (uri.empty()) return CapabilityStatus::Failed(L"The URI must not be empty.");
    return OnMainThread([&uri]() {
        NSURL* url = [NSURL URLWithString:NativeString(uri)];
        if (!url || url.scheme.length == 0) {
            return CapabilityStatus::Failed(L"The URI is not valid.");
        }
        return [[NSWorkspace sharedWorkspace] openURL:url]
            ? CapabilityStatus::Ready()
            : CapabilityStatus::Failed(L"NSWorkspace could not open the URI.");
    });
}

CapabilityStatus MacOpenFolder(const fs::path& folder) {
    if (folder.empty()) return CapabilityStatus::Failed(L"The folder path must not be empty.");
    return OnMainThread([&folder]() {
        error_code error;
        if (!fs::is_directory(folder, error)) {
            return CapabilityStatus::Failed(error
                ? L"The folder could not be inspected."
                : L"The folder does not exist.");
        }
        NSURL* url = [NSURL fileURLWithPath:NativePath(folder) isDirectory:YES];
        return [[NSWorkspace sharedWorkspace] openURL:url]
            ? CapabilityStatus::Ready()
            : CapabilityStatus::Failed(L"NSWorkspace could not open the folder.");
    });
}

CapabilityStatus MacRevealInFolder(const fs::path& folder, const fs::path& item) {
    if (folder.empty()) return CapabilityStatus::Failed(L"The folder path must not be empty.");
    return OnMainThread([&folder, &item]() {
        const fs::path target = item.empty() ? folder : item;
        error_code error;
        if (!fs::exists(target, error)) {
            return CapabilityStatus::Failed(error
                ? L"The item could not be inspected."
                : L"The item to reveal does not exist.");
        }
        NSURL* url = [NSURL fileURLWithPath:NativePath(target)];
        [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:@[url]];
        return CapabilityStatus::Ready();
    });
}

CapabilityStatus MacNotificationCapability() {
    if (!IsPackagedApplication()) {
        return CapabilityStatus::Unavailable(
            L"Native notifications require a packaged MineBackup.app bundle.");
    }
    RefreshNotificationSettings();
    return CurrentNotificationStatus();
}

CapabilityStatus MacNotify(const wstring& title, const wstring& message) {
    if (title.empty() || message.empty()) {
        return CapabilityStatus::Failed(L"Notification title and message must not be empty.");
    }
    const auto capability = MacNotificationCapability();
    NotificationState state;
    {
        lock_guard lock(g_notificationMutex);
        state = g_notificationState;
    }
    if (state == NotificationState::Authorized) {
        QueueNotification(title, message);
        return CapabilityStatus::Ready(L"The native notification was queued.");
    }
    if (state == NotificationState::NotDetermined) {
        InstallNotificationDelegate();
        const wstring notificationTitle = title;
        const wstring notificationMessage = message;
        [[UNUserNotificationCenter currentNotificationCenter]
            requestAuthorizationWithOptions:(UNAuthorizationOptionAlert | UNAuthorizationOptionSound)
            completionHandler:^(BOOL granted, NSError* error) {
                if (error) {
                    SetNotificationState(NotificationState::Failed,
                        NativeError(error, L"macOS notification authorization failed."));
                }
                else if (granted) {
                    SetNotificationState(NotificationState::Authorized,
                        L"Native macOS notifications are authorized.");
                    QueueNotification(notificationTitle, notificationMessage);
                }
                else {
                    SetNotificationState(NotificationState::Denied,
                        L"Notification permission was denied in System Settings.");
                }
            }];
        return CapabilityStatus::PermissionRequired(
            L"macOS is requesting permission to show notifications.");
    }
    return capability;
}

CapabilityStatus MacAutostartCapability() {
    return OnMainThread([]() { return LoginItemStatus(); });
}

CapabilityStatus MacSetAutostart(bool enabled) {
    return OnMainThread([enabled]() {
        if (!IsPackagedApplication()) {
            return CapabilityStatus::Unavailable(
                L"Login-item integration requires a packaged MineBackup.app bundle.");
        }
        if (enabled && !IsInApplicationsFolder()) {
            return CapabilityStatus::PermissionRequired(
                L"Move MineBackup.app to /Applications or ~/Applications before enabling launch at login.");
        }
        SMAppService* service = [SMAppService mainAppService];
        if (enabled && service.status == SMAppServiceStatusEnabled) return LoginItemStatus();
        if (!enabled && service.status == SMAppServiceStatusNotRegistered) {
            return CapabilityStatus::Ready(L"MineBackup is not registered in Login Items.");
        }
        NSError* error = nil;
        const BOOL changed = enabled
            ? [service registerAndReturnError:&error]
            : [service unregisterAndReturnError:&error];
        if (!changed) {
            if (service.status == SMAppServiceStatusRequiresApproval) {
                return CapabilityStatus::PermissionRequired(
                    L"Approve MineBackup in System Settings > General > Login Items.");
            }
            return CapabilityStatus::Failed(NativeError(error,
                enabled ? L"MineBackup could not be registered as a login item."
                        : L"MineBackup could not be removed from Login Items."));
        }
        return LoginItemStatus();
    });
}

CapabilityStatus MacOpenAutostartSettings() {
    return OnMainThread([]() {
        if (!IsInApplicationsFolder()) {
            NSURL* applications = [NSURL fileURLWithPath:@"/Applications" isDirectory:YES];
            if (![[NSWorkspace sharedWorkspace] openURL:applications]) {
                return CapabilityStatus::Failed(L"The Applications folder could not be opened.");
            }
            return CapabilityStatus::Ready(
                L"Move MineBackup.app into the Applications folder, then enable launch at login.");
        }
        [SMAppService openSystemSettingsLoginItems];
        return CapabilityStatus::Ready(L"Opened Login Items in System Settings.");
    });
}

CapabilityStatus MacActivateApplication() {
    return OnMainThread([]() {
        [NSApplication sharedApplication];
        [NSApp activateIgnoringOtherApps:YES];
        return CapabilityStatus::Ready();
    });
}

namespace {

atomic_bool g_launchObservationInstalled{false};
atomic_bool g_launchedAsLoginItem{false};
id g_launchObservation = nil;

void InspectLoginItemLaunchEvent() {
    NSAppleEventDescriptor* event =
        [[NSAppleEventManager sharedAppleEventManager] currentAppleEvent];
    NSAppleEventDescriptor* attribute =
        [event attributeDescriptorForKeyword:keyAELaunchedAsLogInItem];
    if (attribute && attribute.booleanValue) {
        g_launchedAsLoginItem.store(true, memory_order_release);
    }
}

} // namespace

void MacBeginLaunchObservation() {
    if (g_launchObservationInstalled.exchange(true, memory_order_acq_rel)) return;
    void (^install)(void) = ^{
        g_launchObservation = [[NSNotificationCenter defaultCenter]
            addObserverForName:NSApplicationWillFinishLaunchingNotification
                        object:nil
                         queue:nil
                    usingBlock:^(NSNotification*) {
                        InspectLoginItemLaunchEvent();
                    }];
    };
    if ([NSThread isMainThread]) install();
    else dispatch_sync(dispatch_get_main_queue(), install);
}

bool MacWasLaunchedAsLoginItem() noexcept {
    return g_launchedAsLoginItem.load(memory_order_acquire);
}

namespace {

bool ApplicationFinishedLaunching() {
    return [[NSRunningApplication currentApplication] isFinishedLaunching];
}

CFStringRef CreateNativeString(const string& value) {
    return CFStringCreateWithBytes(kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(value.data()), static_cast<CFIndex>(value.size()),
        kCFStringEncodingUTF8, false);
}

void ShowPrelaunchAlert(const string& title, const string& message, int iconType) {
    CFStringRef titleText = CreateNativeString(title);
    CFStringRef messageText = CreateNativeString(message);
    const CFOptionFlags level = iconType == 2 ? kCFUserNotificationStopAlertLevel
        : iconType == 1 ? kCFUserNotificationCautionAlertLevel
                        : kCFUserNotificationNoteAlertLevel;
    CFOptionFlags response = 0;
    CFUserNotificationDisplayAlert(0, level, nullptr, nullptr, nullptr,
        titleText, messageText, CFSTR("OK"), nullptr, nullptr, &response);
    if (titleText) CFRelease(titleText);
    if (messageText) CFRelease(messageText);
}

bool ConfirmPrelaunchAlert(const string& title, const string& message) {
    CFStringRef titleText = CreateNativeString(title);
    CFStringRef messageText = CreateNativeString(message);
    CFOptionFlags response = 1;
    const SInt32 status = CFUserNotificationDisplayAlert(0,
        kCFUserNotificationCautionAlertLevel, nullptr, nullptr, nullptr,
        titleText, messageText, CFSTR("Import"), CFSTR("Not now"), nullptr,
        &response);
    if (titleText) CFRelease(titleText);
    if (messageText) CFRelease(messageText);
    return status == 0 && response == kCFUserNotificationDefaultResponse;
}

} // namespace

void MacShowAlert(const string& title, const string& message, int iconType) {
    if (!ApplicationFinishedLaunching()) {
        ShowPrelaunchAlert(title, message, iconType);
        return;
    }
    OnMainThread([&title, &message, iconType]() {
        [NSApplication sharedApplication];
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = NativeString(title);
        alert.informativeText = NativeString(message);
        alert.alertStyle = iconType == 2 ? NSAlertStyleCritical
            : iconType == 1 ? NSAlertStyleWarning : NSAlertStyleInformational;
        [alert addButtonWithTitle:@"OK"];
        [NSApp activateIgnoringOtherApps:YES];
        [alert runModal];
        return true;
    });
}

bool MacConfirmAlert(const string& title, const string& message) {
    if (!ApplicationFinishedLaunching()) {
        return ConfirmPrelaunchAlert(title, message);
    }
    return OnMainThread([&title, &message]() {
        [NSApplication sharedApplication];
        NSAlert* alert = [[NSAlert alloc] init];
        alert.messageText = NativeString(title);
        alert.informativeText = NativeString(message);
        alert.alertStyle = NSAlertStyleWarning;
        [alert addButtonWithTitle:@"Import"];
        [alert addButtonWithTitle:@"Not now"];
        [NSApp activateIgnoringOtherApps:YES];
        return [alert runModal] == NSAlertFirstButtonReturn;
    });
}
