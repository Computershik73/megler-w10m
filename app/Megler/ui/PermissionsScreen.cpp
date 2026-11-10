// An app's permissions, as Android 11 and later's App info > Permissions has them (player/PermissionHost.h): each dangerous
// group its manifest asks for, in the list of what the app may have -- Allowed, Ask every time (where there is any), Not
// allowed -- and a tap on one gives its choices: Allow, Ask every time, Don't allow. Every group may be refused, those Megler
// has nothing behind too, and the location, the camera and the microphone by Megler's own no over Windows' consent; a yes
// of Megler's leaves them to Windows, whose answer is said where it is known (in the app's own package: Megler's process is
// another package, whose consent is not the app's) with the way to Windows' privacy settings. Then Google Play services' own
// use of the location (one answer for every app), and the other permissions the manifest asks for, read only, as Android's
// "All permissions" lists what it grants with the install.
//
// Reached from the app's page in Megler (Permissions), from its App Settings, and in the app's own package from its
// Settings (the player's menu), where it opens in place of them and Back goes back to them.
#include "pch.h"

#include "library/Library.h"
#include "player/PermissionHost.h"
#include "shell/AppPackage.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Theme.h"

#include "megler.h"

#include <cstring>

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {

/* Android's words for the permissions it grants with the install (the platform's permlab strings, and its Special app access
   for those it grants there), by the permission's name after "android.permission.". */
const struct {
    const char* name;
    const char* label;
    bool special;
} kNormal[] = {
    {"INTERNET", "Have full network access", false},
    {"ACCESS_NETWORK_STATE", "View network connections", false},
    {"ACCESS_WIFI_STATE", "View Wi-Fi connections", false},
    {"CHANGE_WIFI_STATE", "Connect and disconnect from Wi-Fi", false},
    {"CHANGE_NETWORK_STATE", "Change network connectivity", false},
    {"CHANGE_WIFI_MULTICAST_STATE", "Allow Wi-Fi Multicast reception", false},
    {"BLUETOOTH", "Pair with Bluetooth devices", false},
    {"BLUETOOTH_ADMIN", "Access Bluetooth settings", false},
    {"NFC", "Control Near Field Communication", false},
    {"VIBRATE", "Control vibration", false},
    {"WAKE_LOCK", "Prevent phone from sleeping", false},
    {"RECEIVE_BOOT_COMPLETED", "Run at startup", false},
    {"FOREGROUND_SERVICE", "Run foreground service", false},
    {"FOREGROUND_SERVICE_DATA_SYNC", "Run foreground service with the type \"dataSync\"", false},
    {"FOREGROUND_SERVICE_MEDIA_PLAYBACK", "Run foreground service with the type \"mediaPlayback\"", false},
    {"FOREGROUND_SERVICE_LOCATION", "Run foreground service with the type \"location\"", false},
    {"FOREGROUND_SERVICE_MICROPHONE", "Run foreground service with the type \"microphone\"", false},
    {"FOREGROUND_SERVICE_CAMERA", "Run foreground service with the type \"camera\"", false},
    {"FOREGROUND_SERVICE_PHONE_CALL", "Run foreground service with the type \"phoneCall\"", false},
    {"FOREGROUND_SERVICE_REMOTE_MESSAGING", "Run foreground service with the type \"remoteMessaging\"", false},
    {"FOREGROUND_SERVICE_SPECIAL_USE", "Run foreground service with the type \"specialUse\"", false},
    {"USE_BIOMETRIC", "Use biometric hardware", false},
    {"USE_FINGERPRINT", "Use fingerprint hardware", false},
    {"MODIFY_AUDIO_SETTINGS", "Change your audio settings", false},
    {"GET_PACKAGE_SIZE", "Measure app storage space", false},
    {"EXPAND_STATUS_BAR", "Expand/collapse status bar", false},
    {"SET_WALLPAPER", "Set wallpaper", false},
    {"SET_ALARM", "Set an alarm", false},
    {"KILL_BACKGROUND_PROCESSES", "Close other apps", false},
    {"READ_SYNC_SETTINGS", "Read sync settings", false},
    {"WRITE_SYNC_SETTINGS", "Toggle sync on and off", false},
    {"READ_SYNC_STATS", "Read sync statistics", false},
    {"AUTHENTICATE_ACCOUNTS", "Create accounts and set passwords", false},
    {"MANAGE_ACCOUNTS", "Add or remove accounts", false},
    {"USE_CREDENTIALS", "Use accounts on the device", false},
    {"DISABLE_KEYGUARD", "Disable your screen lock", false},
    {"REORDER_TASKS", "Reorder running apps", false},
    {"TRANSMIT_IR", "Transmit infrared", false},
    {"FLASHLIGHT", "Control flashlight", false},
    {"QUERY_ALL_PACKAGES", "Query all packages", false},
    {"REQUEST_DELETE_PACKAGES", "Request deleting packages", false},
    {"REQUEST_IGNORE_BATTERY_OPTIMIZATIONS", "Ask to ignore battery optimizations", false},
    {"USE_FULL_SCREEN_INTENT", "Display full screen notifications", false},
    {"ACCESS_NOTIFICATION_POLICY", "Access Do Not Disturb", false},
    {"READ_PROFILE", "Read your own contact card", false},
    {"BROADCAST_STICKY", "Send sticky broadcast", false},
    {"SYSTEM_ALERT_WINDOW", "Display over other apps", true},
    {"REQUEST_INSTALL_PACKAGES", "Install unknown apps", true},
    {"MANAGE_EXTERNAL_STORAGE", "All files access", true},
    {"WRITE_SETTINGS", "Modify system settings", true},
    {"SCHEDULE_EXACT_ALARM", "Alarms & reminders", true},
    {"USE_EXACT_ALARM", "Alarms & reminders", true},
    {"PACKAGE_USAGE_STATS", "Usage access", true},
    {"MANAGE_MEDIA", "Media management apps", true},
    {"BIND_NOTIFICATION_LISTENER_SERVICE", "Notification access", true},
    {"BIND_ACCESSIBILITY_SERVICE", "Accessibility", true},
    {"BIND_DEVICE_ADMIN", "Device admin apps", true},
};

/* Others' permissions that apps often ask for, by their whole name. */
const struct {
    const char* name;
    const char* label;
} kOthers[] = {
    {"com.google.android.c2dm.permission.RECEIVE", "Receive data from Internet"},
    {"com.google.android.gms.permission.AD_ID", "Advertising ID"},
    {"com.google.android.finsky.permission.BIND_GET_INSTALL_REFERRER_SERVICE", "Play Install Referrer API"},
    {"com.android.vending.BILLING", "Google Play billing service"},
    {"com.android.vending.CHECK_LICENSE", "Google Play license check"},
    {"com.google.android.providers.gsf.permission.READ_GSERVICES", "Read Google service configuration"},
    {"com.android.launcher.permission.INSTALL_SHORTCUT", "Install shortcuts"},
    {"com.android.launcher.permission.UNINSTALL_SHORTCUT", "Uninstall shortcuts"},
};

/// The app's entry in the library, by its Android package.
std::optional<ApkEntry> entryOf(const std::string& package) {
    for (const auto& e : Library::instance().entries())
        if (e.report.package == package) return e;
    return std::nullopt;
}

/// Whether the app has its dangerous permissions with its install, as megler.Permissions decides it: an app for Android 5.1
/// or older (or of no target), and Google's own packages.
bool withInstall(const ApkReport& r) {
    return r.targetSdk < 23 || r.package == "com.google.android.gms" || r.package == "com.android.vending" ||
           r.package == "com.google.android.gsf";
}

enum class List { allowed, everyTime, notAllowed };

/// One group as things stand: which list it is in, the line under its title, and which choice is marked.
struct Standing {
    List list = List::allowed;
    std::string said;
    int chosen = -1; // 0 Allow, 1 Ask every time, 2 Don't allow (an app with its permissions with its install: 0 Allow, 1 Don't allow)
};

/// What Windows says of a group it decides, where it can be known: in the app's own package. `known` false in Megler's.
struct WindowsWord {
    bool known = false;
    int state = MEGLER_PERMISSION_UNDECIDED;
    std::string said;
};

Standing standing(const permissionHost::Group& g, const ApkReport& r, const std::string& package, bool own, const WindowsWord& w) {
    using namespace permissionHost;
    Standing s;
    const int kept = permissionHost::kept(package, g.name);
    const bool refused = kept == MEGLER_PERMISSION_DENIED || kept == MEGLER_PERMISSION_DENIED_FIXED;
    if (withInstall(r)) {
        s.list = refused ? List::notAllowed : List::allowed;
        s.chosen = refused ? 1 : 0;
        s.said = refused ? "Not allowed" : "Allowed with its install";
        if (!refused && g.decider == Decider::windows) s.said += w.known ? "; " + w.said : std::string("; Windows decides, in its privacy settings");
        return s;
    }
    if (kept == EVERY_TIME) {
        s.list = List::everyTime;
        s.chosen = 1;
        s.said = "Megler asks each time the app starts and wants it";
        if (own) {
            const int run = thisRun(package, g.name);
            if (run == MEGLER_PERMISSION_GRANTED) s.said += ": allowed this time";
            else if (run == MEGLER_PERMISSION_DENIED || run == MEGLER_PERMISSION_DENIED_FIXED) s.said += ": not allowed this time";
        }
        return s;
    }
    if (refused) {
        s.list = List::notAllowed;
        s.chosen = 2;
        s.said = kept == MEGLER_PERMISSION_DENIED ? "Not allowed; the app may ask again" : "Not allowed; the app isn't asked";
        if (g.decider == Decider::windows) s.said += ", whatever Windows says";
        return s;
    }
    s.chosen = 0;
    switch (g.decider) {
    case Decider::windows:
        if (!w.known) {
            s.said = "Windows decides, in its privacy settings: known in the app's own Settings";
        } else {
            s.list = w.state == MEGLER_PERMISSION_GRANTED ? List::allowed : List::notAllowed;
            s.said = w.said;
        }
        break;
    case Decider::none: s.said = "Allowed"; break;
    case Decider::megler:
        if (kept == MEGLER_PERMISSION_GRANTED) {
            s.said = "Allowed";
        } else if (const int elsewhere = g.name == "location_accuracy" ? locationAccuracyElsewhere() : MEGLER_PERMISSION_UNDECIDED;
                   elsewhere != MEGLER_PERMISSION_UNDECIDED) {
            // Google's setting, one for every app, as another app's answer left it: the app goes by it (megler.LocationAccuracy)
            // and is not asked.
            const bool on = elsewhere == MEGLER_PERMISSION_GRANTED;
            s.list = on ? List::allowed : List::notAllowed;
            s.chosen = on ? 0 : 2;
            s.said = std::string(on ? "Allowed" : "Not allowed") + ", as said in another app: Google's setting is one for every app";
        } else {
            s.list = List::notAllowed;
            s.chosen = -1;
            s.said = "Not asked yet: asked the first time the app wants it";
        }
        break;
    }
    return s;
}

/// "Display over other apps (special app access)" for android.permission.SYSTEM_ALERT_WINDOW; the name for one not known.
std::string labelOf(const std::string& permission, const std::string& package) {
    const std::string android = "android.permission.";
    if (permission.compare(0, android.size(), android) == 0) {
        const std::string name = permission.substr(android.size());
        for (const auto& n : kNormal)
            if (name == n.name) return std::string(n.label) + (n.special ? " (special app access)" : "");
        return name;
    }
    for (const auto& o : kOthers)
        if (permission == o.name) return o.label;
    if (!package.empty() && permission.compare(0, package.size() + 1, package + ".") == 0) return "The app's own: " + permission.substr(package.size() + 1);
    return permission;
}

class PermissionsBody final : public SettingsView, public std::enable_shared_from_this<PermissionsBody> {
public:
    PermissionsBody(AppShell& shell, SettingsScope scope) : shell_(shell), scope_(std::move(scope)) {}

    UIElement view() override {
        if (!host_) {
            host_ = ContentControl();
            host_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            rebuild();
        }
        return host_;
    }

    void refresh() override {
        // In the app's own package what Windows says is read again every two seconds: the user may come back from its
        // privacy settings with another answer.
        if (!host_ || !scope_.own || ++ticks_ % 8 != 0) return;
        if (windowsWords() != windowsSeen_) rebuild();
    }

private:
    /// What Windows says of each of the groups it decides that the app asks for, in one line (to see a change).
    std::string windowsWords() {
        std::string all;
        if (!scope_.own) return all;
        const auto entry = entryOf(scope_.package);
        if (!entry) return all;
        for (const auto& g : permissionHost::groupsOf(entry->report))
            if (g.decider == permissionHost::Decider::windows) all += g.name + "=" + permissionHost::windowsSaid(g.name) + ";";
        return all;
    }

    WindowsWord windowsOf(const permissionHost::Group& g) {
        WindowsWord w;
        if (!scope_.own || g.decider != permissionHost::Decider::windows) return w;
        w.known = true;
        w.state = permissionHost::windows(g.name);
        w.said = permissionHost::windowsSaid(g.name);
        return w;
    }

    std::string label() const { return scope_.label.empty() ? scope_.package : scope_.label; }

    UIElement groupRow(const permissionHost::Group& g, const Standing& s) {
        return button(listItem(g.title, s.said, g.icon), [this, g] { change(g); }, ButtonKind::plain, g.title);
    }

    /// A group's choices: Allow, Ask every time, Don't allow (for an app with its permissions with its install: Allow, Don't
    /// allow), and for one Windows decides, what Windows says and its privacy settings.
    void change(const permissionHost::Group& g) {
        using namespace permissionHost;
        const auto entry = entryOf(scope_.package);
        if (!entry) return;
        const ApkReport& r = entry->report;
        const WindowsWord w = windowsOf(g);
        const Standing s = standing(g, r, scope_.package, scope_.own, w);
        const bool install = withInstall(r);
        AppShell::DialogSpec spec;
        spec.title = g.title + " permission";
        spec.message = explanationOf(g.name);
        if (g.decider == Decider::windows)
            spec.message += std::string(" What Windows says: ") + (w.known ? w.said : "its privacy settings decide it (known in the app's own Settings)") +
                            ". Allow leaves it to Windows; Don't allow refuses it whatever Windows says.";
        if (install)
            spec.message += " This app was made for an older version of Android, or is Google's own: it has its permissions with its install, "
                            "and refusing one may make it misbehave.";
        spec.message += " It applies when the app is next in front.";
        const std::string allowLine = g.decider == Decider::windows ? "As Windows allows it" : g.decider == Decider::none ? "Nothing is asked" : "The app has it";
        spec.choices.push_back({"Allow", allowLine});
        if (!install) spec.choices.push_back({"Ask every time", "Megler asks each time the app starts and wants it"});
        spec.choices.push_back({"Don't allow", "The app is told no and isn't asked"});
        spec.chosen = s.chosen;
        if (g.decider == Decider::windows) spec.buttons.push_back("Windows' Settings");
        spec.buttons.push_back("Cancel");
        const size_t buttons = spec.buttons.size();
        const std::wstring page = windowsSettingsPage(g.name);
        const std::weak_ptr<PermissionsBody> weak = weak_from_this();
        const std::string package = scope_.package, name = g.name;
        const Decider decider = g.decider;
        shell_.dialog(std::move(spec), [weak, package, name, decider, install, buttons, page](int button, std::string) {
            auto self = weak.lock();
            if (!self || button < 0) return;
            if (size_t(button) < buttons) {
                if (decider == Decider::windows && button == 0) winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(page));
                return;
            }
            int choice = button - int(buttons);
            if (install && choice == 1) choice = 2;
            // Allow: Megler's yes where it asks itself; elsewhere nothing kept, the group decided as by default again.
            const int state = choice == 0 ? (decider == Decider::megler ? MEGLER_PERMISSION_GRANTED : MEGLER_PERMISSION_UNDECIDED)
                              : choice == 1 ? EVERY_TIME
                                            : MEGLER_PERMISSION_DENIED_FIXED;
            keep(package, name, state);
            shellLog::write("permissions: " + package + "'s " + name + " set in its App Permissions: " +
                            (choice == 0 ? "allowed" : choice == 1 ? "ask every time" : "not allowed"));
            self->rebuild();
        });
    }

    /// Google Play services' own use of the location, one answer for every app (permissionHost::gmsLocation).
    void changeGmsLocation() {
        const int state = permissionHost::gmsLocation();
        AppShell::DialogSpec spec;
        spec.title = "Google Play Services' Location";
        spec.message = "Location accuracy, personal safety, place suggestions: Google Play services' own features follow where the phone is, "
                       "in every app Windows lets have the location, apart from what the app itself asks for. One answer for every app; "
                       "it applies when an app next starts.";
        spec.choices = {{"Allow", "Their requests are made with a tag of their own, which Google Play services takes"},
                        {"Don't allow", "Their requests are refused, as Google Play services refuses them without one"},
                        {"Ask", "Megler asks the next time they want it"}};
        spec.chosen = state == MEGLER_PERMISSION_GRANTED ? 0 : state == MEGLER_PERMISSION_DENIED_FIXED ? 1 : 2;
        spec.buttons = {"Cancel"};
        const std::weak_ptr<PermissionsBody> weak = weak_from_this();
        shell_.dialog(std::move(spec), [weak](int button, std::string) {
            auto self = weak.lock();
            if (!self || button < 1) return;
            const int choice = button - 1;
            permissionHost::keepGmsLocation(choice == 0 ? MEGLER_PERMISSION_GRANTED : choice == 1 ? MEGLER_PERMISSION_DENIED_FIXED : MEGLER_PERMISSION_UNDECIDED);
            self->rebuild();
        });
    }

    void rebuild() {
        if (!host_) return;
        const auto& p = palette();
        auto column = vstack(0);
        column.Padding(ThicknessHelper::FromLengths(16, 0, 16, 24));
        const auto entry = entryOf(scope_.package);
        if (!entry || !entry->report.permissionsKnown) {
            add(column, listSection("Permissions", {listItem("Not Known Yet", "Megler has not read which permissions the app asks for: the Library "
                                                                              "reads it again once Megler is updated")}));
            host_.Content(column);
            return;
        }
        const ApkReport& r = entry->report;
        windowsSeen_ = windowsWords();

        std::string about = "What " + label() + " may have of what Android asks you about. A tap on one changes it; it applies when the app is next in front.";
        if (withInstall(r))
            about += " This app was made for an older version of Android, or is Google's own: it has its permissions with its install, and "
                     "refusing one may make it misbehave.";
        auto intro = text(about, font::bodyMedium, weightNormal(), p.onSurfaceVariant);
        intro.TextWrapping(TextWrapping::Wrap);
        intro.Margin(ThicknessHelper::FromLengths(0, 8, 0, 0));
        add(column, intro);

        std::vector<UIElement> allowed, everyTime, notAllowed;
        for (const auto& g : permissionHost::groupsOf(r)) {
            const Standing s = standing(g, r, scope_.package, scope_.own, windowsOf(g));
            (s.list == List::allowed ? allowed : s.list == List::everyTime ? everyTime : notAllowed).push_back(groupRow(g, s));
        }
        if (allowed.empty() && everyTime.empty() && notAllowed.empty()) {
            add(column, listSection("Permissions", {listItem("None", "The app asks for no permission Android asks the user about")}));
        } else {
            if (allowed.empty()) allowed.push_back(listItem("No permissions allowed", ""));
            add(column, listSection("Allowed", allowed));
            if (!everyTime.empty()) add(column, listSection("Ask every time", everyTime));
            if (notAllowed.empty()) notAllowed.push_back(listItem("No permissions denied", ""));
            add(column, listSection("Not allowed", notAllowed));
        }

        // Google Play services in the app's process: a Java app's.
        if (r.engine == "java") {
            const int gms = permissionHost::gmsLocation();
            const std::string said = gms == MEGLER_PERMISSION_GRANTED      ? "Allowed, in every app"
                                     : gms == MEGLER_PERMISSION_DENIED_FIXED ? "Not allowed, in every app"
                                                                             : "Not asked yet: asked the first time they want it";
            add(column, listSection("Google Play Services", {button(listItem("Their Own Use of the Location", said, glyphs::place), [this] { changeGmsLocation(); },
                                                                    ButtonKind::plain, "Their Own Use of the Location")},
                                    "Google Play services run in the app's process here, and their own features (location accuracy, personal "
                                    "safety, place suggestions) ask for the location as Google Play services. One answer for every app; Megler's "
                                    "Settings change it too."));
        }

        // Every other permission the manifest asks for: granted with the install, as Android grants them.
        std::vector<UIElement> others;
        for (const auto& permission : r.permissions) {
            if (!r.asks(permission) || !permissionHost::groupOfPermission(permission).empty()) continue;
            others.push_back(listItem(labelOf(permission, r.package), permission));
        }
        if (!others.empty())
            add(column, listSection("All Permissions", others,
                                    "Granted with the install, as Android grants them, and not asked about; the special ones too, whose screens of "
                                    "Android's settings Megler answers."));

        auto footer = text("Its own package declares " + appPackage::describeCapabilities(r.capabilities()) +
                               " to Windows, and nothing else. What Megler says of a permission is the app's to hear; what Windows refuses, "
                               "Windows refuses whatever is said here.",
                           font::bodySmall, weightNormal(), p.onSurfaceVariant);
        footer.TextWrapping(TextWrapping::Wrap);
        footer.Margin(ThicknessHelper::FromLengths(0, 16, 0, 0));
        add(column, footer);
        host_.Content(column);
    }

    AppShell& shell_;
    SettingsScope scope_;
    ContentControl host_{nullptr};
    std::string windowsSeen_;
    int ticks_ = 0;
};

class PermissionsScreen final : public Page {
public:
    PermissionsScreen(AppShell& shell, SettingsScope scope) : Page(shell), body_(makePermissionsView(shell, std::move(scope))) {}

    std::string title() override { return "App Permissions"; }

    UIElement view() override {
        if (!view_) view_ = scrollingPage(body_->view());
        return view_;
    }

    void refresh() override { body_->refresh(); }

private:
    std::shared_ptr<SettingsView> body_;
    UIElement view_{nullptr};
};

} // namespace

std::shared_ptr<SettingsView> makePermissionsView(AppShell& shell, SettingsScope scope) {
    return std::make_shared<PermissionsBody>(shell, std::move(scope));
}

std::shared_ptr<Page> makePermissionsScreen(AppShell& shell, std::string package, std::string label) {
    return std::make_shared<PermissionsScreen>(shell, SettingsScope{std::move(package), std::move(label), false});
}

std::string permissionsSummary(const std::string& package, bool own) {
    const auto entry = entryOf(package);
    if (!entry || !entry->report.permissionsKnown) return "Not known yet";
    std::string allowed, refused;
    for (const auto& g : permissionHost::groupsOf(entry->report)) {
        WindowsWord w;
        if (own && g.decider == permissionHost::Decider::windows) {
            w.known = true;
            w.state = permissionHost::windows(g.name);
            w.said = permissionHost::windowsSaid(g.name);
        }
        const Standing s = standing(g, entry->report, package, own, w);
        std::string& to = s.list == List::allowed ? allowed : refused;
        to += (to.empty() ? "" : ", ") + g.title;
    }
    if (allowed.empty() && refused.empty()) return "None that Android asks about";
    return (allowed.empty() ? std::string("None allowed") : "Allowed: " + allowed) + (refused.empty() ? "" : "; not allowed or asked: " + refused);
}

} // namespace megler::app::ui
