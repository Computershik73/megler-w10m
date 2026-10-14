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
/// a file of that name is in LocalState/flags (the core's hk_flag likewise).
bool flag(const wchar_t* name) {
    const bool on = files::exists(platform::localStateDirectory() + L"/flags/" + name);
    if (on) shellLog::write("flag: " + narrow(name) + " is on");
    return on;
}

/// What the UI thread answers the render thread with.
struct PanelAnswer {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    HRESULT result = E_PENDING;
};

} // namespace

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
    : panel_(panel), dispatcher_(panel.Dispatcher()), surfaceWidth_(width), surfaceHeight_(height) {
    setPanelSize(float(panel.ActualWidth()), float(panel.ActualHeight()));
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

// MARK: megler_window_host (the app's render thread)

bool WindowHost::attachDevice(void* user, void* device) {
    return static_cast<WindowHost*>(user)->attach(static_cast<ID3D11Device*>(device));
}

void* WindowHost::acquireTarget(void* user, int width, int height) {
    return static_cast<WindowHost*>(user)->acquire(width, height);
}

void WindowHost::present(void* user) { static_cast<WindowHost*>(user)->presentFrame(); }

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
    device_.copy_from(device); // our own reference
    device_->GetImmediateContext(context_.put());
    featureLevel_ = device_->GetFeatureLevel();
    return createSwapChain(unsigned(std::max(1, surfaceWidth_.load())), unsigned(std::max(1, surfaceHeight_.load())));
}

bool WindowHost::createSwapChain(unsigned width, unsigned height) {
    com_ptr<IDXGIDevice1> dxgiDevice;
    com_ptr<IDXGIAdapter> adapter;
    com_ptr<IDXGIFactory2> factory;
    HRESULT hr = device_->QueryInterface(IID_PPV_ARGS(dxgiDevice.put()));
    if (SUCCEEDED(hr)) hr = dxgiDevice->GetAdapter(adapter.put());
    if (SUCCEEDED(hr)) hr = adapter->GetParent(IID_PPV_ARGS(factory.put()));
    if (FAILED(hr)) {
        shellLog::write(format("window: no DXGI 1.2 factory behind %s device, 0x%08X", deviceOwner_, unsigned(hr)));
        return false;
    }

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
    hr = factory->CreateSwapChainForComposition(device_.get(), &desc, nullptr, chain.put());
    if (FAILED(hr)) {
        shellLog::write(format("window: CreateSwapChainForComposition(%ux%u) failed, 0x%08X", width, height, unsigned(hr)));
        return false;
    }

    // The panel takes it on its own thread, and this one waits: the core is
    // told the truth about whether there is a screen. megler_run is never
    // called on the UI thread (Session.cpp), so the UI thread is free to
    // answer; the wait still ends by itself if it does not.
    auto answer = std::make_shared<PanelAnswer>();
    SwapChainPanel panel = panel_;
    if (flag(L"no-panel")) {
        // The swap chain is made and presented to, and never shown.
        std::lock_guard<std::mutex> lock(answer->mutex);
        answer->done = true;
        answer->result = S_OK;
    } else
    try {
        dispatcher_.RunAsync(CoreDispatcherPriority::High, [answer, panel, chain] {
            HRESULT result = E_FAIL;
            try {
                result = panel.as<ISwapChainPanelNative>()->SetSwapChain(chain.get());
            } catch (const winrt::hresult_error& e) {
                result = e.code();
            }
            std::lock_guard<std::mutex> lock(answer->mutex);
            answer->result = result;
            answer->done = true;
            answer->changed.notify_all();
        });
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("window: couldn't reach the UI thread, 0x%08X", unsigned(e.code())));
        return false;
    }
    {
        std::unique_lock<std::mutex> lock(answer->mutex);
        if (!answer->changed.wait_for(lock, std::chrono::seconds(10), [&] { return answer->done; })) {
            shellLog::write("window: the UI thread didn't take the swap chain within 10 s");
            return false;
        }
        if (FAILED(answer->result)) {
            shellLog::write(format("window: the panel refused the swap chain, 0x%08X", unsigned(answer->result)));
            return false;
        }
    }

    swapChain_ = chain;
    swapChain2_ = nullptr;
    chain->QueryInterface(IID_PPV_ARGS(swapChain2_.put()));
    backBuffer_ = nullptr;
    hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer_.put()));
    if (FAILED(hr)) {
        shellLog::write(format("window: no back buffer, 0x%08X", unsigned(hr)));
        return false;
    }
    bufferWidth_ = width;
    bufferHeight_ = height;
    transformDirty_ = true;
    describe();
    if (flag(L"early-present")) {
        const HRESULT presented = swapChain_->Present(0, 0);
        shellLog::write(format("window: an empty frame presented at once, 0x%08X", unsigned(presented)));
    }
    noPresent_ = flag(L"no-present");
    shellLog::write(format("window: swap chain %ux%u x3 on %s device (feature level %s)%s", width, height, deviceOwner_,
                           levelName(featureLevel_), swapChain2_ ? "" : "; no IDXGISwapChain2: it cannot be scaled to the panel"));
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
        // (Flush lets go of what the context still holds of the old buffers;
        // it changes no pipeline state.)
        context_->Flush();
        hr = swapChain_->ResizeBuffers(0, UINT(width), UINT(height), DXGI_FORMAT_UNKNOWN, 0);
        if (SUCCEEDED(hr)) hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(backBuffer_.put()));
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

void WindowHost::presentFrame() {
    if (!swapChain_ || !backBuffer_ || !target_) return;
    if (noPresent_) {
        ++presented_;
        Sleep(16);
        return;
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
    context_->CopyResource(backBuffer_.get(), target_.get());
    const HRESULT hr = swapChain_->Present(1, 0);
    if (FAILED(hr)) {
        // Said once per kind of failure, not sixty times a second.
        if (hr != lastPresentError_) {
            lastPresentError_ = hr;
            shellLog::write(format("window: Present failed, 0x%08X%s", unsigned(hr),
                                   hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET
                                       ? format(" (the device was lost, reason 0x%08X)", unsigned(device_->GetDeviceRemovedReason())).c_str()
                                       : ""));
        }
        return;
    }
    lastPresentError_ = S_OK;
    ++presented_;
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
void WindowHost::writeScreenshot(const std::wstring& path) {
    D3D11_TEXTURE2D_DESC td = {};
    target_->GetDesc(&td);
    td.Usage = D3D11_USAGE_STAGING;
    td.BindFlags = 0;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    td.MiscFlags = 0;
    com_ptr<ID3D11Texture2D> staging;
    HRESULT hr = device_->CreateTexture2D(&td, nullptr, staging.put());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (SUCCEEDED(hr)) {
        context_->CopyResource(staging.get(), target_.get());
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
    shellLog::write(format("window: a Java app's pictures: a Direct3D device of the shell's own, %s, feature level %s", kind, levelName(featureLevel_)));
    return createSwapChain(width, height);
}

void WindowHost::showPixels(const void* pixels, int width, int height, int stride) {
    // No picture: the last one stays (megler.h; the core does not pass this on today).
    if (!pixels || width <= 0 || height <= 0 || stride < width * 4) return;
    std::lock_guard<std::mutex> lock(pixelsMutex_);
    if (!ownDeviceState_) {
        shellLog::write(format("window: the app's first picture, %dx%d: it draws its views in memory, not with OpenGL", width, height));
        ownDeviceState_ = makeOwnDevice(unsigned(width), unsigned(height)) ? 1 : -1;
    }
    if (ownDeviceState_ < 0) {
        if (!picturesDropped_++) shellLog::write("window: the app's pictures are dropped: there is nothing to show them with (see above)");
        return;
    }
    // The texture a game would have rendered into, made the same way; again when the picture's size is another.
    if (!target_ || surfaceWidth_.load() != width || surfaceHeight_.load() != height) {
        // (Should that end in a new swap chain, the UI thread has to take it: it must not be waiting for this lock meanwhile.)
        showsPixels_ = false;
        if (!acquire(width, height)) {
            if (!picturesDropped_++) shellLog::write("window: the app's pictures are dropped: no texture to put them in (see above)");
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
    context_->UpdateSubresource(target_.get(), 0, nullptr, flipped_.data(), UINT(rowBytes), 0);
    presentFrame();
    showsPixels_ = true;
    // The panel's size changed while that was being shown (the UI thread found this lock taken and left it to us): again, at the new size.
    if (transformDirty_.exchange(false) && swapChain_ && backBuffer_ && !noPresent_) {
        applyTransform();
        context_->CopyResource(backBuffer_.get(), target_.get());
        swapChain_->Present(0, 0);
    }
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
        const bool kept = ownDeviceState_ > 0 && target_ && showsPixels_ && surfaceWidth_.load() == width && surfaceHeight_.load() == height &&
                          flippedWidth_ == width && flippedHeight_ == height && flipped_.size() >= size_t(width) * size_t(height) * 4;
        if (kept) {
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
            context_->UpdateSubresource(target_.get(), 0, &box, flipped_.data() + size_t(box.top) * rowBytes + size_t(left) * 4, UINT(rowBytes), 0);
            presentFrame();
            if (transformDirty_.exchange(false) && swapChain_ && backBuffer_ && !noPresent_) {
                applyTransform();
                context_->CopyResource(backBuffer_.get(), target_.get());
                swapChain_->Present(0, 0);
            }
            return;
        }
    }
    showPixels(pixels, width, height, stride);
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
    if (!swapChain_ || !backBuffer_ || !target_ || noPresent_) return;
    if (transformDirty_.exchange(false)) applyTransform();
    context_->CopyResource(backBuffer_.get(), target_.get());
    const HRESULT hr = swapChain_->Present(0, 0);
    if (FAILED(hr) && hr != lastPresentError_) {
        lastPresentError_ = hr;
        shellLog::write(format("window: Present failed while showing the last picture again, 0x%08X", unsigned(hr)));
    }
}

} // namespace megler::app
