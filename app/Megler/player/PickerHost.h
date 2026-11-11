// The phone's own picker of files and its camera, for what a Java app asks of Android's documents UI, photo picker and camera
// app (megler_system_host.pick, core/include/megler.h; the framework's megler.Pickers): Windows' FileOpenPicker and
// CameraCaptureUI, shown on the UI thread. What the user picked or took is copied into the request's folder (one of the
// app's data, which the core names) and the core is answered (megler_pick_done) from a thread of the shell's: the names of
// the copies, the user backing out, or Windows failing. The thunk returns at once: false while a picker or the camera is up
// already (Windows refuses a second), and then the app hears the user backed out.
//
//   FileOpenPicker   its filter from the request's MIME types (filterFor: "image/*" the pictures' extensions, a type whose
//                    kind is not known "*"), opening in the pictures, videos or music library for an app that takes those
//                    (with thumbnails) and in the documents otherwise; one file, or several (PickMultipleFilesAsync: Windows
//                    has no limit, the first `max` are taken). Windows 10 Mobile's picker takes the whole screen and hides
//                    the app's window meanwhile (the session pauses the app: player/Session.cpp), a PC's is a window of its own.
//   CameraCaptureUI  Windows' camera, for a photo (JPEG) or a video (MP4, at most `duration_s`, low definition for quality 0):
//                    Windows 10 Mobile's since 1511 (it ignores the crop it never offers), and a PC's with a camera. The
//                    file it leaves in the app's TempState is copied as IMG_<date>_<time>.jpg / VID_<date>_<time>.mp4, as
//                    Android's camera names them, and deleted.
//
// The log says what was asked and what came back: the types, how many files and their extensions -- never their names.
//
// For a run no one picks in (an auto run, a test on the phone), flag files in LocalState\flags answer the next request in place
// of the user, nothing shown; only their names are read, and each is removed once used:
//   pick-cancel        the next request is answered as the user backing out;
//   pick-file-<name>   the next request is answered with the file <name> in the Pictures library's folder Megler (where
//                      packages are copied to the phone: Phone\Pictures\Megler over USB), as if the user had picked it or
//                      the camera had taken it -- copied into the request's folder like any picked file. A request for
//                      several takes every such flag there is (sorted by name, at most its `max`), one for one file the
//                      first. A name the folder does not have is answered as Windows failing.
// (tools/phone-flags.py set pick-file-photo.jpg; then the app's "attach" or "take a photo".)
#pragma once

#include "megler.h"

#include <string>
#include <vector>

namespace megler::app::pickerHost {

/// UI thread, once (systemHost::attach): the window the pickers are shown over.
void attach(winrt::Windows::UI::Core::CoreDispatcher dispatcher);
/// megler_system_host.pick.
bool MEGLER_CB thunk(void* user, const megler_pick_request* request);

/// What Windows' picker is given for a request's MIME types ('\n' after each, as megler_pick_request has them): the
/// extensions it shows (".jpg"...; "*" for every file), where it opens (MEGLER_PICK_START_*, as asked), and whether it
/// shows thumbnails rather than a list.
struct Filter {
    std::vector<std::wstring> extensions;
    int start = MEGLER_PICK_START_DOCUMENTS;
    bool thumbnails = false;
};
Filter filterFor(const std::string& types, int start);

/// The flags' answer to a request (see above), the flags it used removed: `found` false when there is none.
struct FlagAnswer {
    bool found = false, cancel = false;
    std::vector<std::wstring> names;
};
FlagAnswer takeFlags(const std::wstring& flagsFolder, bool multiple, int max);

/// The files `names` of `source` (a folder's path) copied into `folder` and the core answered for request `id`: MEGLER_PICK_OK
/// with the copies' names, or MEGLER_PICK_FAILED when one is not there or could not be copied. Off the UI thread; what
/// megler_pick_done was given is returned too.
struct Answer {
    int status = MEGLER_PICK_FAILED;
    std::vector<std::string> names;
};
Answer answerFromFolder(unsigned id, const std::wstring& source, const std::vector<std::wstring>& names, const std::wstring& folder);

/// For the tests: where pick-file-<name> flags take their files from, in place of the Pictures library's Megler folder.
void setFlagSource(const std::wstring& folder);

} // namespace megler::app::pickerHost
