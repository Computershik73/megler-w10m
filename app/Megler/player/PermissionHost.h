// The user's say over what an Android app's dangerous permissions guard: megler_system_host.permission (core/include/megler.h),
// which the framework's megler.Permissions asks of each of Android's permission groups that the app's manifest asks for. One
// answer for each group and app, kept from run to run; three ways of deciding:
//
//   location, camera, microphone   Windows': what its privacy settings say of this app's package (DeviceAccessInformation,
//                                  nothing asked), and Windows' own question when the app requests the permission
//                                  (Geolocator.RequestAccessAsync; MediaCapture.InitializeAsync, which opens the camera or
//                                  microphone for a moment), the first time. Windows asks only once: a no is for good, and
//                                  Settings > Privacy changes it. A place set by hand in Megler's Settings is the app's to
//                                  have; a phone without the camera or the microphone has nothing to refuse.
//   storage, read_media_visual, read_media_aural, sms, phone, call_log   Megler's: its own question, Allow or Don't Allow,
//                                  over the app (ui/AppShell.h, dialogWhenFree), kept in the app's settings as
//                                  megler.permission.<group> = granted, denied, denied-fixed (shell/Settings.h). A second no
//                                  is for good, as on Android 11 and later; Back puts the question away and changes
//                                  nothing. The app's Settings change it (ui/SettingsScreen.cpp, Permissions). Under the
//                                  flag allow-permissions (LocalState\flags) every one is allowed with no one asked, for a
//                                  run no one watches; the flag ui-dialog-<name>-1 answers one that is up (ui/UiShots.h).
//   contacts, calendar, sensors, nearby_devices, notifications, activity_recognition   granted, no one asked: Megler gives an
//                                  app none of the phone's contacts, calendar, body sensors, Bluetooth devices or
//                                  notifications, so there is nothing for the user to refuse.
//
// What each asks for, and each answer, is in the log.
#pragma once

#include "megler.h"

#include <string>
#include <vector>

namespace megler::app {
struct ApkReport;
}

namespace megler::app::permissionHost {

/// megler_system_host.permission for the app that runs here (systemHost::runningPackage): MEGLER_PERMISSION_*.
int MEGLER_CB thunk(void* user, const char* group, bool ask);

enum class Decider { windows, megler, none };

struct Group {
    std::string name;     // Android's group in small letters: "camera"
    std::string what;     // what it lets the app have, in words: "the camera"
    Decider decider;
};

/// The groups of the dangerous permissions an app's manifest asks for (its report's, on Android 14), in Android's order of
/// groups; none for a report that names no permissions.
std::vector<Group> groupsOf(const ApkReport& report);

/// What the user said of a group Megler asks about, for the app `package`: MEGLER_PERMISSION_*, kept in its settings.
int kept(const std::string& package, const std::string& group);
/// Kept: GRANTED, DENIED, DENIED_FIXED; UNDECIDED forgets, and the app asks again.
void keep(const std::string& package, const std::string& group, int state);

/// What Windows' privacy settings say of this process's package for a group Windows decides: MEGLER_PERMISSION_*.
int windows(const std::string& group);
/// Windows' privacy settings page of a group Windows decides ("ms-settings:privacy-webcam"), empty for another.
std::wstring windowsSettingsPage(const std::string& group);

} // namespace megler::app::permissionHost
