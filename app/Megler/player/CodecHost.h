// A Java app's MediaCodec: megler_codec_host (core/include/megler.h) over Media
// Foundation's decoder transforms, the synchronous ones Windows itself has
// (MFTEnumEx, IMFTransform's ProcessInput and ProcessOutput; all of it is
// there for an app on Windows 10 Mobile 1703).
//
// The core hands over one sample at a time, already in the form a transform
// takes: H.264 and H.265 access units in Annex-B with the parameter sets in
// front of the first, raw AAC with its AudioSpecificConfig, MP3 frames
// (core/src/drivers/hk_java_media.c). Each push is decoded at once, and what
// comes out is kept here until the core pulls it:
// - a picture as the decoder wrote it, NV12, in the order the decoder hands
//   them out, which for Media Foundation's decoders is display order. Its
//   buffer is this host's own and is held, not copied, until the core lets
//   the picture go (a decoder that brings buffers of its own has them copied
//   out, so that its pool is not drained; so is P010, which a decoder of
//   ten-bit streams may offer alone, as NV12 with the high eight bits);
// - sound as 16-bit PCM, two channels at most (a decoder asked for stereo
//   where it can downmix, the first two channels taken where it cannot).
// A decoder's change of picture size or sound format
// (MF_E_TRANSFORM_STREAM_CHANGE) is followed: the output type is chosen again
// and the next pictures say their new size.
//
// Each decoder has a thread of its own in COM's multithreaded apartment, where
// all of its Media Foundation calls are made: the threads that ask are the
// app's, running Android code on stacks of the size the app chose (as
// MediaHost.h says), and two decoders -- an app's picture and its sound --
// then decode at the same time. Each call waits for that thread; pulls and
// releases do not go to it.
//
// mfplat.dll is delay-loaded (tools/build-appx.sh): a device without it lists
// no decoder instead of not starting.
//
// One CodecHost serves the process: shared().
#pragma once

#include "megler.h"

#include <string>

namespace megler::app {

class CodecHost {
public:
    static CodecHost& shared();
    CodecHost(const CodecHost&) = delete;
    CodecHost& operator=(const CodecHost&) = delete;

    /// The callbacks for megler_run_config.codec, with this as their user.
    megler_codec_host callbacks();
    /// Which decoder each kind would have, by name, in a line ("H.264: Microsoft H264 Video Decoder MFT; H.265: none; ...").
    std::string summary();
    /// For the tests: P010 rather than NV12 from a decoder that offers both, for decoders opened after this, so that the
    /// way taken for one that offers P010 alone is gone through on a PC whose decoders offer NV12 too.
    void preferTenBits(bool prefer);

    struct Codec;
    struct Picture;

private:
    CodecHost() = default;

    static bool supportsThunk(void* user, int kind);
    static void* openThunk(void* user, int kind, const megler_codec_format* format);
    static bool pushThunk(void* user, void* codec, const uint8_t* data, size_t size, int64_t timeUs);
    static void drainThunk(void* user, void* codec);
    static void* pullPictureThunk(void* user, void* codec, megler_codec_picture* picture);
    static void releasePictureThunk(void* user, void* picture);
    static bool pullSoundThunk(void* user, void* codec, megler_codec_sound* sound);
    static void flushThunk(void* user, void* codec);
    static void closeThunk(void* user, void* codec);
};

} // namespace megler::app
