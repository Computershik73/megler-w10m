// A diagnosis asked for over USB alone, for a phone on a PC without the Device Portal: the owner makes the folder
// Pictures\Megler\diag\<name> on the phone (MTP shows Pictures to any PC), <name> the Android package of an app's own
// package or "megler" for Megler's. Then:
//   - the names of the files in its "flags" folder are this package's flags (LocalState\flags, core: mg_flag), put there
//     before the app runs, and taken away again once the folder no longer names them (LocalState\flags-from-pictures.txt
//     says which were put there so; a flag set through the portal is left alone);
//   - the package's log (LocalState\megler.log, and megler.prev.log once) is copied into the folder every 10 s and when
//     the app is suspended, for the PC to copy off over MTP.
// Nothing is done where the folder is not there. A Lumia 650's owner moved the phone to a PC without the portal while its
// TikTok Lite was being diagnosed (package 626).
#pragma once

namespace megler::app::picturesDiag {

/// Looks for the folder and puts the flags in place on a thread of its own; once a run. From OnLaunched.
void start();
/// Until start's thread has put the flags in place (at most 5 s; start() first if it was not called): before the app's run
/// reads them (player/Session.cpp). Off the UI thread.
void waitForFlags();
/// The log copied now, waited for 2 s at most: as the app is suspended. Nothing where no folder asked for it.
void exportNow();

} // namespace megler::app::picturesDiag
