#include "pch.h"

#include "ui/AppShell.h"

#include "auto/AutoRun.h"
#include "library/Library.h"
#include "player/Session.h"
#include "player/StorageHost.h"
#include "shell/AppPackage.h"
#include "shell/Checks.h"
#include "shell/Files.h"
#include "shell/PickedFile.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/GooglePlay.h"
#include "ui/Ota.h"
#include "ui/SeparateApps.h"
#include "ui/SharedFolders.h"
#include "ui/Theme.h"
#include "ui/UiShots.h"
#include "library/PhoneAppsList.h"
#include "player/SystemHost.h"

#include "megler.h"

#include <winrt/Windows.System.UserProfile.h>

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::ViewManagement;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::UI::Core::CoreDispatcherPriority;

struct AppShell::Stack {
    std::vector<std::shared_ptr<Page>> pages;
};

AppShell::AppShell() : import_(std::make_shared<ImportProgress>()) {}

/*
 * The UI thread's watch: every half second a ping through its dispatcher, at high priority; one it has not answered in two
 * seconds is said with where the thread stands (megler_thread_where), again at five and at ten seconds, and when it answers,
 * how late. The phone ends an app whose UI thread does not answer, as a hang, with an error report and no dump: Instagram,
 * signing in with Google (package 567), went so with nothing in the log, and only the shell's taking the window away
 * after it (a fault in CoreUIComponents.dll) was seen. A watch that was not run itself for two seconds (Megler suspended)
 * starts its ping again rather than count that time.
 */
static void watchUiThread(winrt::Windows::UI::Core::CoreDispatcher dispatcher) {
    const DWORD ui = GetCurrentThreadId();
    std::thread([dispatcher, ui] {
        auto answered = std::make_shared<std::atomic<uint64_t>>(0);
        uint64_t sent = 0;
        ULONGLONG sentAt = 0, last = GetTickCount64();
        int said = 0;
        for (;;) {
            Sleep(500);
            const ULONGLONG now = GetTickCount64();
            const bool frozen = now - last > 2000;
            last = now;
            if (answered->load() >= sent || frozen) {
                if (said && !frozen) shellLog::write(format("ui: the UI thread answered again, %llu ms after it was asked", now - sentAt));
                said = 0;
                sentAt = now;
                const uint64_t n = ++sent;
                try {
                    dispatcher.RunAsync(CoreDispatcherPriority::High, [answered, n] { *answered = n; });
                } catch (...) {
                    return;  // the window is gone
                }
                continue;
            }
            const ULONGLONG late = now - sentAt;
            const int step = late >= 10000 ? 3 : late >= 5000 ? 2 : late >= 2000 ? 1 : 0;
            if (step > said) {
                said = step;
                char where[3000];
                if (!megler_thread_where(ui, where, sizeof(where))) strcpy_s(where, "it could not be stopped to look");
                shellLog::write(format("ui: the UI thread has not answered for %llu ms: %s", late, where));
                shellLog::flush();
            }
        }
    }).detach();
}

AppShell::~AppShell() {
    if (timer_) timer_.Stop();
}

AppShell::Stack& AppShell::currentStack() { return *stacks_[size_t(selectedTab_)]; }

UIElement AppShell::root() {
    const auto& p = palette();
    dispatcher_ = Window::Current().Dispatcher();
    static bool watched = false;
    if (!watched) {
        watched = true;
        watchUiThread(dispatcher_);
    }
    root_ = Grid();
    root_.RequestedTheme(p.dark ? ElementTheme::Dark : ElementTheme::Light);
    root_.Background(brush(p.surface));
    for (int i = 0; i < 3; ++i) {
        RowDefinition row;
        row.Height(i == 1 ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star) : GridLengthHelper::Auto());
        root_.RowDefinitions().Append(row);
    }

    if (ApiInformation::IsTypePresent(L"Windows.UI.ViewManagement.StatusBar")) {
        try {
            auto status = StatusBar::GetForCurrentView();
            status.BackgroundColor(p.surface);
            status.BackgroundOpacity(1.0);
            status.ForegroundColor(p.onSurface);
        } catch (...) {
        }
    }
    try {
        auto bar = ApplicationView::GetForCurrentView().TitleBar();
        bar.BackgroundColor(p.surface);
        bar.ForegroundColor(p.onSurface);
        bar.ButtonBackgroundColor(p.surface);
        bar.ButtonForegroundColor(p.onSurface);
        bar.InactiveBackgroundColor(p.surface);
        bar.ButtonInactiveBackgroundColor(p.surface);
    } catch (...) {
    }

    // An app's own package shows its splash screen's picture until its app is open, not the library (poll).
    const std::shared_ptr<Page> roots[] = {appPackage::isSingleApp() ? makeStartScreen(*this) : makeLibraryScreen(*this), makeChecksScreen(*this),
                                           makeLogScreen(*this), makeSettingsScreen(*this)};
    for (const auto& page : roots) {
        auto stack = std::make_unique<Stack>();
        stack->pages.push_back(page);
        stacks_.push_back(std::move(stack));
    }
    buildChrome();
    buildTabBar();
    showTop();

    // Keep clear of the status bar and the navigation bar. On the phone
    // UseVisible alone (App.cpp) still leaves the window spanning the whole
    // display, so the content is padded by the gap between the window and
    // its visible bounds, again whenever they change (rotation, the
    // navigation bar hiding or coming back) -- as gecko-w10m's and
    // podium-w10m's shells do.
    ApplicationView::GetForCurrentView().VisibleBoundsChanged([this](auto&&, auto&&) { applyVisibleBounds(); });
    Window::Current().SizeChanged([this](auto&&, auto&&) { applyVisibleBounds(); });
    applyVisibleBounds();

    // Hardware Back (the phone's only navigation button besides the camera).
    using winrt::Windows::UI::Core::SystemNavigationManager;
    SystemNavigationManager::GetForCurrentView().BackRequested([this](auto&&, auto const& e) { e.Handled(goBack()); });

    // What a running app asks of the phone (player/SystemHost.h): an APK to install goes to the installer, as one picked.
    {
        std::weak_ptr<AppShell> weak = weak_from_this();
        systemHost::Hooks hooks;
        hooks.install = [weak](const std::wstring& path, const std::string& name, const std::string& from) {
            if (auto self = weak.lock()) self->installApkPath(path, name, from);
        };
        hooks.uninstall = [weak](const std::string& package, const std::string& from) {
            if (auto self = weak.lock()) self->uninstallApp(package, from);
        };
        hooks.cannotLaunch = [weak](const std::string& why) {
            if (auto self = weak.lock()) self->alert("Couldn't Start the App", why);
        };
        // A permission the app requests that Megler asks the user for itself (player/PermissionHost.h), in its turn.
        hooks.ask = [weak](const std::string& title, const std::string& message, const std::vector<std::string>& buttons, std::function<void(int)> done) {
            auto self = weak.lock();
            if (!self) {
                done(-1);
                return;
            }
            DialogSpec spec;
            spec.title = title;
            spec.message = message;
            spec.buttons = buttons;
            self->dialogWhenFree(std::move(spec), [done](int button, std::string) { done(button); });
        };
        systemHost::attach(std::move(hooks));
    }

    timer_ = DispatcherTimer();
    timer_.Interval(std::chrono::milliseconds(250));
    timer_.Tick([this](auto&&, auto&&) { poll(); });
    timer_.Start();
    return root_;
}

void AppShell::applyVisibleBounds() {
    auto view = ApplicationView::GetForCurrentView();
    auto visible = view.VisibleBounds();
    auto window = Window::Current().Bounds();
    // Negative or absurd gaps mean the rectangles aren't comparable (they
    // were seen briefly during rotation); padding nothing beats padding wrong.
    auto sane = [](double v) { return v > 0 && v < 400 ? v : 0.0; };
    Thickness covered{};
    covered.Left = sane(visible.X - window.X);
    covered.Top = sane(visible.Y - window.Y);
    covered.Right = sane((window.X + window.Width) - (visible.X + visible.Width));
    covered.Bottom = sane((window.Y + window.Height) - (visible.Y + visible.Height));
    // A full-screen page (the player) has the whole display, under the bars where they are, and is told what they cover.
    if (root_) root_.Padding(fullScreen_ ? Thickness{} : covered);
    if (fullScreen_ && !stacks_.empty() && !currentStack().pages.empty())
        currentStack().pages.back()->barsCover(covered.Left, covered.Top, covered.Right, covered.Bottom);
}

void AppShell::applyFullScreen(bool fullScreen, unsigned orientations, bool hideStatus, bool hideNavigation, bool lightStatus) {
    orientations = fullScreen ? orientations : 0;
    // The phone hides its navigation bar only in full-screen mode, and that hides the status bar too.
    hideNavigation = fullScreen && hideNavigation;
    hideStatus = fullScreen && (hideStatus || hideNavigation);
    lightStatus = fullScreen && lightStatus;
    if (fullScreen == fullScreen_ && orientations == orientations_ && hideStatus == hideStatus_ && hideNavigation == hideNavigation_ &&
        lightStatus == lightStatus_)
        return;
    const bool turn = fullScreen != fullScreen_ || orientations != orientations_;
    fullScreen_ = fullScreen;
    orientations_ = orientations;
    hideStatus_ = hideStatus;
    hideNavigation_ = hideNavigation;
    lightStatus_ = lightStatus;
    using namespace winrt::Windows::Graphics::Display;
    if (turn) {
        try {
            // The library is a portrait screen; a full-screen page says which ways up it may be shown -- the player as its
            // app asks (a game lying down, an app as its activity in front wants it, or as the phone is held), 0 being
            // any. Off a phone there is nothing to turn back to: no preference.
            DisplayInformation::AutoRotationPreferences(fullScreen ? DisplayOrientations(orientations)
                                                                   : (platform::isMobile() ? DisplayOrientations::Portrait : DisplayOrientations::None));
        } catch (const hresult_error& e) {
            shellLog::write("ui: AutoRotationPreferences failed: " + narrow(e.message()));
        }
    }
    bool entered = false;
    try {
        // A full-screen page lies under the phone's bars (UseCoreWindow), which then cover what they cover of it; the bars go
        // only when it asks (the player: when its app does).
        auto view = ApplicationView::GetForCurrentView();
        view.SetDesiredBoundsMode(fullScreen ? ApplicationViewBoundsMode::UseCoreWindow : ApplicationViewBoundsMode::UseVisible);
        if (hideNavigation)
            entered = view.TryEnterFullScreenMode();
        else
            view.ExitFullScreenMode();
    } catch (const hresult_error& e) {
        shellLog::write("ui: full-screen mode failed: " + narrow(e.message()));
    }
    if (ApiInformation::IsTypePresent(L"Windows.UI.ViewManagement.StatusBar")) {
        try {
            const auto& p = palette();
            auto status = StatusBar::GetForCurrentView();
            if (hideStatus) {
                status.HideAsync();
            } else {
                // Over a full-screen page the bar is see-through: under it is what the app paints there (an Android app's
                // decor paints its status bar's colour), with icons light or dark as the app's bar is.
                status.BackgroundColor(p.background);
                status.BackgroundOpacity(fullScreen ? 0.0 : 1.0);
                status.ForegroundColor(fullScreen ? (lightStatus ? rgb(0x000000) : rgb(0xFFFFFF)) : p.label);
                status.ShowAsync();
            }
        } catch (...) {
        }
    }
    // (The log's words for the old two: "upright" and "landscape".)
    const char* held = orientations == 0                                  ? "as the phone is held"
                       : orientations == unsigned(DisplayOrientations::Portrait) ? "upright"
                       : !(orientations & (unsigned(DisplayOrientations::Portrait) | unsigned(DisplayOrientations::PortraitFlipped)))
                           ? "landscape"
                       : !(orientations & (unsigned(DisplayOrientations::Landscape) | unsigned(DisplayOrientations::LandscapeFlipped)))
                           ? "upright, either way up"
                           : "turned as the phone is";
    shellLog::write(!fullScreen ? std::string("ui: back to portrait")
                                : std::string("ui: ") + held + format(" (DisplayOrientations %u), full screen", orientations) +
                                      (hideNavigation ? (entered ? ", the phone's bars hidden" : ", the phone's bars hidden (full-screen mode refused)")
                                       : hideStatus   ? ", the status bar hidden, the navigation bar shown"
                                                      : ", the phone's bars shown"));
    applyVisibleBounds();
}

void AppShell::buildChrome() {
    const auto& p = palette();
    // Material 3's small top app bar: 64 high, Back (an icon button) at its start, the title in title large, the page's
    // actions at its end.
    navigationBar_ = Grid();
    navigationBar_.Height(64);
    navigationBar_.Background(brush(p.surface));
    for (int i = 0; i < 3; ++i) {
        ColumnDefinition c;
        c.Width(i == 1 ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star) : GridLengthHelper::Auto());
        navigationBar_.ColumnDefinitions().Append(c);
    }

    backButton_ = iconButton(glyphs::back, [this] { pop(); }, "Back", p.onSurface);
    backButton_.Margin(ThicknessHelper::FromLengths(4, 0, 0, 0));
    backButton_.VerticalAlignment(VerticalAlignment::Center);
    navigationBar_.Children().Append(backButton_);

    titleText_ = text("", font::titleLarge, weightNormal(), p.onSurface);
    titleText_.TextWrapping(TextWrapping::NoWrap);
    titleText_.TextTrimming(TextTrimming::CharacterEllipsis);
    titleText_.VerticalAlignment(VerticalAlignment::Center);
    titleText_.Margin(ThicknessHelper::FromLengths(16, 0, 8, 0));
    Grid::SetColumn(titleText_, 1);
    navigationBar_.Children().Append(titleText_);

    toolbarHost_ = ContentControl();
    toolbarHost_.VerticalAlignment(VerticalAlignment::Center);
    toolbarHost_.HorizontalContentAlignment(HorizontalAlignment::Right);
    toolbarHost_.Foreground(brush(p.onSurfaceVariant));
    toolbarHost_.Margin(ThicknessHelper::FromLengths(0, 0, 4, 0));
    Grid::SetColumn(toolbarHost_, 2);
    navigationBar_.Children().Append(toolbarHost_);

    Grid::SetRow(navigationBar_, 0);
    root_.Children().Append(navigationBar_);

    contentHost_ = Border();
    Grid::SetRow(contentHost_, 1);
    root_.Children().Append(contentHost_);
}

void AppShell::buildTabBar() {
    const auto& p = palette();
    // Material 3's navigation bar: 80 high on the surface container, each destination an icon over its label, the chosen
    // one's icon on a pill of the secondary container.
    tabBar_ = Grid();
    tabBar_.Height(80);
    tabBar_.Background(brush(p.surfaceContainer));
    tabButtons_.clear();
    tabIndicators_.clear();
    tabLabels_.clear();
    const std::pair<const char*, wchar_t> tabs[] = {
        {"Library", glyphs::library}, {"Device", glyphs::device}, {"Log", glyphs::log}, {"Settings", glyphs::settings}};
    for (int i = 0; i < 4; ++i) {
        ColumnDefinition c;
        c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        tabBar_.ColumnDefinitions().Append(c);
        auto content = vstack(4);
        content.HorizontalAlignment(HorizontalAlignment::Center);
        content.VerticalAlignment(VerticalAlignment::Center);
        Border indicator;
        indicator.Width(64);
        indicator.Height(32);
        indicator.CornerRadius(CornerRadiusHelper::FromUniformRadius(16));
        auto g = icon(tabs[i].second, 24);
        g.HorizontalAlignment(HorizontalAlignment::Center);
        indicator.Child(g);
        indicator.HorizontalAlignment(HorizontalAlignment::Center);
        add(content, indicator);
        auto t = text(tabs[i].first, font::labelMedium, weightMedium());
        t.HorizontalAlignment(HorizontalAlignment::Center);
        t.TextWrapping(TextWrapping::NoWrap);
        add(content, t);
        auto b = button(content, [this, i] { selectTab(i); }, ButtonKind::plain, tabs[i].first);
        b.VerticalAlignment(VerticalAlignment::Stretch);
        b.Padding(ThicknessHelper::FromLengths(0, 12, 0, 16));
        Grid::SetColumn(b, i);
        tabBar_.Children().Append(b);
        tabButtons_.push_back(b);
        tabIndicators_.push_back(indicator);
        tabLabels_.push_back(t);
    }
    Grid::SetRow(tabBar_, 2);
    root_.Children().Append(tabBar_);
}

void AppShell::showTop() {
    const auto& p = palette();
    auto page = currentStack().pages.back();
    contentHost_.Child(nullptr);
    contentHost_.Child(page->view());
    for (size_t i = 0; i < tabButtons_.size(); ++i) {
        const bool chosen = int(i) == selectedTab_;
        // (The icon takes the button's colour; the label has its own, as Material's do.)
        tabButtons_[i].Foreground(brush(chosen ? p.onSecondaryContainer : p.onSurfaceVariant));
        tabIndicators_[i].Background(brush(chosen ? p.secondaryContainer : withAlpha(p.secondaryContainer, 0)));
        tabLabels_[i].Foreground(brush(chosen ? p.onSurface : p.onSurfaceVariant));
    }
    page->appeared();
    page->refresh();
    refreshChrome();
}

void AppShell::refreshChrome() {
    if (stacks_.empty() || !titleText_) return;
    auto& stack = currentStack();
    auto page = stack.pages.back();
    const bool full = page->fullScreen();
    applyFullScreen(full, page->orientations(), page->hideStatusBar(), page->hideNavigationBar(), page->lightStatusBar());
    setVisible(navigationBar_, !full);
    setVisible(tabBar_, !full);
    const bool canGoBack = stack.pages.size() > 1;
    titleText_.Text(hs(page->title()));
    setVisible(backButton_, canGoBack);
    titleText_.Margin(ThicknessHelper::FromLengths(canGoBack ? 4 : 16, 0, 8, 0));
    auto item = page->toolbarItem();
    if (toolbarHost_.Content() != item) toolbarHost_.Content(item);
    try {
        using winrt::Windows::UI::Core::AppViewBackButtonVisibility;
        using winrt::Windows::UI::Core::SystemNavigationManager;
        SystemNavigationManager::GetForCurrentView().AppViewBackButtonVisibility(
            canGoBack ? AppViewBackButtonVisibility::Visible : AppViewBackButtonVisibility::Collapsed);
    } catch (...) {
    }
}

void AppShell::push(std::shared_ptr<Page> page) {
    auto& stack = currentStack();
    stack.pages.back()->disappeared();
    stack.pages.push_back(std::move(page));
    shellLog::write("ui: showing " + stack.pages.back()->title());
    showTop();
}

void AppShell::pop() {
    auto& stack = currentStack();
    if (stack.pages.size() <= 1) return;
    // The popped page may be the caller (a button of its own): it is let go
    // of only once this turn of the UI thread is over.
    auto gone = stack.pages.back();
    gone->disappeared();
    stack.pages.pop_back();
    showTop();
    runOnUiThread([gone] {});
}

bool AppShell::goBack() {
    if (dialogOpen_) {
        closeDialog(-1); // Back dismisses a dialog, as on Android
        return true;
    }
    if (currentStack().pages.back()->handleBack()) return true;
    if (currentStack().pages.back()->backIsTheSystems()) return false;
    if (currentStack().pages.size() > 1) {
        pop();
        return true;
    }
    if (selectedTab_ != 0) {
        selectTab(0);
        return true;
    }
    return false;
}

void AppShell::quitAndClose() {
    if (!currentStack().pages.back()->quitAndClose()) {
        shellLog::write("ui: no app runs: Megler closes");
        Application::Current().Exit();
    }
}

void AppShell::selectTab(int tab) {
    if (tab == selectedTab_) {
        // Tapping the current tab again goes to its root.
        std::vector<std::shared_ptr<Page>> gone;
        while (currentStack().pages.size() > 1) {
            gone.push_back(currentStack().pages.back());
            if (gone.size() == 1) gone.back()->disappeared();
            currentStack().pages.pop_back();
        }
        showTop();
        // (As in pop(): one of them may be the caller.)
        runOnUiThread([gone] {});
        return;
    }
    currentStack().pages.back()->disappeared();
    selectedTab_ = tab;
    showTop();
}

void AppShell::poll() {
    try {
        if (!stacks_.empty()) currentStack().pages.back()->refresh();
        refreshChrome();
        updateDisplayRequest();
        platform::sampleMemory();
        autorun::tick(*this);
        uiShots::poll(*this);
        phoneAppsList::publish();
        tellAskedInstalls();
        // Google Play or not, asked at the first start; once wanted, Megler's own installed (ui/GooglePlay.h).
        googlePlay::tick(*this);
        // A newer Megler on GitHub, for this Windows and processor: offered, downloaded and installed (ui/Ota.h).
        ota::tick(*this);
        // An app's own package opens its app as soon as the library has it read: straight into the player when it can
        // run here, else its page, which says why not. Before the player, Megler's folders the app has not got are offered
        // (ui/SharedFolders.h), once the phone's folders are known, to an app that asks for the storage.
        if (appPackage::isSingleApp() && !singleAppOpened_) {
            const auto entry = Library::instance().find(appPackage::singleAppId());
            if (entry && entry->analysed && (!entry->report.ready() || StorageHost::shared().searched())) {
                singleAppOpened_ = true;
                shellLog::write("app package: opening " + entry->report.package);
                const std::string id = entry->id;
                if (entry->report.ready() && playStoreCanStart(entry->report.package)) {
                    // The app's private files Megler handed over when it made this package, taken first (shell/AppPackage.h).
                    std::weak_ptr<AppShell> weak = weak_from_this();
                    const bool offerFolders = sharedFolders::wantedBy(entry->report);
                    runInBackground([] { appPackage::takeHandover(); },
                                    [weak, id, offerFolders] {
                                        auto play = [weak, id](int, int) {
                                            if (auto self = weak.lock()) self->push(makePlayerScreen(*self, id));
                                        };
                                        if (offerFolders) sharedFolders::offer(false, play);
                                        else play(0, 0);
                                    });
                } else {
                    push(makeAppDetailScreen(*this, id));
                }
            }
        }
        // Megler itself lists its added folders for the separate apps once the phone's folders are known (and Settings
        // again whenever they change).
        if (!appPackage::isSingleApp() && !foldersPublished_ && StorageHost::shared().searched()) {
            foldersPublished_ = true;
            sharedFolders::publish();
        }
        // Megler's own storage of before, to the shared folder, once: from the start after, Megler's apps (an auto run's)
        // find /sdcard where the apps' own packages do.
        if (!appPackage::isSingleApp() && !storageShared_ && !appPackage::meglersStorageShared()) {
            storageShared_ = true;
            runInBackground([] { appPackage::shareMeglersStorage(); });
        }
    } catch (const hresult_error& e) {
        shellLog::write("ui: refresh failed: " + narrow(e.message()));
    } catch (const std::exception& e) {
        shellLog::write(std::string("ui: refresh failed: ") + e.what());
    }
}

void AppShell::updateDisplayRequest() {
    // Keep the screen on while an app runs or a check measures: the phone
    // dims and locks after its timeout otherwise, and a locked phone
    // suspends us.
    const session::State state = session::snapshot().state;
    // And while packages are installed: an app's own package made and installed (or updated, or uninstalled), each of
    // Update All's, and Megler's own update from Pictures\Megler or GitHub, its download too (ui/Ota.h). A phone that locks
    // meanwhile suspends Megler, and an install ends with no result (the owner asked for it, 2026-11-02, when the Device
    // Portal's upload took minutes), and the download stops.
    const bool installing = !separateApps::working().empty() || appPackage::selfUpdating() || ota::busy();
    // And always, while Megler is in front, under the flag keep-screen-on (LocalState\flags): a phone left waiting for the
    // next package, which is copied to it and installed from it, or for a test, is not to lock meanwhile.
    const bool wanted = state == session::State::starting || state == session::State::running || checks::running() || installing ||
                        files::exists(platform::localStateDirectory() + L"\\flags\\keep-screen-on");
    if (wanted == displayRequestActive_) return;
    try {
        if (!displayRequest_) displayRequest_ = winrt::Windows::System::Display::DisplayRequest();
        if (wanted)
            displayRequest_.RequestActive();
        else
            displayRequest_.RequestRelease();
        shellLog::write(wanted ? (installing ? "display: keeping the screen on while packages are installed" : "display: keeping the screen on")
                               : "display: screen timeout allowed again");
    } catch (const hresult_error& e) {
        shellLog::write("display: DisplayRequest failed: " + narrow(e.message()));
    }
    displayRequestActive_ = wanted;
}

void AppShell::runInBackground(std::function<void()> work, std::function<void()> then) {
    std::weak_ptr<AppShell> weak = weak_from_this();
    auto dispatcher = dispatcher_;
    std::thread([work = std::move(work), then = std::move(then), weak, dispatcher] {
        try {
            work();
        } catch (const std::exception& e) {
            shellLog::write(std::string("worker: uncaught exception: ") + e.what());
        } catch (const hresult_error& e) {
            shellLog::write("worker: uncaught WinRT error: " + narrow(e.message()));
        } catch (...) {
            shellLog::write("worker: uncaught exception");
        }
        if (!then || !dispatcher) return;
        dispatcher.RunAsync(CoreDispatcherPriority::Normal, [then, weak] {
            if (!weak.lock()) return;
            try {
                then();
            } catch (const std::exception& e) {
                shellLog::write(std::string("ui: completion failed: ") + e.what());
            } catch (const hresult_error& e) {
                shellLog::write("ui: completion failed: " + narrow(e.message()));
            }
        });
    }).detach();
}

void AppShell::runOnUiThread(std::function<void()> work) {
    std::weak_ptr<AppShell> weak = weak_from_this();
    dispatcher_.RunAsync(CoreDispatcherPriority::Normal, [work = std::move(work), weak] {
        if (weak.lock()) work();
    });
}

// MARK: Dialogs

void AppShell::dialog(DialogSpec spec, std::function<void(int, std::string)> done) {
    if (dialogOpen_ || !root_) return;
    dialogOpen_ = true;
    const auto& p = palette();
    dialogDone_ = std::move(done);
    // Material 3's basic dialog: the high surface container, corners of 28 and a margin of 24 within, over a scrim; its
    // headline in headline small, its text in body medium, its buttons text buttons at its end.
    dialogLayer_ = Grid();
    Grid::SetRowSpan(dialogLayer_, 3);
    dialogLayer_.Background(brush(p.scrim));
    dialogLayer_.Tapped([this](auto&&, auto const& e) {
        if (e.OriginalSource().try_as<Grid>() == dialogLayer_) closeDialog(-1);
    });
    auto column = vstack(0);
    if (!spec.title.empty()) {
        auto t = text(spec.title, font::headlineSmall, weightNormal(), p.onSurface);
        t.Margin(ThicknessHelper::FromLengths(0, 0, 0, 16));
        add(column, t);
    }
    if (!spec.message.empty()) add(column, text(spec.message, font::bodyMedium, weightNormal(), p.onSurfaceVariant));
    dialogField_ = nullptr;
    dialogSecret_ = nullptr;
    if (spec.field && spec.secret) {
        auto field = passwordField(dialogSecret_, spec.fieldPlaceholder);
        field.Margin(ThicknessHelper::FromLengths(0, 16, 0, 0));
        add(column, field);
    } else if (spec.field) {
        auto field = textField(dialogField_, spec.fieldText, spec.fieldPlaceholder);
        field.Margin(ThicknessHelper::FromLengths(0, 16, 0, 0));
        add(column, field);
        if (!spec.fieldSuggestions.empty()) {
            // Material's suggestion chips: outlined, corners of 8, 32 high.
            auto chips = hstack(8);
            chips.Margin(ThicknessHelper::FromLengths(0, 12, 0, 0));
            for (const std::string& suggestion : spec.fieldSuggestions) {
                auto label = text(suggestion, font::labelLarge, weightMedium(), p.onSurfaceVariant);
                label.VerticalAlignment(VerticalAlignment::Center);
                auto chip = button(label, [this, suggestion] {
                    if (dialogField_) dialogField_.Text(hs(suggestion));
                }, ButtonKind::rounded, suggestion);
                chip.Height(32);
                chip.Padding(ThicknessHelper::FromLengths(16, 0, 16, 0));
                chip.BorderBrush(brush(p.outline));
                chip.BorderThickness(ThicknessHelper::FromUniformLength(1));
                chip.HorizontalAlignment(HorizontalAlignment::Left);
                add(chips, chip);
            }
            add(column, chips);
        }
    }
    auto actions = hstack(8);
    actions.HorizontalAlignment(HorizontalAlignment::Right);
    actions.Margin(ThicknessHelper::FromLengths(0, 24, -12, -8));
    for (size_t i = 0; i < spec.buttons.size(); ++i) {
        const bool confirming = i + 1 == spec.buttons.size();
        auto b = textButton(spec.buttons[i], [this, i] { closeDialog(int(i)); }, confirming && spec.destructive ? p.error : p.primary);
        add(actions, b);
    }
    add(column, actions);
    Border sheet;
    sheet.Child(column);
    sheet.Background(brush(p.surfaceContainerHigh));
    sheet.CornerRadius(CornerRadiusHelper::FromUniformRadius(28));
    sheet.Padding(ThicknessHelper::FromUniformLength(24));
    sheet.MinWidth(280);
    sheet.MaxWidth(560);
    sheet.Margin(ThicknessHelper::FromLengths(24, 24, 24, 24));
    sheet.VerticalAlignment(VerticalAlignment::Center);
    dialogLayer_.Children().Append(sheet);
    root_.Children().Append(dialogLayer_);
    if (dialogField_) {
        dialogField_.SelectAll();
        dialogField_.Focus(FocusState::Programmatic);
    }
    if (dialogSecret_) dialogSecret_.Focus(FocusState::Programmatic);
}

void AppShell::dialogWhenFree(DialogSpec spec, std::function<void(int, std::string)> done) {
    if (!root_) {
        if (done) done(-1, std::string());
        return;
    }
    if (dialogOpen_) {
        waitingDialogs_.emplace_back(std::move(spec), std::move(done));
        return;
    }
    dialog(std::move(spec), std::move(done));
}

void AppShell::closeDialog(int button) {
    if (!dialogOpen_) return;
    const std::string typed = dialogField_    ? narrow(std::wstring(dialogField_.Text()))
                              : dialogSecret_ ? narrow(std::wstring(dialogSecret_.Password()))
                                              : std::string();
    uint32_t at = 0;
    if (dialogLayer_ && root_.Children().IndexOf(dialogLayer_, at)) root_.Children().RemoveAt(at);
    dialogLayer_ = nullptr;
    dialogField_ = nullptr;
    dialogSecret_ = nullptr;
    dialogOpen_ = false;
    auto done = std::move(dialogDone_);
    dialogDone_ = nullptr;
    if (done) done(button, typed);
    // The next that waited its turn (dialogWhenFree), unless the answer opened another.
    if (!dialogOpen_ && !waitingDialogs_.empty()) {
        auto next = std::move(waitingDialogs_.front());
        waitingDialogs_.pop_front();
        dialog(std::move(next.first), std::move(next.second));
    }
}

void AppShell::alert(std::string title, std::string message) {
    DialogSpec spec;
    spec.title = std::move(title);
    spec.message = std::move(message);
    spec.buttons = {"OK"};
    dialog(std::move(spec), nullptr);
}

void AppShell::confirm(std::string title, std::string message, std::string confirmTitle, bool destructive,
                       std::function<void()> onConfirm, std::function<void()> onCancel) {
    DialogSpec spec;
    spec.title = std::move(title);
    spec.message = std::move(message);
    spec.buttons = {"Cancel", std::move(confirmTitle)};
    spec.destructive = destructive;
    dialog(std::move(spec), [onConfirm, onCancel](int button, std::string) {
        if (button == 1) {
            if (onConfirm) onConfirm();
        } else if (onCancel) {
            onCancel();
        }
    });
}

// MARK: Importing an APK

namespace {

/// The languages whose splits a bundle's unpacking keeps: English, in which an
/// app's default strings are as a rule, and the languages the phone's owner
/// prefers (Windows' list, the first three), by their codes: "en,ru".
std::string bundleLanguages() {
    std::string list = "en";
    int added = 0;
    try {
        for (auto const& tag : winrt::Windows::System::UserProfile::GlobalizationPreferences::Languages()) {
            std::string code = narrow(tag);
            const size_t dash = code.find('-');
            if (dash != std::string::npos) code.resize(dash);
            for (auto& ch : code) ch = char(tolower((unsigned char)ch));
            if (code.empty() || ("," + list + ",").find("," + code + ",") != std::string::npos) continue;
            list += "," + code;
            if (++added == 3) break;
        }
    } catch (const hresult_error&) {
    }
    return list;
}

/// megler_bundle_unpack's progress: from the worker thread, read by the poll.
void unpackProgress(void* user, uint64_t done, uint64_t total) {
    auto* progress = static_cast<ImportProgress*>(user);
    progress->total = total;
    progress->copied = done;
}

/// The old way, where the picked file can't be read where it is: a copy into the library by streaming rather than
/// StorageFile::CopyAsync (on the phone CopyAsync of a large file failed at once with E_FAIL; podium-w10m met it with an
/// 850 MB IPSW), under a .part name until it is whole. Empty, or why not. Off the UI thread.
std::string streamCopy(winrt::Windows::Storage::StorageFile const& file, const std::wstring& target, ImportProgress& progress) {
    CREATEFILE2_EXTENDED_PARAMETERS params = {};
    params.dwSize = sizeof(params);
    params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
    std::string error;
    {
        winrt::file_handle out{CreateFile2((target + L".part").c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, &params)};
        if (!out) return format("the copy could not be created (error %lu)", GetLastError());
        auto input = file.OpenReadAsync().get();
        winrt::Windows::Storage::Streams::Buffer buffer(1 << 20);
        uint64_t copied = 0;
        for (;;) {
            auto chunk = input.ReadAsync(buffer, buffer.Capacity(), winrt::Windows::Storage::Streams::InputStreamOptions::None).get();
            if (chunk.Length() == 0) break;
            DWORD written = 0;
            if (!WriteFile(out.get(), chunk.data(), chunk.Length(), &written, nullptr) || written != chunk.Length()) {
                error = format("writing the copy failed (error %lu) -- is the phone's storage full?", GetLastError());
                break;
            }
            copied += written;
            progress.copied = copied;
        }
        if (error.empty() && progress.total && copied != progress.total)
            error = format("the copy ended after %llu of %llu bytes", (unsigned long long)copied, (unsigned long long)progress.total);
    }
    if (error.empty() && !files::move(target + L".part", target)) error = format("the copy could not be put in place (error %lu)", GetLastError());
    if (!error.empty()) files::remove(target + L".part");
    return error;
}

/// The picker, then the app installed (ui/SeparateApps.h): an APK straight from where it is, read through the broker's
/// handles (shell/PickedFile.h) with no copy unless Megler has to keep one; a bundle unpacked from where it is into its
/// base and splits, then installed from them.
/// Where an APK (or a bundle of split APKs) comes from: a file the picker or the phone's file manager gave, or a file of
/// Megler's own at `path` (an APK an Android app asked to install, handed over in its data or the shared folder).
struct ApkSource {
    winrt::Windows::Storage::StorageFile file{nullptr};
    std::wstring path;
    std::string name;
};

/// The APK into the library and installed as an app of the phone's own (ui/SeparateApps.h): read where it is when the
/// broker gives its handle, else copied; a bundle unpacked into its base and the splits this phone can use.
winrt::fire_and_forget installFrom(std::weak_ptr<AppShell> weakShell, std::shared_ptr<ImportProgress> progress, ApkSource source,
                                   std::function<void(std::string, std::string)> done) {
    std::string id, error;
    // What a bundle is unpacked for, asked here on the UI thread: the density a Java app is told the screen has (160 for
    // each of Windows' view pixels: player/PlayerScreen), and the phone's languages.
    double scale = 1.0;
    try {
        scale = winrt::Windows::Graphics::Display::DisplayInformation::GetForCurrentView().RawPixelsPerViewPixel();
    } catch (const hresult_error&) {
    }
    const int density = std::max(120, std::min(640, int(160.0 * (scale > 0 ? scale : 1.0) + 0.5)));
    const std::string languages = bundleLanguages();
    winrt::apartment_context ui;
    try {
        const std::string name = source.file ? narrow(source.file.Name()) : source.name;
        const std::string target = Library::idFor(name);
        const bool bundle = Library::isBundle(name);
        const std::wstring finalPath = Library::instance().directory() + L"\\" + widen(target);
        progress->name = name;
        co_await winrt::resume_background();
        std::shared_ptr<PickedFile> picked;
        std::wstring bundlePath;
        if (source.file) {
            // Read where it is, under a path of Megler's that is no file (the copy's, if one is made, is finalPath).
            picked = std::make_shared<PickedFile>(source.file, finalPath + (bundle ? L".bundle.picked" : L".picked"));
            progress->total = picked->size();
            if (picked->ok()) {
                bundlePath = picked->path();
            } else if (bundle) {
                bundlePath = finalPath + L".bundle";
                shellLog::write(format("import: copying %s (%llu bytes) to unpack it", name.c_str(), (unsigned long long)picked->size()));
                error = streamCopy(source.file, bundlePath, *progress);
            } else {
                shellLog::write(format("import: copying %s (%llu bytes) into the library as %s", name.c_str(),
                                       (unsigned long long)picked->size(), target.c_str()));
                error = streamCopy(source.file, finalPath, *progress);
            }
        } else if (bundle) {
            bundlePath = source.path;
        } else {
            shellLog::write(format("import: copying %s into the library as %s", narrow(source.path).c_str(), target.c_str()));
            if (!files::copy(source.path, finalPath)) error = "the APK could not be copied into Megler's library";
        }
        const bool inPlace = picked && picked->ok();
        if (error.empty() && bundle) {
            progress->unpacking = true;
            progress->copied = 0;
            progress->total = 0;
            const std::string apkPath = narrow(finalPath), splitsPath = narrow(Library::splitsDirFor(finalPath));
            shellLog::write(format("import: unpacking %s for %d dpi, languages %s", name.c_str(), density, languages.c_str()));
            std::string answer;
            if (char* json = megler_bundle_unpack(narrow(bundlePath).c_str(), apkPath.c_str(), splitsPath.c_str(), density, languages.c_str(),
                                                &unpackProgress, progress.get())) {
                answer = json;
                megler_free(json);
            }
            if (picked && !inPlace) files::remove(bundlePath);
            winrt::Windows::Data::Json::JsonObject o{nullptr};
            if (!winrt::Windows::Data::Json::JsonObject::TryParse(hs(answer), o) || !o || !o.HasKey(L"ok")) {
                error = "the bundle could not be unpacked";
            } else if (!o.GetNamedBoolean(L"ok", false)) {
                error = narrow(o.GetNamedString(L"error", L"the bundle could not be unpacked"));
            } else {
                const auto count = [&](const wchar_t* key) {
                    return o.HasKey(key) && o.GetNamedValue(key).ValueType() == winrt::Windows::Data::Json::JsonValueType::Array
                               ? unsigned(o.GetNamedArray(key).Size())
                               : 0u;
                };
                shellLog::write(format("import: %s unpacked: its base and %u splits kept, %u left out, %u expansion files", name.c_str(),
                                       count(L"splits"), count(L"leftOut"), count(L"expansions")));
            }
        } else if (error.empty() && !inPlace) {
            // An app in one APK has no splits: a folder of them left by a bundle of the same name is another app's.
            files::removeTree(Library::splitsDirFor(finalPath));
        }
        co_await ui;
        if (error.empty()) {
            id = target;
            auto shell = weakShell.lock();
            if (!bundle && inPlace) {
                // Straight from where it is: the library has it once it is installed (or kept).
                if (shell) separateApps::installPicked(*shell, picked, id);
            } else {
                Library::instance().added(id);
                if (shell) separateApps::install(*shell, id, true);
            }
        }
    } catch (const hresult_error& e) {
        error = format("0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str());
    }
    if (!error.empty()) shellLog::write("import: failed: " + error);
    *progress = ImportProgress();
    done(std::move(id), std::move(error));
}

winrt::fire_and_forget pickAndInstall(std::weak_ptr<AppShell> weakShell, std::shared_ptr<ImportProgress> progress,
                                      std::function<void(std::string, std::string)> done) {
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Pickers;
    StorageFile file{nullptr};
    std::string error;
    try {
        FileOpenPicker picker;
        picker.ViewMode(PickerViewMode::List);
        picker.SuggestedStartLocation(PickerLocationId::Downloads);
        picker.FileTypeFilter().Append(L".apk");
        picker.FileTypeFilter().Append(L".apks");
        picker.FileTypeFilter().Append(L".xapk");
        picker.FileTypeFilter().Append(L".apkm");
        file = co_await picker.PickSingleFileAsync();
    } catch (const hresult_error& e) {
        error = format("0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str());
    }
    if (!file) {
        *progress = ImportProgress();
        done(std::string(), std::move(error));
        co_return;
    }
    ApkSource source;
    source.file = file;
    installFrom(weakShell, progress, std::move(source), std::move(done));
}

} // namespace

void AppShell::importApk(std::function<void(std::string, std::string)> done) {
    // Taken before the picker opens: a second picker while one is up throws.
    if (import_->active) return;
    // (Nothing is installed while Megler does not know which of its apps are installed: it says why.)
    if (!separateApps::requireList(*this)) return;
    *import_ = ImportProgress();
    import_->active = true;
    std::weak_ptr<AppShell> weak = weak_from_this();
    pickAndInstall(weak, import_, [weak, done](std::string id, std::string error) {
        if (weak.lock() && done) done(std::move(id), std::move(error));
    });
}

void AppShell::installApkFile(winrt::Windows::Storage::StorageFile const& file) {
    const std::string name = narrow(file.Name());
    shellLog::write("import: " + name + " opened in Megler");
    if (!separateApps::requireList(*this)) return;
    if (import_->active) {
        alert("Another APK Is on Its Way", "Megler is installing an APK already. Open " + name + " again when it is done.");
        return;
    }
    std::weak_ptr<AppShell> weak = weak_from_this();
    confirm("Install " + name + "?",
            "Megler reads it, tells you whether it can run here, and installs it as an app of the phone's own; the Library shows "
            "how it goes.",
            "Install", false, [this, weak, file] {
                if (!weak.lock() || import_->active) return;
                *import_ = ImportProgress();
                import_->active = true;
                ApkSource source;
                source.file = file;
                if (selectedTab_ != 0) selectTab(0);
                installFrom(weak, import_, std::move(source), [weak](std::string, std::string error) {
                    if (auto self = weak.lock(); self && !error.empty()) self->alert("Couldn't Install This APK", error);
                });
            });
}

void AppShell::installApkPath(const std::wstring& path, const std::string& name, const std::string& from, bool listAsked) {
    if (!listAsked) shellLog::write("import: " + from + " asks to install " + name + " (" + narrow(path) + ")");
    /*
     * An APK an app's own package handed over (the shared folder's incoming\, player/SystemHost.cpp) goes once its request is
     * answered, whatever the answer: the library has a copy of its own (or its base and splits) by then, and an app that
     * asks again hands it over again. None ever went, and they added up (499 MB on 2026-10-30).
     */
    const std::wstring shared = appPackage::sharedFolder(), incoming = shared + L"\\incoming\\";
    const bool handed = !shared.empty() && path.size() > incoming.size() && _wcsnicmp(path.c_str(), incoming.c_str(), incoming.size()) == 0;
    const auto drop = [path, handed] {
        if (handed) files::remove(path);
    };
    /*
     * Nothing is installed while Megler does not know which of its apps are installed. Megler started or woken by an app's
     * request has not asked for the list yet (Windows's package manager's), or asked long ago: it is asked for first, and
     * the request goes on once it comes, the app's installer session waiting for the outcome. Only a list that cannot be had
     * then has the app that asked told so, and the shell say why. Woken by Play Store's request for WhatsApp, after Play
     * Store's request to uninstall the one installed before had started it, Megler answered at once that the Device Portal,
     * which listed the apps then, "has not answered yet" (576).
     */
    if (separateApps::list() != separateApps::List::ready && !listAsked) {
        shellLog::write("import: " + name + " waits for the list of the installed apps");
        std::weak_ptr<AppShell> weak = weak_from_this();
        separateApps::askList(*this, [weak, path, name, from] {
            if (auto self = weak.lock()) self->installApkPath(path, name, from, true);
        });
        return;
    }
    if (separateApps::list() != separateApps::List::ready) {
        const std::string why = separateApps::list() == separateApps::List::unavailable ? separateApps::listProblem() : "it has not answered yet";
        phoneAppsList::installOutcome(name, "failed", "Not installed: " + appPackage::packageLister() + " did not tell Megler which apps are installed (" +
                                                        why + ").");
        separateApps::requireList(*this);
        drop();
        return;
    }
    // What becomes of it is told to the app that asked (library/PhoneAppsList.h): its installer session waits for it.
    if (import_->active) {
        phoneAppsList::installOutcome(name, "busy", "Megler is installing another APK. Ask again when it is done.");
        alert("Another APK Is on Its Way", "Megler is installing an APK already. Ask " + from + " again when it is done.");
        drop();
        return;
    }
    std::weak_ptr<AppShell> weak = weak_from_this();
    confirm("Install " + name + "?", from + " wants to install this app. Megler reads it, tells you whether it can run here, and installs it "
                                            "as an app of the phone's own.",
            "Install", false,
            [this, weak, path, name, drop] {
                if (!weak.lock()) return;
                if (import_->active) {
                    phoneAppsList::installOutcome(name, "busy", "Megler is installing another APK. Ask again when it is done.");
                    drop();
                    return;
                }
                *import_ = ImportProgress();
                import_->active = true;
                ApkSource source;
                source.path = path;
                source.name = name;
                installFrom(weak, import_, std::move(source), [weak, name, drop](std::string id, std::string error) {
                    drop();
                    auto self = weak.lock();
                    if (!error.empty()) {
                        phoneAppsList::installOutcome(name, "failed", error);
                        if (self) self->alert("Couldn't Install This APK", error);
                    } else if (self && !id.empty()) {
                        self->askedInstalls_.push_back({id, name});
                    }
                });
            },
            [name, drop] {
                drop();
                phoneAppsList::installOutcome(name, "declined", "Not installed: Megler was told no.");
            });
}

/*
 * Google Play Store starts only once Google's own Google Play services, which Megler has built in, is installed as an app of
 * its own (the owner's wish, 2026-11-02): users' Play Store, started with microG's or with none, had no Android ID and said
 * "Server error". False, with what to do said, when it is not.
 */
bool AppShell::playStoreCanStart(const std::string& package) {
    if (package != "com.android.vending" || appPackage::hasGoogleServices()) return true;
    shellLog::write("app package: Google Play Store not started: Google's own Google Play services is not installed as an app of its own");
    if (appPackage::isSingleApp()) {
        confirm("Install Google Play Services First",
                "Google Play Store runs with Google Play services, which Megler has built in. Open Megler, install Google Play services "
                "from its Library, then start Google Play Store again.",
                "Open Megler", false, [] { systemHost::openMegler(L"library"); });
        return false;
    }
    std::string gms;
    for (const ApkEntry& e : Library::instance().entries())
        if (e.report.package == "com.google.android.gms") gms = e.id;
    std::weak_ptr<AppShell> weak = weak_from_this();
    confirm("Install Google Play Services First",
            "Google Play Store runs with Google Play services, which Megler has built in: install them from the Library first, then "
            "start Google Play Store.",
            gms.empty() ? "OK" : "Show Google Play Services", false, [weak, gms] {
                if (auto self = weak.lock(); self && !gms.empty()) {
                    if (self->selectedTab_ != 0) self->selectTab(0);
                    self->push(makeAppDetailScreen(*self, gms));
                }
            });
    return false;
}

void AppShell::uninstallApp(const std::string& package, const std::string& from) {
    shellLog::write("import: " + from + " asks to uninstall " + package);
    const std::string outcome = package + ".uninstall";
    std::optional<ApkEntry> found;
    for (const ApkEntry& e : Library::instance().entries())
        if (e.report.package == package) found = e;
    if (!found) {
        phoneAppsList::installOutcome(outcome, "failed", package + " is not in Megler's library.");
        return;
    }
    const std::string id = found->id, title = found->title();
    // What Android's uninstall does: the app goes, its own package and Megler's copy of it, so that the phone's apps no
    // longer have it (library/PhoneAppsList.h), which the app that asked waits for.
    confirm("Uninstall " + title + "?",
            from + " wants to uninstall this app. It goes from the phone and from Megler's library, with its private files; what it has "
                   "in Android's storage stays in the shared folder.",
            "Uninstall", true,
            [this, package, id] {
                if (separateApps::status(package).installed == separateApps::Installed::yes) separateApps::uninstall(*this, package);
                Library::instance().remove(id);
            },
            [outcome] { phoneAppsList::installOutcome(outcome, "declined", "Not uninstalled: Megler was told no."); });
}

void AppShell::tellAskedInstalls() {
    // An app that cannot run here is in the library but not in the phone's apps: the app that asked is told why.
    for (auto it = askedInstalls_.begin(); it != askedInstalls_.end();) {
        const auto entry = Library::instance().find(it->first);
        if (entry && entry->analysed) {
            if (!entry->report.ready()) phoneAppsList::installOutcome(it->second, "incompatible", entry->report.status());
            it = askedInstalls_.erase(it);
        } else {
            ++it;
        }
    }
}

void AppShell::openLink(winrt::Windows::Foundation::Uri const& uri) {
    std::wstring what = std::wstring(uri.Path());
    if (what.empty()) what = std::wstring(uri.Host());
    while (!what.empty() && what.front() == L'/') what.erase(what.begin());
    winrt::Windows::Foundation::WwwFormUrlDecoder decoder(uri.Query());
    auto value = [&decoder](const wchar_t* key) {
        try {
            return narrow(std::wstring(decoder.GetFirstValueByName(key)));
        } catch (const hresult_error&) {
            return std::string();
        }
    };
    shellLog::write("app: the link " + narrow(std::wstring(uri.RawUri())));
    if (what == L"install") {
        const std::string name = value(L"name");
        if (name.empty() || name.find_first_of("\\/:") != std::string::npos || name.find("..") != std::string::npos) {
            shellLog::write("app: the link's name is not a file's of the shared folder's incoming: nothing installed");
            return;
        }
        /*
         * The app's own package put the APK in its shared folder, which is Megler's only where both are on the same storage:
         * Windows gives a package on a memory card the card's (or none), and Megler in the phone's memory does not see it.
         */
        const std::wstring shared = appPackage::sharedFolder(), path = shared.empty() ? std::wstring() : shared + L"\\incoming\\" + widen(name);
        if (path.empty() || !files::exists(path)) {
            shellLog::write("app: " + name + " is not in " + (shared.empty() ? std::string("the shared folder: Windows gave Megler none") : narrow(path)) +
                            ": the app that asked is likely on other storage than Megler (a memory card)");
            phoneAppsList::installOutcome(name, "failed", "Megler did not find the APK in its shared folder: the app and Megler are on different storage.");
            alert("Couldn't Install This APK",
                  "An app of the phone's own asked Megler to install " + name + ", and Megler did not find it in its shared folder" +
                      (shared.empty() ? std::string(" (Windows gave Megler none)") : std::string()) +
                      ". As a rule, that is because the app and Megler are on different storage -- one on the memory card, the other in "
                      "the phone's memory -- and Windows gives each storage a shared folder of its own. Keep them on the same storage: "
                      "move the app (or Megler) to the phone's memory in Settings › System › Storage.");
            return;
        }
        installApkPath(path, name, "An app of the phone's own");
    } else if (what == L"uninstall") {
        const std::string package = value(L"package");
        if (!package.empty()) uninstallApp(package, "An app of the phone's own");
    } else if (what == L"run") {
        const std::string package = value(L"package");
        // An app's own package is in front now, which is all a launcher wants of it. Another app may want one of its
        // activities (Play Store, microG's sign-in): the app starts it, now or once it runs (megler_start_activity).
        if (appPackage::isSingleApp()) {
            const std::string activity = value(L"activity");
            if (!activity.empty() && package == appPackage::single().package) megler_start_activity(activity.c_str());
            return;
        }
        std::optional<ApkEntry> found;
        for (const ApkEntry& e : Library::instance().entries())
            if (e.report.package == package && e.report.ready()) found = e;
        if (!found) {
            alert("Couldn't Start the App", package + " is not in Megler's library.");
            return;
        }
        if (session::snapshot().state != session::State::idle || session::spent()) {
            alert("Megler Runs One App at a Time", found->title() + " can start once Megler is closed and opened again, or as an app of the phone's "
                                                                   "own: its page in the Library, Install.");
            return;
        }
        if (!playStoreCanStart(found->report.package)) return;
        if (selectedTab_ != 0) selectTab(0);
        push(makePlayerScreen(*this, found->id));
    } else if (what == L"library") {
        // (Google Play Store's own package sends the owner here to install Google Play services: the Library in front.)
        if (selectedTab_ != 0) selectTab(0);
    }
}

// MARK: Lifecycle

void AppShell::suspending() {
    if (timer_) timer_.Stop();
}

void AppShell::resuming() {
    if (timer_) timer_.Start();
}

} // namespace megler::app::ui
