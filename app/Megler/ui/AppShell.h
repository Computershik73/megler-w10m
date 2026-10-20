// The app's frame: four tabs (Library, Device, Log, Settings), each with its
// own navigation stack, a navigation bar with the title, and the plumbing
// every screen shares -- the 250 ms poll, worker threads, dialogs and the APK
// picker. podium-w10m's ui/AppShell, for Megler.
//
// Each screen is a Page: it builds its XAML once and refreshes it from the
// state behind it on every poll. Nothing the core or a worker thread does
// touches XAML; they change state, and the poll shows it.
//
// The player is a Page too, but a full-screen one: while it is on top there
// is no navigation bar and no tab bar, the display is turned to landscape,
// and hardware Back goes to the running app.
#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace megler::app::ui {

class AppShell;

class Page : public std::enable_shared_from_this<Page> {
public:
    explicit Page(AppShell& shell) : shell_(shell) {}
    virtual ~Page() = default;

    /// Built on first use, on the UI thread.
    virtual winrt::Windows::UI::Xaml::UIElement view() = 0;
    virtual std::string title() = 0;
    /// The navigation bar's trailing item, or null. Asked again after every
    /// refresh, so it can change with state.
    virtual winrt::Windows::UI::Xaml::UIElement toolbarItem() { return nullptr; }
    /// The whole screen, in landscape, without the shell's bars, under the phone's.
    virtual bool fullScreen() { return false; }
    /// For a full-screen page: upright instead of in landscape.
    virtual bool upright() { return false; }
    /// For a full-screen page: the phone's status bar and navigation bar hidden (the phone hides its navigation bar only in
    /// full-screen mode, which hides the status bar too), and the status bar's icons dark, for a light bar under them.
    virtual bool hideStatusBar() { return fullScreen(); }
    virtual bool hideNavigationBar() { return fullScreen(); }
    virtual bool lightStatusBar() { return false; }
    /// How much of each edge of the window the phone's bars cover now, in view pixels (0 for a bar that is hidden): after
    /// every change of the bars or of the window's visible part.
    virtual void barsCover(double left, double top, double right, double bottom) {}
    /// Hardware Back while this page is on top. True: the page took it.
    virtual bool handleBack() { return false; }
    /// Every poll while visible, and right after being shown.
    virtual void refresh() {}
    virtual void appeared() {}
    virtual void disappeared() {}

protected:
    AppShell& shell_;
};

/// An APK on its way into the library: the picker is up (no name yet), or
/// what was picked is being copied -- and, for a bundle of split APKs, then
/// unpacked (`unpacking`; copied and total are then the unpacking's bytes).
struct ImportProgress {
    bool active = false;
    bool unpacking = false;
    std::string name;
    uint64_t copied = 0, total = 0;
};

class AppShell : public std::enable_shared_from_this<AppShell> {
public:
    AppShell();
    ~AppShell();

    /// Builds the window content (UI thread). Call once.
    winrt::Windows::UI::Xaml::UIElement root();

    // Navigation
    void push(std::shared_ptr<Page> page);
    void pop();
    /// Hardware Back: the top page's own handling, else a pop, else from
    /// another tab back to the Library. False when there is nothing to go
    /// back to (the system then leaves the app).
    bool goBack();
    void selectTab(int tab);
    /// Re-reads the current page's title, toolbar item and full-screen wish.
    void refreshChrome();

    // Work off the UI thread. `work` runs on a new thread; `then` afterwards
    // on the UI thread (if the app is still there).
    void runInBackground(std::function<void()> work, std::function<void()> then = nullptr);
    void runOnUiThread(std::function<void()> work);

    // Dialogs (one at a time; a second request while one is up is dropped)
    void alert(std::string title, std::string message);
    void confirm(std::string title, std::string message, std::string confirmTitle, bool destructive,
                 std::function<void()> onConfirm);

    /// The file picker for an .apk, or a bundle of split APKs (.apks, .xapk),
    /// and the copy of what was picked into the library (LocalState\apks): a
    /// container can't open a picked file by path, and the core reads APKs by
    /// path. A bundle's copy is unpacked there by the core (megler_bundle_unpack)
    /// into the base and the splits this phone can use, and then removed.
    /// `done` gets the library's id of the copy, empty when nothing was picked,
    /// or an error.
    void importApk(std::function<void(std::string id, std::string error)> done);
    const ImportProgress& importProgress() const { return *import_; }

    // Lifecycle (from App)
    void suspending();
    void resuming();

private:
    struct Stack;
    void buildChrome();
    void buildTabBar();
    void showTop();
    void applyFullScreen(bool fullScreen, bool upright, bool hideStatus, bool hideNavigation, bool lightStatus);
    void applyVisibleBounds();
    void poll();
    void updateDisplayRequest();
    Stack& currentStack();

    std::vector<std::unique_ptr<Stack>> stacks_;
    int selectedTab_ = 0;
    bool dialogOpen_ = false;
    bool fullScreen_ = false;
    bool upright_ = false;
    bool hideStatus_ = false, hideNavigation_ = false, lightStatus_ = false;
    bool displayRequestActive_ = false;
    bool singleAppOpened_ = false;  // an app's own package: its app has been opened (shell/AppPackage.h)
    bool foldersPublished_ = false; // Megler: its added folders listed for the separate apps (ui/SharedFolders.h)
    bool storageShared_ = false;    // Megler: the copy of its storage to the shared folder has been started (shell/AppPackage.h)
    std::shared_ptr<ImportProgress> import_;

    winrt::Windows::UI::Xaml::Controls::Grid root_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid navigationBar_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button backButton_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock backLabel_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock titleText_{nullptr};
    winrt::Windows::UI::Xaml::Controls::ContentControl toolbarHost_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Border contentHost_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid tabBar_{nullptr};
    std::vector<winrt::Windows::UI::Xaml::Controls::Button> tabButtons_;
    winrt::Windows::UI::Xaml::DispatcherTimer timer_{nullptr};
    winrt::Windows::UI::Core::CoreDispatcher dispatcher_{nullptr};
    winrt::Windows::System::Display::DisplayRequest displayRequest_{nullptr};
};

// The screens.
std::shared_ptr<Page> makeLibraryScreen(AppShell& shell);
std::shared_ptr<Page> makeAppDetailScreen(AppShell& shell, std::string apkId);
std::shared_ptr<Page> makePlayerScreen(AppShell& shell, std::string apkId);
std::shared_ptr<Page> makeChecksScreen(AppShell& shell);
std::shared_ptr<Page> makeLogScreen(AppShell& shell);
std::shared_ptr<Page> makeSettingsScreen(AppShell& shell);

/// Whose settings a settings view shows and changes (shell/Settings.h): Megler's, or one app's own.
struct SettingsScope {
    std::string package; // the app's Android package; empty: Megler's
    std::string label;   // the app's name
    bool own = false;    // this process is that app's own package: its folders and what it is are shown too
};
/// The body of a settings page, unscrolled: for a page, or for the player's menu in an app's own package.
class SettingsView {
public:
    virtual ~SettingsView() = default;
    virtual winrt::Windows::UI::Xaml::UIElement view() = 0;
    /// Every poll while shown.
    virtual void refresh() = 0;
};
std::shared_ptr<SettingsView> makeSettingsView(AppShell& shell, SettingsScope scope);
/// One app's own settings, on its page in Megler.
std::shared_ptr<Page> makeAppSettingsScreen(AppShell& shell, std::string package, std::string label);

} // namespace megler::app::ui
