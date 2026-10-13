#include "pch.h"

#include "player/MediaHost.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace megler::app {

namespace {

constexpr DWORD videoStream = DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM);
constexpr DWORD audioStream = DWORD(MF_SOURCE_READER_FIRST_AUDIO_STREAM);

/// A delay-loaded DLL that is not there raises VcppException(
/// ERROR_SEVERITY_ERROR, ERROR_MOD_NOT_FOUND or ERROR_PROC_NOT_FOUND) at the
/// first call into it. (No C++ objects in these frames, for __try.)
bool missingDll(DWORD code) { return code == 0xC06D007E || code == 0xC06D007F; }

HRESULT startMediaFoundation() {
    __try {
        return MFStartup(MF_VERSION, MFSTARTUP_LITE);
    } __except (missingDll(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }
}

HRESULT createReader(const wchar_t* url, IMFAttributes* attributes, IMFSourceReader** out) {
    __try {
        return MFCreateSourceReaderFromURL(url, attributes, out);
    } __except (missingDll(GetExceptionCode()) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        return HRESULT_FROM_WIN32(ERROR_MOD_NOT_FOUND);
    }
}

/// GUID_NULL, which lives in a library this does not link (uuid.lib).
const GUID noTimeFormat = {};

uint8_t clamp8(int v) { return uint8_t(v < 0 ? 0 : v > 255 ? 255 : v); }

} // namespace

/// One film: a reader for the picture and one for the sound, and what the
/// core was told about them. The worker thread's alone.
struct MediaHost::Film {
    std::wstring url;
    winrt::com_ptr<IMFSourceReader> video, audio;

    // What the core was told at open, and writes its buffers for.
    int width = 0, height = 0;
    int rate = 0, channels = 0;
    int rateNum = 30, rateDen = 1; // pictures a second
    int64_t frameUs = 33333;

    // What the picture's reader puts out now (it may say otherwise after the
    // first sample: the decoder learns the coded size from the stream).
    int frameWidth = 0, frameHeight = 0;
    int cropX = 0, cropY = 0;
    int stride = 0;    // of a buffer that is not an IMF2DBuffer; negative: bottom row first
    bool nv12 = false; // else RGB32
    bool hardware = false;

    // Sound decoded and not yet handed over, and the time of its first frame.
    std::vector<int16_t> pending;
    size_t pendingAt = 0;
    int64_t pendingTimeUs = 0;
    bool audioEnded = false;

    // After a seek the readers start at the key frame before the place asked
    // for: what comes before that place is dropped.
    int64_t skipVideoUntilUs = -1, skipAudioUntilUs = -1;
    uint64_t pictures = 0;

    /// A reader on the file with one stream selected; null and `why` when
    /// there is no such stream or it cannot be decoded.
    static winrt::com_ptr<IMFSourceReader> makeReader(const std::wstring& url, bool video, bool hardware, std::string& why) {
        winrt::com_ptr<IMFAttributes> attributes;
        winrt::com_ptr<IMFSourceReader> reader;
        HRESULT hr = MFCreateAttributes(attributes.put(), 2);
        if (FAILED(hr)) {
            why = format("MFCreateAttributes 0x%08X", unsigned(hr));
            return nullptr;
        }
        if (video) {
            // The decoder's YUV to RGB32, and any size it rounds up to cut back to the picture's.
            attributes->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
            if (hardware) attributes->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        }
        hr = createReader(url.c_str(), attributes.get(), reader.put());
        if (FAILED(hr)) {
            why = format("the file cannot be opened (0x%08X)", unsigned(hr));
            return nullptr;
        }
        reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        hr = reader->SetStreamSelection(video ? videoStream : audioStream, TRUE);
        if (FAILED(hr)) {
            why = format("no %s in the file (0x%08X)", video ? "picture" : "sound", unsigned(hr));
            return nullptr;
        }
        return reader;
    }

    static HRESULT setOutput(IMFSourceReader* reader, DWORD stream, const GUID& major, const GUID& subtype) {
        winrt::com_ptr<IMFMediaType> type;
        HRESULT hr = MFCreateMediaType(type.put());
        if (FAILED(hr)) return hr;
        type->SetGUID(MF_MT_MAJOR_TYPE, major);
        type->SetGUID(MF_MT_SUBTYPE, subtype);
        return reader->SetCurrentMediaType(stream, nullptr, type.get());
    }

    /// The size, crop and row order of what the picture's reader puts out.
    bool readVideoType() {
        winrt::com_ptr<IMFMediaType> type;
        if (FAILED(video->GetCurrentMediaType(videoStream, type.put()))) return false;
        UINT32 w = 0, h = 0;
        if (FAILED(MFGetAttributeSize(type.get(), MF_MT_FRAME_SIZE, &w, &h)) || !w || !h || w > 8192 || h > 8192) return false;
        GUID subtype = {};
        type->GetGUID(MF_MT_SUBTYPE, &subtype);
        nv12 = subtype == MFVideoFormat_NV12;
        frameWidth = int(w);
        frameHeight = int(h);
        cropX = cropY = 0;
        int visibleW = frameWidth, visibleH = frameHeight;
        MFVideoArea area = {};
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE, reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr)) &&
            area.Area.cx > 0 && area.Area.cy > 0 && area.OffsetX.value >= 0 && area.OffsetY.value >= 0 &&
            area.OffsetX.value + area.Area.cx <= frameWidth && area.OffsetY.value + area.Area.cy <= frameHeight) {
            cropX = area.OffsetX.value;
            cropY = area.OffsetY.value;
            visibleW = area.Area.cx;
            visibleH = area.Area.cy;
        }
        stride = int(INT32(MFGetAttributeUINT32(type.get(), MF_MT_DEFAULT_STRIDE, 0)));
        // Not said: what Windows takes such a type to mean, which for RGB is the bottom row first.
        if (!stride) stride = nv12 ? frameWidth : -frameWidth * 4;
        if (!width) {
            width = visibleW;
            height = visibleH;
        }
        return true;
    }

    /// Null, and why, when Windows cannot play the file.
    static Film* open(const std::wstring& url, std::string& why) {
        auto film = std::make_unique<Film>();
        film->url = url;
        // The software decoder first: these are small pictures, and a phone's
        // hardware decoder is at its best with a Direct3D device this has none of.
        for (int attempt = 0; attempt < 2 && !film->video; attempt++) {
            std::string whyNot;
            auto reader = makeReader(url, true, attempt == 1, whyNot);
            HRESULT hr = E_FAIL;
            if (reader) {
                hr = setOutput(reader.get(), videoStream, MFMediaType_Video, MFVideoFormat_RGB32);
                if (FAILED(hr)) {
                    const HRESULT rgb = hr;
                    hr = setOutput(reader.get(), videoStream, MFMediaType_Video, MFVideoFormat_NV12);
                    if (FAILED(hr)) whyNot = format("its picture cannot be decoded (RGB32 0x%08X, NV12 0x%08X)", unsigned(rgb), unsigned(hr));
                }
            }
            if (SUCCEEDED(hr)) {
                film->video = reader;
                film->hardware = attempt == 1;
                film->width = film->height = 0;
                if (!film->readVideoType()) {
                    whyNot = "its decoder would not say the picture's size";
                    film->video = nullptr;
                }
            }
            if (!film->video) {
                shellLog::write(format("media: %s decoder: %s", attempt ? "with hardware decoders allowed" : "software", whyNot.c_str()));
                why = whyNot;
            }
        }
        if (!film->video) return nullptr;

        winrt::com_ptr<IMFMediaType> native;
        UINT32 num = 0, den = 0;
        if (SUCCEEDED(film->video->GetNativeMediaType(videoStream, 0, native.put())) &&
            SUCCEEDED(MFGetAttributeRatio(native.get(), MF_MT_FRAME_RATE, &num, &den)) && num && den && num / den <= 240) {
            film->frameUs = int64_t(1000000) * den / num;
        } else {
            num = 30;
            den = 1;
        }
        film->rateNum = int(num);
        film->rateDen = int(den);

        std::string noSound;
        film->audio = makeReader(url, false, false, noSound);
        if (film->audio) {
            winrt::com_ptr<IMFMediaType> type;
            const HRESULT hr = setOutput(film->audio.get(), audioStream, MFMediaType_Audio, MFAudioFormat_PCM);
            UINT32 bits = 0;
            if (SUCCEEDED(hr) && SUCCEEDED(film->audio->GetCurrentMediaType(audioStream, type.put()))) {
                film->rate = int(MFGetAttributeUINT32(type.get(), MF_MT_AUDIO_SAMPLES_PER_SECOND, 0));
                film->channels = int(MFGetAttributeUINT32(type.get(), MF_MT_AUDIO_NUM_CHANNELS, 0));
                bits = MFGetAttributeUINT32(type.get(), MF_MT_AUDIO_BITS_PER_SAMPLE, 0);
            }
            if (FAILED(hr) || bits != 16 || film->rate <= 0 || film->channels <= 0 || film->channels > 8) {
                noSound = format("its sound cannot be decoded to 16-bit PCM (0x%08X, %u bits, %d Hz, %d channels)", unsigned(hr), unsigned(bits),
                                 film->rate, film->channels);
                film->audio = nullptr;
            }
        }
        if (!film->audio) {
            film->rate = film->channels = 0;
            shellLog::write("media: the film plays without sound: " + noSound);
        }
        return film.release();
    }

    int64_t durationUs() const {
        PROPVARIANT value;
        PropVariantInit(&value);
        int64_t us = 0;
        if (SUCCEEDED(video->GetPresentationAttribute(DWORD(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &value)) && value.vt == VT_UI8)
            us = int64_t(value.uhVal.QuadPart / 10);
        PropVariantClear(&value);
        return us;
    }

    /// One row-ordered copy of the picture's visible part into the core's RGBA.
    void copyPicture(const uint8_t* top, int pitch, uint8_t* rgba) const {
        const int w = std::min(width, frameWidth - cropX), h = std::min(height, frameHeight - cropY);
        if (w < width || h < height) memset(rgba, 0, size_t(width) * size_t(height) * 4);
        if (!nv12) {
            for (int y = 0; y < h; y++) {
                const uint8_t* s = top + ptrdiff_t(cropY + y) * pitch + size_t(cropX) * 4;
                uint8_t* d = rgba + size_t(y) * size_t(width) * 4;
                for (int x = 0; x < w; x++, s += 4, d += 4) {
                    d[0] = s[2];
                    d[1] = s[1];
                    d[2] = s[0];
                    d[3] = 255;
                }
            }
            return;
        }
        // NV12 (a decoder with nothing to convert for it): the brightness, then rows of U and V pairs at half size. BT.601, video range.
        const uint8_t* chroma = top + ptrdiff_t(frameHeight) * pitch;
        for (int y = 0; y < h; y++) {
            const int sy = cropY + y;
            const uint8_t* ys = top + ptrdiff_t(sy) * pitch + cropX;
            const uint8_t* uv = chroma + ptrdiff_t(sy / 2) * pitch;
            uint8_t* d = rgba + size_t(y) * size_t(width) * 4;
            for (int x = 0; x < w; x++, d += 4) {
                const int sx = cropX + x;
                const int c = 298 * (int(ys[x]) - 16), u = int(uv[sx & ~1]) - 128, v = int(uv[(sx & ~1) + 1]) - 128;
                d[0] = clamp8((c + 409 * v + 128) >> 8);
                d[1] = clamp8((c - 100 * u - 208 * v + 128) >> 8);
                d[2] = clamp8((c + 516 * u + 128) >> 8);
                d[3] = 255;
            }
        }
    }

    /// 1 and the next picture, 0 at the end, -1 when the decoder fails.
    int readVideo(uint8_t* rgba, int64_t* timeUs) {
        for (int tries = 0; tries < 2000; tries++) {
            DWORD flags = 0;
            LONGLONG time = 0;
            winrt::com_ptr<IMFSample> sample;
            const HRESULT hr = video->ReadSample(videoStream, 0, nullptr, &flags, &time, sample.put());
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
                shellLog::write(format("media: the picture's decoder failed after %llu pictures (0x%08X, flags 0x%X)", pictures, unsigned(hr),
                                       unsigned(flags)));
                return -1;
            }
            if (flags & MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED) setOutput(video.get(), videoStream, MFMediaType_Video, nv12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
            if (flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) {
                readVideoType();
                shellLog::write(format("media: the decoder's pictures are now %dx%d (shown %dx%d from %d,%d), %s", frameWidth, frameHeight, width, height,
                                       cropX, cropY, nv12 ? "NV12" : "RGB32"));
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) return 0;
            if (!sample) continue; // a gap in the stream
            const int64_t us = time / 10;
            if (skipVideoUntilUs >= 0 && us + frameUs / 2 < skipVideoUntilUs) continue;
            skipVideoUntilUs = -1;

            winrt::com_ptr<IMFMediaBuffer> buffer;
            if (FAILED(sample->ConvertToContiguousBuffer(buffer.put()))) return -1;
            bool copied = false;
            if (auto planar = buffer.try_as<IMF2DBuffer>()) {
                BYTE* top = nullptr;
                LONG pitch = 0;
                if (SUCCEEDED(planar->Lock2D(&top, &pitch)) && top) {
                    copyPicture(top, int(pitch), rgba);
                    planar->Unlock2D();
                    copied = true;
                }
            }
            if (!copied) {
                BYTE* data = nullptr;
                DWORD length = 0;
                if (FAILED(buffer->Lock(&data, nullptr, &length)) || !data) return -1;
                const size_t rowBytes = size_t(stride < 0 ? -stride : stride);
                const size_t rows = nv12 ? size_t(frameHeight) * 3 / 2 : size_t(frameHeight);
                if (rowBytes && length >= rowBytes * rows) {
                    copyPicture(stride < 0 ? data + rowBytes * size_t(frameHeight - 1) : data, stride, rgba);
                    copied = true;
                }
                buffer->Unlock();
                if (!copied) {
                    shellLog::write(format("media: a picture of %u bytes is not %dx%d with rows of %d", unsigned(length), frameWidth, frameHeight, stride));
                    return -1;
                }
            }
            if (pictures == 0)
                shellLog::write(format("media: the first picture, at %lld ms: %dx%d %s from the %s decoder", (long long)(us / 1000), frameWidth, frameHeight,
                                       nv12 ? "NV12" : "RGB32", hardware ? "hardware" : "software"));
            pictures++;
            *timeUs = us;
            return 1;
        }
        return -1;
    }

    /// How many frames of sound were written (up to `maxFrames`), 0 at the end, -1 when the decoder fails.
    int readAudio(int16_t* out, int maxFrames, int64_t* timeUs) {
        if (!audio || maxFrames <= 0) return 0;
        const size_t want = size_t(maxFrames) * size_t(channels);
        while (pending.size() - pendingAt < want && !audioEnded) {
            DWORD flags = 0;
            LONGLONG time = 0;
            winrt::com_ptr<IMFSample> sample;
            const HRESULT hr = audio->ReadSample(audioStream, 0, nullptr, &flags, &time, sample.put());
            if (FAILED(hr) || (flags & MF_SOURCE_READERF_ERROR)) {
                shellLog::write(format("media: the sound's decoder failed (0x%08X, flags 0x%X)", unsigned(hr), unsigned(flags)));
                return -1;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                audioEnded = true;
                break;
            }
            if (!sample) continue;
            winrt::com_ptr<IMFMediaBuffer> buffer;
            BYTE* data = nullptr;
            DWORD length = 0;
            if (FAILED(sample->ConvertToContiguousBuffer(buffer.put())) || FAILED(buffer->Lock(&data, nullptr, &length))) return -1;
            const size_t frame = size_t(channels) * 2;
            size_t skip = 0; // whole frames of this sample that come before the place sought
            int64_t us = time / 10;
            if (skipAudioUntilUs >= 0) {
                if (us < skipAudioUntilUs) skip = size_t((skipAudioUntilUs - us) * rate / 1000000);
                if (skip * frame >= length) skip = length / frame;
                else skipAudioUntilUs = -1;
                us += int64_t(skip) * 1000000 / rate;
            }
            const size_t values = (length / frame - skip) * size_t(channels);
            if (values) {
                if (pendingAt == pending.size()) {
                    pending.clear();
                    pendingAt = 0;
                    pendingTimeUs = us;
                } else if (pendingAt > 65536) {
                    pending.erase(pending.begin(), pending.begin() + ptrdiff_t(pendingAt));
                    pendingAt = 0;
                }
                const int16_t* from = reinterpret_cast<const int16_t*>(data + skip * frame);
                pending.insert(pending.end(), from, from + values);
            }
            buffer->Unlock();
        }
        const size_t have = std::min(want, pending.size() - pendingAt);
        const int frames = int(have / size_t(channels));
        if (!frames) return 0;
        memcpy(out, pending.data() + pendingAt, size_t(frames) * size_t(channels) * 2);
        *timeUs = pendingTimeUs;
        pendingAt += size_t(frames) * size_t(channels);
        pendingTimeUs += int64_t(frames) * 1000000 / rate;
        return frames;
    }

    bool seek(int64_t timeUs) {
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = timeUs * 10;
        const HRESULT hr = video->SetCurrentPosition(noTimeFormat, position);
        if (audio) audio->SetCurrentPosition(noTimeFormat, position);
        pending.clear();
        pendingAt = 0;
        audioEnded = false;
        skipVideoUntilUs = skipAudioUntilUs = timeUs > 0 ? timeUs : -1;
        if (FAILED(hr)) shellLog::write(format("media: the film would not go to %lld ms (0x%08X)", (long long)(timeUs / 1000), unsigned(hr)));
        return SUCCEEDED(hr);
    }
};

MediaHost& MediaHost::shared() {
    // Never destroyed: an app's threads may still be in a call when the process ends.
    static MediaHost* const host = new MediaHost();
    return *host;
}

MediaHost::~MediaHost() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        quit_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

megler_media_host MediaHost::callbacks() {
    megler_media_host host = {};
    host.user = this;
    host.open = &openThunk;
    host.read_video = &readVideoThunk;
    host.read_audio = &readAudioThunk;
    host.seek = &seekThunk;
    host.close = &closeThunk;
    return host;
}

void MediaHost::run(const std::function<void()>& work) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!worker_.joinable()) worker_ = std::thread([this] { workerLoop(); });
    idle_.wait(lock, [this] { return !work_; });
    bool finished = false;
    work_ = &work;
    finished_ = &finished;
    wake_.notify_one();
    done_.wait(lock, [&finished] { return finished; });
}

void MediaHost::workerLoop() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return quit_ || work_; });
        if (quit_) break;
        const std::function<void()>* work = work_;
        lock.unlock();
        (*work)();
        lock.lock();
        work_ = nullptr;
        *finished_ = true;
        finished_ = nullptr;
        done_.notify_all();
        idle_.notify_one();
    }
    lock.unlock();
    if (startState_ > 0) MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
}

bool MediaHost::started() {
    if (!startState_) {
        const HRESULT hr = startMediaFoundation();
        startState_ = SUCCEEDED(hr) ? 1 : -1;
        if (FAILED(hr)) shellLog::write(format("media: Media Foundation would not start (0x%08X): films cannot be played", unsigned(hr)));
    }
    return startState_ > 0;
}

void* MediaHost::openThunk(void* user, const char* path, megler_media_info* info) {
    auto* self = static_cast<MediaHost*>(user);
    Film* film = nullptr;
    self->run([&] {
        if (!self->started()) return;
        std::wstring url = widen(path ? path : "");
        std::replace(url.begin(), url.end(), L'/', L'\\');
        std::string why;
        film = Film::open(url, why);
        if (!film) {
            shellLog::write("media: Windows cannot play this film: " + why);
            return;
        }
        info->width = film->width;
        info->height = film->height;
        info->frame_rate_num = film->rateNum;
        info->frame_rate_den = film->rateDen;
        info->sample_rate = film->rate;
        info->channels = film->channels;
        info->duration_us = film->durationUs();
        shellLog::write(format("media: opened a film of %dx%d (decoded as %dx%d %s by the %s decoder), %d/%d frames a second, sound %d Hz in %d channels",
                               film->width, film->height, film->frameWidth, film->frameHeight, film->nv12 ? "NV12" : "RGB32",
                               film->hardware ? "hardware" : "software", film->rateNum, film->rateDen, film->rate, film->channels));
    });
    return film;
}

int MediaHost::readVideoThunk(void* user, void* media, uint8_t* rgba, int64_t* timeUs) {
    int result = -1;
    static_cast<MediaHost*>(user)->run([&] { result = static_cast<Film*>(media)->readVideo(rgba, timeUs); });
    return result;
}

int MediaHost::readAudioThunk(void* user, void* media, int16_t* samples, int maxFrames, int64_t* timeUs) {
    int result = -1;
    static_cast<MediaHost*>(user)->run([&] { result = static_cast<Film*>(media)->readAudio(samples, maxFrames, timeUs); });
    return result;
}

bool MediaHost::seekThunk(void* user, void* media, int64_t timeUs) {
    bool result = false;
    static_cast<MediaHost*>(user)->run([&] { result = static_cast<Film*>(media)->seek(timeUs); });
    return result;
}

void MediaHost::closeThunk(void* user, void* media) {
    // The readers are let go of where they were made.
    static_cast<MediaHost*>(user)->run([&] { delete static_cast<Film*>(media); });
}

} // namespace megler::app
