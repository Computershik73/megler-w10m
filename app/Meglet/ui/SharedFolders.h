// The phone's folders the user gave Meglet (Settings, Add a Folder) for the separate apps (docs/PER-APP-PACKAGES.md).
//
// Windows remembers a folder picked in the folder picker for the one package it was picked in (FutureAccessList), and
// the Documents and Downloads folders and a memory card have no other way in (player/StorageHost.h): a separate app has
// to be given each of them by the user once more. So Meglet writes the list of its added folders into the shared folder,
// and a separate app offers, before its app starts, each of them it has not got: a dialog that says which folder, then
// the folder picker. A folder the user says "Not Now" to is not offered at start again; the player's menu offers all.
#pragma once

#include <functional>

namespace meglet::app::ui::sharedFolders {

/// Meglet itself: writes its added folders into the shared folder, where the separate apps read them. Nothing in a
/// separate app, or before the storage host's search is over. On the UI thread.
void publish();

/// A separate app: offers each of Meglet's folders it has not got, those the user said "Not Now" to before only with
/// `all`. `done` is told how many were offered (-1 when Meglet lists none) and how many added; at once when there is
/// nothing to offer. On the UI thread, once the storage host's search is over.
void offer(bool all, std::function<void(int offered, int added)> done);

}  // namespace meglet::app::ui::sharedFolders
