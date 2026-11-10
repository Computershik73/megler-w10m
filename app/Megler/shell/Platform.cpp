#include "pch.h"

#include "shell/Platform.h"

#include "player/StorageHost.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/PackageVersions.h"
#include "shell/Placement.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include "megler.h"

#include <winrt/Windows.Security.ExchangeActiveSyncProvisioning.h>

#if defined(MG_ELF_HOST)
// core/src/host/win32/mg_core_loader.h: the core's ELF into this process (desktop Windows, docs/DESKTOP.md).
extern "C" bool mg_core_load(const char* path, char* err, size_t errlen);
#endif

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
// depend on each other (tools/build-appx.sh stages all or none). On desktop
// Windows the two are Megler's own build of gecko-w10m's ANGLE, with the C
// runtime linked in and no mozglue (tools/build-angle.py --arch).
#if defined(MG_ELF_HOST)
const wchar_t* const angleFiles[] = {L"libGLESv2.dll", L"libEGL.dll"};
#else
const wchar_t* const angleFiles[] = {L"vcruntime140.dll", L"msvcp140.dll", L"mozglue.dll", L"libGLESv2.dll", L"libEGL.dll"};
#endif

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

bool isXbox() {
    static const bool xbox = deviceFamily() == "Windows.Xbox" || asXbox();
    return xbox;
}

bool asXbox() {
    static const bool flagged = [] {
        if (deviceFamily() == "Windows.Xbox" || deviceFamily() == "Windows.Mobile") return false;
        if (!files::exists(localStateDirectory() + L"\\flags\\as-xbox")) return false;
        shellLog::write("flag: as-xbox is on: Megler takes an Xbox's ways (its window 960 by 540 and never turned for an app; the log's ZIP in "
                        "LocalState)");
        return true;
    }();
    return flagged;
}

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
        // (An app's own package, "<major>.<minor>.<Megler's build>.<n>": made by "<major>.<minor>.0.<Megler's build>".)
        return packageVersions::madeBy(format("%u.%u.%u.%u", v.Major, v.Minor, v.Build, v.Revision));
    } catch (...) {
        return "unknown";
    }
}

std::string ownPackageVersion() {
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
#elif defined(_M_ARM64)
    return "ARM64";
#elif defined(_M_X64)
    return "x64";
#elif defined(_M_IX86)
    return "x86";
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

std::string deviceSummary() {
    std::string text;
    try {
        // (What Windows calls the device for Exchange: its maker, its model's code, its variant. A phone given a later Windows
        // than its maker gave it may say another model's.)
        winrt::Windows::Security::ExchangeActiveSyncProvisioning::EasClientDeviceInformation eas;
        const std::string maker = narrow(eas.SystemManufacturer()), product = narrow(eas.SystemProductName());
        text = (maker.empty() ? std::string() : maker + " ") + (product.empty() ? std::string("(no model said)") : product);
        try {
            const std::string sku = narrow(eas.SystemSku());
            if (!sku.empty() && sku != product) text += " (" + sku + ")";
        } catch (...) {
        }
    } catch (...) {
        text = "a device Windows does not name";
    }
    MEMORYSTATUSEX status = {};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status))
        text += ", " + formattedByteCount(status.ullTotalPhys) + " of memory, " + formattedByteCount(status.ullAvailPhys) + " of it free";
    return text;
}

std::string diskSummary() {
    const auto room = [](const std::wstring& folder) {
        ULARGE_INTEGER free = {}, total = {};
        return GetDiskFreeSpaceExW(folder.c_str(), &free, &total, nullptr)
                   ? formattedByteCount(free.QuadPart) + " free of " + formattedByteCount(total.QuadPart)
                   : format("not said (error %lu)", GetLastError());
    };
    const auto drive = [](const std::wstring& path) { return path.size() >= 2 && path[1] == L':' ? std::wstring(1, wchar_t(towupper(path[0]))) : path; };
    const std::wstring local = localStateDirectory(), install = installDirectory(), shared = appPackage::sharedFolder();
    std::string text = "LocalState's " + room(local);
    if (drive(install) != drive(local)) text += "; the package's " + room(install);
    // (A package installed on a memory card may be given no publisher's folder: the services' copies and the apps' shared
    // storage then have nowhere to go.)
    if (shared.empty()) text += "; no shared folder (Windows gave Megler no folder of its publisher's)";
    else if (drive(shared) != drive(local)) text += "; the shared folder's " + room(shared);
    return text;
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
    // Which drive each is on: a package on a memory card has its LocalState there, and the card's shared folder or none
    // (shell/Placement.h).
    shellLog::write("storage: " + placement::summary(installDirectory(), localStateDirectory(), appPackage::sharedFolder()));
    shellLog::write("memory: " + memorySummary());
    // What the phone is, and the room its disks have: a first start copies hundreds of megabytes (shell/BundledApks.h), and
    // a phone of 512 MB is told an app may use 390 MB (a Lumia 630).
    shellLog::write("phone: " + deviceSummary());
    shellLog::write("disk: " + diskSummary());
    shellLog::write("ANGLE: " + angle().summary);
    // The phone's folders for an app's storage are looked for now, on a thread of their own: by the time an app is started they are known.
    StorageHost::shared().start();

#if defined(MG_ELF_HOST)
    {
        // On desktop Windows the core is an ELF of Android's ABI beside Megler.exe (docs/DESKTOP.md), loaded before
        // anything of it is called; until it is, every function of megler.h does nothing and says 0.
        char why[2048] = {};
        static const std::string corePath = packageDirectory + "\\megler-core.so";
        if (!mg_core_load(corePath.c_str(), why, sizeof(why))) {
            shellLog::write(std::string("core: not loaded: ") + why);
            gCanRun = false;
            return;
        }
        shellLog::write("core: " + corePath + " loaded");
    }
#endif
    megler_config config = {};
    config.data_dir = dataDirectory.c_str();
    config.package_dir = packageDirectory.c_str();
    config.log = &shellLog::core;
    config.log_user = nullptr;
    config.memory_limit = &memoryLimit;
    config.memory_usage = &memoryUsage;
    // An app's own package keeps the shared storage in the folder every such package shares (shell/AppPackage.h); Megler too,
    // once its storage of before has been copied there.
    static const std::string storageDirectory =
        appPackage::isSingleApp() || appPackage::meglersStorageShared() ? narrow(appPackage::sharedFolder()) : std::string();
    if (!storageDirectory.empty()) {
        config.storage_dir = storageDirectory.c_str();
        shellLog::write("shared storage: " + storageDirectory);
    }
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
    line(format("angle-package: all %d DLLs are in the package; loading them in this order: ", (int)std::size(angleFiles)) + order);
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
