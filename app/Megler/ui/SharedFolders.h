// The phone's folders the user gave Megler (Settings, Add a Folder) for the separate apps (docs/PER-APP-PACKAGES.md).
//
// Windows remembers a folder picked in the folder picker for the one package it was picked in (FutureAccessList), and
// the Documents and Downloads folders and a memory card have no other way in (player/StorageHost.h): a separate app has
// to be given each of them by the user once more. So Megler writes the list of its added folders into the shared folder,
// and a separate app offers, before its app starts, each of them it has not got: a dialog that says which folder, then
// the folder picker. A folder the user says "Not Now" to is not offered at start again; the player's menu offers all.
// Only an app that asks for the shared storage is offered them at start (wantedBy): every app was, a calculator too.
#pragma once

#include <functional>

namespace megler::app {
struct ApkReport;
}

namespace megler::app::ui::sharedFolders {

/// Whether a separate app is offered Megler's folders as it starts: its manifest asks for the shared storage (a storage or
/// media permission, on whichever Android: an app that saves its pictures through MediaStore asks for WRITE_EXTERNAL_STORAGE
/// up to Android 9 alone), and the user has not refused it the storage for good. A report that names no permissions: yes,
/// as before.
bool wantedBy(const ApkReport& report);

/// Megler itself: writes its added folders into the shared folder, where the separate apps read them. Nothing in a
/// separate app, or before the storage host's search is over. On the UI thread.
void publish();

/// A separate app: offers each of Megler's folders it has not got, those the user said "Not Now" to before only with
/// `all`. `done` is told how many were offered (-1 when Megler lists none) and how many added; at once when there is
/// nothing to offer. On the UI thread, once the storage host's search is over.
void offer(bool all, std::function<void(int offered, int added)> done);

}  // namespace megler::app::ui::sharedFolders
