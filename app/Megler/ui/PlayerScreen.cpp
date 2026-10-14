// The player: a running app's screen, full-screen and in landscape.
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
// Hardware Back is the app's (AKEYCODE_BACK). The way out is a menu with
// Quit, opened by three quick taps in the top left corner (or Escape, on a
// keyboard): a phone has no other button to give it.
#include "pch.h"

#include "library/Library.h"
#include "player/KeyboardHost.h"
#include "player/Session.h"
#include "player/WindowHost.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Components.h"

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

class PlayerScreen final : public Page {
public:
    PlayerScreen(AppShell& shell, std::string apkId) : Page(shell), id_(std::move(apkId)) {
        if (const auto entry = Library::instance().find(id_)) {
            title_ = entry->title();
            upright_ = entry->report.upright();
            java_ = entry->report.engine == "java";
        }
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
    bool upright() override { return upright_; }

    bool handleBack() override {
        const session::State state = session::snapshot().state;
        if (state == session::State::running) {
            if (menuShown_)
                showMenu(false);
            else if (keyboardUp_)
                putKeyboardAway(true); // while the keyboard is up, Back is the keyboard's, as on Android
            else
                session::backKey();
            return true;
        }
        // Nothing to go back to while the app is being started.
        return state == session::State::starting || (state == session::State::idle && !started_);
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

        // What Windows gives its touch keyboard to: a text box. Nobody sees
        // this one or can touch it; it has the focus while an Android text
        // field is being edited (see "The keyboard" below), and is switched
        // off the rest of the time: Windows hands the focus back to the box
        // that had it last when Megler comes back from the background, and on
        // a phone a text box with the focus brings the keyboard up.
        keyboardField_ = TextBox();
        keyboardField_.IsEnabled(false);
        keyboardField_.IsTabStop(false);
        keyboardField_.Width(1);
        keyboardField_.Height(1);
        keyboardField_.MinWidth(1);
        keyboardField_.MinHeight(1);
        keyboardField_.Opacity(0);
        keyboardField_.IsHitTestVisible(false);
        keyboardField_.HorizontalAlignment(HorizontalAlignment::Left);
        keyboardField_.VerticalAlignment(VerticalAlignment::Top);
        keyboardField_.IsSpellCheckEnabled(false);
        keyboardField_.AcceptsReturn(false);
        keyboardField_.TextChanged([this](auto&&, auto&&) { keyboardFieldChanged(); });
        keyboardField_.KeyDown([this](auto&&, KeyRoutedEventArgs const& e) {
            if (e.Key() != winrt::Windows::System::VirtualKey::Enter) return;
            session::textKey(MEGLER_TEXT_ENTER);
            e.Handled(true);
        });
        root.Children().Append(keyboardField_);

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
        root.Children().Append(buildEndPanel());

        root.SizeChanged([this](auto&&, SizeChangedEventArgs const& e) {
            if (auto window = session::window()) window->setPanelSize(e.NewSize().Width, e.NewSize().Height);
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
                    if (e.VirtualKey() != winrt::Windows::System::VirtualKey::Escape) return;
                    if (session::snapshot().state == session::State::running) showMenu(!menuShown_);
                });
            } catch (const hresult_error&) {
            }
        }
    }

    void disappeared() override {
        releaseAllTouches();
        detachKeyboard();
        // A start that failed left nothing running: the next Play may try again.
        session::forget();
    }

    void refresh() override {
        const session::Snapshot snap = session::snapshot();
        running_ = snap.state == session::State::running;
        if (snap.state == session::State::idle && !started_) {
            waitForLandscape();
            busyText_.Text(hs("Starting " + title_ + "…"));
        }
        const auto window = session::window();
        const uint32_t frames = window ? window->presented() : 0;
        measureFrameRate(frames);

        const bool ended = snap.state == session::State::exited || snap.state == session::State::failed;
        const bool waitingForFrame = (snap.state == session::State::starting || snap.state == session::State::running) && frames == 0;
        const bool stopping = running_ && snap.stopRequested;
        setVisible(busyOverlay_, !ended && (!started_ || waitingForFrame || stopping));
        if (stopping) {
            busyText_.Text(hs("Stopping " + title_ + "…"));
            // An app that does not answer the stop: the process can still be ended.
            setVisible(closeWhileBusy_, std::chrono::steady_clock::now() - stopAsked_ > std::chrono::seconds(4));
        }
        if (ended && menuShown_) showMenu(false);
        if (ended && !endShown_) showEnd(snap);
        if (menuShown_ && window) {
            const auto audio = session::audio();
            menuStatus_.Text(hs(window->description() + format(" · %d frames a second", framesPerSecond_) +
                                (audio ? " · sound: " + audio->status() : std::string())));
        }
    }

private:
    // MARK: Starting

    /// Play turned the display to landscape as this page was shown; the
    /// surface's size is the panel's once that has happened -- the same size
    /// twice in a row, wider than tall. A device that does not turn (a
    /// desktop window) is taken as it is after three seconds.
    ///
    /// A locked phone does not turn either (the app runs under the lock
    /// screen, which is upright). For a run nobody watches there, the flag
    /// "as-landscape" (a file of that name in the flags folder) gives the
    /// app the surface it would have had: what is on the glass is then
    /// stretched, and the pictures an auto run takes are right.
    void waitForLandscape() {
        const double width = host_.ActualWidth(), height = host_.ActualHeight();
        ++waited_;
        const bool settled = width > 0 && height > 0 && width == lastWidth_ && height == lastHeight_;
        lastWidth_ = width;
        lastHeight_ = height;
        if (!settled) return;
        // (An upright app waits the same way for the display to be upright: it is, coming from the library, unless the
        // phone was lying on its side under a game a moment ago.)
        if (upright_ ? height >= width : width > height) {
            start(width, height);
        } else if (waited_ >= 12) {
            if (!upright_ && files::exists(platform::localStateDirectory() + L"\\flags\\as-landscape")) {
                shellLog::write("player: the screen did not turn; the app gets a landscape surface all the same (the flag as-landscape)");
                start(height, width);
            } else {
                start(width, height);
            }
        }
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
        const double pixelWidth = width * scale, pixelHeight = height * scale;
        const int cap = settings::getInt(settings::renderCap, settings::renderCapDefault);
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
        shellLog::write(format("player: panel %.0fx%.0f DIPs at scale %.2f (%.0fx%.0f pixels, %.0f dpi), render cap %d -> surface %dx%d, "
                               "density %d",
                               width, height, scale, pixelWidth, pixelHeight, rawDpi, cap, surfaceWidth, surfaceHeight, density));
        if (!session::start(*entry, panel_, surfaceWidth, surfaceHeight, density)) {
            failBeforeStart("An app has already run since Megler was opened. Close Megler and open it again to play.");
            return;
        }
        if (auto window = session::window()) window->setPanelSize(float(width), float(height));
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
        framesPerSecond_ = int(double(frames - rateAnchorFrames_) / seconds + 0.5);
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
        column.Width(420);
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
        auto box = card(column, 18, 20);
        box.HorizontalAlignment(HorizontalAlignment::Center);
        box.VerticalAlignment(VerticalAlignment::Center);
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
    // A Java app's text field asks for the keyboard through KeyboardHost; the
    // hidden text box takes the focus and Windows brings its keyboard up.
    // What is typed arrives as changes to that box's text. The box always
    // holds a short fixed beginning (so that a Backspace has something to
    // delete even when the user has typed nothing yet) followed by what was
    // typed since the field was touched; each change is told to the app as
    // what it is against the text before: so many Backspaces for what is no
    // longer there, then the new end as typed text. That way a correction
    // the keyboard itself makes -- a word replaced by a suggestion -- comes
    // out right, as long as the app's cursor is where the typing left it.
    // A new touch of a field starts the box afresh.

    static constexpr const wchar_t* keyboardStart = L"a ";
    static constexpr size_t keyboardStartLength = 2;

    void attachKeyboard() {
        if (keyboardAttached_) return;
        keyboardAttached_ = true;
        KeyboardHost::shared().attach([this](int kind, int flags, int enter) { showKeyboard(kind, flags, enter); }, [this] { putKeyboardAway(false); });
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

    void detachKeyboard() {
        if (!keyboardAttached_) return;
        keyboardAttached_ = false;
        KeyboardHost::shared().detach();
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
        try {
            auto view = winrt::Windows::UI::ViewManagement::ApplicationView::GetForCurrentView();
            if (show)
                view.ExitFullScreenMode();
            else
                view.TryEnterFullScreenMode();
        } catch (const hresult_error& e) {
            shellLog::write("player: the navigation bar cannot be shown or hidden: " + narrow(e.message()));
        }
    }

    /// putKeyboardAway, once the event that is being answered is over.
    void putKeyboardAwayLater() {
        try {
            std::weak_ptr<Page> alive = weak_from_this();
            keyboardField_.Dispatcher().RunAsync(winrt::Windows::UI::Core::CoreDispatcherPriority::Normal, [this, alive] {
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
    /// field, it only starts the box afresh.
    void showKeyboard(int kind, int flags, int enter) {
        if (!keyboardField_ || !running_ || menuShown_) return;
        using winrt::Windows::UI::Xaml::Input::InputScopeNameValue;
        InputScopeNameValue value = InputScopeNameValue::Default;
        switch (kind) {
        case MEGLER_KEYBOARD_NUMBER: value = InputScopeNameValue::Number; break;
        case MEGLER_KEYBOARD_DATETIME: value = InputScopeNameValue::Number; break;
        case MEGLER_KEYBOARD_PHONE: value = InputScopeNameValue::TelephoneNumber; break;
        case MEGLER_KEYBOARD_EMAIL: value = InputScopeNameValue::EmailSmtpAddress; break;
        case MEGLER_KEYBOARD_URL: value = InputScopeNameValue::Url; break;
        case MEGLER_KEYBOARD_PASSWORD: value = InputScopeNameValue::Password; break;
        case MEGLER_KEYBOARD_NUMBER_PASSWORD: value = InputScopeNameValue::NumericPin; break;
        default: break;
        }
        try {
            winrt::Windows::UI::Xaml::Input::InputScope scope;
            winrt::Windows::UI::Xaml::Input::InputScopeName name;
            name.NameValue(value);
            scope.Names().Append(name);
            keyboardField_.InputScope(scope);
            // Suggestions only for ordinary text that did not refuse them: a password or a number has none.
            keyboardField_.IsTextPredictionEnabled(kind == MEGLER_KEYBOARD_TEXT && !(flags & MEGLER_KEYBOARD_F_NO_SUGGESTIONS));
        } catch (const hresult_error&) {
        }
        if (!keyboardWanted_)
            shellLog::write(format("player: the app asks for the keyboard (kind %d, flags 0x%X, Enter %d)", kind, unsigned(flags), enter));
        keyboardWanted_ = true;
        startKeyboardText();
        keyboardField_.IsEnabled(true);
        keyboardField_.IsTabStop(true);
        keyboardField_.Focus(FocusState::Programmatic);
        try {
            winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryShow();
        } catch (const hresult_error&) {
        }
    }

    /// The app no longer wants it, or (`byUser`) Back was pressed while it was up.
    void putKeyboardAway(bool byUser) {
        if (!keyboardWanted_ && !keyboardUp_) return;
        keyboardWanted_ = false;
        if (byUser) session::keyboardClosed();
        try {
            winrt::Windows::UI::ViewManagement::InputPane::GetForCurrentView().TryHide();
        } catch (const hresult_error&) {
        }
        dropKeyboardFocus();
    }

    /// The hidden box gives the focus up (a control that is switched off
    /// cannot keep it), and with it the keyboard; and it stays off, so that
    /// nothing gives it the focus back until the app asks again.
    void dropKeyboardFocus() {
        if (!keyboardField_) return;
        keyboardField_.IsTabStop(false);
        keyboardField_.IsEnabled(false);
    }

    void startKeyboardText() {
        keyboardText_ = keyboardStart;
        keyboardField_.Text(keyboardText_);
        keyboardField_.SelectionStart(int32_t(keyboardText_.size()));
    }

    /// The box's text has changed: tell the app the difference.
    void keyboardFieldChanged() {
        if (!keyboardField_) return;
        std::wstring now(keyboardField_.Text());
        if (now == keyboardText_) return; // set by startKeyboardText, not typed
        if (!keyboardWanted_) {
            startKeyboardText();
            return;
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
        std::wstring added = now.substr(common);
        added.erase(std::remove(added.begin(), added.end(), L'\r'), added.end());
        if (!added.empty()) session::textCommit(narrow(added));
        keyboardText_ = now;
        // Deleted into the fixed beginning, or grown long: afresh. (The keyboard then knows nothing of what came before.)
        if (common < keyboardStartLength || keyboardText_.size() > 200) startKeyboardText();
    }

    /// How much of the app's picture the keyboard covers, in the surface's pixels.
    void tellKeyboardHeight(float occludedDips) {
        const auto window = session::window();
        if (!window || !touchLayer_) return;
        const float layerWidth = float(touchLayer_.ActualWidth()), layerHeight = float(touchLayer_.ActualHeight());
        const float surfaceWidth = float(window->surfaceWidth()), surfaceHeight = float(window->surfaceHeight());
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
    bool menuShown_ = false;
    bool endShown_ = false;
    int waited_ = 0;
    double lastWidth_ = 0, lastHeight_ = 0;
    bool upright_ = false; // the app is held upright, not in landscape
    bool java_ = false;    // an app drawn through the shell, not a game
    std::chrono::steady_clock::time_point stopAsked_{};
    std::chrono::steady_clock::time_point rateAnchorTime_ = std::chrono::steady_clock::now();
    uint32_t rateAnchorFrames_ = 0;
    int framesPerSecond_ = 0;

    TextBox keyboardField_{nullptr};
    std::wstring keyboardText_;   // what the hidden box held when the app was last told
    bool keyboardAttached_ = false;
    bool keyboardWanted_ = false; // the app asked for the keyboard and has not been told it is gone
    bool keyboardUp_ = false;     // Windows says it is on the screen
    bool navigationBarShown_ = false; // out of full-screen mode while the keyboard is up
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
