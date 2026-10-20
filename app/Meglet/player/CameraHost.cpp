#include "pch.h"

#include "player/CameraHost.h"

#include "shell/Files.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Capture.Frames.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Devices.h>
#include <winrt/Windows.Media.MediaProperties.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <map>
#include <sstream>

// The interface a locked buffer's memory is reached through, declared here by hand as StorageHost.cpp declares its own:
// <MemoryBuffer.h>'s, Windows 10 SDK 10.0.10240 on.
namespace {
struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable) IMemoryBufferByteAccess : ::IUnknown {
    virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};
} // namespace

namespace meglet::app {

using namespace winrt::Windows::Media::Capture;
using namespace winrt::Windows::Media::Capture::Frames;
using winrt::Windows::Devices::Enumeration::DeviceAccessInformation;
using winrt::Windows::Devices::Enumeration::DeviceAccessStatus;
using winrt::Windows::Devices::Enumeration::DeviceClass;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::EnclosureLocation;
using winrt::Windows::Devices::Enumeration::Panel;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncAction;
using winrt::Windows::Graphics::Imaging::BitmapBufferAccessMode;
using winrt::Windows::Graphics::Imaging::BitmapPixelFormat;
using winrt::Windows::Graphics::Imaging::SoftwareBitmap;
using winrt::Windows::Media::Devices::AutoFocusRange;
using winrt::Windows::Media::Devices::FocusMode;
using winrt::Windows::Media::Devices::FocusSettings;

namespace {

/// How long a call of Windows' may take before the app's thread is let go: far longer than any that returns. Opening has a
/// limit of its own, the user's answer to Windows' question being part of it.
constexpr std::chrono::seconds callLimit{15};
constexpr std::chrono::seconds openLimit{180};

/// A thread in COM's multithreaded apartment that runs one piece of work at a time for whoever waits for it. A piece of
/// work that has not returned within its limit is left to the thread: that wait answers false, and so does every later one,
/// at once (CodecHost.cpp's worker, which a decoder of the phone's that never returned taught). What the work uses must
/// outlive the caller's wait: captured by value, or shared.
class Worker {
public:
    Worker() : thread_([this] { loop(); }) {}
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    bool run(std::function<void()> work, std::chrono::milliseconds limit) {
        auto job = std::make_shared<Job>();
        job->work = std::move(work);
        std::unique_lock<std::mutex> lock(mutex_);
        if (stuck_) return false;
        idle_.wait(lock, [this] { return !job_ || stuck_; });
        if (stuck_) return false;
        job_ = job;
        wake_.notify_all();
        if (!done_.wait_for(lock, limit, [&job] { return job->finished; })) {
            stuck_ = true;
            idle_.notify_all();
            return false;
        }
        return true;
    }

private:
    struct Job {
        std::function<void()> work;
        bool finished = false;
    };

    void loop() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::unique_lock<std::mutex> lock(mutex_);
        for (;;) {
            wake_.wait(lock, [this] { return job_ != nullptr; });
            std::shared_ptr<Job> job = job_;
            lock.unlock();
            try {
                job->work();
            } catch (const winrt::hresult_error& e) {
                shellLog::write(format("camera: an unexpected error of Windows' (0x%08X): %s", unsigned(e.code()), narrow(e.message()).c_str()));
            } catch (...) {
                shellLog::write("camera: an unexpected exception");
            }
            lock.lock();
            job->finished = true;
            job_ = nullptr;
            done_.notify_all();
            idle_.notify_one();
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }

    std::mutex mutex_;
    std::condition_variable wake_, done_, idle_;
    std::shared_ptr<Job> job_;
    bool stuck_ = false;
    std::thread thread_;
};

Worker& worker() {
    // Never destroyed: an app's thread may be in a call when the process ends.
    static Worker* const w = new Worker();
    return *w;
}

const char* panelName(Panel p) {
    switch (p) {
    case Panel::Front: return "front";
    case Panel::Back: return "back";
    case Panel::Top: return "top";
    case Panel::Bottom: return "bottom";
    case Panel::Left: return "left";
    case Panel::Right: return "right";
    default: return "unknown";
    }
}

const char* accessName(DeviceAccessStatus s) {
    switch (s) {
    case DeviceAccessStatus::Allowed: return "allowed";
    case DeviceAccessStatus::DeniedByUser: return "denied by the user";
    case DeviceAccessStatus::DeniedBySystem: return "denied by the system";
    default: return "not yet asked";
    }
}

/// What Windows' privacy setting for the camera says of this app, for the log.
std::string cameraAccess() {
    try {
        return accessName(DeviceAccessInformation::CreateFromDeviceClass(DeviceClass::VideoCapture).CurrentStatus());
    } catch (const winrt::hresult_error& e) {
        return format("not known (0x%08X)", unsigned(e.code()));
    }
}

/// meglet.h's word for why MediaCapture would not open: the user's or the system's no, something else holding the camera, or anything else.
int errorOf(HRESULT hr) {
    switch (unsigned(hr)) {
    case 0x80070005u: return MEGLET_CAMERA_DENIED;     // E_ACCESSDENIED: the consent refused, or the capability missing
    case 0xC00D3704u:                                // MF_E_HW_MFT_FAILED_START_STREAMING: another app streams from it
    case 0xC00DABE4u:                                // MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED
    case 0x80070020u:                                // ERROR_SHARING_VIOLATION
    case 0x800700AAu: return MEGLET_CAMERA_BUSY;       // ERROR_BUSY
    default: return MEGLET_CAMERA_FAILED;
    }
}

std::string hresultText(const winrt::hresult_error& e) {
    std::string text = narrow(e.message());
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return format("0x%08X%s%s", unsigned(e.code()), text.empty() ? "" : " ", text.c_str());
}

/// A camera as found: the source group and colour stream it is (or the device, where there are no groups), and what it is.
struct Device {
    winrt::hstring group, source, device;
    std::string id, name;
    Panel panel = Panel::Unknown;
    uint32_t angle = 0;
    bool angleKnown = false;
    int facing = MEGLET_CAMERA_BACK, orientation = 0;
    int focus = -1, light = -1;
};

} // namespace

struct CameraHost::Camera {
    int index = 0;
    MediaCapture capture{nullptr};
    MediaFrameSource source{nullptr};
    MediaFrameReader reader{nullptr};
    winrt::event_token arrived{}, failed{};
    bool focusControl = false, continuousFocus = false, macro = false, torch = false, torchOn = false;
    // The picture's way to the core: one call at a time, and none once `streaming` has gone false under the lock.
    std::mutex frameMutex;
    bool streaming = false, gone = false;
    meglet_camera_frame_fn frame = nullptr;
    void* core = nullptr;
    uint64_t frames = 0, converted = 0;
    bool firstSaid = false, fromSurfaceSaid = false;
    // Focusing: what is under way answers only if nothing has been asked since (`focusAsked`).
    std::mutex focusMutex;
    unsigned focusAsked = 0;
    meglet_camera_focus_fn done = nullptr;
    void* doneCore = nullptr;
};

namespace {

std::mutex gMutex; // everything below
bool gSearched = false;
std::vector<Device> gDevices;
std::wstring gStateFile, gFlagFolder;
bool gPortraitNative = false;

/// LocalState\camera.txt: "<focus> <light> <id>" a line, what was seen when each camera was last opened.
void readState(std::vector<Device>& devices) {
    std::string text;
    if (gStateFile.empty() || !files::read(gStateFile, text)) return;
    std::istringstream lines(text);
    std::string line;
    while (std::getline(lines, line)) {
        int focus = -1, light = -1;
        char id[512] = {};
        if (sscanf(line.c_str(), "%d %d %511[^\r\n]", &focus, &light, id) != 3) continue;
        for (Device& d : devices)
            if (d.id == id) d.focus = focus, d.light = light;
    }
}

/// With gMutex held.
void writeState() {
    if (gStateFile.empty()) return;
    std::string text;
    for (const Device& d : gDevices)
        if (d.focus >= 0) text += format("%d %d %s\n", d.focus, d.light, d.id.c_str());
    files::write(gStateFile, text);
}

/// Android's number for how far a camera's picture is turned, from where Windows says the camera is.
void placeDevice(Device& d) {
    d.facing = d.panel == Panel::Front ? MEGLET_CAMERA_FRONT : MEGLET_CAMERA_BACK;
    const bool inside = d.panel == Panel::Front || d.panel == Panel::Back;
    if (!inside) {
        d.orientation = 0;
    } else if (d.facing == MEGLET_CAMERA_BACK) {
        // On a device upright by nature the sensor is read a quarter turn off it, the long side along the screen's long side.
        d.orientation = int(((gPortraitNative ? 90u : 0u) + d.angle) % 360u);
    } else {
        // A front camera looks the other way, so the same mounting turns its picture the other way round.
        d.orientation = int((360u - ((gPortraitNative ? 90u : 0u) + d.angle) % 360u) % 360u);
    }
}

std::vector<Device> findGroups() {
    std::vector<Device> found;
    for (const MediaFrameSourceGroup& g : MediaFrameSourceGroup::FindAllAsync().get()) {
        MediaFrameSourceInfo best{nullptr};
        for (const MediaFrameSourceInfo& info : g.SourceInfos()) {
            if (info.SourceKind() != MediaFrameSourceKind::Color) continue;
            const MediaStreamType type = info.MediaStreamType();
            if (type != MediaStreamType::VideoPreview && type != MediaStreamType::VideoRecord) continue;
            if (!best || (type == MediaStreamType::VideoPreview && best.MediaStreamType() != MediaStreamType::VideoPreview)) best = info;
        }
        if (!best) continue;
        Device d;
        d.group = g.Id();
        d.source = best.Id();
        d.name = narrow(g.DisplayName());
        DeviceInformation info = best.DeviceInformation();
        d.id = narrow(info ? info.Id() : g.Id());
        EnclosureLocation where = info ? info.EnclosureLocation() : nullptr;
        if (where) {
            d.panel = where.Panel();
            try {
                d.angle = where.RotationAngleInDegreesClockwise();
                d.angleKnown = true;
            } catch (const winrt::hresult_error&) {
            }
        }
        // One camera may be in several groups (its own, and one with a depth sensor beside it): the first is kept.
        bool seen = false;
        for (const Device& o : found) seen |= o.id == d.id;
        if (!seen) found.push_back(std::move(d));
    }
    return found;
}

std::vector<Device> findDevices() {
    std::vector<Device> found;
    for (const DeviceInformation& info : DeviceInformation::FindAllAsync(DeviceClass::VideoCapture).get()) {
        Device d;
        d.device = info.Id();
        d.id = narrow(info.Id());
        d.name = narrow(info.Name());
        if (EnclosureLocation where = info.EnclosureLocation()) {
            d.panel = where.Panel();
            try {
                d.angle = where.RotationAngleInDegreesClockwise();
                d.angleKnown = true;
            } catch (const winrt::hresult_error&) {
            }
        }
        found.push_back(std::move(d));
    }
    return found;
}

int flagOrientation(int index) {
    if (gFlagFolder.empty()) return -1;
    for (int degrees : {0, 90, 180, 270})
        if (files::exists(gFlagFolder + L"\\camera-orientation-" + std::to_wstring(index) + L"-" + std::to_wstring(degrees))) return degrees;
    return -1;
}

std::string describe(int i, const Device& d) {
    return format("%d %s (%s, turned %d; panel %s, mounted at %s)", i, d.name.c_str(), d.facing == MEGLET_CAMERA_FRONT ? "front" : "back", d.orientation,
                  panelName(d.panel), d.angleKnown ? std::to_string(d.angle).c_str() : "?");
}

/// The cameras, looked for once, on the worker (Windows' calls want an apartment).
void search() {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gSearched) return;
    }
    struct Search {
        std::vector<Device> found;
        std::string how;
    };
    auto s = std::make_shared<Search>();
    if (!worker().run(
            [s] {
                try {
                    s->found = findGroups();
                    s->how = "as media frame source groups";
                } catch (const winrt::hresult_error& e) {
                    s->how = "media frame source groups failed (" + hresultText(e) + ")";
                }
                if (s->found.empty()) {
                    try {
                        s->found = findDevices();
                        s->how += ", then as video capture devices";
                    } catch (const winrt::hresult_error& e) {
                        s->how += ", then video capture devices failed (" + hresultText(e) + ")";
                    }
                }
            },
            callLimit))
        s = std::make_shared<Search>(Search{{}, "but Windows did not answer in time"});
    std::vector<Device> found = s->found;
    const std::string how = s->how;
    // Android's order: the back cameras first, then the front ones, then the rest.
    std::stable_sort(found.begin(), found.end(), [](const Device& a, const Device& b) {
        auto rank = [](Panel p) { return p == Panel::Back ? 0 : p == Panel::Front ? 1 : 2; };
        return rank(a.panel) < rank(b.panel);
    });
    std::lock_guard<std::mutex> lock(gMutex);
    if (gSearched) return;
    gSearched = true;
    readState(found);
    for (size_t i = 0; i < found.size(); i++) {
        placeDevice(found[i]);
        const int forced = flagOrientation(int(i));
        if (forced >= 0) {
            shellLog::write(format("camera: flag camera-orientation-%d-%d: camera %d's picture is taken as turned %d degrees, not %d", int(i), forced, int(i), forced,
                                   found[i].orientation));
            found[i].orientation = forced;
        }
    }
    gDevices = std::move(found);
    std::string list;
    for (size_t i = 0; i < gDevices.size(); i++) list += (i ? "; " : "") + describe(int(i), gDevices[i]);
    shellLog::write(format("camera: %d cameras, looked for %s%s%s", int(gDevices.size()), how.c_str(), gDevices.empty() ? "" : ": ", list.c_str()));
}

bool device(int index, Device& out) {
    search();
    std::lock_guard<std::mutex> lock(gMutex);
    if (index < 0 || index >= int(gDevices.size())) return false;
    out = gDevices[size_t(index)];
    return true;
}

/// MediaCapture's InitializeAsync on the UI thread, where Windows can ask the user; on this one where there is none (the
/// tests' program). The error it ended with, S_OK when it opened.
HRESULT initialize(const MediaCapture& capture, const MediaCaptureInitializationSettings& settings, std::string& message) {
    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    try {
        dispatcher = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().Dispatcher();
    } catch (const winrt::hresult_error&) {
    }
    if (!dispatcher) {
        try {
            capture.InitializeAsync(settings).get();
            return S_OK;
        } catch (const winrt::hresult_error& e) {
            message = hresultText(e);
            return e.code();
        }
    }
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        HRESULT hr = E_FAIL;
        std::string message;
    };
    auto state = std::make_shared<State>();
    auto finish = [state](HRESULT hr, std::string text) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->hr = hr;
        state->message = std::move(text);
        state->done = true;
        state->cv.notify_all();
    };
    winrt::agile_ref<MediaCapture> agileCapture{capture};
    winrt::agile_ref<MediaCaptureInitializationSettings> agileSettings{settings};
    try {
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [agileCapture, agileSettings, finish] {
            try {
                IAsyncAction op = agileCapture.get().InitializeAsync(agileSettings.get());
                op.Completed([finish](const IAsyncAction& done, AsyncStatus status) {
                    if (status == AsyncStatus::Completed) {
                        finish(S_OK, "");
                        return;
                    }
                    HRESULT hr = E_FAIL;
                    try {
                        hr = done.ErrorCode();
                    } catch (...) {
                    }
                    finish(hr == S_OK ? E_FAIL : hr, status == AsyncStatus::Canceled ? "cancelled" : "");
                });
            } catch (const winrt::hresult_error& e) {
                finish(e.code(), hresultText(e));
            }
        });
    } catch (const winrt::hresult_error& e) {
        message = "the UI thread could not be asked: " + hresultText(e);
        return e.code();
    }
    // The user's answer is part of the wait. A UI thread that never runs the call (the app going away) is given up on before
    // the caller's own limit, so that this thread is free again for the next camera.
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, std::chrono::seconds(170), [&state] { return state->done; })) {
        message = "neither the UI thread nor Windows answered within 170 seconds";
        return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    message = state->message;
    if (message.empty() && FAILED(state->hr)) message = format("0x%08X", unsigned(state->hr));
    return state->hr;
}

/// The colour stream of an initialized MediaCapture: the one the group named, else the first preview stream, else a record one.
MediaFrameSource colourSource(const MediaCapture& capture, const Device& d) {
    auto sources = capture.FrameSources();
    if (!d.source.empty() && sources.HasKey(d.source)) return sources.Lookup(d.source);
    MediaFrameSource best{nullptr};
    for (const auto& kv : sources) {
        MediaFrameSourceInfo info = kv.Value().Info();
        if (info.SourceKind() != MediaFrameSourceKind::Color) continue;
        const MediaStreamType type = info.MediaStreamType();
        if (type != MediaStreamType::VideoPreview && type != MediaStreamType::VideoRecord) continue;
        if (!best || (type == MediaStreamType::VideoPreview && best.Info().MediaStreamType() != MediaStreamType::VideoPreview)) best = kv.Value();
    }
    return best;
}

int framesPerSecond(const MediaFrameFormat& f) {
    auto rate = f.FrameRate();
    return rate.Denominator() ? int((rate.Numerator() + rate.Denominator() / 2) / rate.Denominator()) : 0;
}

/// The sizes a stream offers, with their slowest and fastest rates, the largest first, as many as meglet.h has room for: those
/// up to 1280 x 720, or with `large` up to 1920 x 1080. Every picture is copied into the app's Java and drawn for its view, by
/// the processor, and a scanner takes the largest size of its view's shape: at 1080p that is 3 MB a picture into Java's heap,
/// fifteen times a second, for no better a code read off a phone's screen. The flag file camera-large-sizes offers the rest.
void sizesOf(const MediaFrameSource& source, meglet_camera_caps& caps, bool large) {
    const int longest = large ? 1920 : 1280, most = large ? 1920 * 1080 : 1280 * 720;
    std::map<std::pair<int, int>, std::pair<int, int>> sizes;
    for (const MediaFrameFormat& f : source.SupportedFormats()) {
        const int w = int(f.VideoFormat().Width()), h = int(f.VideoFormat().Height()), fps = framesPerSecond(f);
        if (w < 32 || h < 32 || (w & 1) || (h & 1) || w > longest || h > longest || w * h > most || fps <= 0) continue;
        auto it = sizes.find({w, h});
        if (it == sizes.end()) sizes[{w, h}] = {fps, fps};
        else it->second = {std::min(it->second.first, fps), std::max(it->second.second, fps)};
    }
    std::vector<meglet_camera_size> list;
    for (const auto& s : sizes) list.push_back({s.first.first, s.first.second, s.second.first, s.second.second});
    std::stable_sort(list.begin(), list.end(), [](const meglet_camera_size& a, const meglet_camera_size& b) { return a.width * a.height > b.width * b.height; });
    caps.size_count = 0;
    for (const meglet_camera_size& s : list) {
        if (caps.size_count == MEGLET_CAMERA_MAX_SIZES) break;
        caps.sizes[caps.size_count++] = s;
    }
}

} // namespace

CameraHost& CameraHost::shared() {
    // Never destroyed: the app's threads may be in a call when the process ends.
    static CameraHost* const host = new CameraHost();
    return *host;
}

void CameraHost::configure(const std::wstring& stateFile, const std::wstring& flagFolder, bool portraitNative) {
    std::lock_guard<std::mutex> lock(gMutex);
    gStateFile = stateFile;
    gFlagFolder = flagFolder;
    gPortraitNative = portraitNative;
}

meglet_camera_host CameraHost::callbacks() {
    meglet_camera_host host = {};
    host.user = this;
    host.count = &countThunk;
    host.info = &infoThunk;
    host.open = &openThunk;
    host.start = &startThunk;
    host.stop = &stopThunk;
    host.focus = &focusThunk;
    host.torch = &torchThunk;
    host.close = &closeThunk;
    return host;
}

std::string CameraHost::summary() {
    search();
    std::lock_guard<std::mutex> lock(gMutex);
    std::string text = format("%d cameras", int(gDevices.size()));
    for (size_t i = 0; i < gDevices.size(); i++) text += (i ? "; " : ": ") + describe(int(i), gDevices[i]);
    return text;
}

int CameraHost::countThunk(void*) {
    search();
    std::lock_guard<std::mutex> lock(gMutex);
    return int(gDevices.size());
}

bool CameraHost::infoThunk(void*, int index, meglet_camera_info* info) {
    Device d;
    if (!device(index, d)) return false;
    *info = {};
    info->facing = d.facing;
    info->orientation = d.orientation;
    info->focus = d.focus;
    info->light = d.light;
    snprintf(info->name, sizeof(info->name), "%s", d.name.c_str());
    return true;
}

void* CameraHost::openThunk(void*, int index, meglet_camera_caps* caps, int* error) {
    *caps = {};
    *error = MEGLET_CAMERA_FAILED;
    Device d;
    if (!device(index, d)) {
        snprintf(caps->why, sizeof(caps->why), "there is no camera %d", index);
        return nullptr;
    }
    auto camera = std::make_shared<Camera>();
    camera->index = index;
    struct Opened {
        int err = MEGLET_CAMERA_FAILED;
        std::string why;
        meglet_camera_caps caps = {};
        // What Windows' privacy setting for the camera said of this app before it was opened, and after.
        std::string before, after;
    };
    auto o = std::make_shared<Opened>();
    bool large = false;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        large = !gFlagFolder.empty() && files::exists(gFlagFolder + L"\\camera-large-sizes");
    }
    if (large) shellLog::write("camera: flag camera-large-sizes: sizes up to 1920 x 1080 are offered");
    const bool ran = worker().run(
        [camera, d, o, large] {
            o->before = o->after = cameraAccess();
            MediaCaptureInitializationSettings settings;
            try {
                if (!d.group.empty()) settings.SourceGroup(MediaFrameSourceGroup::FromIdAsync(d.group).get());
                else settings.VideoDeviceId(d.device);
                settings.SharingMode(MediaCaptureSharingMode::ExclusiveControl);
                settings.StreamingCaptureMode(StreamingCaptureMode::Video);
                settings.MemoryPreference(MediaCaptureMemoryPreference::Cpu);
                camera->capture = MediaCapture();
            } catch (const winrt::hresult_error& e) {
                o->why = "the capture could not be set up: " + hresultText(e);
                return;
            }
            std::string message;
            const HRESULT hr = initialize(camera->capture, settings, message);
            o->after = cameraAccess();
            if (FAILED(hr)) {
                o->err = errorOf(hr);
                o->why = "Windows would not open it: " + message;
                camera->capture = nullptr;
                return;
            }
            try {
                camera->source = colourSource(camera->capture, d);
                if (!camera->source) {
                    o->why = "it has no colour stream";
                    camera->capture.Close();
                    camera->capture = nullptr;
                    return;
                }
                sizesOf(camera->source, o->caps, large);
                auto controller = camera->capture.VideoDeviceController();
                auto focus = controller.FocusControl();
                if (focus.Supported()) {
                    camera->focusControl = true;
                    for (FocusMode m : focus.SupportedFocusModes()) camera->continuousFocus |= m == FocusMode::Continuous;
                    for (AutoFocusRange r : focus.SupportedFocusRanges()) camera->macro |= r == AutoFocusRange::Macro;
                }
                camera->torch = controller.TorchControl().Supported();
                // A camera that stops for good (another app takes it, it is unplugged): the core hears it once.
                std::weak_ptr<Camera> weak = camera;
                camera->failed = camera->capture.Failed([weak](const MediaCapture&, const MediaCaptureFailedEventArgs& args) {
                    shellLog::write(format("camera: Windows says the camera failed: 0x%08X %s", unsigned(args.Code()), narrow(args.Message()).c_str()));
                    if (auto c = weak.lock()) {
                        std::lock_guard<std::mutex> lock(c->frameMutex);
                        if (c->streaming && c->frame && !c->gone) c->frame(c->core, nullptr);
                        c->gone = true;
                    }
                });
                o->err = MEGLET_CAMERA_OK;
            } catch (const winrt::hresult_error& e) {
                o->why = "its stream could not be read: " + hresultText(e);
                try {
                    camera->capture.Close();
                } catch (...) {
                }
                camera->capture = nullptr;
            }
        },
        openLimit);
    const int err = ran ? o->err : MEGLET_CAMERA_FAILED;
    const std::string why = ran ? o->why : "Windows did not answer within three minutes";
    if (err == MEGLET_CAMERA_OK) *caps = o->caps;
    if (err != MEGLET_CAMERA_OK) {
        *error = err;
        snprintf(caps->why, sizeof(caps->why), "%s (the camera for this app: %s%s%s)", why.c_str(), o->after.c_str(), o->after == o->before ? "" : ", before: ",
                 o->after == o->before ? "" : o->before.c_str());
        shellLog::write(format("camera: camera %d (%s) could not be opened: %s", index, d.name.c_str(), caps->why));
        if (err == MEGLET_CAMERA_DENIED)
            shellLog::write("camera: Settings, Privacy, Camera lets Meglet use the camera; Android apps are told \"Fail to connect to camera service\" until it does");
        return nullptr;
    }
    caps->focus = camera->focusControl;
    caps->macro = camera->macro;
    caps->torch = camera->torch;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        for (Device& known : gDevices)
            if (known.id == d.id) known.focus = camera->focusControl, known.light = camera->torch;
        writeState();
    }
    std::string sizes;
    for (int i = 0; i < caps->size_count; i++)
        sizes += format("%s%dx%d@%d-%d", i ? ", " : "", caps->sizes[i].width, caps->sizes[i].height, caps->sizes[i].min_fps, caps->sizes[i].max_fps);
    shellLog::write(format("camera: camera %d (%s) open, the camera for this app %s; focus %s%s%s, torch %s; sizes %s", index, d.name.c_str(), o->after.c_str(),
                           camera->focusControl ? "yes" : "no", camera->continuousFocus ? ", continuous" : "", camera->macro ? ", macro" : "",
                           camera->torch ? "yes" : "no", sizes.c_str()));
    return new std::shared_ptr<Camera>(std::move(camera));
}

namespace {

std::shared_ptr<CameraHost::Camera> of(void* handle) { return handle ? *static_cast<std::shared_ptr<CameraHost::Camera>*>(handle) : nullptr; }

/// Each picture from the reader to the core, NV12 as it lies in the locked buffer.
void arrived(const std::shared_ptr<CameraHost::Camera>& c, const MediaFrameReader& reader) {
    MediaFrameReference ref = reader.TryAcquireLatestFrame();
    if (!ref) return;
    VideoMediaFrame video = ref.VideoMediaFrame();
    if (!video) return;
    SoftwareBitmap bitmap = video.SoftwareBitmap();
    if (!bitmap) {
        // Frames were asked for in memory; a driver that hands them over on the graphics processor all the same has each
        // copied out (on this thread of Windows', which may wait), and says so once.
        auto surface = video.Direct3DSurface();
        if (!surface) return;
        if (!c->fromSurfaceSaid) {
            c->fromSurfaceSaid = true;
            shellLog::write(format("camera: camera %d hands its pictures over on the graphics processor: each is copied to memory", c->index));
        }
        bitmap = SoftwareBitmap::CreateCopyFromSurfaceAsync(surface).get();
        if (!bitmap) return;
    }
    if (bitmap.BitmapPixelFormat() != BitmapPixelFormat::Nv12) {
        bitmap = SoftwareBitmap::Convert(bitmap, BitmapPixelFormat::Nv12);
        c->converted++;
    }
    auto buffer = bitmap.LockBuffer(BitmapBufferAccessMode::Read);
    const auto y = buffer.GetPlaneDescription(0), uv = buffer.GetPlaneDescription(1);
    auto reference = buffer.CreateReference();
    uint8_t* data = nullptr;
    uint32_t capacity = 0;
    winrt::check_hresult(reference.as<IMemoryBufferByteAccess>()->GetBuffer(&data, &capacity));
    meglet_camera_frame f = {};
    f.width = y.Width;
    f.height = y.Height;
    f.y = data + y.StartIndex;
    f.y_stride = y.Stride;
    f.uv = data + uv.StartIndex;
    f.uv_stride = uv.Stride;
    if (auto when = ref.SystemRelativeTime()) f.time_us = when.Value().count() / 10;
    else f.time_us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool whole = data && size_t(uv.StartIndex) + size_t(uv.Stride) * size_t((f.height + 1) / 2) <= capacity &&
                       size_t(y.StartIndex) + size_t(y.Stride) * size_t(f.height) <= capacity;
    {
        std::lock_guard<std::mutex> lock(c->frameMutex);
        if (whole && c->streaming && !c->gone && c->frame) {
            c->frame(c->core, &f);
            if (!c->firstSaid) {
                c->firstSaid = true;
                shellLog::write(format("camera: camera %d's first picture: %dx%d NV12, rows of %d and %d bytes%s", c->index, f.width, f.height, f.y_stride, f.uv_stride,
                                       c->converted ? " (converted from the stream's own format)" : ""));
            }
            c->frames++;
        }
    }
    reference.Close();
    buffer.Close();
}

} // namespace

bool CameraHost::startThunk(void*, void* handle, int width, int height, int fps, meglet_camera_frame_fn frame, void* core) {
    auto c = of(handle);
    if (!c || !c->capture) return false;
    struct Started {
        bool ok = false;
        std::string what;
    };
    auto r = std::make_shared<Started>();
    const bool ran = worker().run(
        [c, width, height, fps, frame, core, r] {
            bool& ok = r->ok;
            std::string& what = r->what;
            // The stream's format of this size: NV12 rather than another (the reader would convert), the rate nearest the app's
            // at or under it, else the slowest above it.
            MediaFrameFormat best{nullptr};
            int bestScore = INT_MAX;
            for (const MediaFrameFormat& f : c->source.SupportedFormats()) {
                if (int(f.VideoFormat().Width()) != width || int(f.VideoFormat().Height()) != height) continue;
                const int rate = framesPerSecond(f);
                int score = rate <= fps ? (fps - rate) * 2 : (rate - fps) * 2 + 1000;
                if (f.Subtype() != L"NV12") score += 10000;
                if (score < bestScore) best = f, bestScore = score;
            }
            if (!best) {
                what = format("it offers no %dx%d", width, height);
                return;
            }
            try {
                c->source.SetFormatAsync(best).get();
                c->reader = c->capture.CreateFrameReaderAsync(c->source, L"NV12").get();
                {
                    std::lock_guard<std::mutex> lock(c->frameMutex);
                    c->frame = frame;
                    c->core = core;
                    c->streaming = true;
                    c->gone = false;
                    c->firstSaid = false;
                    c->frames = c->converted = 0;
                }
                std::weak_ptr<Camera> weak = c;
                c->arrived = c->reader.FrameArrived([weak](const MediaFrameReader& reader, const MediaFrameArrivedEventArgs&) {
                    auto camera = weak.lock();
                    if (!camera) return;
                    try {
                        arrived(camera, reader);
                    } catch (const winrt::hresult_error& e) {
                        static std::atomic<int> said{0};
                        if (said.fetch_add(1) < 5) shellLog::write("camera: a picture could not be read: " + hresultText(e));
                    }
                });
                const MediaFrameReaderStartStatus status = c->reader.StartAsync().get();
                if (status != MediaFrameReaderStartStatus::Success) {
                    what = format("its reader would not start (status %d: 1 unknown, 2 the device is not there, 3 the format, 4 not exclusive)", int(status));
                    {
                        std::lock_guard<std::mutex> lock(c->frameMutex);
                        c->streaming = false;
                    }
                    c->reader.FrameArrived(c->arrived);
                    c->reader.Close();
                    c->reader = nullptr;
                    return;
                }
                ok = true;
                what = format("%dx%d %s at %d frames a second", width, height, narrow(best.Subtype()).c_str(), framesPerSecond(best));
            } catch (const winrt::hresult_error& e) {
                what = "it would not stream: " + hresultText(e);
                {
                    std::lock_guard<std::mutex> lock(c->frameMutex);
                    c->streaming = false;
                }
                if (c->reader) {
                    try {
                        c->reader.Close();
                    } catch (...) {
                    }
                    c->reader = nullptr;
                }
            }
        },
        callLimit);
    const bool ok = ran && r->ok;
    shellLog::write(format("camera: camera %d %s %s", c->index, ok ? "streams" : "does not stream:", ran ? r->what.c_str() : "Windows did not answer in time"));
    return ok;
}

void CameraHost::stopThunk(void*, void* handle) {
    auto c = of(handle);
    if (!c || !c->reader) return;
    {
        // No call of the core's after this: one under way ends first, holding the lock.
        std::lock_guard<std::mutex> lock(c->frameMutex);
        c->streaming = false;
    }
    {
        std::lock_guard<std::mutex> lock(c->focusMutex);
        c->focusAsked++;
        c->done = nullptr;
    }
    worker().run(
        [c] {
            try {
                if (c->torchOn) c->capture.VideoDeviceController().TorchControl().Enabled(false);
            } catch (const winrt::hresult_error&) {
            }
            c->torchOn = false;
            try {
                c->reader.FrameArrived(c->arrived);
                c->reader.StopAsync().get();
                c->reader.Close();
            } catch (const winrt::hresult_error& e) {
                shellLog::write("camera: its reader would not stop: " + hresultText(e));
            }
            c->reader = nullptr;
        },
        callLimit);
    shellLog::write(format("camera: camera %d stopped after %llu pictures", c->index, (unsigned long long)c->frames));
}

bool CameraHost::focusThunk(void*, void* handle, int mode, meglet_camera_focus_fn done, void* core) {
    auto c = of(handle);
    if (!c || !c->reader) return false;
    unsigned asked;
    {
        std::lock_guard<std::mutex> lock(c->focusMutex);
        asked = ++c->focusAsked;
        c->done = mode == MEGLET_CAMERA_FOCUS_NONE ? nullptr : done;
        c->doneCore = core;
    }
    if (mode == MEGLET_CAMERA_FOCUS_NONE) return true;
    if (!c->focusControl) return false;
    auto ok = std::make_shared<bool>(false);
    const bool ran = worker().run(
        [c, mode, asked, ok] {
            try {
                auto control = c->capture.VideoDeviceController().FocusControl();
                FocusSettings settings;
                settings.Mode(mode == MEGLET_CAMERA_FOCUS_CONTINUOUS && c->continuousFocus ? FocusMode::Continuous : FocusMode::Single);
                settings.AutoFocusRange(mode == MEGLET_CAMERA_FOCUS_MACRO && c->macro ? AutoFocusRange::Macro : AutoFocusRange::FullRange);
                settings.DisableDriverFallback(false);
                try {
                    control.Configure(settings);
                } catch (const winrt::hresult_error& e) {
                    static std::atomic<int> said{0};
                    if (said.fetch_add(1) < 3) shellLog::write("camera: focus settings refused, focusing as the camera does: " + hresultText(e));
                }
                std::weak_ptr<Camera> weak = c;
                control.FocusAsync().Completed([weak, asked](const IAsyncAction&, AsyncStatus status) {
                    auto camera = weak.lock();
                    if (!camera) return;
                    // Answered under the lock: after a later request, or the cancel, has returned, nothing of this one comes.
                    std::lock_guard<std::mutex> lock(camera->focusMutex);
                    if (camera->focusAsked != asked || !camera->done) return;
                    meglet_camera_focus_fn fn = camera->done;
                    camera->done = nullptr;
                    fn(camera->doneCore, status == AsyncStatus::Completed);
                });
                *ok = true;
            } catch (const winrt::hresult_error& e) {
                static std::atomic<int> said{0};
                if (said.fetch_add(1) < 3) shellLog::write("camera: it would not focus: " + hresultText(e));
            }
        },
        callLimit);
    return ran && *ok;
}

void CameraHost::torchThunk(void*, void* handle, bool on) {
    auto c = of(handle);
    if (!c || !c->reader || !c->torch) return;
    worker().run(
        [c, on] {
            try {
                c->capture.VideoDeviceController().TorchControl().Enabled(on);
                c->torchOn = on;
            } catch (const winrt::hresult_error& e) {
                shellLog::write("camera: the torch would not turn " + std::string(on ? "on: " : "off: ") + hresultText(e));
            }
        },
        callLimit);
}

void CameraHost::closeThunk(void* user, void* handle) {
    auto c = of(handle);
    if (!c) return;
    stopThunk(user, handle);
    worker().run(
        [c] {
            try {
                c->capture.Failed(c->failed);
                c->capture.Close();
            } catch (const winrt::hresult_error&) {
            }
            c->source = nullptr;
            c->capture = nullptr;
        },
        callLimit);
    shellLog::write(format("camera: camera %d closed", c->index));
    delete static_cast<std::shared_ptr<Camera>*>(handle);
}

} // namespace meglet::app
