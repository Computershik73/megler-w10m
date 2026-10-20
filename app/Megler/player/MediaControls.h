// The system media controls (Windows.Media.SystemMediaTransportControls) for a Java app: the play, pause, next and
// previous of the lock screen and of the volume flyout, sent to the app as Android's media keys, and what the app's
// media session says it plays (megler_media_session_host, core/include/megler.h): its state, title, artist and art.
//
// Windows keeps an app's sound going while it is in the background only when the app has the backgroundMediaPlayback
// capability (Package.appxmanifest) and, for sound that is not Windows' own MediaPlayer's, has play and pause on in these
// controls and answers their presses. So they are on for as long as a Java app runs. An app that says nothing through a
// media session is shown playing while it is heard (heard()).
#pragma once

#include "megler.h"

namespace megler::app::mediaControls {

/// UI thread: a Java app runs. The controls are on, play and pause among them.
void start();
/// UI thread: the app has ended. The controls are off.
void stop();
/// UI thread, a few times a second while the app runs: whether it was heard just now. Decides what the controls show
/// until the app's media session says it itself.
void heard(bool audible);

/// The callbacks for megler_run_config.media_session.
megler_media_session_host callbacks();

} // namespace megler::app::mediaControls
