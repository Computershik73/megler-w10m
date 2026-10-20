// The test program's way to the shell's cameras: meglet_camera_host as
// app/Meglet/player/CameraHost.cpp implements it over Windows.Media.Capture,
// for mg_tests --camera-windows (mg_tests_main.c), which opens the PC's first
// camera through it and the core's camera code and takes a few pictures.
//
// What that shows: the host's own work -- the cameras found, one opened,
// its sizes, a format set, a frame reader's NV12 pictures handed over, focus
// and the light where the camera has them, stop and close -- against a
// PC's camera and Windows. What it cannot show: a test program is no app
// container (no consent prompt: a desktop program's access to the camera is
// the privacy setting for desktop apps), and a PC's camera is no phone's.
// Without a camera there is nothing to open, which the mode says.
//
// Only tools/build-tests.sh win builds this file, with CameraHost.cpp itself;
// the rest of the shell it uses is stood in for by mg_storage_windows.cpp.
#include "pch.h"

#include "player/CameraHost.h"

#include <cstdio>

extern "C" {

/// The shell's cameras, as on a device held sideways (a PC), with nothing kept between runs and no flags. (Windows' calls
/// are all made on the host's own thread: this one needs no apartment.)
const meglet_camera_host* mg_camera_windows_host(void) {
    static meglet_camera_host host;
    host = meglet::app::CameraHost::shared().callbacks();
    return &host;
}

/// The cameras found, in a line.
void mg_camera_windows_summary(char* out, size_t size) { snprintf(out, size, "%s", meglet::app::CameraHost::shared().summary().c_str()); }

} // extern "C"
