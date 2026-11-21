// The default apps every Java app goes by (core/framework-overlay/megler/Defaults.java, megler.Resolver): defaults.ini in the
// folder of the phone's apps (library/PhoneAppsList.h, <shared>\phone-apps), "key=value" lines --
//   browser=<package>/<activity>         the browser web links open in; none, or "windows": Windows' own
//   links.<package>=all | off            which web links that app opens ("Open supported links"): every one its filters
//                                        name, or none; none said: those its sites verify it for
//   always.<kind>=<package>/<activity>   the answer "Always" of "Open with" for that kind of intent, or "windows"
// -- which the apps write too (an "Always", Android's screens of default apps): read again each time it is shown, and each
// change written over what the file has then. Megler's Settings show the browser and the "Always" answers, an app's own
// settings its links (ui/SettingsScreen.cpp).
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

namespace megler::app::defaultApps {

/// The file's keys and values now; none where there is no file.
std::map<std::string, std::string> read();
/// `key` set to `value`, or taken out with nothing; false when the file could not be written.
bool set(const std::string& key, const std::optional<std::string>& value);
/// Every "always." key whose answer is the app `package`'s taken out; how many went.
int clearAlways(const std::string& package);

/// A browser the phone has: an activity of an app of the library whose filter takes web links of any host as a browser does
/// (VIEW, BROWSABLE, DEFAULT, https, no host and no type), as the shell described it -- the list megler.Resolver.browsers()
/// makes for the apps.
struct Browser {
    std::string id;    // "<package>/<activity>"
    std::string label; // the activity's label, else the app's
};
std::vector<Browser> browsers();

/// The sites whose links the app's filters name (VIEW, BROWSABLE, http or https, a host), each once, with whether a filter
/// that asks to be verified (android:autoVerify) names it.
std::vector<std::pair<std::string, bool>> linkDomains(const std::string& package);

/// A kind of intent as the user reads it: "Links to example.org", "geo: addresses", "image/png files, EDIT".
std::string kindInWords(const std::string& kind);
/// The app a kept answer names, by its label in the library ("Windows" for Windows').
std::string labelOf(const std::string& id);

} // namespace megler::app::defaultApps
