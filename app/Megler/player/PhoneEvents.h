// The phone's text messages and calls, for an Android app as Android has them (megler_sms_received, megler_call_logged,
// core/include/megler.h; the framework's megler.Phone).
//
// A text message is Windows' chat message store's (Windows.ApplicationModel.Chat, the "chat" capability): one that comes
// in while an app runs is told when the store says it was made. A call is the phone's call history's
// (Windows.ApplicationModel.Calls, "phoneCallHistoryPublic"): Windows says when a call's state changes but not who calls,
// so once a call is over the history is read and the calls that began since the app started are told -- WhatsApp's flash
// call, a missed call whose number is the code, comes so. What the phone had before the app started is never told.
#pragma once

namespace megler::app::phoneEvents {

/// Starts watching, once, as an app starts: from now on what comes is told to the core. Any thread. Only what the app's
/// permissions ask for (megler_appx_capabilities): the messages for RECEIVE_SMS, READ_SMS or Google Play services' SMS
/// Retriever (`messages`), the calls for READ_PHONE_STATE or READ_CALL_LOG (`calls`). The store of what is not asked for is
/// never opened, and an app's own package without the capability would be refused it.
void start(bool messages, bool calls);
/// Reads again what may have come while the app was suspended (a call takes the screen, and Megler may be suspended
/// under it): on resuming. Any thread.
void check();

} // namespace megler::app::phoneEvents
