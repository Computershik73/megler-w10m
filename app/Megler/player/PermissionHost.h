// The user's say over what an Android app's dangerous permissions guard: megler_system_host.permission (core/include/megler.h),
// which the framework's megler.Permissions asks of each of Android's permission groups that the app's manifest asks for. One
// answer for each group and app, kept from run to run; three ways of deciding by default:
//
//   location, camera, microphone   Windows': what its privacy settings say of this app's package (DeviceAccessInformation,
//                                  nothing asked), and Windows' own question when the app requests the permission
//                                  (Geolocator.RequestAccessAsync; MediaCapture.InitializeAsync, which opens the camera or
//                                  microphone for a moment), the first time. Windows asks only once: a no is for good, and
//                                  Settings > Privacy changes it. A place set by hand in Megler's Settings is the app's to
//                                  have; a phone without the camera or the microphone has nothing to refuse. The location
//                                  refused by the system (the phone's location switched off) is the app's to have, as an
//                                  Android app's permission is whether the location is on or off: the app is told the
//                                  location is off (player/LocationHost.h) and asks the user to switch it on. What Windows
//                                  says of the location is said in the log as it is first read and as it changes.
//   storage, read_media_visual, read_media_aural, sms, phone, call_log   Megler's: its own question, Allow or Don't Allow,
//                                  over the app (ui/AppShell.h, dialogWhenFree), kept in the app's settings as
//                                  megler.permission.<group> = granted, denied, denied-fixed (shell/Settings.h). A second no
//                                  is for good, as on Android 11 and later; Back puts the question away and changes
//                                  nothing. Under the flag allow-permissions (LocalState\flags) every one is allowed with no
//                                  one asked, for a run no one watches; the flag ui-dialog-<name>-1 answers one that is up
//                                  (ui/UiShots.h).
//   contacts, calendar, sensors, notifications, activity_recognition   granted, no one asked: Megler gives an app none of the
//                                  phone's contacts, calendar, body sensors or notifications, so there is nothing for the
//                                  user to be asked about.
//   nearby_devices                 granted, no one asked: Bluetooth LE's devices (player/BluetoothHost.h), which Windows asks
//                                  the user nothing for -- its privacy settings of "other devices" refuse them, where the
//                                  user has said so there -- and which an app without them could not reach at all.
//
// And the user's own say over every group, in the app's App Permissions (ui/PermissionsScreen.cpp), as Android's App info >
// Permissions has it, kept likewise:
//   Allow                          granted -- for a group Windows decides, Windows' answer as above (Windows' yes or no is
//                                  Windows', changed in its privacy settings);
//   Don't allow                    denied-fixed: the app is told no and not asked, whoever decides the group by default --
//                                  over Windows' yes for the location, the camera and the microphone, and for the groups
//                                  granted with no one asked;
//   Ask every time                 ask: Megler's question at the app's first request of the group in each run, its answer
//                                  that run's alone (kept in this process, thisRun); for a group Windows decides, Windows'
//                                  own answer after Megler's yes.
//
// What each asks for, and each answer, is in the log. The answers and what they come to (everything but Windows' objects and
// the questions) are player/PermissionRules.cpp, which the shell's tests build (core/tests/mg_shell_windows.cpp).
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
    std::string name;    // Android's group in small letters: "camera"
    std::string what;    // what it lets the app have, in words: "the camera"
    std::string title;   // its row's, as Android's settings name it: "Camera"
    Decider decider;     // who decides it when the user has said nothing in the app's permissions
    wchar_t icon = 0;    // Segoe MDL2 Assets' code point
};

/// The groups of the dangerous permissions an app's manifest asks for (its report's, on Android 14), in Android's order of
/// groups; none for a report that names no permissions.
std::vector<Group> groupsOf(const ApkReport& report);
/// The group of that name, whether an app asks for it or not; nullptr for none of Android's.
const Group* groupNamed(const std::string& name);
/// The group of one of Android's dangerous permissions ("android.permission.CAMERA": "camera"); empty for another.
std::string groupOfPermission(const std::string& permission);
/// Megler's question about a group, for `app` (its label): "Allow <app> to ...?", and what it amounts to here.
std::string questionOf(const std::string& group, const std::string& app);
std::string explanationOf(const std::string& group);

/// What the user said of a group, kept as megler.permission.<group>: beside MEGLER_PERMISSION_* (UNDECIDED, nothing said;
/// GRANTED; DENIED; DENIED_FIXED), EVERY_TIME, "Ask every time".
constexpr int EVERY_TIME = 16;
/// What is kept for the app `package`.
int kept(const std::string& package, const std::string& group);
/// Kept: GRANTED, DENIED, DENIED_FIXED, EVERY_TIME; UNDECIDED forgets, and the group is decided as by default again.
void keep(const std::string& package, const std::string& group, int state);
/// Every answer kept for the app forgotten, as Android's Clear storage resets an app's permissions: when its data is to be
/// cleared (megler_ask_clear_data from its page in Megler; megler_clear_data_asked at its start, for the app's own Clear
/// storage). Windows' own answers (the location, the camera, the microphone) are Windows' to change, in its privacy settings.
void forgetAll(const std::string& package);

/// What the app is told of a group it asks for (MEGLER_PERMISSION_*): from who decides it by default, what the user said
/// (kept), what Windows says of this app's package for a group Windows decides (otherwise not looked at) and this run's
/// answer to a group asked every time (UNDECIDED while there is none).
int decide(Decider decider, int kept, int windows, int thisRun);
/// This run's answer to a group asked every time, for the app that runs in this process: UNDECIDED while none was given.
int thisRun(const std::string& package, const std::string& group);
/// Keeps it, for this process's life (the question's answer; UNDECIDED forgets it).
void rememberThisRun(const std::string& package, const std::string& group, int state);
/// Whether the user's own no stands between the app and a group: Don't allow, or this run's no to a question asked every
/// time -- whatever Windows says.
bool refusedByUser(const std::string& package, const std::string& group);

/// What Windows' privacy settings say of this process's package for a group Windows decides: MEGLER_PERMISSION_*. The
/// location switched off by the system is GRANTED: Android's location permission does not depend on the location's switch.
int windows(const std::string& group);
/// The same in words, for the app's settings: "Allowed by Windows", "The phone's location is switched off: ...".
std::string windowsSaid(const std::string& group);
/// Windows' privacy settings page of a group Windows decides ("ms-settings:privacy-webcam"), empty for another.
std::wstring windowsSettingsPage(const std::string& group);

} // namespace megler::app::permissionHost
