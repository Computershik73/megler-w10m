// The app's frame, as Material 3 has it: four destinations (Library, Device, Log, Settings) in a navigation bar at the
// bottom, each with its own navigation stack, a top app bar with the title, Back and the page's actions, and the plumbing
// every screen shares -- the 250 ms poll, worker threads, Material's dialogs and the APK picker.
//
// Each screen is a Page: it builds its XAML once and refreshes it from the
// state behind it on every poll. Nothing the core or a worker thread does
// touches XAML; they change state, and the poll shows it.
//
// The player is a Page too, but a full-screen one: while it is on top there
// is no navigation bar and no tab bar, the display is turned to landscape,
// and hardware Back goes to the running app.
#pragma once

#include <deque>
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
    /// After handleBack said false: the Back is the system's, which takes Megler's window away, rather than the shell's
    /// (no page popped, no tab changed) -- a running app that went to the back with it.
    virtual bool backIsTheSystems() { return false; }
    /// The flag ui-quit (ui/UiShots.h): the page's running app stopped, and Megler closed once it has ended. False: the
    /// page has no app running, and Megler is closed at once.
    virtual bool quitAndClose() { return false; }
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
    /// The flag ui-quit: the running app stopped as its Quit stops it, then Megler closed; Megler closed at once without one.
    void quitAndClose();
    void selectTab(int tab);
    /// Re-reads the current page's title, toolbar item and full-screen wish.
    void refreshChrome();

    // Work off the UI thread. `work` runs on a new thread; `then` afterwards
    // on the UI thread (if the app is still there).
    void runInBackground(std::function<void()> work, std::function<void()> then = nullptr);
    void runOnUiThread(std::function<void()> work);

    // Dialogs, Material's: one at a time; a second request while one is up is dropped.
    struct DialogSpec {
        std::string title, message;
        bool field = false;                  // a text field under the text
        std::string fieldText, fieldPlaceholder;
        std::vector<std::string> fieldSuggestions; // chips under the field, each putting itself in it
        std::vector<std::string> buttons;    // at the dialog's end, the confirming one last
        bool destructive = false;            // the confirming one in the error colour
    };
    /// `done` gets the index of the button pressed (-1: dismissed, by Back or a tap beside it) and the field's text.
    void dialog(DialogSpec spec, std::function<void(int button, std::string text)> done);
    /// The same, shown once no other is up (dialog drops a second): a question an app asked for that must be answered, as a
    /// permission's (player/PermissionHost.h), waits its turn.
    void dialogWhenFree(DialogSpec spec, std::function<void(int button, std::string text)> done);
    void alert(std::string title, std::string message);
    /// Whether a dialog is up (dialog() shows one at a time).
    bool dialogShown() const { return dialogOpen_; }
    /// The dialog up, answered with its button `button` (from the left) as if it had been pressed: a flag's (ui/UiShots.h).
    void pressDialog(int button) { closeDialog(button); }
    void confirm(std::string title, std::string message, std::string confirmTitle, bool destructive,
                 std::function<void()> onConfirm, std::function<void()> onCancel = nullptr);

    /// The file picker for an .apk, or a bundle of split APKs (.apks, .xapk, .apkm),
    /// and the app installed (ui/SeparateApps.h). An APK is read where it is: a
    /// container can't open a picked file by its path, but the core is given it
    /// through the broker's handles (shell/PickedFile.h), and Megler copies it into
    /// the library (LocalState\apks) only when it has to keep it. A bundle is
    /// unpacked from where it is by the core (megler_bundle_unpack) into the base
    /// and the splits this phone can use, there. Where the broker gives no
    /// handle, what was picked is copied first, as before. `done` gets the
    /// library's id it goes by, empty when nothing was picked, or an error.
    void importApk(std::function<void(std::string id, std::string error)> done);
    const ImportProgress& importProgress() const { return *import_; }
    /// An APK (or a bundle) opened in Megler from the phone's file manager: installed as importApk installs a picked one,
    /// once the user says yes in Material's dialog.
    void installApkFile(winrt::Windows::Storage::StorageFile const& file);
    /// An APK at a path of Megler's own that an Android app (`from`, its name) asked to install: likewise, after a yes. Once
    /// the list of the installed apps has been asked for (`listAsked`), when it was not there.
    void installApkPath(const std::wstring& path, const std::string& name, const std::string& from, bool listAsked = false);
    /// Another app the phone has that an Android app (`from`, its name) asked to uninstall: after a yes, its own package and
    /// Megler's copy of it go; a no is told to the app that asked (library/PhoneAppsList.h).
    void uninstallApp(const std::string& package, const std::string& from);
    /// A megler: link Megler was opened with (App.cpp, OnActivated): megler:install?name= (an APK an app's own package
    /// handed over in the shared folder's incoming\), megler:uninstall?package= (an app it asks to uninstall),
    /// megler:run?package= (an app a launcher starts).
    void openLink(winrt::Windows::Foundation::Uri const& uri);

    // Lifecycle (from App)
    void suspending();
    void resuming();

private:
    struct Stack;
    void buildChrome();
    void buildTabBar();
    void closeDialog(int button);
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
    // APKs Android apps asked to install that the library has taken: its id and the name they came by, till it has them read
    std::vector<std::pair<std::string, std::string>> askedInstalls_;
    /// At every poll: tells an app that asked of an APK the library has read that cannot run here (library/PhoneAppsList.h).
    void tellAskedInstalls();
    bool playStoreCanStart(const std::string& package);

    winrt::Windows::UI::Xaml::Controls::Grid root_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid navigationBar_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Button backButton_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBlock titleText_{nullptr};
    winrt::Windows::UI::Xaml::Controls::ContentControl toolbarHost_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Border contentHost_{nullptr};
    winrt::Windows::UI::Xaml::Controls::Grid tabBar_{nullptr};
    std::vector<winrt::Windows::UI::Xaml::Controls::Button> tabButtons_;
    std::vector<winrt::Windows::UI::Xaml::Controls::Border> tabIndicators_; // the pill behind the chosen one's icon
    std::vector<winrt::Windows::UI::Xaml::Controls::TextBlock> tabLabels_;
    winrt::Windows::UI::Xaml::Controls::Grid dialogLayer_{nullptr};
    winrt::Windows::UI::Xaml::Controls::TextBox dialogField_{nullptr};
    std::function<void(int, std::string)> dialogDone_;
    std::deque<std::pair<DialogSpec, std::function<void(int, std::string)>>> waitingDialogs_; // dialogWhenFree's, in turn
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
/// An app's own package: the splash screen's picture until its app is open, in place of the library.
std::shared_ptr<Page> makeStartScreen(AppShell& shell);

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
