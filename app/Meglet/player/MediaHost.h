// A running app's films: meglet_media_host (core/include/meglet.h) over Media
// Foundation's source reader, which every Windows 10 has (the phone's 1703+
// included) and which needs no capability for a file in the app's own folder.
//
// The core has no decoders of its own. When an app plays a film (Unity's
// VideoPlayer through libmediandk, core/src/ndk/mg_media.c) the core copies
// the film to a file under the data folder and asks for it to be opened here,
// then asks for one picture or one block of sound at a time: RGBA with the
// top row first, and 16-bit PCM. MP4 with H.264 and AAC is what Windows
// decodes and what games ship; anything else fails to open, and the app is
// told the file cannot be played.
//
// A film is two source readers on the same file, one with only the picture
// selected and one with only the sound: the core asks for the two at paces of
// their own (the app's video and audio decoders are separate objects), and a
// reader that is asked for one stream while the other is selected keeps the
// other's samples queued without bound.
//
// Everything Media Foundation is done on a thread of this class's own, which
// lives in COM's multithreaded apartment: the threads that ask are the app's,
// running Android code on stacks of the size the app chose, and a software
// decoder does not fit every one of those. Each call waits for that thread.
//
// mfplat.dll and mfreadwrite.dll are delay-loaded (tools/build-appx.sh), so a
// device without them gets "the film cannot be opened" instead of an app that
// does not start.
//
// One MediaHost serves the process: shared().
#pragma once

#include "meglet.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>

namespace meglet::app {

class MediaHost {
public:
    static MediaHost& shared();
    MediaHost(const MediaHost&) = delete;
    MediaHost& operator=(const MediaHost&) = delete;

    /// The callbacks for meglet_run_config.media, with this as their user.
    meglet_media_host callbacks();

    struct Film;

private:
    MediaHost() = default;
    ~MediaHost();

    static void* openThunk(void* user, const char* path, meglet_media_info* info);
    static int readVideoThunk(void* user, void* media, uint8_t* rgba, int64_t* timeUs);
    static int readAudioThunk(void* user, void* media, int16_t* samples, int maxFrames, int64_t* timeUs);
    static bool seekThunk(void* user, void* media, int64_t timeUs);
    static void closeThunk(void* user, void* media);

    /// Runs `work` on the worker thread and returns when it has run.
    void run(const std::function<void()>& work);
    void workerLoop();
    /// The worker's: Media Foundation is started (once; false when it cannot be).
    bool started();

    std::mutex mutex_; // work_, finished_, quit_, and the worker's start
    std::condition_variable wake_, done_, idle_;
    std::thread worker_;
    const std::function<void()>* work_ = nullptr;
    bool* finished_ = nullptr;
    bool quit_ = false;
    // The worker's alone.
    int startState_ = 0; // 0 not tried, 1 started, -1 cannot be
};

} // namespace meglet::app
