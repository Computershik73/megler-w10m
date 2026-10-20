// The apps the phone has, for an Android app that lists or starts them -- a launcher: megler_run_config.apps_dir
// (core/include/megler.h), the framework's megler.PhoneApps. Megler writes it into the shared folder from its library,
// whenever the library changes; every package of Megler's (an app's own package too) gives it to the app it runs.
//
//   <shared>\phone-apps\apps.txt     a line for each app that can run here: its package, launcher activity, label,
//                                    version name, version code, separated by tabs (UTF-8)
//   <shared>\phone-apps\<package>.png its icon
#pragma once

#include <string>

namespace megler::app::phoneAppsList {

/// The folder, empty when there is no shared folder.
std::wstring folder();
/// Megler, UI thread, at every poll: writes the list and the icons again when the library has changed (on a worker).
void publish();

} // namespace megler::app::phoneAppsList
