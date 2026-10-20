// The screen of a running app: meglet_window_host (core/include/meglet.h) over a
// XAML SwapChainPanel.
//
// ANGLE makes the Direct3D 11 device; this makes a composition swap chain on
// that same device -- at feature level 9_3, where most of these phones stop,
// two devices cannot share a texture -- and gives it to the panel. Each frame
// the app has rendered into the target texture handed out here, and present()
// copies that to the back buffer and presents. Nothing else is ever done on
// the device context: its pipeline state is ANGLE's.
//
// What gecko-w10m and podium-w10m learned getting a swap chain onto this
// panel on these phones, kept here:
//
//  * A DXGI 1.2 composition swap chain: FLIP_SEQUENTIAL, B8G8R8A8_UNORM and
//    three buffers -- with two, a frame that took a little longer than a
//    refresh waited for the next one and everything ran at 30 fps.
//  * ISwapChainPanelNative::SetSwapChain only ever on the panel's own (UI)
//    thread: from another thread it "works" and corrupts the system
//    compositor, which kills the app a moment later inside CoreUIComponents.
//    The callbacks arrive on the app's render thread, so attach_device posts
//    that one call to the UI thread and waits for it.
//  * XAML shows swap-chain pixels as logical units, so the chain carries a
//    matrix transform (IDXGISwapChain2) that scales its buffers -- the size
//    of the app's surface, not of the panel -- onto the panel, letterboxed if
//    the two differ in shape.
//
// The picture arrives upside down (row 0 of the target is the bottom of the
// image, OpenGL's convention kept through the copy). The swap chain's
// transform takes only scale and translation, and whether a negative scale
// passes is not something to find out on a phone; the panel is flipped
// instead, with a XAML ScaleTransform (PlayerScreen), which the compositor
// applies to the swap chain's visual like to any other.
//
// An app whose user interface is Java draws no OpenGL: its views are drawn in
// memory (player/GfxHost.h) and each finished picture of the whole window
// arrives through present_pixels, on the app's main thread, top row first.
// (One that draws with OpenGL ES on a view as well has its window composed
// by the core, the views over the GL frames read back, and the pictures
// come the same way, from the thread that swapped a frame too.)
// There is no ANGLE then and no device to share, so the device is this
// class's own, made when the first picture comes; from there on the picture
// takes the way a game's frame takes -- the same swap chain on the panel, the
// same target texture copied to the back buffer, the same scaling and the
// same screenshot. So that all of that stays as it is, the picture is put
// into the target texture bottom row first, which is how a game's frame lies
// in it: the panel's flip then shows it the right way up, and a touch needs
// no other arithmetic than a game's. Such an app shows a picture only when
// something changed, so the last one is shown again by the UI thread when
// the panel's size changes (a game's next frame does that by itself); the
// device context is then used by two threads, one at a time, under a lock
// that nothing of a game's ever takes.
//
// Where the app's views are drawn on the graphics processor (GfxHost), the
// device is GfxHost's, not one of this class's own: the window's picture is
// then drawn into the target texture there, the changed part flipped into
// place by Direct2D (GfxHost::setWindow, meglet_gfx_host.present), and shown
// from it without ever being read back into memory. Direct2D uses that
// device's context from the app's threads, so every use of it here is under
// Direct2D's lock (DeviceLock). Nothing here waits for the graphics
// processor: the swap chain keeps its three buffers, and a frame latency of
// two (IDXGIDevice1::SetMaximumFrameLatency) is what makes Present hold the
// app's thread when it runs ahead of the display.
//
// One WindowHost lives from Play to the end of the process: one app runs per
// process, and the core may call it for as long as the app's threads live.
#pragma once

#include "meglet.h"
#include "player/GfxHost.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace meglet::app {

class WindowHost {
public:
    struct Rect {
        float x = 0, y = 0, width = 0, height = 0;
    };
    /// The largest rectangle with the content's shape centred in the area.
    /// Used for the swap chain's transform (in panel DIPs) and for mapping
    /// touches back, so the two can never disagree.
    static Rect letterbox(float areaWidth, float areaHeight, float contentWidth, float contentHeight);

    /// UI thread. `width` x `height` is the surface the app is about to be
    /// given (meglet_run_config), the swap chain's first size.
    WindowHost(winrt::Windows::UI::Xaml::Controls::SwapChainPanel const& panel, int width, int height);
    WindowHost(const WindowHost&) = delete;
    WindowHost& operator=(const WindowHost&) = delete;

    /// The callbacks for meglet_run_config.window, with this as their user.
    meglet_window_host callbacks();
    /// Where GfxHost shows the window's pictures it drew on the graphics
    /// processor (GfxHost::setWindow): this window's target texture.
    GfxHost::Window gfxWindow();

    /// UI thread: the panel's size in DIPs, at first and whenever it changes.
    void setPanelSize(float width, float height);

    /// The surface the app draws to, in pixels: meglet_run_config's until the
    /// app has made its window surface, then that one's.
    int surfaceWidth() const { return surfaceWidth_.load(); }
    int surfaceHeight() const { return surfaceHeight_.load(); }
    /// Frames presented so far.
    uint32_t presented() const { return presented_.load(); }
    /// Any thread: the next frame the app presents is also written to this
    /// file, as a BMP at half its size. For a run nobody watches (the auto
    /// run's "shots"): what was on the screen, to be fetched with the logs.
    /// A Java app's last picture is written at once: it may show no other.
    void requestScreenshot(std::wstring path);
    /// "1280x720, Direct3D 11_0" once ANGLE's device is attached.
    std::string description();

private:
    static bool attachDevice(void* user, void* device);
    static void* acquireTarget(void* user, int width, int height);
    static void present(void* user);
    static void releaseTarget(void* user);
    static void presentPixels(void* user, const void* pixels, int width, int height, int stride);
    static void presentPixelsPart(void* user, const void* pixels, int width, int height, int stride, int x, int y, int partWidth, int partHeight);

    // All on the app's render thread.
    bool attach(ID3D11Device* device);
    bool createSwapChain(unsigned width, unsigned height);
    ID3D11Texture2D* acquire(int width, int height);
    void presentFrame();
    void writeScreenshot(const std::wstring& path);
    void applyTransform();
    void describe();

    // A Java app's pictures (present_pixels): on the app's main thread, or
    // on a thread of its that swaps OpenGL ES frames (the core composes the
    // window then, and calls one at a time), but for showAgain, which is the
    // UI thread's. All of it under pixelsMutex_.
    void showPixels(const void* pixels, int width, int height, int stride);
    void showPixelsPart(const void* pixels, int width, int height, int stride, int x, int y, int partWidth, int partHeight);
    bool makeOwnDevice(unsigned width, unsigned height);
    void showAgain();
    void writePixelsShot(const std::wstring& path);
    // ... and those GfxHost draws on the graphics processor (gfxWindow), on the thread that draws them.
    static ID3D11Texture2D* gfxTarget(void* user, int width, int height);
    static void gfxShow(void* user);
    ID3D11Texture2D* pictureTarget(int width, int height);
    void showTarget();
    /// The device was GfxHost's and is lost: everything made on it goes, and the next picture makes a device of this class's own.
    void leaveLostDevice();
    struct DeviceLock;

    winrt::Windows::UI::Xaml::Controls::SwapChainPanel panel_{nullptr};
    winrt::Windows::UI::Core::CoreDispatcher dispatcher_{nullptr};

    winrt::com_ptr<ID3D11Device> device_;
    winrt::com_ptr<ID3D11DeviceContext> context_;
    winrt::com_ptr<IDXGISwapChain1> swapChain_;
    winrt::com_ptr<IDXGISwapChain2> swapChain2_;
    winrt::com_ptr<ID3D11Texture2D> backBuffer_;
    winrt::com_ptr<ID3D11Texture2D> target_;
    D3D_FEATURE_LEVEL featureLevel_ = D3D_FEATURE_LEVEL_9_1;
    unsigned bufferWidth_ = 0, bufferHeight_ = 0;
    HRESULT lastPresentError_ = S_OK;

    // Written on the UI thread, read on the render thread.
    std::atomic<float> panelWidth_{0}, panelHeight_{0};
    std::atomic<bool> transformDirty_{true};

    std::atomic<int> surfaceWidth_, surfaceHeight_;
    std::atomic<uint32_t> presented_{0};
    /// The flag "no-present" (WindowHost.cpp): frames are counted and dropped.
    bool noPresent_ = false;
    std::mutex descriptionMutex_;
    std::string description_;
    std::mutex shotMutex_;
    std::wstring shotPath_;
    std::atomic<bool> shotWanted_{false};

    /// Whose device it is, for the log: ANGLE's, or this class's own.
    const char* deviceOwner_ = "ANGLE's";
    // A Java app's pictures. pixelsMutex_ guards the device context while
    // pictures are shown through it; a game never takes it.
    std::mutex pixelsMutex_;
    std::atomic<bool> showsPixels_{false}; // the device is this class's own and a picture has been shown
    int ownDeviceState_ = 0;                // 0 not tried, 1 made, -1 cannot be
    std::vector<uint8_t> flipped_;          // the picture, bottom row first: what goes into the target texture
    int flippedWidth_ = 0, flippedHeight_ = 0;
    bool flippedStale_ = false;             // GfxHost drew into the texture since: flipped_ is not the picture any more
    bool sharedDevice_ = false;             // the device is GfxHost's: its context is used under Direct2D's lock
    uint64_t picturesDropped_ = 0;
};

} // namespace meglet::app
