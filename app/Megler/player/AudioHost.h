// A running app's sound: megler_audio_host (core/include/megler.h) over XAudio2
// 2.9, which every Windows 10 has (the phone's 1703+ included) and which
// needs no capability.
//
// The core's mixer hands over 16-bit stereo ten milliseconds at a time and
// runs as fast as write() lets it, so write() is the clock: it queues the
// block and returns, waiting first while more than about 50 ms is already
// queued. When there is nothing to play through -- the engine is not built
// yet, the app is paused, or the output device is gone and has not come back
// -- write() still takes each block's own time before dropping it, so an app
// that paces itself by its audio callbacks runs no faster for being silent.
//
// XAudio2 is built and destroyed on a thread of this class's own, which
// lives in COM's multithreaded apartment for as long as the engine does
// (as podium-w10m's output does, which runs on the same phones). Never
// on the thread that asked for sound: that is the app's own, running Android
// code, and building the engine takes a second and a half the first time.
// prepare() starts it as Play is pressed, so it is usually there before the
// app's first sound. It is built again after a critical error (the output
// changed: headphones, Bluetooth).
//
// What happens to the blocks is counted and said in the log (a second after
// the first, four seconds later, then every half minute): how many came, how
// loud, how many XAudio2 took and how many frames it says it has played.
// "Played" and not heard means the phone's volume or route, not this.
//
// XAudio2_9.dll is delay-loaded (tools/build-appx.sh), so a device without it
// gets "no sound" instead of an app that does not start.
//
// A media app's sound goes through Windows.Media's player instead, fed by the
// mixer through a MediaStreamSource (MediaOut): Windows takes such an app for
// a player and keeps it, and its sound, going in the background and with the
// screen off. It did not do so for XAudio2's Media category: Spotify on a
// Lumia 650 was suspended a moment after it was minimized, playing (653, 656),
// and an extended execution session kept it running unheard, for ten minutes
// (655). A media app is a Java app (setMedia) whose media session has said it
// plays (mediaSessionPlays: Spotify's, a podcast app's, YouTube's), from then
// on; not a call's sound, and not a game's or a feed's without a session,
// which keep XAudio2's shorter way to the speaker. The player's buffer is kept
// short (no buffer time, real-time playback), as the mixer's blocks are the
// app's clock. Should the player fail, the app's sound goes back to XAudio2
// for the rest of the run. Flags: audio-player has every Java app's sound go
// through the player from the start, audio-xaudio2 none.
//
// Like the WindowHost, one AudioHost lives from Play to the end of the
// process.
#pragma once

#include "megler.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace megler::app {

class AudioHost {
public:
    AudioHost();
    ~AudioHost();
    AudioHost(const AudioHost&) = delete;
    AudioHost& operator=(const AudioHost&) = delete;

    /// The callbacks for megler_run_config.audio, with this as their user.
    megler_audio_host callbacks();

    /// Any thread: starts building the output for this rate, if it is not
    /// built or being built already.
    void prepare(int rate);

    /// Any thread: the app is in the background (nothing is played, blocks
    /// are dropped at their own pace) or back.
    void setPaused(bool paused) { paused_.store(paused); }

    /// Before prepare: the output is a media app's (AudioCategory_Media, heard in the background) rather than a game's.
    void setMedia(bool media) { media_.store(media); }

    /// Any thread: the app's media session says it plays. A media app's sound goes to Windows.Media's player from now on.
    void mediaSessionPlays();

    /// Where a call's sound goes (megler_audio_host.route, MEGLER_AUDIO_ROUTE_*), the last any app asked: the microphone
    /// hears for a call (Communications, with the echo taken out) while it is not the default.
    static int callRoute();

    /// Whether a block with sound in it came from the app in the last `ms` milliseconds.
    bool audibleWithin(unsigned ms) const {
        const unsigned long long at = lastAudible_.load();
        return at != 0 && GetTickCount64() - at <= ms;
    }

    /// "44100 Hz", "silent so far", or why there is no sound.
    std::string status();

    /// The Device page's check: a second of a 440 Hz tone through an engine
    /// of its own, and what XAudio2 said it did with it, a line at a time.
    /// Blocks for that second; not for the UI thread. `level` is the tone's,
    /// of full scale.
    static void testTone(const std::function<void(const std::string&)>& line, double level = 0.25);

    struct Engine;
    struct MediaOut;

private:
    static bool openThunk(void* user, int rate);
    static void writeThunk(void* user, const int16_t* samples, int frames);
    static void routeThunk(void* user, int route);

    // The core's side: open on the thread that first played a sound, write
    // on the mixer's.
    bool open(int rate);
    void write(const int16_t* samples, int frames);
    void dropAtItsOwnPace(int frames);

    // The mixer's: a block to the player; false when it does not take it (the block is then dropped at its own pace).
    bool writeMedia(MediaOut& out, const int16_t* samples, int frames);

    // The owner thread's.
    void ownerLoop();
    void retire(std::shared_ptr<Engine>& engine);
    std::shared_ptr<MediaOut> makeMedia(int rate, std::string& why);
    void retireMedia(std::shared_ptr<MediaOut>& out);
    void logCounts();
    void setStatus(const std::string& text);

    std::mutex mutex_; // engine_, quit_, and the owner thread's start
    std::condition_variable wake_;
    /// Made and let go of by the owner thread; write() plays through a
    /// reference of its own.
    std::shared_ptr<Engine> engine_;
    /// A media app's player, in place of the engine (made and let go of by the owner thread, as the engine).
    std::shared_ptr<MediaOut> mediaOut_;
    /// The player failed, or could not be made: XAudio2 for the rest of the run.
    std::atomic<bool> mediaFailed_{false};
    /// The app's media session has said it plays (mediaSessionPlays).
    std::atomic<bool> sessionPlayed_{false};
    std::thread owner_;
    bool quit_ = false;
    std::atomic<int> rate_{44100};
    /// The route asked (MEGLER_AUDIO_ROUTE_*): the engine is made again when it is not the one it was made for.
    std::atomic<int> route_{0};
    bool routedOnce_ = false; // the owner thread's: a call's route has been asked of the phone
    /// Where the mixer's clock stands while its blocks are dropped (QPC
    /// ticks; 0: they are being played). write()'s alone.
    long long paceDeadline_ = 0;
    std::atomic<bool> paused_{false};
    std::atomic<bool> media_{false};
    std::atomic<unsigned long long> lastAudible_{0};
    /// IXAudio2EngineCallback::OnCriticalError, on XAudio2's thread.
    std::atomic<bool> lost_{false};
    std::atomic<long> lostCode_{0};

    // What became of the mixer's blocks.
    std::atomic<uint64_t> blocksIn_{0}, blocksSilent_{0}, blocksQueued_{0}, blocksDropped_{0}, blocksRefused_{0};
    std::atomic<int> peak_{0}; // since the last line in the log
    // The owner thread's, for those lines.
    unsigned long long countsFirstAt_ = 0, countsNextAt_ = 0;
    uint64_t countsLoggedIn_ = 0;

    std::mutex statusMutex_;
    std::string status_ = "silent so far";
};

} // namespace megler::app
