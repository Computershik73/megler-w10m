#include "pch.h"

#include "player/BiometricHost.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Security.Credentials.UI.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace megler::app::biometricHost {
namespace {

using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Security::Credentials::UI::UserConsentVerificationResult;
using winrt::Windows::Security::Credentials::UI::UserConsentVerifier;
using winrt::Windows::Security::Credentials::UI::UserConsentVerifierAvailability;
using winrt::Windows::UI::Core::CoreDispatcher;
using winrt::Windows::UI::Core::CoreDispatcherPriority;

std::mutex gMutex; // the check that runs
IAsyncOperation<UserConsentVerificationResult> gRunning{nullptr};
bool gCanceled = false;
int gSaid = -1; // the availability last written to the log

/// `f` on a thread of the multithreaded apartment, waited for: the core's calls come on threads with no apartment.
template <class F>
void inMta(F&& f) {
    std::thread t([&f] {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            f();
        } catch (const winrt::hresult_error& e) {
            shellLog::write(format("biometric: 0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str()));
        } catch (...) {
            shellLog::write("biometric: an exception");
        }
        winrt::uninit_apartment();
    });
    t.join();
}

CoreDispatcher uiDispatcher() {
    try {
        return winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().Dispatcher();
    } catch (const winrt::hresult_error&) {
        return nullptr;
    }
}

int availableThunk(void*) {
    int can = MEGLER_BIOMETRIC_NONE;
    const char* why = "Windows could not be asked";
    inMta([&can, &why] {
        switch (UserConsentVerifier::CheckAvailabilityAsync().get()) {
        case UserConsentVerifierAvailability::Available: can = MEGLER_BIOMETRIC_READY, why = "Windows Hello is ready"; break;
        case UserConsentVerifierAvailability::DeviceBusy: can = MEGLER_BIOMETRIC_READY, why = "Windows Hello is busy now"; break;
        case UserConsentVerifierAvailability::NotConfiguredForUser:
            can = MEGLER_BIOMETRIC_NOT_SET_UP, why = "Windows Hello is not set up (Settings > Accounts > Sign-in options)";
            break;
        case UserConsentVerifierAvailability::DisabledByPolicy: can = MEGLER_BIOMETRIC_NONE, why = "Windows Hello is turned off by a policy"; break;
        default: can = MEGLER_BIOMETRIC_NONE, why = "the phone has no Windows Hello"; break;
        }
    });
    bool say;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        say = gSaid != can;
        gSaid = can;
    }
    if (say) shellLog::write(std::string("biometric: ") + why);
    return can;
}

int resultOf(UserConsentVerificationResult r) {
    switch (r) {
    case UserConsentVerificationResult::Verified: return MEGLER_BIOMETRIC_VERIFIED;
    case UserConsentVerificationResult::Canceled: return MEGLER_BIOMETRIC_CANCELED;
    case UserConsentVerificationResult::RetriesExhausted: return MEGLER_BIOMETRIC_LOCKED;
    default: return MEGLER_BIOMETRIC_UNAVAILABLE;
    }
}

int verifyThunk(void*, const char* message) {
    const CoreDispatcher dispatcher = uiDispatcher();
    if (!dispatcher) {
        shellLog::write("biometric: no window to show Windows Hello's dialog over");
        return MEGLER_BIOMETRIC_UNAVAILABLE;
    }
    struct State {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        int result = MEGLER_BIOMETRIC_UNAVAILABLE;
    };
    auto state = std::make_shared<State>();
    auto finish = [state](int result) {
        std::lock_guard<std::mutex> lock(state->mutex);
        state->result = result;
        state->done = true;
        state->cv.notify_all();
    };
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gCanceled = false;
    }
    const winrt::hstring text = hs(message && message[0] ? message : "Megler");
    dispatcher.RunAsync(CoreDispatcherPriority::Normal, [finish, text] {
        try {
            {
                std::lock_guard<std::mutex> lock(gMutex);
                if (gCanceled) return finish(MEGLER_BIOMETRIC_CANCELED);
            }
            IAsyncOperation<UserConsentVerificationResult> op = UserConsentVerifier::RequestVerificationAsync(text);
            {
                std::lock_guard<std::mutex> lock(gMutex);
                gRunning = op;
            }
            op.Completed([finish](const IAsyncOperation<UserConsentVerificationResult>& done, AsyncStatus status) {
                int result = MEGLER_BIOMETRIC_UNAVAILABLE;
                if (status == AsyncStatus::Completed) result = resultOf(done.GetResults());
                else if (status == AsyncStatus::Canceled) result = MEGLER_BIOMETRIC_CANCELED;
                else shellLog::write(format("biometric: Windows Hello's check failed (0x%08X)", unsigned(done.ErrorCode().value)));
                finish(result);
            });
        } catch (const winrt::hresult_error& e) {
            shellLog::write(format("biometric: Windows Hello's dialog could not be shown: 0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str()));
            finish(MEGLER_BIOMETRIC_UNAVAILABLE);
        }
    });
    int result;
    {
        std::unique_lock<std::mutex> lock(state->mutex);
        // However long the user takes; a UI thread that never runs it is given up on after ten minutes.
        if (!state->cv.wait_for(lock, std::chrono::minutes(10), [&state] { return state->done; })) shellLog::write("biometric: no answer within ten minutes");
        result = state->result;
    }
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gRunning = nullptr;
    }
    static const char* const said[] = {"verified the user", "was canceled", "is locked: too many tries", "could not check"};
    shellLog::write(std::string("biometric: Windows Hello ") + said[result]);
    return result;
}

void cancelThunk(void*) {
    IAsyncOperation<UserConsentVerificationResult> op{nullptr};
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gCanceled = true;
        op = gRunning;
    }
    const CoreDispatcher dispatcher = uiDispatcher();
    if (!op || !dispatcher) return;
    shellLog::write("biometric: the app canceled its check");
    dispatcher.RunAsync(CoreDispatcherPriority::Normal, [op] {
        try {
            op.Cancel();
        } catch (const winrt::hresult_error&) {
        }
    });
}

} // namespace

megler_biometric_host callbacks() {
    megler_biometric_host host = {};
    host.available = &availableThunk;
    host.verify = &verifyThunk;
    host.cancel = &cancelThunk;
    return host;
}

} // namespace megler::app::biometricHost
