// What the phone is at this moment, for an app to be told of: its time zone
// and its battery (megler_set_timezone and megler_set_battery, core/include/megler.h).
//
// Windows names its time zones as tzdata does (Windows.Globalization's
// Calendar), which is the name Android wants. Of the battery it says how full
// it is, whether it is being charged and whether power is plugged in
// (Windows.System.Power's PowerManager); it does not say a voltage or a
// temperature, and the core makes those up.
#pragma once

namespace megler::app::phoneState {

/// Tells the core the time zone and the battery as they are, and from then
/// on every change of the battery, for as long as the process lives. Before
/// an app is started; the first call does it and the others nothing.
void tell();

} // namespace megler::app::phoneState
