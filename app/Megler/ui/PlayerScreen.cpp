// The player: a running app's screen, full-screen, turned as the app asks.
//
// A SwapChainPanel the app's frames are shown in (player/WindowHost.h), with
// a transparent layer over it that takes the touches. The frames arrive
// upside down, so the panel carries a ScaleTransform of -1 in y; the touch
// layer is not transformed, which makes a touch's position what the user
// sees: y down from the top of the picture, as megler_touch wants it.
//
// The surface the app draws to is the panel's size in pixels, capped so the
// longer side is at most the Render Size setting (1280 by default): a phone
// panel of 2560x1440 is four times the pixels of 1280x720, for a GPU of the
// games' own day. The swap chain is scaled back up to the panel.
//
// Which way up: the app says how it wants the screen held -- its manifest's
// android:screenOrientation before it runs (the APK's report), then its
// activity in front's (megler_display_host, player/DisplayHost.h) -- and the
// phone's display is turned the ways that allows (AutoRotationPreferences,
// through ui/AppShell.cpp): a game lying down, an app that asks for nothing
// as the phone is held. When the window then changes -- the phone turned, a
// PC's window resized or maximized -- the surface follows it once its size
// has held for a poll (followWindow): the same density and pixels for a DIP
// as at the start, the rotation from DisplayInformation, megler_set_surface,
// and the bars' and the keyboard's insets told again for the new size; the
// app has Android's configuration change of it. Meanwhile the last picture is
// letterboxed into the new window, undistorted, and touches map onto it. A
// window that does not turn (a PC's, an Xbox's) and is not of the shape an
// app that asks for one wants gives it a surface of that shape in its middle
// (areaFor), as Android letterboxes such an app on a large screen.
//
// Hardware Back is the app's (AKEYCODE_BACK). The way out is a menu with
// Quit, opened by three quick taps in the top left corner (or Escape, on a
// keyboard): a phone has no other button to give it.
#include "pch.h"

#include "library/Library.h"
#include "player/BarsHost.h"
#include "player/DisplayHost.h"
#include "player/KeyboardHost.h"
#include "player/MediaControls.h"
#include "player/Session.h"
#include "player/StorageHost.h"
#include "player/WindowHost.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Components.h"

#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.UI.Text.Core.h>

#include <cmath>
#include <map>

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Input;
using winrt::Windows::Foundation::Point;

namespace {

/// The corner that opens the menu, in DIPs, and how quick three taps must be.
/// Small and quick on purpose: an app (unlike a game) has things of its own
/// in that corner -- its title, a way back, the first row of a list -- and
/// every tap there goes to the app as well. With 76 DIPs and a second and a
/// half, two unhurried taps on a list's first row and a third soon after
/// opened the menu over the app's own dialog.
constexpr double menuCorner = 56;
constexpr auto menuTapWindow = std::chrono::milliseconds(700);

// MARK: Saving the log
//
// The end panel's Save the Log: the shell's log of this run and of the two before (LocalState\megler.log,
// megler.prev.log, megler.prev2.log) copied into "Megler logs" in the phone's Documents, each named after the app and
// the time, so that it can be sent on from the phone. An app container has no way into Documents but the folder picker
// (player/StorageHost.h): the folder Android's storage has as Documents is used when there is one; else the picker opens
// in Documents, and the phone's Documents folder chosen there is kept as that link (StorageHost::add), so that it is asked
// for once.

/// `s` with what a file's name cannot have made '_'.
std::string fileNamePart(std::string s) {
    for (char& c : s)
        if (std::strchr("\\/:*?\"<>|", c) || (unsigned char)c < 32) c = '_';
    return s;
}

winrt::fire_and_forget saveTheLog(std::weak_ptr<AppShell> weakShell, std::string label) {
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Pickers;
    namespace access = winrt::Windows::Storage::AccessCache;
    std::string error, where;
    unsigned saved = 0;
    try {
        StorageFolder documents{nullptr};
        for (const auto& link : StorageHost::shared().links()) {
            if (_stricmp(link.name.c_str(), "Documents") != 0 || link.token.empty()) continue;
            try {
                documents = co_await access::StorageApplicationPermissions::FutureAccessList().GetFolderAsync(hs(link.token));
            } catch (const hresult_error& e) {
                shellLog::write(format("player: the folder linked as Documents is not handed back (0x%08X): the picker asks", unsigned(e.code())));
            }
            break;
        }
        if (!documents) {
            FolderPicker picker;
            picker.ViewMode(PickerViewMode::List);
            picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
            // (A folder picker with no file type named throws; any type will do.)
            picker.FileTypeFilter().Append(L"*");
            documents = co_await picker.PickSingleFolderAsync();
            if (!documents) co_return;
            if (_stricmp(narrow(std::wstring(documents.Name())).c_str(), "Documents") == 0 && StorageHost::shared().nameFree("Documents")) {
                const std::string why = StorageHost::shared().add(documents, "Documents");
                shellLog::write(why.empty() ? "player: the phone's Documents linked as /sdcard/Documents" : "player: Documents not linked: " + why);
            }
        }
        StorageFolder logs = co_await documents.CreateFolderAsync(L"Megler logs", CreationCollisionOption::OpenIfExists);
        where = narrow(std::wstring(logs.Path()));
        SYSTEMTIME t;
        GetLocalTime(&t);
        const std::string stamp = format("%04u-%02u-%02u %02u-%02u-%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        for (const wchar_t* name : {L"megler.log", L"megler.prev.log", L"megler.prev2.log"}) {
            const std::wstring path = platform::localStateDirectory() + L"\\" + name;
            if (!files::exists(path)) continue;
            StorageFile file = co_await StorageFile::GetFileFromPathAsync(hstring(path));
            co_await file.CopyAsync(logs, hs(fileNamePart(label) + " " + stamp + " " + narrow(name)), NameCollisionOption::GenerateUniqueName);
            saved++;
        }
    } catch (const hresult_error& e) {
        error = format("0x%08X %s", unsigned(e.code()), narrow(std::wstring(e.message())).c_str());
    }
    shellLog::write(error.empty() ? format("player: %u log files saved to %s", saved, where.c_str()) : "player: the log could not be saved: " + error);
    auto shell = weakShell.lock();
    if (!shell) co_return;
    if (!error.empty())
        shell->alert("Couldn't Save the Log", error);
    else
        shell->alert("Log Saved", format("%u files in %s.", saved, where.c_str()));
}

class PlayerScreen final : public Page {
public:
    PlayerScreen(AppShell& shell, std::string apkId) : Page(shell), id_(std::move(apkId)) {
        if (const auto entry = Library::instance().find(id_)) {
            title_ = entry->title();
            java_ = entry->report.engine == "java";
            // How the app asks to be held until it says so itself (its manifest's, as the report reduces it): a game lying down
            // whatever it says, as games always were here; an app as the phone is held. The app's own wish, once its activity
            // is up, comes through orientationWanted.
            const std::string& o = entry->report.orientation;
            wish_ = o == "landscape" ? MEGLER_ORIENTATION_SENSOR_LANDSCAPE : o == "portrait" ? MEGLER_ORIENTATION_PORTRAIT
                    : java_                ? MEGLER_ORIENTATION_UNSPECIFIED
                                           : MEGLER_ORIENTATION_SENSOR_LANDSCAPE;
        }
        // A game has the whole screen; a Java app has the phone's bars, as on Android, until its window asks them away.
        bars_ = java_ ? 0 : MEGLER_BAR_STATUS | MEGLER_BAR_NAVIGATION;
    }

    ~PlayerScreen() override {
        if (keyToken_) {
            try {
                Window::Current().CoreWindow().KeyDown(keyToken_);
            } catch (...) {
            }
        }
    }

    std::string title() override { return title_; }
    bool fullScreen() override { return true; }
    unsigned orientations() override { return orientationsFor(wish_); }
    // The bars the app wants hidden (BarsHost); the navigation bar comes back while the keyboard is up (showNavigationBar).
    bool hideNavigationBar() override { return (bars_ & MEGLER_BAR_NAVIGATION) && !navigationBarShown_; }
    bool hideStatusBar() override { return (bars_ & MEGLER_BAR_STATUS) || hideNavigationBar(); }
    bool lightStatusBar() override { return (bars_ & MEGLER_BAR_LIGHT_STATUS) != 0; }

    void barsCover(double left, double top, double right, double bottom) override {
        cover_[0] = left;
        cover_[1] = top;
        cover_[2] = right;
        cover_[3] = bottom;
        tellInsets();
    }

    bool backIsTheSystems() override { return backLeaves_; }

    bool quitAndClose() override {
        const session::State state = session::snapshot().state;
        if (state != session::State::running && state != session::State::starting) return false;
        shellLog::write("ui: the app quits, and Megler closes once it has ended");
        exitWhenEnded_ = true;
        quit();
        return true;
    }

    bool handleBack() override {
        const session::State state = session::snapshot().state;
        backLeaves_ = false;
        if (state == session::State::running) {
            if (settingsShown_)
                showSettings(false);
            else if (menuShown_)
                showMenu(false);
            else if (keyboardUp_)
                putKeyboardAway(true); // while the keyboard is up, Back is the keyboard's, as on Android
            else {
                const int answer = session::backKey();
                // The app went to the back: the system's Back takes the window away, the app kept as it is.
                if (answer == MEGLER_BACK_LEAVE) {
                    backLeaves_ = true;
                    return false;
                }
                // It ended with it: in its own package, Megler goes as soon as it has ended, as a second Back did before.
                if (answer == MEGLER_BACK_ENDED && appPackage::isSingleApp()) exitWhenEnded_ = true;
            }
            return true;
        }
        // Nothing to go back to while the app is being started.
        if (state == session::State::starting || (state == session::State::idle && !started_)) return true;
        // An app's own package has nothing but the app (shell/AppPackage.h): once it has ended, Back leaves.
        if (appPackage::isSingleApp()) {
            Application::Current().Exit();
            return true;
        }
        return false;
    }

    UIElement view() override {
        if (view_) return view_;
        const auto& p = palette();
        Grid root;
        root.Background(brush(rgb(0x000000)));

        panel_ = SwapChainPanel();
        // Row 0 of what the app renders is the bottom of its picture.
        Media::ScaleTransform flip;
        flip.ScaleY(-1);
        panel_.RenderTransform(flip);
        panel_.RenderTransformOrigin(Point{0.5f, 0.5f});
        root.Children().Append(panel_);

        // Over the panel, untransformed: touches in what the user sees.
        touchLayer_ = Grid();
        touchLayer_.Background(brush(withAlpha(rgb(0x000000), 0)));
        root.Children().Append(touchLayer_);
        wireTouches();

        // "Starting ..." until the first frame, and "Stopping ..." after Quit.
        auto busy = vstack(14);
        busy.HorizontalAlignment(HorizontalAlignment::Center);
        busy.VerticalAlignment(VerticalAlignment::Center);
        ProgressRing ring;
        ring.IsActive(true);
        ring.Width(36);
        ring.Height(36);
        ring.Foreground(brush(p.accentText));
        ring.HorizontalAlignment(HorizontalAlignment::Center);
        add(busy, ring);
        busyText_ = text("", font::subheadline, weightMedium(), p.secondaryLabel);
        busyText_.HorizontalAlignment(HorizontalAlignment::Center);
        busyText_.TextAlignment(TextAlignment::Center);
        add(busy, busyText_);
        closeWhileBusy_ = button(text("Close Megler", font::subheadline, weightSemibold(), p.label),
                                 [] { Application::Current().Exit(); }, ButtonKind::pill, "Close Megler");
        closeWhileBusy_.Background(brush(p.card));
        closeWhileBusy_.Padding(ThicknessHelper::FromLengths(20, 9, 20, 9));
        closeWhileBusy_.HorizontalAlignment(HorizontalAlignment::Center);
        setVisible(closeWhileBusy_, false);
        add(busy, closeWhileBusy_);
        busyOverlay_ = Grid();
        busyOverlay_.Children().Append(busy);
        root.Children().Append(busyOverlay_);

        root.Children().Append(buildMenu());
        if (appPackage::isSingleApp()) root.Children().Append(buildSettings());
        root.Children().Append(buildEndPanel());

        root.SizeChanged([this](auto&&, SizeChangedEventArgs const& e) {
            if (auto window = session::window()) window->setPanelSize(e.NewSize().Width, e.NewSize().Height);
            // The display turned (the window's shape with it): the surface follows at once, not after the next poll, as
            // nothing drags a phone's window. A PC's window being resized settles first (refresh).
            const bool turned = (e.NewSize().Width > e.NewSize().Height) != (windowW_ > windowH_);
            if (started_ && turned && windowW_ > 0) followWindow(true);
        });
        host_ = root;
        view_ = root;
        return view_;
    }

    void appeared() override {
        attachKeyboard();
        if (!keyToken_) {
            try {
                keyToken_ = Window::Current().CoreWindow().KeyDown([this](auto&&, winrt::Windows::UI::Core::KeyEventArgs const& e) {
                    if (e.VirtualKey() == winrt::Windows::System::VirtualKey::Enter && keyboardWanted_ && editFocused_) {
                        // (Unless the same Enter came as a line feed in the text a moment ago: see editTextUpdating.)
                        if (std::chrono::steady_clock::now() - lastNewline_ > std::chrono::milliseconds(150)) session::textKey(MEGLER_TEXT_ENTER);
                        e.Handled(true);
                        return;
                    }
                    if (e.VirtualKey() == winrt::Windows::System::VirtualKey::Back && keyboardWanted_ && editFocused_) {
                        // The text service does not delete for a text-edit context: Backspace comes as a key, and the
                        // field it belongs to deletes (as Windows' own sample control does).
                        keyboardBackspace();
                        e.Handled(true);
                        return;
                    }
                    if (e.VirtualKey() != winrt::Windows::System::VirtualKey::Escape) return;
                    if (session::snapshot().state == session::State::running) showMenu(!menuShown_);
                });
            } catch (const hresult_error&) {
            }
        }
    }

    void disappeared() override {
        if (mediaControls_) mediaControls::stop(), mediaControls_ = false;
        releaseAllTouches();
        detachKeyboard();
        // A start that failed left nothing running: the next Play may try again.
        session::forget();
    }

    void refresh() override {
        const session::Snapshot snap = session::snapshot();
        running_ = snap.state == session::State::running;
        if (snap.state == session::State::idle && !started_) {
            turnWindow();
            waitForShape();
            busyText_.Text(hs("Starting " + title_ + "…"));
        }
        const auto window = session::window();
        const uint32_t frames = window ? window->presented() : 0, pictures = window ? window->pictures() : 0;
        // The busy overlay first: nothing after it here (the system's media controls, the display's questions) may keep it up.
        showBusy(snap, pictures, "at the player's poll");
        sayNoPicture(snap, window.get());
        measureFrameRate(frames);

        const bool ended = snap.state == session::State::exited || snap.state == session::State::failed;
        // A Java app's sound goes on in the background only while the system media controls stand for it. (Windows' media
        // controls may refuse: the app plays on without them, and the rest of this poll is done all the same.)
        try {
            if (java_ && running_ && !mediaControls_) mediaControls::start(), mediaControls_ = true;
            if (mediaControls_ && ended) mediaControls::stop(), mediaControls_ = false;
            if (mediaControls_) {
                const auto heardFrom = session::audio();
                mediaControls::heard(heardFrom && heardFrom->audibleWithin(600));
            }
        } catch (const hresult_error& e) {
            if (!mediaControlsFailed_) shellLog::write(format("player: the system's media controls failed, 0x%08X: ", unsigned(e.code())) + narrow(e.message()));
            mediaControlsFailed_ = true;
            if (ended) mediaControls_ = false;
        }
        if (started_ && (snap.state == session::State::starting || running_)) followWindow(false);
        if (ended && settingsShown_) showSettings(false);
        if (ended && menuShown_) showMenu(false);
        if (settingsShown_ && settings_) settings_->refresh();
        if (ended && exitWhenEnded_) {
            Application::Current().Exit();
            return;
        }
        if (ended && !endShown_) showEnd(snap);
        if (menuShown_ && window) {
            const auto audio = session::audio();
            menuStatus_.Text(hs(window->description() + format(" · %d frames a second", framesPerSecond_) +
                                (audio ? " · sound: " + audio->status() : std::string())));
        }
    }

private:
    // MARK: Starting

    /// Play turned the display as this page was shown (landscape for a
    /// game, as the app's manifest asks for an app); the surface's size is
    /// the panel's once that has happened -- the same size twice in a row,
    /// of the shape asked for. A device that does not turn (a desktop
    /// window) is taken as it is after three seconds, the app's surface of
    /// its shape in it (areaFor).
    ///
    /// A locked phone does not turn either (the app runs under the lock
    /// screen, which is upright). For a run nobody watches there, the flag
    /// "as-landscape" (a file of that name in the flags folder) gives the
    /// app the surface it would have had: what is on the glass is then
    /// stretched, and the pictures an auto run takes are right.
    void waitForShape() {
        const double width = host_.ActualWidth(), height = host_.ActualHeight();
        ++waited_;
        const bool settled = width > 0 && height > 0 && width == lastWidth_ && height == lastHeight_;
        lastWidth_ = width;
        lastHeight_ = height;
        if (!settled) return;
        // (An upright app waits the same way for the display to be upright: it is, coming from the library, unless the
        // phone was lying on its side under a game a moment ago.)
        if (shapeFits(width, height)) {
            start(width, height);
        } else if (waited_ >= 12) {
            if (landscapeOnly(orientations()) && files::exists(platform::localStateDirectory() + L"\\flags\\as-landscape")) {
                shellLog::write("player: the screen did not turn; the app gets a landscape surface all the same (the flag as-landscape)");
                start(height, width);
            } else {
                start(width, height);
            }
        }
    }

    // MARK: Which way up, and the window's changes

    /// Windows' DisplayOrientations (Landscape 1, Portrait 2, LandscapeFlipped 4, PortraitFlipped 8; 0: as the device is held)
    /// for how an Android app asks to be held (SCREEN_ORIENTATION_*). Android's landscape is the phone turned a quarter
    /// anticlockwise, which is Windows' Landscape on a phone upright by nature (Display.getRotation's ROTATION_90).
    static unsigned orientationsFor(int wish) {
        switch (wish) {
        case MEGLER_ORIENTATION_LANDSCAPE: return 1;
        case MEGLER_ORIENTATION_PORTRAIT: return 2;
        case MEGLER_ORIENTATION_REVERSE_LANDSCAPE: return 4;
        case MEGLER_ORIENTATION_REVERSE_PORTRAIT: return 8;
        case MEGLER_ORIENTATION_SENSOR_LANDSCAPE:
        case MEGLER_ORIENTATION_USER_LANDSCAPE: return 1 | 4;
        case MEGLER_ORIENTATION_SENSOR_PORTRAIT:
        case MEGLER_ORIENTATION_USER_PORTRAIT: return 2 | 8;
        case MEGLER_ORIENTATION_SENSOR:
        case MEGLER_ORIENTATION_FULL_SENSOR: return 1 | 2 | 4 | 8;
        case MEGLER_ORIENTATION_NOSENSOR: {
            // The device's own way up, whatever its sensor says.
            try {
                return winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().NativeOrientation() ==
                               winrt::Windows::Graphics::Display::DisplayOrientations::Landscape
                           ? 1u
                           : 2u;
            } catch (const hresult_error&) {
                return 2;
            }
        }
        case MEGLER_ORIENTATION_LOCKED: {
            // Held as it is now.
            try {
                const unsigned now = unsigned(winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().CurrentOrientation());
                return now ? now : 0u;
            } catch (const hresult_error&) {
                return 0;
            }
        }
        default: return 0; // unspecified, user, fullUser: as the user lets the phone turn
        }
    }

    static bool portraitOnly(unsigned o) { return o && !(o & (1 | 4)); }
    static bool landscapeOnly(unsigned o) { return o && !(o & (2 | 8)); }

    /// Whether a window of this size is of the shape the app asks for (any shape, for an app that asks for none).
    bool shapeFits(double width, double height) {
        const unsigned want = orientations();
        return (!portraitOnly(want) || height >= width) && (!landscapeOnly(want) || width > height);
    }

    /// The part of the window the app's surface is made for, in DIPs: the whole of it, unless the window does not turn (a
    /// PC's, an Xbox's) and is not of the shape the app asks for: then a strip of that shape in its middle, of the window's
    /// proportions turned the other way (as tall as the window for an app that asks for portrait, as wide for one that asks
    /// for landscape), as Android letterboxes an app of one orientation on a large screen. A phone's window turns instead.
    winrt::Windows::Foundation::Size areaFor(double width, double height) {
        const unsigned want = orientations();
        if (platform::isMobile() || width <= 0 || height <= 0) return {float(width), float(height)};
        if (portraitOnly(want) && width > height) return {float(height * height / width), float(height)};
        if (landscapeOnly(want) && height > width) return {float(width), float(width * width / height)};
        return {float(width), float(height)};
    }

    /// Android's rotation of the display now (Surface.ROTATION_*): quarter turns of the picture from the screen's natural way
    /// up, as DXGI_MODE_ROTATION has them for DisplayInformation's CurrentOrientation against its NativeOrientation -- a phone
    /// upright by nature turned into Landscape is 1, into LandscapeFlipped 3.
    static int currentRotation() {
        using winrt::Windows::Graphics::Display::DisplayOrientations;
        try {
            auto info = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
            const bool portraitNative = info.NativeOrientation() == DisplayOrientations::Portrait;
            switch (info.CurrentOrientation()) {
            case DisplayOrientations::Landscape: return portraitNative ? 1 : 0;
            case DisplayOrientations::Portrait: return portraitNative ? 0 : 3;
            case DisplayOrientations::LandscapeFlipped: return portraitNative ? 3 : 2;
            case DisplayOrientations::PortraitFlipped: return portraitNative ? 2 : 1;
            default: return 0;
            }
        } catch (const hresult_error&) {
            return 0;
        }
    }

    /// The app asked for the screen held otherwise (its activity in front's, SCREEN_ORIENTATION_*; UI thread): the display
    /// turns the ways that allows, and the surface follows the window as it turns -- or, where it does not turn, takes the
    /// shape asked for at the next poll.
    void orientationWanted(int requested) {
        if (requested == wish_ || requested == MEGLER_ORIENTATION_BEHIND) return;
        shellLog::write(format("player: the app wants the screen held %d (Android's SCREEN_ORIENTATION_*): DisplayOrientations %u", requested,
                               orientationsFor(requested)));
        wish_ = requested;
        shell_.refreshChrome();
        turnWindow();
    }

    /// A window that does not turn by itself (a PC's) and is not of the shape the app asks for is turned: as wide as it was
    /// tall and as tall as it was wide (ApplicationView::TryResizeView), as Chrome OS opens and turns an Android app's window
    /// to the orientation it asks for. Once for each wish, and never a window in full-screen mode or an Xbox's, which has
    /// the television; where Windows will not, the app's surface is a strip of its shape in the window (areaFor).
    void turnWindow() {
        if (platform::isMobile() || platform::isXbox() || !host_ || turnedFor_ == wish_) return;
        const double width = host_.ActualWidth(), height = host_.ActualHeight();
        if (width <= 0 || height <= 0) return; // (not laid out yet: asked again at the next poll)
        turnedFor_ = wish_;
        if (shapeFits(width, height)) return;
        try {
            auto view = winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView();
            if (view.IsFullScreenMode()) return;
            const auto bounds = view.VisibleBounds();
            const bool turned = view.TryResizeView(winrt::Windows::Foundation::Size(bounds.Height, bounds.Width));
            shellLog::write(format("player: the window, %.0fx%.0f DIPs, is turned for the app's wish (%d): %s", double(bounds.Width), double(bounds.Height), wish_,
                                   turned ? "done" : "Windows would not; the app's surface is a strip of its shape in it"));
        } catch (const hresult_error& e) {
            shellLog::write("player: the window could not be turned: " + narrow(e.message()));
        }
    }

    /// The window as it is now, once its size has held for a poll (or at once, `now`, when the display turned): the app's
    /// surface made of it as at the start (areaFor; the start's pixels for a DIP and density, other ones only when the
    /// window went to a screen of another scale), told to the app with the display's rotation when it is another surface
    /// (megler_set_surface), and the insets of the phone's bars and of the keyboard told again for it. On a phone, a window
    /// not of the shape the app asks for is one the display has not turned yet: the surface stays until it has.
    void followWindow(bool now) {
        if (pixelsPerDip_ <= 0 || !host_) return;
        const double width = host_.ActualWidth(), height = host_.ActualHeight();
        if (width <= 0 || height <= 0) return;
        const bool held = width == seenWidth_ && height == seenHeight_;
        seenWidth_ = width;
        seenHeight_ = height;
        if (!held && !now) return;
        if (platform::isMobile() && !shapeFits(width, height)) return;
        double scale = startScale_, rawDpi = 0, logicalDpi = 96;
        try {
            auto info = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
            scale = info.RawPixelsPerViewPixel();
            rawDpi = info.RawDpiX();
            logicalDpi = info.LogicalDpi();
        } catch (const hresult_error&) {
        }
        const auto area = areaFor(width, height);
        double ppd = pixelsPerDip_, dpi = density_;
        if (java_) {
            // A Java app keeps its pixels for a DIP and its density, as Android keeps a window's density when it is resized --
            // unless the window went to a screen of another scale, which is another density.
            if (scale > 0 && startScale_ > 0 && std::abs(scale - startScale_) > 0.01) {
                ppd = pixelsPerDip_ * scale / startScale_;
                dpi = density_ * scale / startScale_;
            }
        } else {
            // A game: the panel's own pixels and density, within the render cap, as at the start -- however large the window
            // grows (a PC's, maximized), and back to the panel's full pixels when it is small again.
            ppd = scale > 0 ? scale : 1.0;
            dpi = rawDpi > 0 ? rawDpi : logicalDpi * 160.0 / 96.0;
            const double longer = std::max(double(area.Width), double(area.Height)) * ppd;
            if (cap_ > 0 && longer > cap_) {
                ppd *= cap_ / longer;
                dpi *= cap_ / longer;
            }
        }
        const int surfaceWidth = std::max(2, int(area.Width * ppd + 0.5) & ~1), surfaceHeight = std::max(2, int(area.Height * ppd + 0.5) & ~1);
        const int density = std::max(120, std::min(640, int(dpi + 0.5))), rotation = currentRotation();
        const bool windowChanged = width != windowW_ || height != windowH_;
        windowW_ = width;
        windowH_ = height;
        if (surfaceWidth == surfaceW_ && surfaceHeight == surfaceH_ && density == surfaceDpi_ && rotation == rotation_) {
            // (The same surface in another window, letterboxed elsewhere in it: what the bars cover of it is told again.)
            if (windowChanged) tellInsets();
            return;
        }
        shellLog::write(format("player: the window is %.0fx%.0f DIPs, turned %d degrees: the app's surface is %dx%d at %d dpi (it was %dx%d at %d dpi, %d degrees)",
                               width, height, rotation * 90, surfaceWidth, surfaceHeight, density, surfaceW_, surfaceH_, surfaceDpi_, rotation_ * 90));
        // (A gesture does not go on across it: Android cancels one as its window changes.)
        releaseAllTouches();
        surfaceW_ = surfaceWidth;
        surfaceH_ = surfaceHeight;
        surfaceDpi_ = density;
        rotation_ = rotation;
        session::setSurface(surfaceWidth, surfaceHeight, density, rotation);
        // What the bars and the keyboard cover of the new surface (the last told was of the old one).
        for (int& inset : insets_) inset = -1;
        tellInsets();
        if (keyboardUp_ && occludedDips_ > 0) tellKeyboardHeight(occludedDips_);
    }

    void start(double width, double height) {
        started_ = true;
        const auto entry = Library::instance().find(id_);
        if (!entry) {
            failBeforeStart("This app is no longer in the library.");
            return;
        }
        if (const std::string blocked = session::unavailable(); !blocked.empty()) {
            failBeforeStart(blocked);
            return;
        }
        double scale = 1.0, rawDpi = 0, logicalDpi = 96;
        try {
            auto info = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView();
            scale = info.RawPixelsPerViewPixel();
            rawDpi = info.RawDpiX();
            logicalDpi = info.LogicalDpi();
        } catch (const hresult_error&) {
        }
        if (scale <= 0) scale = 1.0;
        // The window the surface is made of, and the part of it the surface is for (all of it, but on a window that does not
        // turn and is not of the shape the app asks for: areaFor).
        windowW_ = host_.ActualWidth();
        windowH_ = host_.ActualHeight();
        seenWidth_ = windowW_;
        seenHeight_ = windowH_;
        const double panelWidth = width, panelHeight = height;
        const auto area = areaFor(width, height);
        if (area.Width != float(width) || area.Height != float(height))
            shellLog::write(format("player: the window, %.0fx%.0f DIPs, does not turn and is not of the shape the app asks for: its surface is for %.0fx%.0f "
                                   "of it, in its middle",
                                   width, height, double(area.Width), double(area.Height)));
        width = area.Width;
        height = area.Height;
        const double pixelWidth = width * scale, pixelHeight = height * scale;
        const auto flags = platform::localStateDirectory() + L"\\flags\\";
        const bool measure = files::exists(flags + L"trace-fps");
        traceFps_ = measure;
        const int cap = measure && files::exists(flags + L"render-960")
            ? 960 : settings::getInt(settings::renderCap, settings::renderCapDefault);
        const double longer = std::max(pixelWidth, pixelHeight);
        const double shrink = cap > 0 && longer > cap ? cap / longer : 1.0;
        // (Even sizes: some engines halve their buffers.)
        const int surfaceWidth = std::max(2, int(pixelWidth * shrink + 0.5) & ~1);
        const int surfaceHeight = std::max(2, int(pixelHeight * shrink + 0.5) & ~1);
        // What Android should take the screen's density for: the panel's
        // own, less by as much as the surface is smaller than the panel, so
        // an app lays itself out for the screen's real size. A display that
        // does not say (a desktop's) is taken at Android's 160 a logical inch.
        //
        // That is for a game. An app that lays out text and buttons in
        // Android's density-independent units is given what an Android phone
        // of this size would say, which is not the glass's real pitch but a
        // round scale: 160 for each of Windows' own view pixels. (This
        // phone: 494 real dots an inch, a scale of 3.5 -- Android would call
        // it 560, and its apps would be 411 units wide, as they are here.)
        const double dpi = (java_ ? 160.0 * scale : (rawDpi > 0 ? rawDpi : logicalDpi * 160.0 / 96.0)) * shrink;
        const int density = std::max(120, std::min(640, int(dpi + 0.5)));
        const int rotation = currentRotation();
        shellLog::write(format("player: panel %.0fx%.0f DIPs at scale %.2f (%.0fx%.0f pixels, %.0f dpi), render cap %d -> surface %dx%d, "
                               "density %d, turned %d degrees",
                               width, height, scale, pixelWidth, pixelHeight, rawDpi, cap, surfaceWidth, surfaceHeight, density, rotation * 90));
        // What the surface follows the window with from now on (followWindow): the same pixels for a DIP and the same density,
        // as Android keeps a window's density when it is resized -- the render cap is the start's.
        pixelsPerDip_ = scale * shrink;
        density_ = dpi;
        cap_ = cap;
        startScale_ = scale;
        surfaceW_ = surfaceWidth;
        surfaceH_ = surfaceHeight;
        surfaceDpi_ = density;
        rotation_ = rotation;
        if (!session::start(*entry, panel_, surfaceWidth, surfaceHeight, density, rotation)) {
            failBeforeStart("An app has already run since Megler was opened. Close Megler and open it again to play.");
            return;
        }
        playAt_ = std::chrono::steady_clock::now();
        if (auto window = session::window()) {
            window->setPanelSize(float(panelWidth), float(panelHeight));
            // The busy overlay goes as soon as the app's first picture has come: the window says so from the thread the picture
            // came on, and this posts it to the UI thread ahead of everything else waiting there (the poll would see it within
            // 250 ms, but only when nothing else holds the UI thread up).
            std::weak_ptr<Page> alive = weak_from_this();
            winrt::Windows::UI::Core::CoreDispatcher dispatcher{nullptr};
            try {
                dispatcher = Window::Current().Dispatcher();
            } catch (const hresult_error&) {
            }
            if (dispatcher)
                window->onFirstPicture([alive, dispatcher] {
                    try {
                        dispatcher.RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::High, [alive] {
                            if (auto page = alive.lock()) static_cast<PlayerScreen*>(page.get())->firstPictureCame();
                        });
                    } catch (const hresult_error&) {
                    }
                });
        }
        // What the phone's bars cover, now that the surface's size is known (the core keeps it for the app's start).
        tellInsets();
    }

    /// The busy overlay -- "Starting <app>..." -- is up from Play until the app's first picture has come, whichever way it
    /// came and whether or not its Present put it on the screen (WindowHost::pictures), and again while the app is being
    /// stopped; never once it has ended. At every poll, and at once when the window says the first picture has come.
    ///
    /// It went by the count of the Presents that had succeeded: on an Xbox it stayed up while the app ran (0.1.0.671), the
    /// app's picture showing for a moment as Megler was closed. A picture that comes and is not shown is the window's to say
    /// (its line in the log, with Present's answer); the overlay no longer stands in front of it.
    void showBusy(const session::Snapshot& snap, uint32_t pictures, const char* how) {
        const bool ended = snap.state == session::State::exited || snap.state == session::State::failed;
        const bool waitingForPicture = (snap.state == session::State::starting || snap.state == session::State::running) && pictures == 0;
        const bool stopping = snap.state == session::State::running && snap.stopRequested;
        const bool up = !ended && (!started_ || waitingForPicture || stopping);
        setVisible(busyOverlay_, up);
        if (!up && started_ && pictures > 0 && !firstPictureSaid_) {
            firstPictureSaid_ = true;
            const auto window = session::window();
            const WindowHost::FirstPicture first = window ? window->firstPicture() : WindowHost::FirstPicture{};
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - playAt_).count();
            std::string panel;
            try {
                panel = format("; the panel %.0fx%.0f DIPs at %.2f of the display's pixels for each", panel_.ActualWidth(), panel_.ActualHeight(),
                               double(panel_.CompositionScaleX()));
            } catch (const hresult_error&) {
            }
            const std::string shown = first.shown    ? std::string()
                                      : !first.tried ? std::string(" (being shown)")
                                      : first.result == S_FALSE ? " (not on the screen: it was not presented, " + first.why + ")"
                                                                : format(" (not on the screen: its Present failed, 0x%08X)", unsigned(first.result));
            shellLog::write(format("player: the busy overlay is hidden %.0f ms after Play (%s): the app's first picture came at %.0f ms, %dx%d, %s%s%s", ms,
                                   how, first.ms, first.width, first.height, first.came ? WindowHost::wayName(first.way) : "?", shown.c_str(), panel.c_str()));
        }
        if (stopping) {
            busyText_.Text(hs("Stopping " + title_ + "…"));
            // An app that does not answer the stop: the process can still be ended.
            setVisible(closeWhileBusy_, std::chrono::steady_clock::now() - stopAsked_ > std::chrono::seconds(4));
        }
    }

    /// The window says the app's first picture has come (UI thread, posted from the thread it came on).
    void firstPictureCame() {
        const auto window = session::window();
        if (!window) return;
        showBusy(session::snapshot(), window->pictures(), "at once, on the window's word");
    }

    /// No picture of the app's on the screen 10 s after Play, and again at 60 s, while it starts or runs: where the window
    /// stands, in the log -- whether any came, how, and what their Presents said (WindowHost::state).
    void sayNoPicture(const session::Snapshot& snap, WindowHost* window) {
        if (!started_ || !window || noPictureSaid_ >= 2) return;
        if (snap.state != session::State::starting && snap.state != session::State::running) return;
        const auto after = std::chrono::steady_clock::now() - playAt_;
        if (after < std::chrono::seconds(noPictureSaid_ ? 60 : 10) || window->firstPicture().shown) return;
        noPictureSaid_++;
        shellLog::write(format("player: %lld s after Play no picture of the app's is on the screen (the app %s): ",
                               (long long)std::chrono::duration_cast<std::chrono::seconds>(after).count(),
                               snap.state == session::State::running ? "runs" : "is still starting") +
                        window->state());
    }

    void failBeforeStart(const std::string& why) {
        session::Snapshot snap;
        snap.state = session::State::failed;
        snap.reason = why;
        showEnd(snap);
    }

    void measureFrameRate(uint32_t frames) {
        const auto now = std::chrono::steady_clock::now();
        if (now - rateAnchorTime_ < std::chrono::seconds(1)) return;
        const double seconds = std::chrono::duration<double>(now - rateAnchorTime_).count();
        const uint32_t shown = frames >= rateAnchorFrames_ ? frames - rateAnchorFrames_ : 0;
        const double fps = shown / seconds;
        framesPerSecond_ = int(fps + 0.5);
        if (traceFps_ && shown)
            shellLog::write(format("player: fps sample %.1f (%u presents in %.3f s)", fps, unsigned(shown), seconds));
        rateAnchorTime_ = now;
        rateAnchorFrames_ = frames;
    }

    // MARK: The menu

    UIElement buildMenu() {
        const auto& p = palette();
        auto column = vstack(0);
        column.Width(300);
        auto title = text(title_, font::headline, weightSemibold(), p.label);
        title.TextAlignment(TextAlignment::Center);
        title.HorizontalAlignment(HorizontalAlignment::Center);
        add(column, title);
        menuStatus_ = text("", font::caption, weightNormal(), p.secondaryLabel);
        menuStatus_.TextAlignment(TextAlignment::Center);
        menuStatus_.HorizontalAlignment(HorizontalAlignment::Center);
        menuStatus_.Margin(ThicknessHelper::FromLengths(0, 4, 0, 16));
        add(column, menuStatus_);
        auto resume = primaryButton("Resume", glyphs::play, [this] { showMenu(false); });
        add(column, resume);
        // An app's own package: its own settings, and its folders (SettingsScreen.cpp).
        if (appPackage::isSingleApp()) {
            auto settings = primaryButton("Settings", glyphs::settings, [this] { showSettings(true); }, p.fill);
            settings.Margin(ThicknessHelper::FromLengths(0, 10, 0, 0));
            add(column, settings);
        }
        auto quitButton = primaryButton("Quit", glyphs::close, [this] { quit(); }, p.fill);
        quitButton.Margin(ThicknessHelper::FromLengths(0, 10, 0, 0));
        add(column, quitButton);
        auto hint = text("Three quick taps in the top left corner open this menu. Back goes to the app.", font::caption,
                         weightNormal(), p.tertiaryLabel);
        hint.TextAlignment(TextAlignment::Center);
        hint.Margin(ThicknessHelper::FromLengths(0, 14, 0, 0));
        add(column, hint);
        auto box = card(column, 18, 20);
        box.HorizontalAlignment(HorizontalAlignment::Center);
        box.VerticalAlignment(VerticalAlignment::Center);
        menuOverlay_ = Grid();
        // The backdrop takes the touches, so none reaches the app under it.
        menuOverlay_.Background(brush(withAlpha(rgb(0x000000), 0.6)));
        menuOverlay_.Children().Append(box);
        setVisible(menuOverlay_, false);
        return menuOverlay_;
    }

    void showMenu(bool show) {
        if (show == menuShown_) return;
        menuShown_ = show;
        if (show) {
            releaseAllTouches();
            if (keyboardWanted_ || keyboardUp_) putKeyboardAway(true);
        }
        setVisible(menuOverlay_, show);
        shellLog::write(show ? "player: menu opened" : "player: menu closed");
    }

    /// An app's own package: its settings over the app, from the menu, the app paused under them as under the menu.
    UIElement buildSettings() {
        const auto& p = palette();
        Grid bar;
        ColumnDefinition c0, c1;
        c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        c1.Width(GridLengthHelper::Auto());
        bar.ColumnDefinitions().Append(c0);
        bar.ColumnDefinitions().Append(c1);
        auto heading = text("Settings", font::headline, weightSemibold(), p.label);
        heading.VerticalAlignment(VerticalAlignment::Center);
        bar.Children().Append(heading);
        auto done = button(text("Done", font::body, weightSemibold(), p.accentText), [this] { showSettings(false); }, ButtonKind::plain, "Done");
        Grid::SetColumn(done, 1);
        bar.Children().Append(done);
        bar.Padding(ThicknessHelper::FromLengths(16, 10, 12, 10));
        settingsHost_ = ContentControl();
        settingsHost_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        auto scroll = scrollingPage(settingsHost_);
        Grid panel;
        RowDefinition r0, r1;
        r0.Height(GridLengthHelper::Auto());
        r1.Height(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        panel.RowDefinitions().Append(r0);
        panel.RowDefinitions().Append(r1);
        panel.Children().Append(bar);
        Grid::SetRow(scroll, 1);
        panel.Children().Append(scroll);
        settingsOverlay_ = Grid();
        // Opaque, and over the bars' part of the window as the app is: what is under the phone's bars stays readable.
        settingsOverlay_.Background(brush(p.background));
        settingsOverlay_.Children().Append(panel);
        setVisible(settingsOverlay_, false);
        return settingsOverlay_;
    }

    void showSettings(bool show) {
        if (show == settingsShown_ || !settingsOverlay_) return;
        settingsShown_ = show;
        if (show && !settings_) {
            const auto& app = appPackage::single();
            settings_ = makeSettingsView(shell_, SettingsScope{app.package, app.label, true});
            settingsHost_.Content(settings_->view());
        }
        setVisible(settingsOverlay_, show);
        shellLog::write(show ? "player: settings opened" : "player: settings closed");
    }

    void quit() {
        showMenu(false);
        stopAsked_ = std::chrono::steady_clock::now();
        setVisible(closeWhileBusy_, false);
        session::stop();
    }

    // MARK: The end

    UIElement buildEndPanel() {
        const auto& p = palette();
        auto column = vstack(0);
        // As wide as a dialog, and no wider than the screen less its margins: the phone is 411 wide upright, and a fixed
        // 420 in a card of 20 a side cut its right end off, the Close button's with it.
        column.MaxWidth(420);
        endTitle_ = text("", font::title3, weightBold(), p.label);
        endTitle_.TextAlignment(TextAlignment::Center);
        endTitle_.HorizontalAlignment(HorizontalAlignment::Center);
        add(column, endTitle_);
        // A crash's reason can be a report of many lines.
        endReason_ = monospace("", font::caption, p.secondaryLabel);
        ScrollViewer reasonScroll;
        reasonScroll.MaxHeight(130);
        reasonScroll.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
        reasonScroll.Content(endReason_);
        reasonScroll.Margin(ThicknessHelper::FromLengths(0, 10, 0, 0));
        endReasonHost_ = reasonScroll;
        add(column, reasonScroll);
        endNote_ = text("", font::footnote, weightNormal(), p.secondaryLabel);
        endNote_.TextAlignment(TextAlignment::Center);
        endNote_.Margin(ThicknessHelper::FromLengths(0, 12, 0, 16));
        add(column, endNote_);
        endPrimary_ = primaryButton("Close Megler", glyphs::close, [] { Application::Current().Exit(); });
        add(column, endPrimary_);
        // The log says more than a dialog can; the library and the other
        // tabs stay usable, only Play is gone until Megler is opened again.
        auto log = primaryButton("Show the Log", glyphs::log, [this] {
            AppShell& shell = shell_;
            shell.pop();
            shell.selectTab(2);
        }, p.fill);
        log.Margin(ThicknessHelper::FromLengths(0, 10, 0, 0));
        endSecondary_ = log;
        add(column, log);
        // The log to the phone's Documents, to be sent on from there (saveTheLog).
        auto save = primaryButton("Save the Log to Documents", glyphs::download, [this] {
            saveTheLog(shell_.weak_from_this(), title_);
        }, p.fill);
        save.Margin(ThicknessHelper::FromLengths(0, 10, 0, 0));
        add(column, save);
        auto box = card(column, 18, 20);
        box.HorizontalAlignment(HorizontalAlignment::Center);
        box.VerticalAlignment(VerticalAlignment::Center);
        box.Margin(ThicknessHelper::FromLengths(16, 16, 16, 16));
        endOverlay_ = Grid();
        endOverlay_.Background(brush(withAlpha(rgb(0x000000), 0.72)));
        endOverlay_.Children().Append(box);
        setVisible(endOverlay_, false);
        return endOverlay_;
    }

    void showEnd(const session::Snapshot& snap) {
        endShown_ = true;
        releaseAllTouches();
        const bool neverStarted = snap.state == session::State::failed;
        std::string heading;
        if (neverStarted)
            heading = title_ + " couldn't start";
        else if (snap.stopRequested)
            heading = "You quit " + title_;
        else if (!snap.reason.empty())
            heading = title_ + " has stopped";
        else
            heading = title_ + " has ended";
        endTitle_.Text(hs(heading));
        // ("stopped" is all there is to say about a stop the user asked for.)
        const bool showReason = !snap.reason.empty() && !(snap.stopRequested && snap.reason.size() < 16);
        setVisible(endReasonHost_, showReason);
        endReason_.Text(hs(showReason ? snap.reason : std::string()));
        if (neverStarted) {
            // Nothing ran: the session is as it was, and Back leads back.
            endNote_.Text(L"Nothing was started. The log has the details.");
            setVisible(endPrimary_, false);
        } else {
            std::string note;
            if (snap.reason.empty() && !snap.stopRequested) note = format("It exited with status %d. ", snap.exitStatus);
            note += "One app runs each time Megler is opened: close Megler and open it again to play.";
            endNote_.Text(hs(note));
            setVisible(endPrimary_, true);
        }
        setVisible(endOverlay_, true);
    }

    // MARK: The keyboard
    //
    // A Java app's text field asks for the keyboard through KeyboardHost. The
    // keyboard is Windows' text service's, given to a text-edit context of
    // this screen's own (Windows.UI.Text.Core's CoreTextEditContext) rather
    // than to a XAML text box: a box nobody sees had the focus but was never
    // typed into on the phone, and Windows gave it the focus back by itself
    // after Megler came back from the background. The edit context has the
    // text service's focus only while the app wants the keyboard, and
    // nothing gives it back but this screen.
    //
    // What the context holds is this screen's: a short fixed beginning (so
    // that a Backspace has something to delete even when the user has typed
    // nothing yet) followed by what was typed since the field was touched.
    // Each change the keyboard makes to it is told to the app as what it is
    // against the text before: so many Backspaces for what is no longer
    // there, then the new end as typed text. That way a correction the
    // keyboard itself makes -- a word replaced by a suggestion -- comes out
    // right, as long as the app's cursor is where the typing left it. A new
    // touch of a field starts the text afresh.

    static constexpr const wchar_t* keyboardStart = L"a ";
    static constexpr size_t keyboardStartLength = 2;

    /// The bars the app's window wants hidden, and whether its status bar is light (UI thread): the shell hides and shows
    /// the phone's bars, and what they then cover reaches the app through barsCover.
    void barsWanted(int bars) {
        if (bars == bars_) return;
        bars_ = bars;
        shellLog::write(format("player: the app wants %s%s",
                               (bars & MEGLER_BAR_NAVIGATION) ? "the phone's bars hidden"
                               : (bars & MEGLER_BAR_STATUS)   ? "the status bar hidden, the navigation bar shown"
                                                            : "the phone's bars shown",
                               (bars & MEGLER_BAR_LIGHT_STATUS) ? ", a light status bar" : ""));
        shell_.refreshChrome();
    }

    /// What the phone's bars cover of the app's picture (barsCover, in view pixels), in the surface's pixels: of the surface
    /// the app was last told of, where it is shown in the window it was made for. While the window has changed and the
    /// surface has not followed yet (followWindow), nothing: what the bars cover then is told with the new surface.
    void tellInsets() {
        const auto window = session::window();
        if (!window || !touchLayer_ || surfaceW_ <= 0 || surfaceH_ <= 0) return;
        if (host_ && (host_.ActualWidth() != windowW_ || host_.ActualHeight() != windowH_)) return;
        const float layerWidth = float(touchLayer_.ActualWidth()), layerHeight = float(touchLayer_.ActualHeight());
        const float surfaceWidth = float(surfaceW_), surfaceHeight = float(surfaceH_);
        const auto shown = WindowHost::letterbox(layerWidth, layerHeight, surfaceWidth, surfaceHeight);
        if (shown.width <= 0 || shown.height <= 0) return;
        // How much of the picture a bar that covers `covered` of the window's edge covers, the picture starting `from` that edge.
        auto edge = [](double covered, float from, float size, float pixels) {
            const double inside = std::min(std::max(covered - double(from), 0.0), double(size));
            return int(inside / size * pixels + 0.5);
        };
        const int left = edge(cover_[0], shown.x, shown.width, surfaceWidth);
        const int top = edge(cover_[1], shown.y, shown.height, surfaceHeight);
        const int right = edge(cover_[2], layerWidth - (shown.x + shown.width), shown.width, surfaceWidth);
        const int bottom = edge(cover_[3], layerHeight - (shown.y + shown.height), shown.height, surfaceHeight);
        if (left == insets_[0] && top == insets_[1] && right == insets_[2] && bottom == insets_[3]) return;
        insets_[0] = left;
        insets_[1] = top;
        insets_[2] = right;
        insets_[3] = bottom;
        shellLog::write(format("player: the phone's bars cover %d, %d, %d, %d pixels of the app's left, top, right, bottom", left, top,
                               right, bottom));
        session::setInsets(left, top, right, bottom);
    }

    void attachKeyboard() {
        if (keyboardAttached_) return;
        keyboardAttached_ = true;
        KeyboardHost::shared().attach([this](int kind, int flags, int enter) { showKeyboard(kind, flags, enter); }, [this] { putKeyboardAway(false); });
        BarsHost::shared().attach([this](int bars) { barsWanted(bars); });
        DisplayHost::shared().attach([this](int requested) { orientationWanted(requested); });
        makeEditContext();
        try {
            auto pane = winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView();
            paneShowing_ = pane.Showing([this](auto&&, winrt::Windows::UI::ViewManagement::InputPaneVisibilityEventArgs const& e) {
                // The app's own layout keeps the field in view (it is told the height); Windows is not to push the page up.
                e.EnsuredFocusedElementInView(true);
                keyboardUp_ = true;
                if (!keyboardWanted_) {
                    // Nobody asked for it: it goes again, or it would hide the navigation bar with nothing to put it away.
                    shellLog::write("player: the keyboard came up though the app had not asked for it: put away again");
                    putKeyboardAwayLater();
                    return;
                }
                tellKeyboardHeight(e.OccludedRect().Height);
                showNavigationBar(true);
            });
            paneHiding_ = pane.Hiding([this](auto&&, winrt::Windows::UI::ViewManagement::InputPaneVisibilityEventArgs const& e) {
                e.EnsuredFocusedElementInView(true);
                keyboardUp_ = false;
                occludedDips_ = 0;
                showNavigationBar(false);
                session::keyboardHeight(0);
                // Put away by the user (its own key, or Back, which Windows gives the keyboard first): the app did not ask.
                if (keyboardWanted_) {
                    keyboardWanted_ = false;
                    session::keyboardClosed();
                    shellLog::write("player: the keyboard was put away by the user");
                }
                dropKeyboardFocus();
            });
        } catch (const hresult_error& e) {
            shellLog::write("player: the keyboard's coming and going cannot be watched: " + narrow(e.message()));
        }
    }

    /// The text service's side of the app's field: what it may read of the
    /// text and the caret, and what it changes.
    void makeEditContext() {
        using namespace winrt::Windows::UI::Text::Core;
        if (editContext_) return;
        try {
            editContext_ = CoreTextServicesManager::GetForCurrentView().CreateEditContext();
            editContext_.Name(L"An Android app's text field");
            // The keyboard comes when this screen asks for it, not whenever the context has the focus.
            editContext_.InputPaneDisplayPolicy(CoreTextInputPaneDisplayPolicy::Manual);
            editContext_.TextRequested([this](auto&&, CoreTextTextRequestedEventArgs const& e) {
                auto request = e.Request();
                const CoreTextRange range = request.Range();
                const int32_t size = int32_t(keyboardText_.size());
                const int32_t start = std::min(std::max(range.StartCaretPosition, 0), size);
                const int32_t end = std::min(std::max(range.EndCaretPosition, start), size);
                request.Text(winrt::hstring(keyboardText_.substr(size_t(start), size_t(end - start))));
            });
            editContext_.SelectionRequested([this](auto&&, CoreTextSelectionRequestedEventArgs const& e) {
                CoreTextRange caret{};
                caret.StartCaretPosition = caret.EndCaretPosition = editCaret_;
                e.Request().Selection(caret);
            });
            editContext_.TextUpdating([this](auto&&, CoreTextTextUpdatingEventArgs const& e) { editTextUpdating(e); });
            editContext_.SelectionUpdating([this](auto&&, CoreTextSelectionUpdatingEventArgs const& e) {
                // (The app's cursor is not moved: typing goes on where the app's own cursor is.)
                editCaret_ = std::min(std::max(e.Selection().EndCaretPosition, 0), int32_t(keyboardText_.size()));
                e.Result(CoreTextSelectionUpdatingResult::Succeeded);
            });
            editContext_.FormatUpdating([](auto&&, CoreTextFormatUpdatingEventArgs const& e) { e.Result(CoreTextFormatUpdatingResult::Succeeded); });
            editContext_.LayoutRequested([](auto&&, CoreTextLayoutRequestedEventArgs const& e) {
                // Where the field is, for the keyboard's own windows (candidates): the app's whole picture will do.
                const winrt::Windows::Foundation::Rect window = Window::Current().CoreWindow().Bounds();
                auto bounds = e.Request().LayoutBounds();
                bounds.ControlBounds(window);
                bounds.TextBounds(winrt::Windows::Foundation::Rect{window.X, window.Y, window.Width, 1});
            });
            editContext_.FocusRemoved([this](auto&&, auto&&) {
                editFocused_ = false;
                if (keyboardWanted_) shellLog::write("player: Windows took the text service's focus away from the app's field");
            });
        } catch (const hresult_error& e) {
            editContext_ = nullptr;
            shellLog::write("player: no text-edit context for the keyboard, so typing cannot reach an app: " + narrow(e.message()));
        }
    }

    void detachKeyboard() {
        if (!keyboardAttached_) return;
        keyboardAttached_ = false;
        KeyboardHost::shared().detach();
        BarsHost::shared().detach();
        DisplayHost::shared().detach();
        try {
            auto pane = winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView();
            if (paneShowing_) pane.Showing(paneShowing_);
            if (paneHiding_) pane.Hiding(paneHiding_);
            if (keyboardUp_) pane.TryHide();
        } catch (const hresult_error&) {
        }
        paneShowing_ = {};
        paneHiding_ = {};
        keyboardWanted_ = keyboardUp_ = false;
        showNavigationBar(false);
        dropKeyboardFocus();
    }

    /// The navigation bar while the app has the keyboard up, and away again
    /// after. In full-screen mode the bar comes only by a swipe up from the
    /// bottom edge, which the keyboard covers; without the bar's Back a phone
    /// with no keys of its own has nothing that puts the keyboard away. The
    /// window keeps its size: the bar lies over the picture, as in full
    /// screen.
    void showNavigationBar(bool show) {
        if (show == navigationBarShown_) return;
        navigationBarShown_ = show;
        shell_.refreshChrome();
    }

    /// putKeyboardAway, once the event that is being answered is over.
    void putKeyboardAwayLater() {
        try {
            std::weak_ptr<Page> alive = weak_from_this();
            Window::Current().Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [this, alive] {
                if (alive.expired() || keyboardWanted_) return; // gone, or asked for meanwhile
                try {
                    winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryHide();
                } catch (const hresult_error&) {
                }
                dropKeyboardFocus();
            });
        } catch (const hresult_error&) {
        }
    }

    /// The app asked for the keyboard (UI thread). Asked again for the same
    /// field, it only starts the text afresh.
    void showKeyboard(int kind, int flags, int enter) {
        using winrt::Windows::UI::Text::Core::CoreTextInputScope;
        if (!editContext_ || !running_ || menuShown_) return;
        // Suggestions only for ordinary text that did not refuse them: a password or a number has none.
        CoreTextInputScope scope = (flags & MEGLER_KEYBOARD_F_NO_SUGGESTIONS) ? CoreTextInputScope::Default : CoreTextInputScope::Text;
        switch (kind) {
        case MEGLER_KEYBOARD_NUMBER: scope = CoreTextInputScope::Number; break;
        case MEGLER_KEYBOARD_DATETIME: scope = CoreTextInputScope::Number; break;
        case MEGLER_KEYBOARD_PHONE: scope = CoreTextInputScope::TelephoneNumber; break;
        case MEGLER_KEYBOARD_EMAIL: scope = CoreTextInputScope::EmailAddress; break;
        case MEGLER_KEYBOARD_URL: scope = CoreTextInputScope::Url; break;
        case MEGLER_KEYBOARD_PASSWORD: scope = CoreTextInputScope::Password; break;
        case MEGLER_KEYBOARD_NUMBER_PASSWORD: scope = CoreTextInputScope::PasswordNumeric; break;
        default: break;
        }
        if (!keyboardWanted_)
            shellLog::write(format("player: the app asks for the keyboard (kind %d, flags 0x%X, Enter %d)", kind, unsigned(flags), enter));
        typingTold();
        typing_.kind = kind;
        keyboardWanted_ = true;
        try {
            // A new scope is the text service's to read at the next focus: so the focus is left and taken again.
            if (editFocused_) editContext_.NotifyFocusLeave();
            editFocused_ = false;
            editContext_.InputScope(scope);
            startKeyboardText();
            editContext_.NotifyFocusEnter();
            editFocused_ = true;
        } catch (const hresult_error& e) {
            shellLog::write("player: the text service would not take the app's field: " + narrow(e.message()));
        }
        try {
            winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryShow();
        } catch (const hresult_error&) {
        }
    }

    /// The app no longer wants it, or (`byUser`) Back was pressed while it was up.
    void putKeyboardAway(bool byUser) {
        if (!keyboardWanted_ && !keyboardUp_) return;
        typingTold();
        keyboardWanted_ = false;
        if (byUser) session::keyboardClosed();
        try {
            winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryHide();
        } catch (const hresult_error&) {
        }
        dropKeyboardFocus();
    }

    /// The app's field gives up the text service's focus, and with it the keyboard.
    void dropKeyboardFocus() {
        if (!editContext_ || !editFocused_) return;
        editFocused_ = false;
        try {
            editContext_.NotifyFocusLeave();
        } catch (const hresult_error&) {
        }
    }

    /// The text afresh: the fixed beginning, the caret after it; and the text service is told.
    void startKeyboardText() {
        const int32_t before = int32_t(keyboardText_.size());
        keyboardText_ = keyboardStart;
        editCaret_ = int32_t(keyboardText_.size());
        if (!editContext_ || !editFocused_) return;
        try {
            winrt::Windows::UI::Text::Core::CoreTextRange all{}, caret{};
            all.StartCaretPosition = 0;
            all.EndCaretPosition = before;
            caret.StartCaretPosition = caret.EndCaretPosition = editCaret_;
            editContext_.NotifyTextChanged(all, editCaret_, caret);
        } catch (const hresult_error&) {
        }
    }

    /// The keyboard changed the text: a range of it replaced. Taken as asked, and the difference told to the app.
    void editTextUpdating(winrt::Windows::UI::Text::Core::CoreTextTextUpdatingEventArgs const& e) {
        const winrt::Windows::UI::Text::Core::CoreTextRange range = e.Range();
        const int32_t size = int32_t(keyboardText_.size());
        const int32_t start = std::min(std::max(range.StartCaretPosition, 0), size);
        const int32_t end = std::min(std::max(range.EndCaretPosition, start), size);
        std::wstring inserted(e.Text());
        // A line feed in what the keyboard sends is its Enter: the field is told Enter, and the text keeps no line feed.
        bool enter = false;
        for (wchar_t c : inserted) enter = enter || c == L'\r' || c == L'\n';
        if (enter) inserted.erase(std::remove_if(inserted.begin(), inserted.end(), [](wchar_t c) { return c == L'\r' || c == L'\n'; }), inserted.end());
        const std::wstring now = keyboardText_.substr(0, size_t(start)) + inserted + keyboardText_.substr(size_t(end));
        editCaret_ = std::min(std::max(e.NewSelection().EndCaretPosition, 0), int32_t(now.size()));
        e.Result(winrt::Windows::UI::Text::Core::CoreTextTextUpdatingResult::Succeeded);
        const bool afresh = keyboardTyped(now);
        if (enter) {
            session::textKey(MEGLER_TEXT_ENTER);
            lastNewline_ = std::chrono::steady_clock::now();
        }
        // (The text service may not be told of a change while it is telling of one: the text is started afresh after.)
        if (afresh) {
            std::weak_ptr<Page> alive = weak_from_this();
            try {
                Window::Current().Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [this, alive] {
                    if (!alive.expired() && keyboardWanted_) startKeyboardText();
                });
            } catch (const hresult_error&) {
            }
        }
    }

    /// Backspace: the app's field loses the character before its cursor, and
    /// the edit context the one before its caret -- while there is one after
    /// the fixed beginning; with only that left the app is told all the same.
    void keyboardBackspace() {
        session::textKey(MEGLER_TEXT_BACKSPACE);
        typing_.byKey++;
        typing_.lastByKey = std::chrono::steady_clock::now();
        if (typing_.lastByKey - typing_.lastByText < std::chrono::milliseconds(200)) typing_.close++;
        if (!typingSaid_) {
            typingSaid_ = true;
            shellLog::write("player: typing (a Backspace) reaches the app");
        }
        if (editCaret_ <= int32_t(keyboardStartLength) || editCaret_ > int32_t(keyboardText_.size())) return;
        int32_t from = editCaret_ - 1;
        // (A character outside the basic plane is two halves, and one Backspace.)
        if (from > 0 && IS_LOW_SURROGATE(keyboardText_[size_t(from)]) && IS_HIGH_SURROGATE(keyboardText_[size_t(from - 1)])) --from;
        const int32_t to = editCaret_;
        keyboardText_.erase(size_t(from), size_t(to - from));
        editCaret_ = from;
        try {
            winrt::Windows::UI::Text::Core::CoreTextRange gone{}, caret{};
            gone.StartCaretPosition = from;
            gone.EndCaretPosition = to;
            caret.StartCaretPosition = caret.EndCaretPosition = from;
            editContext_.NotifyTextChanged(gone, 0, caret);
        } catch (const hresult_error&) {
        }
    }

    /// What the keyboard did in the field it leaves, counted, in the log; then the counts start afresh. Only numbers: a
    /// password's characters are never written. Facebook said a password was wrong that the user had typed right, and
    /// nothing in the log could say whether the field got what the keyboard typed. A Backspace that comes both as a key
    /// and as a change to the text (close) would take back two characters for one.
    void typingTold() {
        const Typing t = typing_;
        typing_ = Typing{};
        if (!t.typed && !t.byText && !t.byKey) return;
        shellLog::write(format("player: in that field (kind %d) the keyboard typed %d characters and took back %d by Backspace keys and %d "
                               "by changes to its text%s",
                               t.kind, t.typed, t.byKey, t.byText,
                               t.close ? format(", %d times a key and a change within 0.2 s of each other", t.close).c_str() : ""));
    }

    /// The text as the keyboard has made it: the difference told to the app.
    /// True when the text should be started afresh.
    bool keyboardTyped(const std::wstring& now) {
        if (now == keyboardText_) return false;
        if (!keyboardWanted_) {
            keyboardText_ = now;
            return true;
        }
        size_t common = 0;
        while (common < now.size() && common < keyboardText_.size() && now[common] == keyboardText_[common]) ++common;
        // Not between the two halves of a character outside the basic plane.
        if (common > 0 && now.size() > common && IS_HIGH_SURROGATE(now[common - 1])) --common;
        // One Backspace for each character that is no longer there (a pair of halves is one).
        int gone = 0;
        for (size_t i = common; i < keyboardText_.size(); ++i)
            if (!IS_LOW_SURROGATE(keyboardText_[i]) || i == common) ++gone;
        for (int i = 0; i < gone; ++i) session::textKey(MEGLER_TEXT_BACKSPACE);
        const std::wstring added = now.substr(common);
        if (!added.empty()) session::textCommit(narrow(added));
        typing_.typed += int(added.size());
        if (gone > 0) {
            typing_.byText += gone;
            typing_.lastByText = std::chrono::steady_clock::now();
            if (typing_.lastByText - typing_.lastByKey < std::chrono::milliseconds(200)) typing_.close++;
        }
        if (!typingSaid_ && (gone > 0 || !added.empty())) {
            typingSaid_ = true;
            shellLog::write("player: typing reaches the text service's field and goes to the app");
        }
        keyboardText_ = now;
        // Deleted into the fixed beginning, or grown long: afresh. (The keyboard then knows nothing of what came before.)
        return common < keyboardStartLength || keyboardText_.size() > 200;
    }

    /// How much of the app's picture the keyboard covers, in the pixels of the surface the app was last told of.
    void tellKeyboardHeight(float occludedDips) {
        occludedDips_ = occludedDips;
        const auto window = session::window();
        if (!window || !touchLayer_ || surfaceW_ <= 0 || surfaceH_ <= 0) return;
        const float layerWidth = float(touchLayer_.ActualWidth()), layerHeight = float(touchLayer_.ActualHeight());
        const float surfaceWidth = float(surfaceW_), surfaceHeight = float(surfaceH_);
        const auto shown = WindowHost::letterbox(layerWidth, layerHeight, surfaceWidth, surfaceHeight);
        if (shown.height <= 0) return;
        const float covered = std::min(std::max((shown.y + shown.height) - (layerHeight - occludedDips), 0.0f), shown.height);
        const int pixels = int(covered / shown.height * surfaceHeight + 0.5f);
        shellLog::write(format("player: the keyboard is up, %.0f DIPs high: it covers %d of the app's %d pixel rows", double(occludedDips), pixels,
                               int(surfaceHeight)));
        session::keyboardHeight(pixels);
    }

    // MARK: Touches

    void wireTouches() {
        touchLayer_.PointerPressed([this](auto&&, PointerRoutedEventArgs const& e) {
            auto point = e.GetCurrentPoint(touchLayer_);
            const Point position = point.Position();
            noteCornerTap(position);
            if (!running_ || menuShown_) return;
            const uint32_t pointer = point.PointerId();
            if (touches_.count(pointer)) return;
            // The lowest pointer id no finger is using: the first finger is 0,
            // as Android numbers them.
            int id = 0;
            auto used = [this](int candidate) {
                for (const auto& entry : touches_)
                    if (entry.second == candidate) return true;
                return false;
            };
            while (used(id)) ++id;
            touches_[pointer] = id;
            touchLayer_.CapturePointer(e.Pointer());
            send(0, id, position);
            e.Handled(true);
        });
        touchLayer_.PointerMoved([this](auto&&, PointerRoutedEventArgs const& e) {
            auto point = e.GetCurrentPoint(touchLayer_);
            auto it = touches_.find(point.PointerId());
            if (it == touches_.end()) return;
            send(1, it->second, point.Position());
            e.Handled(true);
        });
        auto end = [this](PointerRoutedEventArgs const& e, int phase) {
            auto point = e.GetCurrentPoint(touchLayer_);
            auto it = touches_.find(point.PointerId());
            if (it == touches_.end()) return;
            const int id = it->second;
            touches_.erase(it);
            send(phase, id, point.Position());
            touchLayer_.ReleasePointerCapture(e.Pointer());
            e.Handled(true);
        };
        touchLayer_.PointerReleased([end](auto&&, PointerRoutedEventArgs const& e) { end(e, 2); });
        touchLayer_.PointerCanceled([end](auto&&, PointerRoutedEventArgs const& e) { end(e, 3); });
        touchLayer_.PointerCaptureLost([end](auto&&, PointerRoutedEventArgs const& e) { end(e, 3); });
    }

    /// A position on the touch layer, in DIPs, to the app's surface pixels,
    /// through the letterbox the swap chain is shown with.
    void send(int phase, int id, Point position) {
        const auto window = session::window();
        if (!window) return;
        const float surfaceWidth = float(window->surfaceWidth()), surfaceHeight = float(window->surfaceHeight());
        const auto shown = WindowHost::letterbox(float(touchLayer_.ActualWidth()), float(touchLayer_.ActualHeight()), surfaceWidth,
                                                 surfaceHeight);
        if (shown.width <= 0 || shown.height <= 0) return;
        const float x = std::min(std::max((position.X - shown.x) / shown.width, 0.0f), 1.0f) * (surfaceWidth - 1);
        const float y = std::min(std::max((position.Y - shown.y) / shown.height, 0.0f), 1.0f) * (surfaceHeight - 1);
        session::touch(phase, id, x, y);
    }

    void releaseAllTouches() {
        for (const auto& [pointer, id] : touches_) {
            (void)pointer;
            session::touch(3, id, 0, 0);
        }
        touches_.clear();
    }

    void noteCornerTap(Point position) {
        if (position.X > menuCorner || position.Y > menuCorner) {
            cornerTaps_ = 0;
            return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (cornerTaps_ == 0 || now - firstCornerTap_ > menuTapWindow) {
            cornerTaps_ = 0;
            firstCornerTap_ = now;
        }
        if (++cornerTaps_ < 3) return;
        cornerTaps_ = 0;
        if (running_) showMenu(true);
    }

    std::string id_;
    std::string title_ = "App";
    UIElement view_{nullptr};
    Grid host_{nullptr};
    SwapChainPanel panel_{nullptr};
    Grid touchLayer_{nullptr};
    Grid busyOverlay_{nullptr};
    TextBlock busyText_{nullptr};
    Button closeWhileBusy_{nullptr};
    Grid menuOverlay_{nullptr};
    Grid settingsOverlay_{nullptr};
    ContentControl settingsHost_{nullptr};
    std::shared_ptr<SettingsView> settings_;
    TextBlock menuStatus_{nullptr};
    Grid endOverlay_{nullptr};
    TextBlock endTitle_{nullptr};
    TextBlock endReason_{nullptr};
    UIElement endReasonHost_{nullptr};
    TextBlock endNote_{nullptr};
    Button endPrimary_{nullptr};
    Button endSecondary_{nullptr};
    winrt::event_token keyToken_{};

    bool started_ = false;
    bool running_ = false;
    std::chrono::steady_clock::time_point playAt_{}; // when the app was started (start)
    bool firstPictureSaid_ = false;                  // the log has said the busy overlay went with the app's first picture
    int noPictureSaid_ = 0;                          // times the log said no picture was on the screen (sayNoPicture)
    bool mediaControls_ = false; // the system media controls stand for the app (a Java app's, while it runs)
    bool mediaControlsFailed_ = false; // Windows' media controls refused something (said once)
    bool menuShown_ = false;
    bool settingsShown_ = false; // an app's own package: its settings are over the menu
    bool endShown_ = false;
    bool exitWhenEnded_ = false; // the app ended its last activity with Back in its own package, or the flag ui-quit stopped it
    bool backLeaves_ = false;    // the app went to the back with the last Back: it is the system's
    int waited_ = 0;
    double lastWidth_ = 0, lastHeight_ = 0;
    int wish_ = MEGLER_ORIENTATION_UNSPECIFIED; // how the app wants the screen held: Android's SCREEN_ORIENTATION_* (orientationWanted)
    int turnedFor_ = -100;                      // the wish a PC's window was last turned for (turnWindow)
    bool java_ = false;    // an app drawn through the shell, not a game
    // The surface as the app was last told of it (start, followWindow), and what it follows the window with: the pixels for
    // a DIP of the window and the density as at the start, and the display's scale then.
    int surfaceW_ = 0, surfaceH_ = 0, surfaceDpi_ = 0, rotation_ = 0;
    double pixelsPerDip_ = 0, density_ = 0, startScale_ = 0;
    int cap_ = 0;                          // the render cap the app was started with (a game's surface keeps within it)
    double windowW_ = 0, windowH_ = 0;     // the window that surface was made of, in DIPs
    double seenWidth_ = 0, seenHeight_ = 0; // the window at the last poll: a size that held for a poll is followed
    float occludedDips_ = 0;               // what the keyboard covers of the window now (0: it is not up)
    std::chrono::steady_clock::time_point stopAsked_{};
    std::chrono::steady_clock::time_point rateAnchorTime_ = std::chrono::steady_clock::now();
    uint32_t rateAnchorFrames_ = 0;
    int framesPerSecond_ = 0;
    bool traceFps_ = false;

    winrt::Windows::UI::Text::Core::CoreTextEditContext editContext_{nullptr};
    std::wstring keyboardText_;   // what the edit context holds, as the app was last told it
    int32_t editCaret_ = 0;       // the text service's caret in it
    bool editFocused_ = false;    // the edit context has the text service's focus
    std::chrono::steady_clock::time_point lastNewline_{}; // the last Enter that came as a line feed in the text
    bool keyboardAttached_ = false;
    bool keyboardWanted_ = false; // the app asked for the keyboard and has not been told it is gone
    bool keyboardUp_ = false;     // Windows says it is on the screen
    bool typingSaid_ = false;     // the log has said that typing reaches the app
    // What the keyboard did in the field being edited, counted (never what it typed): see typingTold.
    struct Typing {
        int kind = 0;             // MEGLER_KEYBOARD_*
        int typed = 0;            // characters added
        int byText = 0;           // characters taken back by a change to the text
        int byKey = 0;            // Backspace keys
        int close = 0;            // a Backspace key and a change that took a character back, within 0.2 s of each other
        std::chrono::steady_clock::time_point lastByText{}, lastByKey{};
    } typing_;
    bool navigationBarShown_ = false; // out of full-screen mode while the keyboard is up
    int bars_ = 0;                    // the phone's bars the app wants hidden: MEGLER_BAR_*
    double cover_[4] = {};            // what the bars cover of the window, in view pixels (barsCover)
    int insets_[4] = {-1, -1, -1, -1}; // that, in the surface's pixels, as the app was last told it
    winrt::event_token paneShowing_{}, paneHiding_{};

    std::map<uint32_t, int> touches_;
    int cornerTaps_ = 0;
    std::chrono::steady_clock::time_point firstCornerTap_{};
};

} // namespace

std::shared_ptr<Page> makePlayerScreen(AppShell& shell, std::string apkId) {
    return std::make_shared<PlayerScreen>(shell, std::move(apkId));
}

} // namespace megler::app::ui
