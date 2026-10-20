// The phone's cameras for an Android app's android.hardware.Camera:
// meglet_camera_host (core/include/meglet.h) over Windows.Media.Capture, with
// only what Windows 10 Mobile 1703 has (frame readers and source groups came
// with 1607).
//
// The cameras are the colour streams of the media frame source groups
// (MediaFrameSourceGroup), or, where there are none, the video capture
// devices (DeviceInformation); the back panel's first, as Android numbers
// them. Which way a camera faces is its enclosure's panel; how far its
// picture is turned is the panel and the rotation it is mounted at
// (EnclosureLocation.RotationAngleInDegreesClockwise): on a phone, upright
// by nature, the sensor is read a quarter turn off the screen's upright,
// which is the 90 (back) and 270 (front) an Android phone says. A flag file
// camera-orientation-<n>-<degrees> in LocalState\flags puts another number
// in its place, should a phone turn its pictures some other way.
//
// Opening a camera is MediaCapture's InitializeAsync, made on the UI thread:
// the first time, that is where Windows asks the user whether Meglet may use
// the camera (the package declares the webcam capability). A refusal is
// E_ACCESSDENIED, which the core turns into Android's "Fail to connect to
// camera service"; the log has the answer and what Windows' privacy setting
// for the camera says. The camera streams through a MediaFrameReader in
// NV12, in memory (MediaCaptureMemoryPreference::Cpu), at the size and the
// nearest frame rate the app chose, each picture handed to the core while its
// buffer is locked. The sizes offered go up to 1280 x 720 (every picture is
// copied into the app's Java; the flag file camera-large-sizes offers up to
// 1920 x 1080). Focus is the FocusControl's (once, close up, continuous),
// the light the TorchControl's.
//
// What was learnt of a camera when it was opened (whether it focuses, has a
// torch) is kept in LocalState\camera.txt for the next start, so that an app
// that asks the package manager before it opens a camera is told.
//
// The core's calls come on the app's own threads, which have no COM
// apartment; each is done on a thread of this class's own in the
// multithreaded apartment while the caller waits, as MediaHost's and
// StorageHost's are. Pictures and focus's answers come on Windows' own
// threads.
//
// One CameraHost serves the process: shared().
#pragma once

#include "meglet.h"

#include <string>

namespace meglet::app {

class CameraHost {
public:
    static CameraHost& shared();
    CameraHost(const CameraHost&) = delete;
    CameraHost& operator=(const CameraHost&) = delete;

    /// Where what was learnt of the cameras is kept, where the flag files are, and whether the device is upright by nature
    /// (a phone). Before the first call. Left alone (the tests' program): nothing kept, no flags, a device held sideways.
    void configure(const std::wstring& stateFile, const std::wstring& flagFolder, bool portraitNative);
    /// The callbacks for meglet_run_config.camera, with this as their user.
    meglet_camera_host callbacks();
    /// The cameras found, one line ("2 cameras: 0 Back camera (back, turned 90; panel back, mounted at 0); ...").
    std::string summary();

    struct Camera;

private:
    CameraHost() = default;

    static int countThunk(void* user);
    static bool infoThunk(void* user, int index, meglet_camera_info* info);
    static void* openThunk(void* user, int index, meglet_camera_caps* caps, int* error);
    static bool startThunk(void* user, void* camera, int width, int height, int fps, meglet_camera_frame_fn frame, void* core);
    static void stopThunk(void* user, void* camera);
    static bool focusThunk(void* user, void* camera, int mode, meglet_camera_focus_fn done, void* core);
    static void torchThunk(void* user, void* camera, bool on);
    static void closeThunk(void* user, void* camera);
};

} // namespace meglet::app
