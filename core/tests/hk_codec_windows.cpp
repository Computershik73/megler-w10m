// The test program's way to the shell's decoders: megler_codec_host as
// app/Megler/player/CodecHost.cpp implements it over Media Foundation's
// decoder transforms, for hk_tests --codec-windows (hk_tests_main.c), which
// runs the codec's tests over real decoders (hk_codec_tests.c) with it.
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
// hk_storage_windows.cpp, which is built with them.
#include "pch.h"

#include "player/CodecHost.h"

#include <cstdio>

extern "C" {

/// The shell's decoders.
const megler_codec_host* hk_codec_windows_host(void) {
    static megler_codec_host host;
    host = megler::app::CodecHost::shared().callbacks();
    return &host;
}

/// Which decoder Windows has for each kind, in a line.
void hk_codec_windows_summary(char* out, size_t size) { snprintf(out, size, "%s", megler::app::CodecHost::shared().summary().c_str()); }

} // extern "C"
