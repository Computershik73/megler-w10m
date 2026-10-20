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
#include "player/BarsHost.h"
#include "player/KeyboardHost.h"
#include "player/Session.h"
#include "player/WindowHost.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Components.h"

#include <winrt/Windows.UI.Text.Core.h>

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
    bool upright() override { return upright_; }
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

    bool handleBack() override {
        const session::State state = session::snapshot().state;
        if (state == session::State::running) {
            if (settingsShown_)
                showSettings(false);
            else if (menuShown_)
                showMenu(false);
            else if (keyboardUp_)
                putKeyboardAway(true); // while the keyboard is up, Back is the keyboard's, as on Android
            else
                session::backKey();
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
        if (ended && settingsShown_) showSettings(false);
        if (ended && menuShown_) showMenu(false);
        if (settingsShown_ && settings_) settings_->refresh();
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
        // What the phone's bars cover, now that the surface's size is known (the core keeps it for the app's start).
        tellInsets();
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

    /// What the phone's bars cover of the app's picture (barsCover, in view pixels), in the surface's pixels.
    void tellInsets() {
        const auto window = session::window();
        if (!window || !touchLayer_) return;
        const float layerWidth = float(touchLayer_.ActualWidth()), layerHeight = float(touchLayer_.ActualHeight());
        const float surfaceWidth = float(window->surfaceWidth()), surfaceHeight = float(window->surfaceHeight());
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
        if (!typingSaid_ && (gone > 0 || !added.empty())) {
            typingSaid_ = true;
            shellLog::write("player: typing reaches the text service's field and goes to the app");
        }
        keyboardText_ = now;
        // Deleted into the fixed beginning, or grown long: afresh. (The keyboard then knows nothing of what came before.)
        return common < keyboardStartLength || keyboardText_.size() > 200;
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
    bool menuShown_ = false;
    bool settingsShown_ = false; // an app's own package: its settings are over the menu
    bool endShown_ = false;
    int waited_ = 0;
    double lastWidth_ = 0, lastHeight_ = 0;
    bool upright_ = false; // the app is held upright, not in landscape
    bool java_ = false;    // an app drawn through the shell, not a game
    std::chrono::steady_clock::time_point stopAsked_{};
    std::chrono::steady_clock::time_point rateAnchorTime_ = std::chrono::steady_clock::now();
    uint32_t rateAnchorFrames_ = 0;
    int framesPerSecond_ = 0;

    winrt::Windows::UI::Text::Core::CoreTextEditContext editContext_{nullptr};
    std::wstring keyboardText_;   // what the edit context holds, as the app was last told it
    int32_t editCaret_ = 0;       // the text service's caret in it
    bool editFocused_ = false;    // the edit context has the text service's focus
    std::chrono::steady_clock::time_point lastNewline_{}; // the last Enter that came as a line feed in the text
    bool keyboardAttached_ = false;
    bool keyboardWanted_ = false; // the app asked for the keyboard and has not been told it is gone
    bool keyboardUp_ = false;     // Windows says it is on the screen
    bool typingSaid_ = false;     // the log has said that typing reaches the app
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
