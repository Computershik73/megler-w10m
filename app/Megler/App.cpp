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

#include "auto/AutoRun.h"
#include "library/Library.h"
#include "player/PhoneState.h"
#include "player/Session.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Theme.h"

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::ApplicationModel::Activation;
using namespace winrt::Windows::ApplicationModel::Core;
using namespace winrt::Windows::UI::Xaml;

namespace megler::app {

struct App : ApplicationT<App, Markup::IXamlMetadataProvider> {
    std::shared_ptr<ui::AppShell> shell_;
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
        });
        Resuming([this](auto&&, auto&&) {
            shellLog::write("app: resuming");
            if (shell_) shell_->resuming();
            session::setSuspended(false);
        });
        try {
            CoreApplication::Exiting([](auto&&, auto&&) { shellLog::write("app: exiting"); });
        } catch (const hresult_error&) {
        }
    }

    void OnLaunched(LaunchActivatedEventArgs const& args) {
        shellLog::write("app: launched (previous state " + std::to_string(int(args.PreviousExecutionState())) + ")");
        copySystemDlls();
        ensureContent();
        Window::Current().Activate();
    }

    /// An APK (or a bundle) opened in Megler from the phone's file manager or another app (Package.appxmanifest, the
    /// file type association): installed once the user says yes, as one picked in the Library.
    void OnFileActivated(FileActivatedEventArgs const& args) {
        shellLog::write(format("app: opened with %u file(s)", unsigned(args.Files().Size())));
        ensureContent();
        Window::Current().Activate();
        for (auto const& item : args.Files()) {
            if (auto file = item.try_as<winrt::Windows::Storage::StorageFile>()) {
                shell_->installApkFile(file);
                break;
            }
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
        if (shell_) return;
        // The folder of the flag files (core: mg_flag; ui/UiShots.h): the Device Portal puts files into a folder, but makes none.
        files::makeDirectories(platform::localStateDirectory() + L"\\flags");
        settings::load(platform::localStateDirectory(), appPackage::sharedFolder(), appPackage::single().package);
        // The core, before any screen asks it anything. Its answer -- can
        // Android code run here -- is in the log and on the screens.
        platform::initializeCore();
        // (Apps' names are read in the phone's language.)
        phoneState::tellLocale();
        Library::instance().load();

        using winrt::Windows::UI::ViewManagement::ApplicationView;
        using winrt::Windows::UI::ViewManagement::ApplicationViewBoundsMode;
        auto view = ApplicationView::GetForCurrentView();
        // Content stays out from under the status bar and the navigation bar
        // (the player takes the whole display back: ui/AppShell.cpp).
        view.SetDesiredBoundsMode(ApplicationViewBoundsMode::UseVisible);
        if (!platform::isMobile()) {
            // On a desktop (the x64 build) open phone-shaped.
            using winrt::Windows::UI::ViewManagement::ApplicationViewWindowingMode;
            view.PreferredLaunchViewSize(winrt::Windows::Foundation::Size(400, 760));
            view.PreferredLaunchWindowingMode(ApplicationViewWindowingMode::PreferredLaunchViewSize);
            view.TryResizeView(winrt::Windows::Foundation::Size(400, 760));
        }

        ui::installStyles();
        shell_ = std::make_shared<ui::AppShell>();
        Window window = Window::Current();
        window.Content(shell_->root());

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
