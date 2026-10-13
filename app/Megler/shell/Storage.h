// The phone's own folders that an Android app may be shown as parts of its
// "SD card": the libraries (documents, music, pictures, videos), the
// downloads, a memory card.
//
// An Android app's files live under Megler's own LocalState, where the layer
// can always read and write. The phone's libraries are another matter: an
// app container reaches them only as far as its capabilities go
// (Package.appxmanifest: picturesLibrary, videosLibrary, musicLibrary,
// documentsLibrary, removableStorage), and the layer opens files by path with
// the plain file functions, not through Windows.Storage's brokered objects --
// whether a capability opens a folder to *those* is the phone's to say. So
// each folder is tried, once, at start: can it be listed, can a file be made
// and removed in it. What can be listed is offered to the layer as a link in
// the app's storage (megler_run_config's storage links); the log says the rest.
#pragma once

#include <string>
#include <vector>

namespace megler::app::storage {

struct Folder {
    std::string android;  // the name Android gives the folder under /sdcard: "Documents", "Download", "DCIM"...
    std::string path;     // the phone's folder, with forward slashes, no slash at the end
    bool canList = false; // its entries can be read
    bool canWrite = false; // a file can be made and removed in it
    std::string note;     // what was found, for the log
};

/// Tries the phone's folders (on a thread of its own; returns at once) and
/// writes one line for each to the log.
void probe();

/// What probe() found, once it is done; empty before. Any thread.
std::vector<Folder> folders();

/// True once probe() has finished.
bool probed();

} // namespace megler::app::storage
