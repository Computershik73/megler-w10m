#include "pch.h"

#include "ui/AppShell.h"

#include "auto/AutoRun.h"
#include "library/Library.h"
#include "player/Session.h"
#include "player/StorageHost.h"
#include "shell/AppPackage.h"
#include "shell/Checks.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/SharedFolders.h"
#include "ui/Theme.h"

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

AppShell::~AppShell() {
    if (timer_) timer_.Stop();
}

AppShell::Stack& AppShell::currentStack() { return *stacks_[size_t(selectedTab_)]; }

UIElement AppShell::root() {
    const auto& p = palette();
    dispatcher_ = Window::Current().Dispatcher();
    root_ = Grid();
    root_.RequestedTheme(ElementTheme::Dark);
    root_.Background(brush(p.background));
    for (int i = 0; i < 3; ++i) {
        RowDefinition row;
        row.Height(i == 1 ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star) : GridLengthHelper::Auto());
        root_.RowDefinitions().Append(row);
    }

    if (ApiInformation::IsTypePresent(L"Windows.UI.ViewManagement.StatusBar")) {
        try {
            auto status = StatusBar::GetForCurrentView();
            status.BackgroundColor(p.background);
            status.BackgroundOpacity(1.0);
            status.ForegroundColor(p.label);
        } catch (...) {
        }
    }
    try {
        auto bar = ApplicationView::GetForCurrentView().TitleBar();
        bar.BackgroundColor(p.background);
        bar.ForegroundColor(p.label);
        bar.ButtonBackgroundColor(p.background);
        bar.ButtonForegroundColor(p.label);
        bar.InactiveBackgroundColor(p.background);
        bar.ButtonInactiveBackgroundColor(p.background);
    } catch (...) {
    }

    const std::shared_ptr<Page> roots[] = {makeLibraryScreen(*this), makeChecksScreen(*this), makeLogScreen(*this),
                                           makeSettingsScreen(*this)};
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

void AppShell::applyFullScreen(bool fullScreen, bool upright, bool hideStatus, bool hideNavigation, bool lightStatus) {
    upright = fullScreen && upright;
    // The phone hides its navigation bar only in full-screen mode, and that hides the status bar too.
    hideNavigation = fullScreen && hideNavigation;
    hideStatus = fullScreen && (hideStatus || hideNavigation);
    lightStatus = fullScreen && lightStatus;
    if (fullScreen == fullScreen_ && upright == upright_ && hideStatus == hideStatus_ && hideNavigation == hideNavigation_ &&
        lightStatus == lightStatus_)
        return;
    const bool turn = fullScreen != fullScreen_ || upright != upright_;
    fullScreen_ = fullScreen;
    upright_ = upright;
    hideStatus_ = hideStatus;
    hideNavigation_ = hideNavigation;
    lightStatus_ = lightStatus;
    using namespace winrt::Windows::Graphics::Display;
    if (turn) {
        try {
            // The library is a portrait screen, games are landscape ones, and an
            // app that is not a game is upright unless it says otherwise. Off a
            // phone there is nothing to turn back to: no preference.
            DisplayInformation::AutoRotationPreferences(
                fullScreen && !upright ? (DisplayOrientations::Landscape | DisplayOrientations::LandscapeFlipped)
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
    shellLog::write(!fullScreen ? std::string("ui: back to portrait")
                                : std::string(upright ? "ui: upright, full screen" : "ui: landscape, full screen") +
                                      (hideNavigation ? (entered ? ", the phone's bars hidden" : ", the phone's bars hidden (full-screen mode refused)")
                                       : hideStatus   ? ", the status bar hidden, the navigation bar shown"
                                                      : ", the phone's bars shown"));
    applyVisibleBounds();
}

void AppShell::buildChrome() {
    const auto& p = palette();
    navigationBar_ = Grid();
    navigationBar_.Height(48);
    navigationBar_.Background(brush(p.background));
    for (int i = 0; i < 3; ++i) {
        ColumnDefinition c;
        c.Width(i == 1 ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star) : GridLengthHelper::Auto());
        navigationBar_.ColumnDefinitions().Append(c);
    }

    titleText_ = text("", font::headline, weightSemibold(), p.label);
    titleText_.TextWrapping(TextWrapping::NoWrap);
    titleText_.TextTrimming(TextTrimming::CharacterEllipsis);
    titleText_.HorizontalAlignment(HorizontalAlignment::Center);
    titleText_.VerticalAlignment(VerticalAlignment::Center);
    titleText_.Margin(ThicknessHelper::FromLengths(96, 0, 96, 0));
    Grid::SetColumnSpan(titleText_, 3);
    navigationBar_.Children().Append(titleText_);

    auto backContent = hstack(2);
    backContent.VerticalAlignment(VerticalAlignment::Center);
    add(backContent, glyph(glyphs::chevronLeft, 16, p.accentText));
    backLabel_ = text("", font::body, weightNormal(), p.accentText);
    backLabel_.TextWrapping(TextWrapping::NoWrap);
    backLabel_.TextTrimming(TextTrimming::CharacterEllipsis);
    backLabel_.MaxWidth(80);
    add(backContent, backLabel_);
    backButton_ = button(backContent, [this] { pop(); }, ButtonKind::plain, "Back");
    backButton_.HorizontalAlignment(HorizontalAlignment::Left);
    backButton_.Padding(ThicknessHelper::FromLengths(10, 0, 12, 0));
    backButton_.VerticalAlignment(VerticalAlignment::Stretch);
    navigationBar_.Children().Append(backButton_);

    toolbarHost_ = ContentControl();
    toolbarHost_.VerticalAlignment(VerticalAlignment::Center);
    toolbarHost_.HorizontalContentAlignment(HorizontalAlignment::Right);
    toolbarHost_.Margin(ThicknessHelper::FromLengths(0, 0, 12, 0));
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
    tabBar_ = Grid();
    tabBar_.Background(brush(rgb(0x121212)));
    tabBar_.BorderBrush(brush(p.separator));
    tabBar_.BorderThickness(ThicknessHelper::FromLengths(0, 1, 0, 0));
    tabButtons_.clear();
    const std::pair<const char*, wchar_t> tabs[] = {
        {"Library", glyphs::library}, {"Device", glyphs::device}, {"Log", glyphs::log}, {"Settings", glyphs::settings}};
    for (int i = 0; i < 4; ++i) {
        ColumnDefinition c;
        c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        tabBar_.ColumnDefinitions().Append(c);
        auto content = vstack(3);
        content.HorizontalAlignment(HorizontalAlignment::Center);
        auto g = glyph(tabs[i].second, 20);
        g.HorizontalAlignment(HorizontalAlignment::Center);
        add(content, g);
        auto t = text(tabs[i].first, 10, weightMedium());
        t.HorizontalAlignment(HorizontalAlignment::Center);
        add(content, t);
        auto b = button(content, [this, i] { selectTab(i); }, ButtonKind::plain, tabs[i].first);
        b.Padding(ThicknessHelper::FromLengths(0, 7, 0, 6));
        Grid::SetColumn(b, i);
        tabBar_.Children().Append(b);
        tabButtons_.push_back(b);
    }
    Grid::SetRow(tabBar_, 2);
    root_.Children().Append(tabBar_);
}

void AppShell::showTop() {
    auto page = currentStack().pages.back();
    contentHost_.Child(nullptr);
    contentHost_.Child(page->view());
    for (size_t i = 0; i < tabButtons_.size(); ++i)
        tabButtons_[i].Foreground(brush(int(i) == selectedTab_ ? palette().accentText : palette().secondaryLabel));
    page->appeared();
    page->refresh();
    refreshChrome();
}

void AppShell::refreshChrome() {
    if (stacks_.empty() || !titleText_) return;
    auto& stack = currentStack();
    auto page = stack.pages.back();
    const bool full = page->fullScreen();
    applyFullScreen(full, page->upright(), page->hideStatusBar(), page->hideNavigationBar(), page->lightStatusBar());
    setVisible(navigationBar_, !full);
    setVisible(tabBar_, !full);
    const bool canGoBack = stack.pages.size() > 1;
    titleText_.Text(hs(page->title()));
    setVisible(backButton_, canGoBack);
    if (canGoBack) backLabel_.Text(hs(stack.pages[stack.pages.size() - 2]->title()));
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
    if (dialogOpen_) return true; // the dialog takes Back itself
    if (currentStack().pages.back()->handleBack()) return true;
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
        // An app's own package opens its app as soon as the library has it read: straight into the player when it can
        // run here, else its page, which says why not. Before the player, Megler's folders the app has not got are offered
        // (ui/SharedFolders.h), once the phone's folders are known.
        if (appPackage::isSingleApp() && !singleAppOpened_) {
            const auto entry = Library::instance().find(appPackage::singleAppId());
            if (entry && entry->analysed && (!entry->report.ready() || StorageHost::shared().searched())) {
                singleAppOpened_ = true;
                shellLog::write("app package: opening " + entry->report.package);
                const std::string id = entry->id;
                if (entry->report.ready()) {
                    // The app's private files Megler handed over when it made this package, taken first (shell/AppPackage.h).
                    std::weak_ptr<AppShell> weak = weak_from_this();
                    runInBackground([] { appPackage::takeHandover(); },
                                    [weak, id] {
                                        sharedFolders::offer(false, [weak, id](int, int) {
                                            if (auto self = weak.lock()) self->push(makePlayerScreen(*self, id));
                                        });
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
    const bool wanted = state == session::State::starting || state == session::State::running || checks::running();
    if (wanted == displayRequestActive_) return;
    try {
        if (!displayRequest_) displayRequest_ = winrt::Windows::System::Display::DisplayRequest();
        if (wanted)
            displayRequest_.RequestActive();
        else
            displayRequest_.RequestRelease();
        shellLog::write(wanted ? "display: keeping the screen on" : "display: screen timeout allowed again");
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

namespace {

ContentDialog makeDialog(const std::string& title, const std::string& message) {
    ContentDialog d;
    if (!title.empty()) d.Title(box_value(hs(title)));
    auto t = text(message, font::body, weightNormal(), palette().label);
    t.TextWrapping(TextWrapping::Wrap);
    d.Content(t);
    d.RequestedTheme(ElementTheme::Dark);
    d.Background(brush(palette().card));
    d.Foreground(brush(palette().label));
    return d;
}

} // namespace

void AppShell::alert(std::string title, std::string message) {
    if (dialogOpen_) return;
    dialogOpen_ = true;
    auto d = makeDialog(title, message);
    d.PrimaryButtonText(L"OK");
    std::weak_ptr<AppShell> weak = weak_from_this();
    d.ShowAsync().Completed([weak](auto&&, auto&&) {
        if (auto self = weak.lock()) self->runOnUiThread([weak] {
                if (auto s = weak.lock()) s->dialogOpen_ = false;
            });
    });
}

void AppShell::confirm(std::string title, std::string message, std::string confirmTitle, bool destructive,
                       std::function<void()> onConfirm) {
    if (dialogOpen_) return;
    dialogOpen_ = true;
    auto d = makeDialog(title, message);
    d.PrimaryButtonText(hs(confirmTitle));
    d.SecondaryButtonText(L"Cancel");
    if (destructive) {
        auto style = Style(xaml_typename<Button>());
        style.Setters().Append(Setter(Control::ForegroundProperty(), brush(palette().red)));
        d.PrimaryButtonStyle(style);
    }
    std::weak_ptr<AppShell> weak = weak_from_this();
    d.ShowAsync().Completed([weak, onConfirm](auto const& op, auto&&) {
        const bool confirmed = op.GetResults() == ContentDialogResult::Primary;
        if (auto self = weak.lock()) self->runOnUiThread([weak, onConfirm, confirmed] {
                if (auto s = weak.lock()) s->dialogOpen_ = false;
                if (confirmed && onConfirm) onConfirm();
            });
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

winrt::fire_and_forget pickAndCopy(std::shared_ptr<ImportProgress> progress,
                                   std::function<void(std::string, std::string)> done) {
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Pickers;
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
        FileOpenPicker picker;
        picker.ViewMode(PickerViewMode::List);
        picker.SuggestedStartLocation(PickerLocationId::Downloads);
        picker.FileTypeFilter().Append(L".apk");
        picker.FileTypeFilter().Append(L".apks");
        picker.FileTypeFilter().Append(L".xapk");
        StorageFile file = co_await picker.PickSingleFileAsync();
        if (file) {
            const std::string name = narrow(file.Name());
            const std::string target = Library::idFor(name);
            const bool bundle = Library::isBundle(name);
            auto properties = co_await file.GetBasicPropertiesAsync();
            const uint64_t size = properties.Size();
            progress->copied = 0;
            progress->total = size;
            progress->name = name;
            shellLog::write(format("import: copying %s (%llu bytes) into the library as %s", name.c_str(),
                                   (unsigned long long)size, target.c_str()));
            // Copied by streaming rather than StorageFile::CopyAsync: on the
            // phone CopyAsync of a large file failed at once with E_FAIL
            // (podium-w10m met it with an 850 MB IPSW), whatever folder the
            // file came from. A read stream from the picked file is what the
            // picker's grant covers; the copy is written with Win32 into
            // LocalState, under a .part name until it is whole.
            const std::wstring finalPath = Library::instance().directory() + L"\\" + widen(target);
            // A bundle's copy is unpacked into finalPath and its splits' folder, and then goes.
            const std::wstring partPath = finalPath + (bundle ? L".bundle.part" : L".part");
            CREATEFILE2_EXTENDED_PARAMETERS params = {};
            params.dwSize = sizeof(params);
            params.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
            winrt::file_handle out{CreateFile2(partPath.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, &params)};
            if (!out) {
                error = format("the copy could not be created (error %lu)", GetLastError());
            } else {
                auto input = co_await file.OpenReadAsync();
                winrt::Windows::Storage::Streams::Buffer buffer(1 << 20);
                uint64_t copied = 0;
                for (;;) {
                    auto chunk = co_await input.ReadAsync(buffer, buffer.Capacity(),
                                                          winrt::Windows::Storage::Streams::InputStreamOptions::None);
                    if (chunk.Length() == 0) break;
                    DWORD written = 0;
                    if (!WriteFile(out.get(), chunk.data(), chunk.Length(), &written, nullptr) || written != chunk.Length()) {
                        error = format("writing the copy failed (error %lu) -- is the phone's storage full?", GetLastError());
                        break;
                    }
                    copied += written;
                    progress->copied = copied;
                }
                out.close();
                if (error.empty() && size && copied != size)
                    error = format("the copy ended after %llu of %llu bytes", (unsigned long long)copied, (unsigned long long)size);
                if (error.empty() && bundle) {
                    // Unpacked on a worker thread: hundreds of megabytes take seconds on a phone.
                    progress->unpacking = true;
                    progress->copied = 0;
                    progress->total = 0;
                    const std::string bundlePath = narrow(partPath), apkPath = narrow(finalPath),
                                      splitsPath = narrow(Library::splitsDirFor(finalPath));
                    shellLog::write(format("import: unpacking %s for %d dpi, languages %s", name.c_str(), density, languages.c_str()));
                    co_await winrt::resume_background();
                    std::string answer;
                    if (char* json = megler_bundle_unpack(bundlePath.c_str(), apkPath.c_str(), splitsPath.c_str(), density, languages.c_str(),
                                                        &unpackProgress, progress.get())) {
                        answer = json;
                        megler_free(json);
                    }
                    files::remove(partPath);
                    co_await ui;
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
                } else if (error.empty()) {
                    if (!files::move(partPath, finalPath)) error = format("the copy could not be put in place (error %lu)", GetLastError());
                    // An app in one APK has no splits: a folder of them left by a bundle of the same name is another app's.
                    else files::removeTree(Library::splitsDirFor(finalPath));
                }
                if (error.empty()) {
                    id = target;
                    Library::instance().added(id);
                } else {
                    files::remove(partPath);
                }
            }
        }
    } catch (const hresult_error& e) {
        error = format("0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str());
    }
    if (!error.empty()) shellLog::write("import: failed: " + error);
    *progress = ImportProgress();
    done(std::move(id), std::move(error));
}

} // namespace

void AppShell::importApk(std::function<void(std::string, std::string)> done) {
    // Taken before the picker opens: a second picker while one is up throws.
    if (import_->active) return;
    *import_ = ImportProgress();
    import_->active = true;
    std::weak_ptr<AppShell> weak = weak_from_this();
    pickAndCopy(import_, [weak, done](std::string id, std::string error) {
        if (weak.lock() && done) done(std::move(id), std::move(error));
    });
}

// MARK: Lifecycle

void AppShell::suspending() {
    if (timer_) timer_.Stop();
}

void AppShell::resuming() {
    if (timer_) timer_.Start();
}

} // namespace megler::app::ui
