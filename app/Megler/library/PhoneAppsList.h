// The apps the phone has, for an Android app that lists or starts them -- a launcher: megler_run_config.apps_dir
// (core/include/megler.h), the framework's megler.PhoneApps. Megler writes it into the shared folder from its library,
// whenever the library changes; every package of Megler's (an app's own package too) gives it to the app it runs.
//
//   <shared>\phone-apps\apps.txt     a line for each app that can run here: its package, launcher activity, label,
//                                    version name, version code, separated by tabs (UTF-8)
//   <shared>\phone-apps\<package>.png its icon
//   <shared>\phone-apps\<package>\  its activities (megler_apk_describe_activities: activities.json and the pictures of their
//                                    icons), for a launcher or a shortcut maker that lists them; source.txt says which APK they
//                                    are of, so that they are written again only for another
//   <shared>\phone-apps\installs\<name>.txt  what Megler's installer last told of the APK an app handed it under that
//                                    name (installOutcome): "<when>\t<what>\t<why>"; the app's installer session waits
//                                    for it, or for the app in apps.txt at the APK's version (megler.Installs)
#pragma once

#include <string>

namespace megler::app::phoneAppsList {

/// The folder, empty when there is no shared folder.
std::wstring folder();
/// Megler, UI thread, at every poll: writes the list and the icons again when the library has changed (on a worker).
void publish();
/// What became of the APK an app handed Megler under `name`, for the app's installer session to tell it: `what` is
/// "declined", "incompatible", "failed" or "busy", `why` a line for the user. (Its success is the app in the list.)
void installOutcome(const std::string& name, const std::string& what, const std::string& why);

} // namespace megler::app::phoneAppsList
