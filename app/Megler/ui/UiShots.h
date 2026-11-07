// Pictures of Megler's own screens, for looking at them from the PC: the auto run's screenshots (auto/AutoRun.cpp) are of
// an app's surface, and the shell's XAML is not in them.
//
// Driven by flag files in LocalState\flags, as the other switches are (tools/phone-flags.py); only their names are read.
// Each is acted on once, when it is first seen, and then removed: one left there would act again whenever Megler starts.
// A new name asks again:
//   ui-shot-<name>          a picture of the window as it is now: LocalState\shot-ui-<name>.bmp (tools/phone.py shots)
//   ui-tab-<name>-<n>       the tab n (0 Library, 1 Device, 2 Log, 3 Settings)
//   ui-open-<name>-<n>      the library's app number n (from 0), its page
//   ui-back-<name>          Back, as the phone's button (an app on its first screen goes to the back with it, as on
//                           Android 12: only the button has Windows take the window away then; ui-quit closes it)
//   ui-quit-<name>          the running app stopped as its Quit stops it, and Megler closed once it has ended
//   ui-dialog-<name>-<n>    the dialog up now answered with its button n, from the left (Google Play's question: 0 microG, 1 Google Play)
//   ui-update-all           Update All (ui/SeparateApps.h): each app package made by another Megler made and installed again
//   ui-update-<package>     the same for the one app of that Android package (ui-update-com.here.app.maps), outdated or not
//   ui-uninstall-<package>  that app's package uninstalled, as its page's Uninstall does once asked (its APK into the library first)
//   ui-probe-breakpoints    whether the system stops a thread at a hardware breakpoint (megler_probe_breakpoints), in the log
#pragma once

namespace megler::app::ui {

class AppShell;

namespace uiShots {

/// UI thread, at every poll: looks for new flags about once a second and does what they say.
void poll(AppShell& shell);

} // namespace uiShots

} // namespace megler::app::ui
