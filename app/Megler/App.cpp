// The application: entry point, lifecycle and the window.
//
// A code-only C++/WinRT XAML app, as gecko-w10m's and podium-w10m's shells
// are: no markup compiler, so the Application implements
// IXamlMetadataProvider itself (the framework asks for it) and builds the
// window content in code.
//
// The lifecycle is a phone's, and it is what a running Android app is told
// of (player/Session.h): leaving the app (the window no longer visible) or
// the phone suspending it pauses the app, coming back resumes it, and the
// window losing or gaining activation is its input focus.
#include "pch.h"

#include "megler.h"

#include "auto/AutoRun.h"
#include "library/Library.h"
#include "player/PhoneEvents.h"
#include "player/PhoneState.h"
#include "player/Session.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/LogMirror.h"
#include "shell/PicturesDiag.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Startup.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Theme.h"

#include <exception>

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Xaml;

namespace megler::app {

namespace {

/// The core is up: the start's watch may ask it where the UI thread stands (megler_thread_where).
std::atomic<bool> gCoreUp{false};

/// What an exception says of itself, for the log.
std::string describe(std::exception_ptr exception) {
    try {
        if (exception) std::rethrow_exception(exception);
    } catch (const hresult_error& e) {
        return format("WinRT error 0x%08X: ", unsigned(e.code())) + narrow(e.message());
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "an exception of another kind";
    }
    return "no exception";
}

/// std::terminate on the UI thread -- an exception out of a noexcept function or a fire_and_forget coroutine -- said, with
/// the exception and the start's step, before abort() ends the process (whose own word the core says: "host: abort()").
[[noreturn]] void onTerminate() {
    shellLog::writeNow("FATAL: std::terminate on the UI thread (" + describe(std::current_exception()) + "), the start's step: " +
                       startup::current());
    abort();
}

} // namespace

struct App : ApplicationT<App, Markup::IXamlMetadataProvider> {
    std::shared_ptr<ui::AppShell> shell_;
    bool contentMade_ = false;
    std::string startFailure_;
    winrt::Windows::System::Display::DisplayRequest autoRunDisplayRequest_{nullptr};
    winrt::Windows::ApplicationModel::ExtendedExecution::ExtendedExecutionSession extendedExecution_{nullptr};

    App() {
        // The log first, before anything that could throw.
        try {
            shellLog::initialize(platform::localStateDirectory());
        } catch (...) {
        }
        shellLog::write("Megler for Windows 10 Mobile " + platform::packageVersion() + " (" + platform::architecture() + ", " +
                        platform::deviceFamily() + " " + platform::osVersion() + ")");
        // (Each thread has its own terminate handler: this is the UI thread's. Any thread's abort() the core says.)
        std::set_terminate(&onTerminate);
        // The start watched until the window is shown (shell/Startup.h): where the UI thread stands, once the core can say.
        startup::launched([](unsigned thread) -> std::string {
            if (!gCoreUp.load()) return std::string();
            char where[3000];
            return megler_thread_where(thread, where, sizeof(where)) ? std::string(where) : std::string("it could not be stopped to look");
        });
        // The logs' copies in the shared folder, where Save All Logs finds every package's: the runs before, which the log has
        // just put aside, and this run's as it goes (shell/LogMirror.h).
        logMirror::start();

        UnhandledException([](auto&&, UnhandledExceptionEventArgs const& e) {
            shellLog::write("FATAL: unhandled XAML exception: " + narrow(e.Message()));
        });
        try {
            CoreApplication::UnhandledErrorDetected([](auto&&, UnhandledErrorDetectedEventArgs const& e) {
                try {
                    e.UnhandledError().Propagate();
                } catch (const hresult_error& error) {
                    shellLog::write("FATAL: unhandled WinRT error: " + narrow(error.message()));
                } catch (...) {
                    shellLog::write("FATAL: unhandled WinRT error, no detail");
                }
            });
        } catch (const hresult_error&) {
        }

        Suspending([this](auto&&, SuspendingEventArgs const&) {
            shellLog::write("app: suspending");
            session::setSuspended(true);
            if (shell_) shell_->suspending();
            // Windows freezes the app as it is: a database lock it held would keep the other apps' Play services waiting
            // until it came back. A second at most for its transactions to end.
            if (!megler_settle_databases(1000)) shellLog::write("app: suspended with a database still locked");
            // And what the app wrote into its files' shared mappings, to the files: Windows may end it while it is suspended.
            megler_settle_files();
            shellLog::flush();
            // (And the log's copy in the shared folder, for Save All Logs: Windows may end the app while it is suspended.)
            logMirror::now();
            // (And the log where a diagnosis over USB asked for it: shell/PicturesDiag.h.)
            picturesDiag::exportNow();
        });
        Resuming([this](auto&&, auto&&) {
            shellLog::write("app: resuming");
            if (shell_) shell_->resuming();
            session::setSuspended(false);
            // A call takes the screen, and may have come and gone while Megler was suspended under it.
            phoneEvents::check();
        });
        try {
            CoreApplication::Exiting([](auto&&, auto&&) {
                shellLog::write("app: exiting");
                shellLog::flush();
                logMirror::now();
            });
        } catch (const hresult_error&) {
        }
        /*
         * The app's place in the background, and the memory Windows allows it there, said: Spotify's music stopped when the
         * phone's screen went off, as a rule, and when it was minimized, now and then (a Lumia 650, 652), and nothing in the
         * log said whether Windows suspended it for its memory, as it does with an app over its background limit.
         */
        try {
            EnteredBackground([](auto&&, auto&&) { shellLog::write("app: in the background; memory: " + platform::memorySummary()); });
            LeavingBackground([](auto&&, auto&&) { shellLog::write("app: leaving the background; memory: " + platform::memorySummary()); });
            winrt::Windows::System::MemoryManager::AppMemoryUsageLimitChanging(
                [](auto&&, winrt::Windows::System::AppMemoryUsageLimitChangingEventArgs const& e) {
                    shellLog::write("memory: Windows changes the app's limit from " + formattedByteCount(e.OldLimit()) + " to " +
                                    formattedByteCount(e.NewLimit()) + "; " + platform::memorySummary());
                });
            // And each step of the app's use up toward its limit, as Windows counts it: an app over it is ended without a word,
            // and the log's last lines then say how near it was.
            winrt::Windows::System::MemoryManager::AppMemoryUsageIncreased([](auto&&, auto&&) {
                using winrt::Windows::System::AppMemoryUsageLevel;
                const AppMemoryUsageLevel level = winrt::Windows::System::MemoryManager::AppMemoryUsageLevel();
                const char* said = level == AppMemoryUsageLevel::Low      ? "low"
                                   : level == AppMemoryUsageLevel::Medium ? "medium"
                                   : level == AppMemoryUsageLevel::High   ? "high"
                                                                          : "over its limit";
                shellLog::writeNow(std::string("memory: Windows counts the app's use as ") + said + " now: " + platform::memorySummary());
            });
        } catch (const hresult_error& e) {
            shellLog::write("app: the background's events are not there: " + narrow(e.message()));
        }
    }

    void OnLaunched(LaunchActivatedEventArgs const& args) {
        shellLog::write("app: launched (previous state " + std::to_string(int(args.PreviousExecutionState())) + ")");
        // A diagnosis asked for over USB: its flags in place before an app runs, the log copied out (shell/PicturesDiag.h).
        startStep("looking for a diagnosis asked for over USB", [] { picturesDiag::start(); });
        copySystemDlls();
        ensureContent();
        activate();
        // A newer Megler copied to the phone's Pictures\Megler: installed by Windows's deployment engine.
        appPackage::installCopiedUpdate();
    }

    /// A megler: link (Package.appxmanifest, the protocol), by which a package of Megler's asks another: an APK an app's own
    /// package hands over to install, an app a launcher starts.
    void OnActivated(IActivatedEventArgs const& args) {
        shellLog::write(format("app: activated (kind %d)", int(args.Kind())));
        ensureContent();
        activate();
        if (args.Kind() == ActivationKind::Protocol && shell_) shell_->openLink(args.as<ProtocolActivatedEventArgs>().Uri());
    }

    /// An APK (or a bundle) opened in Megler from the phone's file manager or another app (Package.appxmanifest, the
    /// file type association): installed once the user says yes, as one picked in the Library.
    void OnFileActivated(FileActivatedEventArgs const& args) {
        shellLog::write(format("app: opened with %u file(s)", unsigned(args.Files().Size())));
        ensureContent();
        activate();
        for (auto const& item : args.Files()) {
            if (auto file = item.try_as<winrt::Windows::Storage::StorageFile>()) {
                if (shell_) shell_->installApkFile(file);
                break;
            }
        }
    }

    /// The window shown: Windows takes the splash screen away at the first frame after this. Then what the start leaves
    /// for after it: the copies of the package's Google APKs, on a thread of their own (Library::afterStart).
    void activate() {
        // (Activated again later -- a megler: link, a file opened in Megler -- it is no step of the start.)
        if (startup::isShown()) {
            Window::Current().Activate();
            return;
        }
        startStep("showing the window", [] { Window::Current().Activate(); });
        startup::shown();
        try {
            Library::instance().afterStart();
        } catch (...) {
            shellLog::writeNow("FATAL: the copies of the package's APKs could not be begun: " + describe(std::current_exception()));
        }
    }

    /// One step of the start, said before it begins (shell/Startup.h). What it throws is said as FATAL with the step, and
    /// the start goes on: a library that cannot be read, a theme that cannot be had, leave a window that says so, not none.
    template <typename Step>
    bool startStep(const char* what, Step&& run) {
        startup::step(what);
        try {
            run();
            return true;
        } catch (...) {
            const std::string why = describe(std::current_exception());
            shellLog::writeNow(std::string("FATAL: the start's step \"") + what + "\" failed: " + why);
            if (startFailure_.empty()) startFailure_ = std::string(what) + ": " + why;
        }
        return false;
    }

    /// What is shown when the screens could not be made: why, where the log is.
    void showStartFailure() {
        try {
            using namespace winrt::Windows::UI::Xaml::Controls;
            TextBlock text;
            text.Text(hs("Megler could not start: " + startFailure_ + "\n\nIts log, megler.log in Megler's LocalState folder, says more."));
            text.TextWrapping(TextWrapping::Wrap);
            text.Margin(ThicknessHelper::FromUniformLength(24));
            text.VerticalAlignment(VerticalAlignment::Center);
            Window::Current().Content(text);
        } catch (...) {
            shellLog::writeNow("FATAL: not even the page that says why could be shown: " + describe(std::current_exception()));
        }
    }

    // The flag copy-system-dlls: the phone's ntdll.dll and KERNELBASE.dll
    // copied into LocalState, to be read on the PC with a disassembler --
    // what an address of theirs in a crash report is, which no public
    // symbols say for the phone's build.
    static void copySystemDlls() {
        const std::wstring local = platform::localStateDirectory();
        if (!files::exists(local + L"\\flags\\copy-system-dlls")) return;
        for (const wchar_t* name : {L"ntdll.dll", L"KERNELBASE.dll"}) {
            const bool ok = files::copy(std::wstring(L"C:\\Windows\\System32\\") + name, local + L"\\" + name);
            shellLog::write(std::string("flags: copy-system-dlls: ") + narrow(name) + (ok ? " copied" : " could not be copied"));
        }
    }

    void ensureContent() {
        if (contentMade_) return;
        contentMade_ = true;
        // The folder of the flag files (core: mg_flag; ui/UiShots.h): the Device Portal puts files into a folder, but makes none.
        files::makeDirectories(platform::localStateDirectory() + L"\\flags");
        startStep("reading the settings", [] { settings::load(platform::localStateDirectory(), appPackage::sharedFolder(), appPackage::single().package); });
        // The core, before any screen asks it anything. Its answer -- can
        // Android code run here -- is in the log and on the screens.
        startStep("starting the core", [] { platform::initializeCore(); });
        gCoreUp = true;
        // (Apps' names are read in the phone's language.)
        startStep("telling the core the phone's language", [] { phoneState::tellLocale(); });
        startStep("reading the library", [] { Library::instance().load(); });

        startStep("setting the window's bounds", [] {
            using winrt::Windows::UI::ViewManagement::ApplicationView;
            using winrt::Windows::UI::ViewManagement::ApplicationViewBoundsMode;
            auto view = ApplicationView::GetForCurrentView();
            // Content stays out from under the status bar and the navigation bar
            // (the player takes the whole display back: ui/AppShell.cpp).
            view.SetDesiredBoundsMode(ApplicationViewBoundsMode::UseVisible);
            if (!platform::isMobile() && !platform::isXbox()) {
                // On a desktop (the x64 build) open phone-shaped. (An Xbox shows an app on the whole television, at 960 by 540
                // of its pixels, whatever it asks.)
                using winrt::Windows::UI::ViewManagement::ApplicationViewWindowingMode;
                view.PreferredLaunchViewSize(winrt::Windows::Foundation::Size(400, 760));
                view.PreferredLaunchWindowingMode(ApplicationViewWindowingMode::PreferredLaunchViewSize);
                view.TryResizeView(winrt::Windows::Foundation::Size(400, 760));
            }
        });

        startStep("loading the screens' styles", [] { ui::installStyles(); });
        const bool made = startStep("making the screens", [this] {
            auto shell = std::make_shared<ui::AppShell>();
            Window::Current().Content(shell->root());
            shell_ = std::move(shell);
        });
        if (!made) {
            shell_.reset();
            showStartFailure();
        }
        Window window = Window::Current();

        // What a running app hears of the window (player/Session.h).
        window.VisibilityChanged([](auto&&, winrt::Windows::UI::Core::VisibilityChangedEventArgs const& e) {
            shellLog::write(e.Visible() ? "app: visible" : "app: no longer visible");
            session::setVisible(e.Visible());
        });
        window.Activated([](auto&&, winrt::Windows::UI::Core::WindowActivatedEventArgs const& e) {
            shellLog::write(format("app: window activation state %d", int(e.WindowActivationState())));
            session::setFocused(e.WindowActivationState() != winrt::Windows::UI::Core::CoreWindowActivationState::Deactivated);
        });
        window.SizeChanged([](auto&&, winrt::Windows::UI::Core::WindowSizeChangedEventArgs const& e) {
            shellLog::write(format("app: window size %.0fx%.0f", double(e.Size().Width), double(e.Size().Height)));
        });
        startAutoRun();
    }

    /// A build made with tools/build-appx.sh --auto runs its checks now,
    /// unattended (auto/AutoRun.h); any other build does nothing.
    void startAutoRun() {
        const std::string description = "Megler " + platform::packageVersion() + " (" + platform::architecture() + ", " +
                                        platform::deviceFamily() + " " + platform::osVersion() + ")";
        if (!autorun::start(description)) return;
        // Nobody touches the phone meanwhile: keep its screen on, or it locks
        // and suspends the app mid-run.
        try {
            autoRunDisplayRequest_ = winrt::Windows::System::Display::DisplayRequest();
            autoRunDisplayRequest_.RequestActive();
        } catch (const hresult_error& e) {
            shellLog::write("auto run: DisplayRequest failed: " + narrow(e.message()));
        }
        // And if the app is left anyway (a phone suspends an app the moment
        // it isn't in front -- that froze podium-w10m's first on-device run
        // three tests in), ask to keep running in the background.
        requestExtendedExecution();
    }

    winrt::fire_and_forget requestExtendedExecution() {
        using namespace winrt::Windows::ApplicationModel::ExtendedExecution;
        try {
            extendedExecution_ = ExtendedExecutionSession();
            extendedExecution_.Reason(ExtendedExecutionReason::Unspecified);
            extendedExecution_.Description(L"Megler device checks");
            extendedExecution_.Revoked([](auto&&, ExtendedExecutionRevokedEventArgs const& e) {
                shellLog::write(std::string("auto run: background execution revoked (") +
                                (e.Reason() == ExtendedExecutionRevokedReason::Resumed ? "resumed" : "system policy") + ")");
            });
            ExtendedExecutionResult result = co_await extendedExecution_.RequestExtensionAsync();
            shellLog::write(std::string("auto run: background execution ") +
                            (result == ExtendedExecutionResult::Allowed ? "allowed" : "denied"));
        } catch (const hresult_error& e) {
            shellLog::write("auto run: background execution unavailable: " + narrow(e.message()));
        }
    }

    // IXamlMetadataProvider: no custom XAML types.
    Markup::IXamlType GetXamlType(Interop::TypeName const&) { return nullptr; }
    Markup::IXamlType GetXamlType(hstring const&) { return nullptr; }
    com_array<Markup::XmlnsDefinition> GetXmlnsDefinitions() { return {}; }
};

} // namespace megler::app

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    megler::app::shellLog::bootMark("wWinMain reached");
    // The entry thread must be MTA; Application::Start makes the ASTA UI
    // thread itself (an STA entry thread makes CoreApplication throw
    // RPC_E_WRONG_THREAD).
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    Application::Start([](auto&&) {
        megler::app::shellLog::bootMark("XAML started");
        winrt::make<megler::app::App>();
    });
    return 0;
}
