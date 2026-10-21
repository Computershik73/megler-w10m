// Pictures of Megler's own screens, for looking at them from the PC: the auto run's screenshots (auto/AutoRun.cpp) are of
// an app's surface, and the shell's XAML is not in them.
//
// Driven by flag files in LocalState\flags, as the other switches are (tools/phone-flags.py); only their names are read.
// Each is acted on once, when it is first seen, and then removed: one left there would act again whenever Megler starts.
// A new name asks again:
//   ui-shot-<name>          a picture of the window as it is now: LocalState\shot-ui-<name>.bmp (tools/phone.py shots)
//   ui-tab-<name>-<n>       the tab n (0 Library, 1 Device, 2 Log, 3 Settings)
//   ui-open-<name>-<n>      the library's app number n (from 0), its page
//   ui-back-<name>          Back, as the phone's button
#pragma once

namespace megler::app::ui {

class AppShell;

namespace uiShots {

/// UI thread, at every poll: looks for new flags about once a second and does what they say.
void poll(AppShell& shell);

} // namespace uiShots

} // namespace megler::app::ui
