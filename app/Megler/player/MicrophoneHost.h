// The phone's microphone for an Android app's android.media.AudioRecord: megler_microphone_host (core/include/megler.h)
// over Windows.Media.Audio's AudioGraph.
//
// Whether an app may hear the microphone is Windows' question to the user, asked the first time an app records:
// MediaCapture, initialised for audio alone on the UI thread, where Windows can ask (the package declares the microphone
// capability). The answer is kept for the process; after a no, the app reads silence.
//
// While an app records, an AudioGraph runs at the rate and channels the app asked for -- the graph resamples the
// microphone to them -- from the default microphone into a frame output node; at each of the graph's quanta its frame,
// 32-bit float, is made 16-bit and handed to the core (megler_microphone_push).
//
// The core's calls come on the app's own threads, which have no COM apartment; the graph is made and stopped on a thread
// of the multithreaded apartment while the caller waits. Quanta come on Windows' own threads.
//
// One MicrophoneHost serves the process: shared().
#pragma once

#include "megler.h"

namespace megler::app {

class MicrophoneHost {
public:
    static MicrophoneHost& shared();
    MicrophoneHost(const MicrophoneHost&) = delete;
    MicrophoneHost& operator=(const MicrophoneHost&) = delete;

    /// The callbacks for megler_run_config.microphone, with this as their user.
    megler_microphone_host callbacks();

private:
    MicrophoneHost() = default;
};

} // namespace megler::app
