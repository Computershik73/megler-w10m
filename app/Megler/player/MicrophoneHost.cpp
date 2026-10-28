#include "pch.h"

#include "player/MicrophoneHost.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Media.Audio.h>
#include <winrt/Windows.Media.Capture.h>
#include <winrt/Windows.Media.Render.h>
#include <winrt/Windows.Media.MediaProperties.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

namespace megler::app {
namespace {

using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncAction;
using winrt::Windows::Media::AudioBufferAccessMode;
using winrt::Windows::Media::AudioFrame;
using winrt::Windows::Media::Audio::AudioDeviceNodeCreationStatus;
using winrt::Windows::Media::Audio::AudioFrameOutputNode;
using winrt::Windows::Media::Audio::AudioGraph;
using winrt::Windows::Media::Audio::AudioGraphCreationStatus;
using winrt::Windows::Media::Audio::AudioGraphSettings;
using winrt::Windows::Media::Capture::MediaCategory;
using winrt::Windows::Media::MediaProperties::AudioEncodingProperties;
using winrt::Windows::Media::Render::AudioRenderCategory;

// The bytes of an AudioBuffer (as CameraHost declares it: the interface is classic COM's).
struct __declspec(uuid("5b0d3235-4dba-4d44-865e-8f1d0e4fd04d")) __declspec(novtable) IMemoryBufferByteAccess : ::IUnknown {
    virtual HRESULT __stdcall GetBuffer(uint8_t** value, uint32_t* capacity) = 0;
};

std::mutex gMutex;
int gAccess = -1;            // Windows' answer, once asked: 1 yes, 0 no
AudioGraph gGraph{nullptr};
AudioFrameOutputNode gOutput{nullptr};
winrt::event_token gQuantum{};
int gChannels = 1;
std::vector<int16_t> gSamples;

/// `f` on a thread of the multithreaded apartment, waited for: the core's calls come on threads with no apartment.
template <class F>
void inMta(F&& f) {
    std::thread t([&f] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            f();
        } catch (const winrt::hresult_error& e) {
            shellLog::write(format("microphone: 0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str()));
        } catch (...) {
            shellLog::write("microphone: an exception");
        }
        winrt::uninit_apartment();
    });
    t.join();
}

/// Windows' question to the user, on the UI thread where it can be asked: MediaCapture for audio alone, which asks for
/// the microphone. True for a yes. Waits as long as the user takes, up to 170 seconds.
bool askWindows() {
    winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
    try {
        dispatcher = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().Dispatcher();
    } catch (const winrt::hresult_error&) {
    }
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false, yes = false;
        winrt::Windows::Media::Capture::MediaCapture capture{nullptr};
    };
    auto state = std::make_shared<State>();
    auto finish = [state](bool yes) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->yes = yes;
        state->done = true;
        state->cv.notify_all();
    };
    auto ask = [state, finish] {
        try {
            winrt::Windows::Media::Capture::MediaCaptureInitializationSettings settings;
            settings.StreamingCaptureMode(winrt::Windows::Media::Capture::StreamingCaptureMode::Audio);
            state->capture = winrt::Windows::Media::Capture::MediaCapture();
            IAsyncAction op = state->capture.InitializeAsync(settings);
            op.Completed([state, finish](const IAsyncAction& done, AsyncStatus status) {
                bool yes = status == AsyncStatus::Completed;
                if (!yes) {
                    try {
                        done.GetResults();
                    } catch (const winrt::hresult_error& e) {
                        shellLog::write(format("microphone: Windows said no (0x%08X)", unsigned(e.code())));
                    }
                }
                // (The capture was only the question; the graph hears the microphone.)
                try {
                    state->capture.Close();
                } catch (...) {
                }
                finish(yes);
            });
        } catch (const winrt::hresult_error& e) {
            shellLog::write(format("microphone: Windows could not be asked: 0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str()));
            finish(false);
        }
    };
    if (dispatcher)
        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, ask);
    else
        ask();
    std::unique_lock<std::mutex> lock(state->mutex);
    if (!state->cv.wait_for(lock, std::chrono::seconds(170), [&state] { return state->done; })) {
        shellLog::write("microphone: neither the UI thread nor Windows answered within 170 seconds");
        return false;
    }
    return state->yes;
}

/// A quantum's frame from the graph: 32-bit float, the channels asked for, made 16-bit for the core.
void onQuantum() {
    AudioFrame frame{nullptr};
    int channels;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (!gOutput) return;
        frame = gOutput.GetFrame();
        channels = gChannels;
    }
    auto buffer = frame.LockBuffer(AudioBufferAccessMode::Read);
    auto reference = buffer.CreateReference();
    uint8_t* data = nullptr;
    uint32_t capacity = 0;
    winrt::check_hresult(reference.as<IMemoryBufferByteAccess>()->GetBuffer(&data, &capacity));
    const uint32_t count = std::min<uint32_t>(buffer.Length(), capacity) / sizeof(float);
    if (!count || !data) return;
    const float* f = reinterpret_cast<const float*>(data);
    gSamples.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        const float v = f[i] < -1.0f ? -1.0f : f[i] > 1.0f ? 1.0f : f[i];
        gSamples[i] = static_cast<int16_t>(v * 32767.0f);
    }
    megler_microphone_push(gSamples.data(), static_cast<int>(count / channels));
}

void stopGraph() {
    AudioGraph graph{nullptr};
    {
        std::lock_guard<std::mutex> lock(gMutex);
        graph = gGraph;
        if (graph && gQuantum) graph.QuantumStarted(gQuantum);
        gGraph = nullptr;
        gOutput = nullptr;
        gQuantum = {};
    }
    if (graph) {
        graph.Stop();
        graph.Close();
    }
}

bool start(void*, int rate, int channels) {
    int access;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        access = gAccess;
    }
    if (access < 0) {
        access = askWindows() ? 1 : 0;
        std::lock_guard<std::mutex> lock(gMutex);
        gAccess = access;
        shellLog::write(access ? "microphone: Windows says the app may hear it" : "microphone: the app may not hear it: it records silence");
    }
    if (!access) return false;
    bool ok = false;
    inMta([&] {
        stopGraph();
        AudioGraphSettings settings(AudioRenderCategory::Speech);
        AudioEncodingProperties encoding = AudioEncodingProperties::CreatePcm(static_cast<uint32_t>(rate), static_cast<uint32_t>(channels), 32);
        encoding.Subtype(L"Float");
        settings.EncodingProperties(encoding);
        const auto made = AudioGraph::CreateAsync(settings).get();
        if (made.Status() != AudioGraphCreationStatus::Success) {
            shellLog::write(format("microphone: no audio graph at %d Hz (status %d)", rate, int(made.Status())));
            return;
        }
        AudioGraph graph = made.Graph();
        const auto input = graph.CreateDeviceInputNodeAsync(MediaCategory::Speech, graph.EncodingProperties()).get();
        if (input.Status() != AudioDeviceNodeCreationStatus::Success) {
            shellLog::write(format("microphone: no microphone to hear (status %d)", int(input.Status())));
            graph.Close();
            return;
        }
        AudioFrameOutputNode output = graph.CreateFrameOutputNode(graph.EncodingProperties());
        input.DeviceInputNode().AddOutgoingConnection(output);
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gGraph = graph;
            gOutput = output;
            gChannels = channels;
            gQuantum = graph.QuantumStarted([](const AudioGraph&, const winrt::Windows::Foundation::IInspectable&) {
                try {
                    onQuantum();
                } catch (const winrt::hresult_error& e) {
                    shellLog::write(format("microphone: a frame was lost (0x%08X)", unsigned(e.code())));
                }
            });
        }
        graph.Start();
        const auto e = graph.EncodingProperties();
        shellLog::write(format("microphone: hearing at %u Hz in %u channel(s)", e.SampleRate(), e.ChannelCount()));
        ok = true;
    });
    return ok;
}

void stop(void*) {
    inMta([] { stopGraph(); });
    shellLog::write("microphone: stopped");
}

} // namespace

MicrophoneHost& MicrophoneHost::shared() {
    static MicrophoneHost host;
    return host;
}

megler_microphone_host MicrophoneHost::callbacks() {
    megler_microphone_host h = {};
    h.user = this;
    h.start = &start;
    h.stop = &stop;
    return h;
}

} // namespace megler::app
