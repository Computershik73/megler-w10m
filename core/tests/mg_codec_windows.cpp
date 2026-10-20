// The test program's way to the shell's decoders: meglet_codec_host as
// app/Meglet/player/CodecHost.cpp implements it over Media Foundation's
// decoder transforms, for mg_tests --codec-windows (mg_tests_main.c), which
// runs the codec's tests over real decoders (mg_codec_tests.c) with it.
//
// What that shows: the host's own work -- the decoders found and made, the
// input and output types set, samples in, pictures and sound out, a change of
// size followed, a flush, a drain -- with Windows' own decoders, and that the
// core's codec hands them what they take. What it cannot show: this is a PC's
// Windows, whose decoders are not a phone's (whether the phone has an H.265
// decoder, and how fast its H.264 one is, is found out there).
//
// Only tools/build-tests.sh win builds this file, with CodecHost.cpp itself
// and the shell's Strings.cpp beside it; the shell's log is stood in for by
// mg_storage_windows.cpp, which is built with them.
#include "pch.h"

#include "player/CodecHost.h"
#include "player/MediaHost.h"

#include <cstdio>

extern "C" {

/// The shell's decoders.
const meglet_codec_host* mg_codec_windows_host(void) {
    static meglet_codec_host host;
    host = meglet::app::CodecHost::shared().callbacks();
    return &host;
}

/// P010 rather than NV12 from decoders that offer both (CodecHost::preferTenBits).
void mg_codec_windows_ten_bits(int prefer) { meglet::app::CodecHost::shared().preferTenBits(prefer != 0); }

/// A decoder's call that does not return in time is given up on (CodecHost::checkCallLimit): 1 when it is.
int mg_codec_windows_call_limit(void) { return meglet::app::CodecHost::checkCallLimit() ? 1 : 0; }

/// The shell's reader of whole files (MediaHost), which a Java app's MediaPlayer and SoundPool hand MP3 and M4A files to.
const meglet_media_host* mg_media_windows_host(void) {
    static meglet_media_host host;
    host = meglet::app::MediaHost::shared().callbacks();
    return &host;
}

/// Which decoder Windows has for each kind, in a line.
void mg_codec_windows_summary(char* out, size_t size) { snprintf(out, size, "%s", meglet::app::CodecHost::shared().summary().c_str()); }

} // extern "C"
