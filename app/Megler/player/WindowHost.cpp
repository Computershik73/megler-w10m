#include "pch.h"

#include "player/WindowHost.h"

#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <algorithm>
#include <condition_variable>

namespace megler::app {

using winrt::com_ptr;
using winrt::Windows::UI::Core::CoreDispatcherPriority;
using winrt::Windows::UI::Xaml::Controls::SwapChainPanel;

namespace {

const char* levelName(D3D_FEATURE_LEVEL level) {
    switch (level) {
    case D3D_FEATURE_LEVEL_9_1: return "9_1";
    case D3D_FEATURE_LEVEL_9_2: return "9_2";
    case D3D_FEATURE_LEVEL_9_3: return "9_3";
    case D3D_FEATURE_LEVEL_10_0: return "10_0";
    case D3D_FEATURE_LEVEL_10_1: return "10_1";
    case D3D_FEATURE_LEVEL_11_0: return "11_0";
    case D3D_FEATURE_LEVEL_11_1: return "11_1";
    default: return "?";
    }
}

/// A switch for trying something on a phone without building for it: on when
/// a file of that name is in LocalState/flags (the core's mg_flag likewise).
bool flag(const wchar_t* name) {
    const bool on = files::exists(platform::localStateDirectory() + L"/flags/" + name);
    if (on) shellLog::write("flag: " + narrow(name) + " is on");
    return on;
}

uint64_t qpcNow() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return uint64_t(counter.QuadPart);
}

double millisecondsPerTick() {
    static const double ms = [] {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        return 1000.0 / double(frequency.QuadPart);
    }();
    return ms;
}

/// What the UI thread answers the render thread with: SetSwapChain's result, and the panel as it was then.
struct PanelAnswer {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    HRESULT result = E_PENDING;
    double width = 0, height = 0;   // DIPs
    float scaleX = 0, scaleY = 0;   // its CompositionScale: the display's pixels for a DIP
    bool visible = true;
};

double msSince(uint64_t from) { return double(qpcNow() - from) * millisecondsPerTick(); }

/// What a Present's answer is, in words, for the log.
std::string presentSaid(HRESULT hr) {
    switch (hr) {
    case S_OK: return "put it on the screen";
    case DXGI_STATUS_OCCLUDED: return "said the window is occluded (DXGI_STATUS_OCCLUDED): it is not on the screen";
    case DXGI_ERROR_INVALID_CALL: return "failed, DXGI_ERROR_INVALID_CALL (0x887A0001): it is not on the screen";
    case DXGI_ERROR_DEVICE_REMOVED: return "failed: the device was removed (0x887A0005)";
    case DXGI_ERROR_DEVICE_RESET: return "failed: the device was reset (0x887A0007)";
    default: return format(FAILED(hr) ? "failed, 0x%08X: it is not on the screen" : "said 0x%08X", unsigned(hr));
    }
}

} // namespace

/// Direct2D's lock on the device for as long as this lives, when the device
/// is GfxHost's: Direct2D uses its immediate context from the app's threads.
struct WindowHost::DeviceLock {
    bool held;
    explicit DeviceLock(const WindowHost& host) : held(host.sharedDevice_) {
        if (held) GfxHost::shared().lockDevice();
    }
    ~DeviceLock() {
        if (held) GfxHost::shared().unlockDevice();
    }
    DeviceLock(const DeviceLock&) = delete;
    DeviceLock& operator=(const DeviceLock&) = delete;
};

WindowHost::Rect WindowHost::letterbox(float areaWidth, float areaHeight, float contentWidth, float contentHeight) {
    Rect r;
    if (areaWidth <= 0 || areaHeight <= 0 || contentWidth <= 0 || contentHeight <= 0) return r;
    const float scale = std::min(areaWidth / contentWidth, areaHeight / contentHeight);
    r.width = contentWidth * scale;
    r.height = contentHeight * scale;
    r.x = (areaWidth - r.width) / 2;
    r.y = (areaHeight - r.height) / 2;
    return r;
}

WindowHost::WindowHost(SwapChainPanel const& panel, int width, int height)
    : panel_(panel), dispatcher_(panel ? panel.Dispatcher() : nullptr), surfaceWidth_(width), surfaceHeight_(height), created_(qpcNow()) {
    directOff_ = flag(L"gfx-window-copy");
    setPanelSize(panel ? float(panel.ActualWidth()) : 0.0f, panel ? float(panel.ActualHeight()) : 0.0f);
}

megler_window_host WindowHost::callbacks() {
    megler_window_host host = {};
    host.user = this;
    host.attach_device = &WindowHost::attachDevice;
    host.acquire_target = &WindowHost::acquireTarget;
    host.present = &WindowHost::present;
    host.release_target = &WindowHost::releaseTarget;
    host.present_pixels = &WindowHost::presentPixels;
    host.present_pixels_part = &WindowHost::presentPixelsPart;
    return host;
}

GfxHost::Window WindowHost::gfxWindow() {
    GfxHost::Window window;
    window.user = this;
    window.target = &WindowHost::gfxTarget;
    window.show = &WindowHost::gfxShow;
    window.back = &WindowHost::gfxBack;
    window.showBack = &WindowHost::gfxShowBack;
    return window;
}

void WindowHost::setPanelSize(float width, float height) {
    panelWidth_ = width;
    panelHeight_ = height;
    transformDirty_ = true;
    // A game's next frame scales itself to the new size; a Java app's picture may be its last for a long while.
    if (showsPixels_) showAgain();
}

std::string WindowHost::description() {
    std::lock_guard<std::mutex> lock(descriptionMutex_);
    return description_;
}

// MARK: The pictures as they come

const char* WindowHost::wayName(Way way) {
    switch (way) {
    case Way::game: return "a frame rendered with ANGLE into the window's texture (present)";
    case Way::memory: return "the views drawn in memory (present_pixels)";
    case Way::memoryPart: return "the views drawn in memory, a part of them new (present_pixels_part)";
    case Way::gpu: return "the views drawn on the graphics processor into the window's texture, copied to the swap chain";
    case Way::gpuDirect: return "the views drawn on the graphics processor straight into the swap chain's buffer";
    }
    return "?";
}

WindowHost::FirstPicture WindowHost::firstPicture() {
    std::lock_guard<std::mutex> lock(firstMutex_);
    return first_;
}

void WindowHost::onFirstPicture(std::function<void()> f) {
    {
        std::lock_guard<std::mutex> lock(firstMutex_);
        if (!first_.tried) {
            firstHook_ = std::move(f);
            return;
        }
    }
    if (f) f();
}

std::string WindowHost::state() {
    std::string swapChain, problem;
    {
        std::lock_guard<std::mutex> lock(descriptionMutex_);
        swapChain = description_;
        problem = problem_;
    }
    const int made = swapChainState_.load();
    const FirstPicture first = firstPicture();
    std::string s = made == 0    ? std::string("no swap chain made yet")
                    : made > 0   ? "a swap chain of " + swapChain + " on " + deviceOwner_.load() + " device"
                                 : "no swap chain: " + problem;
    s += format("; %u pictures came, %u Presents went, %u failed", unsigned(pictures_.load()), unsigned(presented_.load()), unsigned(presentsFailed_.load()));
    if (presentsFailed_.load()) s += format(" (the last 0x%08X)", unsigned(lastFailure_.load()));
    if (first.came)
        s += format("; the first, %dx%d, %s, came %.0f ms after Play%s", first.width, first.height, wayName(first.way), first.ms,
                    first.shown ? format(", and a picture has been on the screen since %.0f ms", first.shownMs).c_str() : ", and none has been on the screen");
    s += format("; the panel %.0fx%.0f DIPs", double(panelWidth_.load()), double(panelHeight_.load()));
    return s;
}

void WindowHost::came(Way way, int width, int height) {
    if (pictures_.fetch_add(1) != 0) return;
    std::lock_guard<std::mutex> lock(firstMutex_);
    first_.came = true;
    first_.way = way;
    first_.width = width;
    first_.height = height;
    first_.ms = msSince(created_);
}

void WindowHost::tried(HRESULT result, const char* why) {
    if (result == S_OK) shownNow("its Present");
    if (firstTried_.load()) return;
    std::function<void()> hook;
    std::string line;
    {
        std::lock_guard<std::mutex> lock(firstMutex_);
        if (first_.tried || !first_.came) return;
        first_.tried = true;
        first_.result = result;
        first_.why = why ? why : "";
        firstTried_ = true;
        line = format("window: the app's first picture, %dx%d, came %.0f ms after Play: %s; ", first_.width, first_.height, first_.ms, wayName(first_.way)) +
               (result == S_FALSE ? "it was not presented: " + first_.why : "Present " + presentSaid(result));
        hook = std::move(firstHook_);
        firstHook_ = nullptr;
    }
    shellLog::write(line);
    if (hook) hook();
}

void WindowHost::shownNow(const char* how) {
    if (shownOnce_.exchange(true)) return;
    std::string line;
    {
        std::lock_guard<std::mutex> lock(firstMutex_);
        first_.shown = true;
        first_.shownMs = msSince(created_);
        // (The first picture's own Present is said in its line, tried's.)
        if (first_.tried)
            line = format("window: a picture of the app's is on the screen at last, %.0f ms after Play (%u pictures came; %s put it there)",
                          first_.shownMs, unsigned(pictures_.load()), how);
    }
    if (!line.empty()) shellLog::write(line);
}

// MARK: megler_window_host (the app's render thread)

bool WindowHost::attachDevice(void* user, void* device) {
    return static_cast<WindowHost*>(user)->attach(static_cast<ID3D11Device*>(device));
}

void* WindowHost::acquireTarget(void* user, int width, int height) {
    return static_cast<WindowHost*>(user)->acquire(width, height);
}

void WindowHost::present(void* user) {
    auto* self = static_cast<WindowHost*>(user);
    self->came(Way::game, self->surfaceWidth_.load(), self->surfaceHeight_.load());
    const char* why = nullptr;
    const HRESULT hr = self->presentFrame(&why);
    self->tried(hr, why);
}

void WindowHost::releaseTarget(void* user) {
    auto* self = static_cast<WindowHost*>(user);
    self->target_ = nullptr;
    shellLog::write("window: the app's surface is gone");
}

bool WindowHost::attach(ID3D11Device* device) {
    if (!device) {
        shellLog::write("window: attach_device without a device");
        return false;
    }
    // (Again, should the core make a second device: everything of the first
    // goes with it.)
    target_ = nullptr;
    backBuffer_ = nullptr;
    swapChain2_ = nullptr;
    swapChain_ = nullptr;
    context_ = nullptr;
    doneQuery_ = nullptr;
    queryIssued_ = queryFailed_ = false;
    device_.copy_from(device); // our own reference
    device_->GetImmediateContext(context_.put());
    featureLevel_ = device_->GetFeatureLevel();
    return createSwapChain(unsigned(std::max(1, surfaceWidth_.load())), unsigned(std::max(1, surfaceHeight_.load())));
}

bool WindowHost::createSwapChain(unsigned width, unsigned height) {
    com_ptr<IDXGIDevice1> dxgiDevice;
    com_ptr<IDXGIAdapter> adapter;
    com_ptr<IDXGIFactory2> factory;
    // (Why there is none, for the player's line when no picture comes: WindowHost::state.)
    auto failed = [this](std::string why) {
        shellLog::write("window: " + why);
        std::lock_guard<std::mutex> lock(descriptionMutex_);
        problem_ = std::move(why);
        swapChainState_ = -1;
        return false;
    };
    HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(adapter.put());
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) return failed(format("no DXGI 1.2 factory behind %s device, 0x%08X", deviceOwner_.load(), unsigned(hr)));

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 3;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    com_ptr<IDXGISwapChain1> chain;
    {
        DeviceLock device(*this);
        hr = factory->CreateSwapChainForComposition(device_.get(), &desc, nullptr, chain.put());
    }
    if (FAILED(hr)) return failed(format("CreateSwapChainForComposition(%ux%u) failed, 0x%08X", width, height, unsigned(hr)));

    // The panel takes it on its own thread, and this one waits: the core is
    // told the truth about whether there is a screen. megler_run is never
    // called on the UI thread (Session.cpp), so the UI thread is free to
    // answer; the wait still ends by itself if it does not.
    auto answer = std::make_shared<PanelAnswer>();
    SwapChainPanel panel = panel_;
    const bool noPanel = !panel || flag(L"no-panel");
    if (noPanel) {
        // The swap chain is made and presented to, and never shown (the flag; or no panel, the shell's test program's).
        std::lock_guard<std::mutex> lock(answer->mutex);
        answer->done = true;
        answer->result = S_OK;
    } else
    try {
        dispatcher_.RunAsync(CoreDispatcherPriority::High, [answer, panel, chain] {
            HRESULT result = E_FAIL;
            double width = 0, height = 0;
            float scaleX = 0, scaleY = 0;
            bool visible = true;
            try {
                result = panel.as<ISwapChainPanelNative>()->SetSwapChain(chain.get());
            } catch (const winrt::hresult_error& e) {
                result = e.code();
            }
            try {
                // Where the pictures go, as the panel stands now: an Xbox shows a XAML app at 200 percent on 1920 by 1080.
                width = panel.ActualWidth();
                height = panel.ActualHeight();
                scaleX = panel.CompositionScaleX();
                scaleY = panel.CompositionScaleY();
                visible = panel.Visibility() == winrt::Windows::UI::Xaml::Visibility::Visible;
            } catch (const winrt::hresult_error&) {
            }
            std::lock_guard<std::mutex> lock(answer->mutex);
            answer->result = result;
            answer->width = width;
            answer->height = height;
            answer->scaleX = scaleX;
            answer->scaleY = scaleY;
            answer->visible = visible;
            answer->done = true;
            answer->changed.notify_all();
        });
    } catch (const winrt::hresult_error& e) {
        return failed(format("couldn't reach the UI thread, 0x%08X", unsigned(e.code())));
    }
    std::string onPanel;
    {
        std::unique_lock<std::mutex> lock(answer->mutex);
        if (!answer->changed.wait_for(lock, std::chrono::seconds(10), [&] { return answer->done; }))
            return failed("the UI thread didn't take the swap chain within 10 s");
        if (FAILED(answer->result)) return failed(format("the panel refused the swap chain, 0x%08X", unsigned(answer->result)));
        onPanel = noPanel ? std::string(panel ? "; on no panel (the flag no-panel)" : "; on no panel (none was given)")
                          : format("; on a panel of %.0fx%.0f DIPs at %.2fx%.2f of the display's pixels for each%s", answer->width, answer->height,
                                   double(answer->scaleX), double(answer->scaleY), answer->visible ? "" : ", which is not visible");
    }

    swapChain_ = chain;
    swapChain2_ = nullptr;
    chain->QueryInterface(IID_PPV_ARGS(swapChain2_.put()));
    directAge_ = 0;
    backBuffer_ = nullptr;
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer_.put()));
    if (FAILED(hr)) return failed(format("no back buffer, 0x%08X", unsigned(hr)));
    bufferWidth_ = width;
    bufferHeight_ = height;
    transformDirty_ = true;
    describe();
    swapChainState_ = 1;
    if (flag(L"early-present")) {
        DeviceLock device(*this);
        const HRESULT presented = swapChain_->Present(0, 0);
        shellLog::write(format("window: an empty frame presented at once, 0x%08X", unsigned(presented)));
    }
    noPresent_ = flag(L"no-present");
    failPresent_ = flag(L"fail-present");
    shellLog::write(format("window: swap chain %ux%u x3 on %s device (feature level %s)%s%s", width, height, deviceOwner_.load(),
                           levelName(featureLevel_), swapChain2_ ? "" : "; no IDXGISwapChain2: it cannot be scaled to the panel", onPanel.c_str()));
    return true;
}

ID3D11Texture2D* WindowHost::acquire(int width, int height) {
    target_ = nullptr;
    if (!device_ || !swapChain_ || width <= 0 || height <= 0) {
        shellLog::write(format("window: acquire_target(%d, %d) without a swap chain", width, height));
        return nullptr;
    }
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = UINT(width);
    td.Height = UINT(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, target_.put());
    if (FAILED(hr)) {
        shellLog::write(format("window: no %dx%d target texture, 0x%08X", width, height, unsigned(hr)));
        return nullptr;
    }

    // The back buffers match the target, so present() is one CopyResource.
    if (UINT(width) != bufferWidth_ || UINT(height) != bufferHeight_) {
        backBuffer_ = nullptr;
        directAge_ = 0;
        // (GfxHost's bitmap over the back buffer too: ResizeBuffers fails while anything holds the buffers.)
        GfxHost::shared().releaseWindowBack();
        {
            // (Flush lets go of what the context still holds of the old buffers;
            // it changes no pipeline state.)
            DeviceLock device(*this);
            context_->Flush();
            hr = swapChain_->ResizeBuffers(0, UINT(width), UINT(height), DXGI_FORMAT_UNKNOWN, 0);
            if (SUCCEEDED(hr)) hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer_.put()));
        }
        if (SUCCEEDED(hr)) {
            bufferWidth_ = UINT(width);
            bufferHeight_ = UINT(height);
        } else {
            shellLog::write(format("window: ResizeBuffers(%d, %d) failed, 0x%08X; making a new swap chain", width, height, unsigned(hr)));
            if (!createSwapChain(UINT(width), UINT(height))) {
                target_ = nullptr;
                return nullptr;
            }
        }
    }
    surfaceWidth_ = width;
    surfaceHeight_ = height;
    transformDirty_ = true;
    describe();
    shellLog::write(format("window: the app's surface is %dx%d", width, height));
    return target_.get();
}

HRESULT WindowHost::presentFrame(const char** notPresented) {
    if (!swapChain_ || !backBuffer_ || !target_) {
        if (notPresented) *notPresented = !swapChain_ ? "there is no swap chain (see above)" : !target_ ? "there is no target texture" : "there is no back buffer";
        return S_FALSE;
    }
    if (noPresent_) {
        ++presented_;
        Sleep(16);
        if (notPresented) *notPresented = "the flag no-present drops it";
        return S_FALSE;
    }
    if (transformDirty_.exchange(false)) applyTransform();
    if (shotWanted_.exchange(false)) {
        std::wstring path;
        {
            std::lock_guard<std::mutex> lock(shotMutex_);
            path.swap(shotPath_);
        }
        if (!path.empty()) writeScreenshot(path);
    }
    // (The buffers get the target's pictures now, not the ones drawn straight into them.)
    directAge_ = 0;
    showsDirect_ = false;
    checkGpu();
    const uint64_t copying = qpcNow();
    context_->CopyResource(backBuffer_.get(), target_.get());
    const uint64_t presenting = qpcNow();
    // (The flag fail-present: Present is not called, and taken as refused -- what the player does with pictures that come
    // and cannot be shown.)
    const HRESULT hr = failPresent_ ? DXGI_ERROR_INVALID_CALL : swapChain_->Present(1, 0);
    const uint64_t presented = qpcNow();
    if (FAILED(hr)) {
        ++presentsFailed_;
        lastFailure_ = hr;
        // Said once per kind of failure, not sixty times a second.
        if (hr != lastPresentError_) {
            lastPresentError_ = hr;
            shellLog::write(format("window: Present failed, 0x%08X%s%s", unsigned(hr),
                                   hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
                                       ? format(" (the device was lost, reason 0x%08X)", unsigned(device_->GetDeviceRemovedReason())).c_str()
                                       : "",
                                   failPresent_ ? " (the flag fail-present)" : ""));
        }
        return hr;
    }
    markShown();
    afterPresent(false, presenting - copying, presented - presenting);
    return hr;
}

void WindowHost::checkGpu() {
    if (!queryIssued_ || !doneQuery_) return;
    ++statsChecked_;
    if (context_->GetData(doneQuery_.get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_FALSE) ++statsGpuBehind_;
}

void WindowHost::markShown() {
    if (queryFailed_ || !device_ || !context_) return;
    if (!doneQuery_) {
        D3D11_QUERY_DESC desc = {};
        desc.Query = D3D11_QUERY_EVENT;
        if (FAILED(device_->CreateQuery(&desc, doneQuery_.put()))) {
            queryFailed_ = true;
            return;
        }
    }
    context_->End(doneQuery_.get());
    queryIssued_ = true;
}

void WindowHost::afterPresent(bool direct, uint64_t copyTicks, uint64_t presentTicks) {
    lastPresentError_ = S_OK;
    ++presented_;
    const uint64_t now = qpcNow();
    const double ms = millisecondsPerTick();
    if (samplingFrames_.load()) {
        std::lock_guard<std::mutex> lock(frameSampleMutex_);
        if (samplingFrames_.load()) {
            if (frameSampleLast_) frameSampleMs_.push_back(double(now - frameSampleLast_) * ms);
            frameSampleLast_ = now;
        }
    }
    if (!statsFrom_) statsFrom_ = now;
    ++(direct ? statsDirect_ : statsCopied_);
    if (!direct) statsCopyTicks_ += copyTicks;
    statsPresentTicks_ += presentTicks;
    const double seconds = double(now - statsFrom_) * ms / 1000.0;
    if (seconds < 5.0) return;
    const uint32_t shown = statsDirect_ + statsCopied_;
    shellLog::write(format("window: %.1f s: %u pictures shown, %u of them drawn straight into the swap chain's buffers and %u copied "
                           "there whole (%.1f ms each to ask for the copy); Present held the thread %.1f ms each; the graphics processor "
                           "had not finished the picture before at %u of %u",
                           seconds, unsigned(shown), unsigned(statsDirect_), unsigned(statsCopied_),
                           statsCopied_ ? double(statsCopyTicks_) * ms / statsCopied_ : 0.0, double(statsPresentTicks_) * ms / shown,
                           unsigned(statsGpuBehind_), unsigned(statsChecked_)));
    statsFrom_ = now;
    statsCopyTicks_ = statsPresentTicks_ = 0;
    statsDirect_ = statsCopied_ = statsGpuBehind_ = statsChecked_ = 0;
}

void WindowHost::startFrameSample() {
    std::lock_guard<std::mutex> lock(frameSampleMutex_);
    frameSampleMs_.clear();
    frameSampleMs_.reserve(256);
    frameSampleLast_ = 0;
    samplingFrames_ = true;
}

std::vector<double> WindowHost::endFrameSample() {
    std::lock_guard<std::mutex> lock(frameSampleMutex_);
    samplingFrames_ = false;
    std::vector<double> result;
    result.swap(frameSampleMs_);
    return result;
}

void WindowHost::requestScreenshot(std::wstring path) {
    // A Java app shows a picture when something changed, not at a steady rate: the one it showed last is written now.
    if (showsPixels_) {
        writePixelsShot(path);
        return;
    }
    std::lock_guard<std::mutex> lock(shotMutex_);
    shotPath_ = std::move(path);
    shotWanted_ = true;
}

// On the app's render thread, like every other use of the device context.
// The target's row 0 is the bottom of the picture, which is the order a BMP
// keeps its rows in, and its bytes are blue, green, red, as a BMP's are.
void WindowHost::writeScreenshot(const std::wstring& path, ID3D11Texture2D* from) {
    if (!from) from = target_.get();
    if (!from) return;
    D3D11_TEXTURE2D_DESC td = {};
    from->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    com_ptr<ID3D11Texture2D> staging;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, staging.put());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(hr)) {
        context_->CopyResource(staging.get(), from);
        hr = context_->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped);
    }
    if (FAILED(hr)) {
        shellLog::write(format("window: no screenshot, 0x%08X", unsigned(hr)));
        return;
    }
    // Half the size, unless that would be too small to read anything in.
    const uint32_t step = td.Width > 960 ? 2 : 1;
    const uint32_t width = td.Width / step, height = td.Height / step, rowBytes = (width * 3 + 3) & ~3u;
    std::string bmp(54 + size_t(rowBytes) * height, '\0');
    auto put32 = [&bmp](size_t at, uint32_t v) { memcpy(&bmp[at], &v, 4); };
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32(2, uint32_t(bmp.size()));
    put32(10, 54);
    put32(14, 40);
    put32(18, width);
    put32(22, height);
    bmp[26] = 1;
    bmp[28] = 24;
    put32(34, rowBytes * height);
    for (uint32_t y = 0; y < height; y++) {
        const uint8_t* from = static_cast<const uint8_t*>(mapped.pData) + size_t(y) * step * mapped.RowPitch;
        char* to = &bmp[54 + size_t(y) * rowBytes];
        for (uint32_t x = 0; x < width; x++) memcpy(to + size_t(x) * 3, from + size_t(x) * step * 4, 3);
    }
    context_->Unmap(staging.get(), 0);
    const bool written = files::write(path, bmp);
    shellLog::write(format("window: screenshot %ux%u %s", width, height, written ? "written" : "could NOT be written"));
}

void WindowHost::applyTransform() {
    if (!swapChain2_ || bufferWidth_ == 0 || bufferHeight_ == 0) return;
    const Rect r = letterbox(panelWidth_, panelHeight_, float(bufferWidth_), float(bufferHeight_));
    if (r.width <= 0 || r.height <= 0) {
        // The panel has no size yet; setPanelSize marks it again.
        return;
    }
    DXGI_MATRIX_3X2_F matrix = {};
    matrix._11 = r.width / float(bufferWidth_);
    matrix._22 = r.height / float(bufferHeight_);
    matrix._31 = r.x;
    matrix._32 = r.y;
    const HRESULT hr = swapChain2_->SetMatrixTransform(&matrix);
    if (FAILED(hr)) shellLog::write(format("window: SetMatrixTransform failed, 0x%08X", unsigned(hr)));
}

void WindowHost::describe() {
    std::lock_guard<std::mutex> lock(descriptionMutex_);
    description_ = format("%ux%u, Direct3D %s", bufferWidth_, bufferHeight_, levelName(featureLevel_));
}

// MARK: A Java app's pictures (megler_window_host.present_pixels; the app's main thread, or a thread that swaps its GL frames)

void WindowHost::presentPixels(void* user, const void* pixels, int width, int height, int stride) {
    static_cast<WindowHost*>(user)->showPixels(pixels, width, height, stride);
}

void WindowHost::presentPixelsPart(void* user, const void* pixels, int width, int height, int stride, int x, int y, int partWidth, int partHeight) {
    static_cast<WindowHost*>(user)->showPixelsPart(pixels, width, height, stride, x, y, partWidth, partHeight);
}

// A device of this class's own: no ANGLE runs for such an app. Hardware
// first, at whatever feature level the phone has (9_3 on most); the
// software one after it, which is slow and still shows a picture.
bool WindowHost::makeOwnDevice(unsigned width, unsigned height) {
    // The device the app's views are drawn on, where they are drawn on the graphics processor: their pictures are then
    // shown from there (gfxWindow), and the ones that come through memory go up to the same texture.
    doneQuery_ = nullptr;
    queryIssued_ = queryFailed_ = false;
    if (ID3D11Device* drawing = GfxHost::shared().device()) {
        device_.copy_from(drawing);
        device_->GetImmediateContext(context_.put());
        featureLevel_ = device_->GetFeatureLevel();
        sharedDevice_ = true;
        deviceOwner_ = "the drawing's (GfxHost)";
        // Present holds the app's thread once two frames wait for the display: nothing else here waits for the processor.
        if (com_ptr<IDXGIDevice1> dxgi = device_.try_as<IDXGIDevice1>()) dxgi->SetMaximumFrameLatency(2);
        shellLog::write(format("window: a Java app's pictures: on the Direct3D device its views are drawn on, feature level %s; shown from there",
                               levelName(featureLevel_)));
        return createSwapChain(width, height);
    }
    sharedDevice_ = false;
    static const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
                                               D3D_FEATURE_LEVEL_9_3,  D3D_FEATURE_LEVEL_9_2,  D3D_FEATURE_LEVEL_9_1};
    const UINT count = UINT(sizeof(levels) / sizeof(levels[0]));
    const char* kind = "the graphics processor's";
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, count, D3D11_SDK_VERSION,
                                   device_.put(), &featureLevel_, context_.put());
    if (FAILED(hr)) {
        shellLog::write(format("window: no Direct3D device of the graphics processor's for a Java app's pictures, 0x%08X; trying the software one", unsigned(hr)));
        kind = "the software one (WARP)";
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, count, D3D11_SDK_VERSION, device_.put(),
                               &featureLevel_, context_.put());
    }
    if (FAILED(hr)) {
        device_ = nullptr;
        context_ = nullptr;
        shellLog::write(format("window: no Direct3D device at all, 0x%08X: the app runs, and its pictures are not shown", unsigned(hr)));
        return false;
    }
    deviceOwner_ = "the shell's own";
    if (com_ptr<IDXGIDevice1> dxgi = device_.try_as<IDXGIDevice1>()) dxgi->SetMaximumFrameLatency(2);
    shellLog::write(format("window: a Java app's pictures: a Direct3D device of the shell's own, %s, feature level %s", kind, levelName(featureLevel_)));
    return createSwapChain(width, height);
}

// The device was GfxHost's and is gone: the swap chain, the texture and the
// device go with it, and the next picture, which comes through memory now,
// makes a device of this class's own (makeOwnDevice) and a swap chain on it.
void WindowHost::leaveLostDevice() {
    shellLog::write("window: the device the app's views were drawn on is lost: the pictures go on a device of the shell's own");
    target_ = nullptr;
    backBuffer_ = nullptr;
    swapChain2_ = nullptr;
    swapChain_ = nullptr;
    doneQuery_ = nullptr;
    queryIssued_ = queryFailed_ = false;
    context_ = nullptr;
    device_ = nullptr;
    sharedDevice_ = false;
    showsPixels_ = false;
    showsDirect_ = false;
    directAge_ = 0;
    bufferWidth_ = bufferHeight_ = 0;
    ownDeviceState_ = 0;
    swapChainState_ = 0;
}

ID3D11Texture2D* WindowHost::gfxTarget(void* user, int width, int height) { return static_cast<WindowHost*>(user)->pictureTarget(width, height); }

void WindowHost::gfxShow(void* user) { static_cast<WindowHost*>(user)->showTarget(); }

// GfxHost is about to draw the window's picture into the target texture: it
// is made now if it is not there or is of another size, and only on
// GfxHost's own device (one of this class's own cannot take its drawing).
ID3D11Texture2D* WindowHost::pictureTarget(int width, int height) {
    if (width <= 0 || height <= 0) return nullptr;
    std::lock_guard<std::mutex> lock(pixelsMutex_);
    if (sharedDevice_ && !GfxHost::shared().device()) leaveLostDevice();
    if (!ownDeviceState_) {
        shellLog::write(format("window: the app draws its views on the graphics processor (its first picture is %dx%d): its swap chain goes on that device", width, height));
        ownDeviceState_ = makeOwnDevice(unsigned(width), unsigned(height)) ? 1 : -1;
    }
    if (ownDeviceState_ < 0 || !sharedDevice_) return nullptr;
    if (!target_ || surfaceWidth_.load() != width || surfaceHeight_.load() != height) {
        showsPixels_ = false;
        if (!acquire(width, height)) return nullptr;
    }
    flippedStale_ = true;
    return target_.get();
}

// ... and has drawn it: shown, as a picture that came through memory is.
void WindowHost::showTarget() {
    std::lock_guard<std::mutex> lock(pixelsMutex_);
    if (!target_) return;
    came(Way::gpu, surfaceWidth_.load(), surfaceHeight_.load());
    DeviceLock device(*this);
    const char* why = nullptr;
    const HRESULT hr = presentFrame(&why);
    showsPixels_ = true;
    if (transformDirty_.exchange(false) && swapChain_ && backBuffer_ && !noPresent_ && !failPresent_) {
        applyTransform();
        context_->CopyResource(backBuffer_.get(), target_.get());
        if (swapChain_->Present(0, 0) == S_OK) shownNow("a Present again at the panel's new size");
    }
    tried(hr, why);
}

ID3D11Texture2D* WindowHost::gfxBack(void* user, int width, int height, int* age) {
    return static_cast<WindowHost*>(user)->backTarget(width, height, age);
}

void WindowHost::gfxShowBack(void* user) { static_cast<WindowHost*>(user)->showBackBuffer(); }

// GfxHost would draw the picture straight into the swap chain's back buffer: there is one to give once the target's way
// has shown a picture on GfxHost's device (which makes the device and the swap chain), at this size.
ID3D11Texture2D* WindowHost::backTarget(int width, int height, int* age) {
    *age = 0;
    if (directOff_ || width <= 0 || height <= 0) return nullptr;
    std::lock_guard<std::mutex> lock(pixelsMutex_);
    if (ownDeviceState_ <= 0 || !sharedDevice_ || noPresent_ || !GfxHost::shared().device()) return nullptr;
    if (!swapChain_ || !backBuffer_ || bufferWidth_ != unsigned(width) || bufferHeight_ != unsigned(height)) return nullptr;
    *age = directAge_.load();
    return backBuffer_.get();
}

// ... and has drawn it: shown as it is, nothing copied.
void WindowHost::showBackBuffer() {
    std::lock_guard<std::mutex> lock(pixelsMutex_);
    if (!swapChain_ || !backBuffer_) return;
    came(Way::gpuDirect, int(bufferWidth_), int(bufferHeight_));
    DeviceLock device(*this);
    if (!showsDirect_.exchange(true)) shellLog::write("window: the app's pictures go straight into the swap chain's buffers, with no copy of the whole");
    // (The UI thread shows the target again at another panel size; it is not the picture now, and the next picture is.)
    showsPixels_ = false;
    if (transformDirty_.exchange(false)) applyTransform();
    if (shotWanted_.exchange(false)) {
        std::wstring path;
        {
            std::lock_guard<std::mutex> shot(shotMutex_);
            path.swap(shotPath_);
        }
        // (The buffer has the whole picture now, before it is shown.)
        if (!path.empty()) writeScreenshot(path, backBuffer_.get());
    }
    checkGpu();
    const uint64_t presenting = qpcNow();
    const HRESULT hr = failPresent_ ? DXGI_ERROR_INVALID_CALL : swapChain_->Present(1, 0);
    const uint64_t presented = qpcNow();
    // A Present that said anything else (an occluded window's) may not have turned the buffers: the next pictures go whole.
    if (hr == S_OK) directAge_ = std::min(directAge_.load() + 1, 3);
    else directAge_ = 0;
    if (FAILED(hr)) {
        ++presentsFailed_;
        lastFailure_ = hr;
        if (hr != lastPresentError_) {
            lastPresentError_ = hr;
            shellLog::write(format("window: Present failed, 0x%08X%s%s", unsigned(hr),
                                   hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
                                       ? format(" (the device was lost, reason 0x%08X)", unsigned(device_->GetDeviceRemovedReason())).c_str()
                                       : "",
                                   failPresent_ ? " (the flag fail-present)" : ""));
        }
        tried(hr);
        return;
    }
    markShown();
    afterPresent(true, 0, presented - presenting);
    tried(hr);
}

void WindowHost::showPixels(const void* pixels, int width, int height, int stride, Way way) {
    // No picture: the last one stays (megler.h; the core does not pass this on today).
    if (!pixels || width <= 0 || height <= 0 || stride < width * 4) return;
    std::lock_guard<std::mutex> lock(pixelsMutex_);
    came(way, width, height);
    if (sharedDevice_ && !GfxHost::shared().device()) leaveLostDevice();
    if (!ownDeviceState_) {
        shellLog::write(format("window: the app draws its views in memory, not with OpenGL (its first picture is %dx%d)", width, height));
        ownDeviceState_ = makeOwnDevice(unsigned(width), unsigned(height)) ? 1 : -1;
    }
    if (ownDeviceState_ < 0) {
        if (!picturesDropped_++) shellLog::write("window: the app's pictures are dropped: there is nothing to show them with (see above)");
        tried(S_FALSE, "there is no Direct3D device to show it with (see above)");
        return;
    }
    // The texture a game would have rendered into, made the same way; again when the picture's size is another.
    if (!target_ || surfaceWidth_.load() != width || surfaceHeight_.load() != height) {
        // (Should that end in a new swap chain, the UI thread has to take it: it must not be waiting for this lock meanwhile.)
        showsPixels_ = false;
        if (!acquire(width, height)) {
            if (!picturesDropped_++) shellLog::write("window: the app's pictures are dropped: no texture to put them in (see above)");
            tried(S_FALSE, "there is no texture to put it in (see above)");
            return;
        }
    }
    // The copy megler.h asks for before this returns, made bottom row first: row 0 of the target texture is the bottom of
    // the picture, as with a game's frame, and everything after this is a game's path. The alpha byte goes along and is
    // not looked at (the swap chain ignores it).
    const size_t rowBytes = size_t(width) * 4;
    flipped_.resize(rowBytes * size_t(height));
    flippedWidth_ = width;
    flippedHeight_ = height;
    for (int y = 0; y < height; y++)
        memcpy(flipped_.data() + size_t(height - 1 - y) * rowBytes, static_cast<const uint8_t*>(pixels) + size_t(y) * size_t(stride), rowBytes);
    flippedStale_ = false;
    DeviceLock device(*this);
    context_->UpdateSubresource(target_.get(), 0, nullptr, flipped_.data(), UINT(rowBytes), 0);
    const char* why = nullptr;
    const HRESULT hr = presentFrame(&why);
    showsPixels_ = true;
    // The panel's size changed while that was being shown (the UI thread found this lock taken and left it to us): again, at the new size.
    if (transformDirty_.exchange(false) && swapChain_ && backBuffer_ && !noPresent_ && !failPresent_) {
        applyTransform();
        context_->CopyResource(backBuffer_.get(), target_.get());
        if (swapChain_->Present(0, 0) == S_OK) shownNow("a Present again at the panel's new size");
    }
    tried(hr, why);
}

// A picture of which only a rectangle is new (megler.h, present_pixels_part):
// that rectangle alone is copied into the kept bottom-up copy and uploaded
// to the texture, and the rest of the texture is the last picture's. When
// nothing is kept to build on -- the first picture, another size, a new
// texture -- it is shown whole.
void WindowHost::showPixelsPart(const void* pixels, int width, int height, int stride, int x, int y, int partWidth, int partHeight) {
    if (!pixels || width <= 0 || height <= 0 || stride < width * 4) return;
    // The rectangle as it fits the picture; one that does not fit at all changes nothing.
    const int left = std::max(x, 0), top = std::max(y, 0), right = std::min(x + partWidth, width), bottom = std::min(y + partHeight, height);
    if (right <= left || bottom <= top) return;
    {
        std::unique_lock<std::mutex> lock(pixelsMutex_);
        if (sharedDevice_ && !GfxHost::shared().device()) leaveLostDevice();
        // (The kept copy may be stale, GfxHost having drawn into the texture since: the texture is the last picture all the
        // same, and only the screenshot reads the copy, which knows.)
        const bool kept = ownDeviceState_ > 0 && target_ && showsPixels_ && surfaceWidth_.load() == width && surfaceHeight_.load() == height &&
                          flippedWidth_ == width && flippedHeight_ == height && flipped_.size() >= size_t(width) * size_t(height) * 4;
        if (kept) {
            came(Way::memoryPart, width, height);
            const size_t rowBytes = size_t(width) * 4, partBytes = size_t(right - left) * 4;
            for (int row = top; row < bottom; row++)
                memcpy(flipped_.data() + size_t(height - 1 - row) * rowBytes + size_t(left) * 4,
                       static_cast<const uint8_t*>(pixels) + size_t(row) * size_t(stride) + size_t(left) * 4, partBytes);
            // In the texture, bottom row first: the rectangle's rows are height - bottom .. height - top.
            D3D11_BOX box = {};
            box.left = UINT(left);
            box.right = UINT(right);
            box.top = UINT(height - bottom);
            box.bottom = UINT(height - top);
            box.front = 0;
            box.back = 1;
            DeviceLock device(*this);
            context_->UpdateSubresource(target_.get(), 0, &box, flipped_.data() + size_t(box.top) * rowBytes + size_t(left) * 4, UINT(rowBytes), 0);
            const char* why = nullptr;
            const HRESULT hr = presentFrame(&why);
            if (transformDirty_.exchange(false) && swapChain_ && backBuffer_ && !noPresent_ && !failPresent_) {
                applyTransform();
                context_->CopyResource(backBuffer_.get(), target_.get());
                if (swapChain_->Present(0, 0) == S_OK) shownNow("a Present again at the panel's new size");
            }
            tried(hr, why);
            return;
        }
    }
    showPixels(pixels, width, height, stride, Way::memoryPart);
}

// Any thread. A Java app's last picture as a BMP, at half its size when it
// is a large one, as a game's screenshot is written: the copy that went into
// the target texture is bottom row first and blue, green, red, which is a
// BMP's own order, and nothing of Direct3D is asked.
void WindowHost::writePixelsShot(const std::wstring& path) {
    std::string bmp;
    uint32_t width = 0, height = 0;
    {
        // (Not waited for long: the app's thread holds the lock while it shows a picture, and for seconds if it has to make a swap chain.)
        std::unique_lock<std::mutex> lock(pixelsMutex_, std::defer_lock);
        for (int tries = 0; tries < 20 && !lock.try_lock(); tries++) Sleep(5);
        if (!lock.owns_lock()) {
            shellLog::write("window: no screenshot: the app is showing a picture just now");
            return;
        }
        // GfxHost drew the last picture into the texture: that is read, as a game's frame is.
        if (flippedStale_ && target_) {
            DeviceLock device(*this);
            writeScreenshot(path);
            return;
        }
        if (flippedWidth_ <= 0 || flippedHeight_ <= 0 || flipped_.size() < size_t(flippedWidth_) * size_t(flippedHeight_) * 4) return;
        const uint32_t step = flippedWidth_ > 960 ? 2 : 1;
        width = uint32_t(flippedWidth_) / step;
        height = uint32_t(flippedHeight_) / step;
        const uint32_t rowBytes = (width * 3 + 3) & ~3u;
        bmp.assign(54 + size_t(rowBytes) * height, '\0');
        auto put32 = [&bmp](size_t at, uint32_t v) { memcpy(&bmp[at], &v, 4); };
        bmp[0] = 'B';
        bmp[1] = 'M';
        put32(2, uint32_t(bmp.size()));
        put32(10, 54);
        put32(14, 40);
        put32(18, width);
        put32(22, height);
        bmp[26] = 1;
        bmp[28] = 24;
        put32(34, rowBytes * height);
        for (uint32_t y = 0; y < height; y++) {
            const uint8_t* from = flipped_.data() + size_t(y) * step * size_t(flippedWidth_) * 4;
            char* to = &bmp[54 + size_t(y) * rowBytes];
            for (uint32_t x = 0; x < width; x++) memcpy(to + size_t(x) * 3, from + size_t(x) * step * 4, 3);
        }
    }
    const bool written = files::write(path, bmp);
    shellLog::write(format("window: screenshot %ux%u %s", width, height, written ? "written" : "could NOT be written"));
}

// UI thread: the panel's size changed and the app may not show another
// picture for a long while. The last one again, scaled to the new size.
void WindowHost::showAgain() {
    // The app's thread is showing a picture this moment: it looks at the panel's size again when it has (showPixels).
    std::unique_lock<std::mutex> lock(pixelsMutex_, std::try_to_lock);
    if (!lock.owns_lock()) return;
    if (!swapChain_ || !backBuffer_ || !target_ || noPresent_ || failPresent_) return;
    if (transformDirty_.exchange(false)) applyTransform();
    DeviceLock device(*this);
    directAge_ = 0;
    context_->CopyResource(backBuffer_.get(), target_.get());
    const HRESULT hr = swapChain_->Present(0, 0);
    // (Not counted with the app's own Presents: the frame rate is the app's. But it may be the first to put a picture of the
    // app's on the screen -- a panel resized as Megler's window goes away, its pictures' own Presents having failed.)
    if (hr == S_OK) shownNow("the UI thread's Present of the last picture again, at the panel's new size");
    if (FAILED(hr) && hr != lastPresentError_) {
        lastPresentError_ = hr;
        shellLog::write(format("window: Present failed while showing the last picture again, 0x%08X", unsigned(hr)));
    }
}

} // namespace megler::app
