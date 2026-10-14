#include "pch.h"

#include "shell/Platform.h"

#include "player/StorageHost.h"
#include "shell/Files.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include "megler.h"

namespace megler::app::platform {

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using winrt::Windows::System::MemoryManager;

namespace {

bool gCanRun = false;

// megler_config's memory callbacks. They are called from the app's own threads,
// many times a second by some engines (a garbage collector asking how much is
// free), and those threads must not call into Windows Runtime: they are
// Android threads, come and go, and belong to no apartment. So what they are
// given is what the shell's own thread last read (sampleMemory, on its tick).
std::atomic<uint64_t> gMemoryLimit{0}, gMemoryUsage{0};

uint64_t memoryLimit() { return gMemoryLimit.load(std::memory_order_relaxed); }
uint64_t memoryUsage() { return gMemoryUsage.load(std::memory_order_relaxed); }

// What libEGL.dll and libGLESv2.dll need beside them, in the order they
// depend on each other (tools/build-appx.sh stages all five or none).
const wchar_t* const angleFiles[] = {L"vcruntime140.dll", L"msvcp140.dll", L"mozglue.dll", L"libGLESv2.dll", L"libEGL.dll"};

DWORD WINAPI threadEntry(void* parameter) {
    std::unique_ptr<std::function<void()>> work(static_cast<std::function<void()>*>(parameter));
    try {
        (*work)();
    } catch (const hresult_error& e) {
        shellLog::write("worker: uncaught WinRT error: " + narrow(e.message()));
    } catch (const std::exception& e) {
        shellLog::write(std::string("worker: uncaught exception: ") + e.what());
    } catch (...) {
        shellLog::write("worker: uncaught exception");
    }
    return 0;
}

} // namespace

bool isMobile() { return deviceFamily() == "Windows.Mobile"; }

std::string deviceFamily() {
    try {
        return narrow(winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamily());
    } catch (...) {
        return "unknown";
    }
}

std::string osVersion() {
    try {
        uint64_t v = std::stoull(narrow(winrt::Windows::System::Profile::AnalyticsInfo::VersionInfo().DeviceFamilyVersion()));
        return format("%llu.%llu.%llu.%llu", (unsigned long long)(v >> 48), (unsigned long long)((v >> 32) & 0xFFFF),
                      (unsigned long long)((v >> 16) & 0xFFFF), (unsigned long long)(v & 0xFFFF));
    } catch (...) {
        return "unknown";
    }
}

std::string packageVersion() {
    try {
        auto v = Package::Current().Id().Version();
        return format("%u.%u.%u.%u", v.Major, v.Minor, v.Build, v.Revision);
    } catch (...) {
        return "unknown";
    }
}

std::string architecture() {
#if defined(_M_ARM)
    return "ARM";
#elif defined(_M_X64)
    return "x64";
#else
    return "other";
#endif
}

const std::wstring& localStateDirectory() {
    static const std::wstring path(winrt::Windows::Storage::ApplicationData::Current().LocalFolder().Path());
    return path;
}

const std::wstring& installDirectory() {
    static const std::wstring path(Package::Current().InstalledLocation().Path());
    return path;
}

std::string memorySummary() {
    try {
        return formattedByteCount(MemoryManager::AppMemoryUsageLimit()) + " limit, " +
               formattedByteCount(MemoryManager::AppMemoryUsage()) + " in use";
    } catch (...) {
        return "MemoryManager unavailable";
    }
}

void sampleMemory() {
    try {
        gMemoryLimit.store(MemoryManager::AppMemoryUsageLimit(), std::memory_order_relaxed);
        gMemoryUsage.store(MemoryManager::AppMemoryUsage(), std::memory_order_relaxed);
    } catch (...) {
    }
}

void initializeCore() {
    sampleMemory();
    // The core copies both paths; they are kept alive all the same.
    static const std::string dataDirectory = narrow(localStateDirectory());
    static const std::string packageDirectory = narrow(installDirectory());
    shellLog::write("LocalState: " + dataDirectory);
    shellLog::write("install folder: " + packageDirectory);
    shellLog::write("memory: " + memorySummary());
    shellLog::write("ANGLE: " + angle().summary);
    // The phone's folders for an app's storage are looked for now, on a thread of their own: by the time an app is started they are known.
    StorageHost::shared().start();

    megler_config config = {};
    config.data_dir = dataDirectory.c_str();
    config.package_dir = packageDirectory.c_str();
    config.log = &shellLog::core;
    config.log_user = nullptr;
    config.memory_limit = &memoryLimit;
    config.memory_usage = &memoryUsage;
    gCanRun = megler_init(&config);
    shellLog::write(std::string("core ") + megler_version() +
                    (gCanRun ? ": ready to run Android code"
                             : ": this process cannot run Android code (megler_init returned false: is codeGeneration granted?)"));
}

bool canRunAndroidCode() { return gCanRun; }

const AngleStatus& angle() {
    static const AngleStatus status = [] {
        AngleStatus s;
        std::string missing;
        for (const wchar_t* name : angleFiles)
            if (!files::exists(installDirectory() + L"\\" + name)) missing += std::string(missing.empty() ? "" : ", ") + narrow(name);
        s.present = missing.empty();
        s.summary = s.present ? "In the package" : "ANGLE missing: " + missing;
        return s;
    }();
    return status;
}

void angleLoadCheck(const std::function<void(const std::string&)>& line) {
    if (!angle().present) {
        line("angle-package: " + angle().summary);
        return;
    }
    std::string order;
    for (const wchar_t* name : angleFiles) order += std::string(order.empty() ? "" : ", ") + narrow(name);
    line("angle-package: all five DLLs are in the package; loading them in this order: " + order);
    for (const wchar_t* name : angleFiles) {
        SetLastError(0);
        HMODULE module = LoadPackagedLibrary(name, 0);
        if (module) {
            line("angle-load: " + narrow(name) + " ok");
            continue;
        }
        const DWORD error = GetLastError();
        const char* meaning = error == ERROR_MOD_NOT_FOUND    ? "it, or a DLL it imports, was not found"
                              : error == ERROR_PROC_NOT_FOUND ? "a DLL it imports lacks a function it needs"
                              : error == ERROR_BAD_EXE_FORMAT ? "not an image for this processor"
                              : error == ERROR_ACCESS_DENIED  ? "access denied"
                                                              : "see the Win32 error";
        line(format("angle-load: %s FAILED, error %lu (%s)", narrow(name).c_str(), error, meaning));
    }
}

bool startThread(std::function<void()> work, size_t stackBytes) {
    auto* parameter = new std::function<void()>(std::move(work));
    HANDLE thread = CreateThread(nullptr, stackBytes, &threadEntry, parameter, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    if (!thread) {
        delete parameter;
        shellLog::write(format("worker: CreateThread failed, error %lu", GetLastError()));
        return false;
    }
    CloseHandle(thread);
    return true;
}

} // namespace megler::app::platform
