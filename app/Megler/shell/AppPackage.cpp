#include "pch.h"

#include "shell/AppPackage.h"

#include "megler.h"
#include "library/Library.h"
#include "player/Session.h"
#include "shell/BundledApks.h"
#if defined(MEGLER_XBOX)
#include "shell/DevicePortal.h"
#endif
#include "shell/Files.h"
#include "shell/PickedFile.h"
#include "shell/Placement.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Management.Deployment.h>

#include <activation.h>
#include <roapi.h>

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace megler::app::appPackage {

using namespace winrt;
using namespace winrt::Windows::ApplicationModel;
using namespace winrt::Windows::Storage;

namespace {

/// A "key=value" line's value, or empty.
std::string iniValue(const std::string& text, const char* key) {
    const std::string k = std::string(key) + "=";
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.compare(0, k.size(), k) == 0) return line.substr(k.size());
        at = end + 1;
    }
    return {};
}

std::string versionString() {
    const auto v = Package::Current().Id().Version();
    return format("%u.%u.%u.%u", unsigned(v.Major), unsigned(v.Minor), unsigned(v.Build), unsigned(v.Revision));
}

bool sameName(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

/// A folder and everything in it; false at the first failure. With `missingOnly`, a file already there as large is left.
bool copyTree(const std::wstring& from, const std::wstring& to, uint64_t& copied, bool missingOnly = false) {
    if (!files::makeDirectories(to)) return false;
    for (const auto& name : files::list(from, L"*")) {
        if (missingOnly && files::exists(to + L"\\" + name) && files::size(to + L"\\" + name) == files::size(from + L"\\" + name)) continue;
        if (!files::copy(from + L"\\" + name, to + L"\\" + name)) return false;
        copied += files::size(to + L"\\" + name);
    }
    for (const auto& name : files::listFolders(from, L"*"))
        if (!copyTree(from + L"\\" + name, to + L"\\" + name, copied, missingOnly)) return false;
    return true;
}

/// A name a package's part may have as it is (core/src/appx/mg_appx.c): letters, digits, '.', '-', '_' and '/'.
bool plainName(const std::string& name) {
    for (char c : name)
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' || c == '/'))
            return false;
    return !name.empty();
}

/// Megler's own files for an app's package: all of the install folder but what the package gets in its own form (the
/// manifest, resources.pri, MeglerApp.ini, app\), what the packaging adds (block map, content types, signature,
/// AppxMetadata), and what an app's package has no use for (Android's files, which are shared; the self-test's
/// libraries; the demos; the codec samples of the auto runs; the template its resources.pri is made from).
void collect(const std::wstring& root, const std::wstring& rel, std::vector<std::pair<std::string, std::wstring>>& out,
             std::vector<std::string>& skipped) {
    const std::wstring dir = rel.empty() ? root : root + L"\\" + rel;
    for (const auto& name : files::list(dir, L"*")) {
        if (rel.empty() && (sameName(name, L"AppxManifest.xml") || sameName(name, L"AppxBlockMap.xml") ||
                            sameName(name, L"[Content_Types].xml") || sameName(name, L"AppxSignature.p7x") || sameName(name, L"MeglerApp.ini") ||
                            sameName(name, L"resources.pri")))
            continue;
        std::string zip = narrow(rel.empty() ? name : rel + L"\\" + name);
        for (char& c : zip)
            if (c == '\\') c = '/';
        if (!plainName(zip)) {
            skipped.push_back(zip);
            continue;
        }
        out.emplace_back(zip, dir + L"\\" + name);
    }
    // (bitgapps, Google's APKs Megler offers in its library, some 100 MB: an app's own package has its app, and the services
    // it hosts come from the shared folder's services\ -- each package of before carried them all the same.)
    for (const auto& name : files::listFolders(dir, L"*")) {
        if (rel.empty() && (sameName(name, L"android") || sameName(name, L"selftest") || sameName(name, L"demo") ||
                            sameName(name, L"codec") || sameName(name, L"app") || sameName(name, L"AppxMetadata") ||
                            sameName(name, L"appx-template") || sameName(name, L"bitgapps")))
            continue;
        collect(root, rel.empty() ? name : rel + L"\\" + name, out, skipped);
    }
}

std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\r' || c == '\n') c = ' ';
    return s;
}

/*
 * The package volume Megler's own package is installed on, which every package Megler installs goes to (installWithPackageManager):
 * the phone's memory, or the memory card where Settings › System › Storage saves new apps there or Megler was moved there.
 * Windows puts a package where its user saves new apps unless it is told a volume, and an app's package on another storage
 * than Megler's gets that storage's publisher cache folder, or none: it found neither Android's files nor its APK there, and
 * Megler, the other way round, none of the APKs it hands over (docs/PER-APP-PACKAGES.md, "A memory card"). Asked of Windows's
 * package manager at each install (FindPackageVolumes, GetPackageVolumesAsync where that will not answer), the volume being
 * the one whose store path holds Megler's install folder (placement::volumeHolding). Null where neither answers or no volume
 * holds it: the package then goes where Windows puts new apps, as before. The log says which, the first time and whenever
 * it is another.
 */
winrt::Windows::Management::Deployment::PackageVolume installVolume() {
    using namespace winrt::Windows::Management::Deployment;
    static std::mutex mutex;
    static std::string said;  // the last said
    std::vector<PackageVolume> volumes;
    std::string why;
    try {
        for (auto const& volume : PackageManager().FindPackageVolumes()) volumes.push_back(volume);
    } catch (hresult_error const& e) {
        why = format("FindPackageVolumes: 0x%08x", unsigned(e.code()));
        try {
            for (auto const& volume : PackageManager().GetPackageVolumesAsync().get()) volumes.push_back(volume);
        } catch (hresult_error const& again) {
            why += format("; GetPackageVolumesAsync: 0x%08x", unsigned(again.code()));
        }
    }
    std::vector<placement::Volume> places;
    for (auto const& volume : volumes) {
        placement::Volume place;
        try {
            place.name = volume.Name();
            place.storePath = volume.PackageStorePath();
            place.mountPoint = volume.MountPoint();
        } catch (hresult_error const&) {
        }
        places.push_back(place);
    }
    const std::wstring install = platform::installDirectory();
    const int at = placement::volumeHolding(places, install, placement::volumeOf(install));
    std::string line;
    if (at >= 0) {
        bool system = false;
        try {
            system = volumes[at].IsSystemVolume();
        } catch (hresult_error const&) {
        }
        line = "app package: packages are installed on Megler's own volume, " + narrow(places[at].storePath) + " (mounted at " +
               narrow(places[at].mountPoint) + ", " + narrow(places[at].name) +
               (system ? ", the system's)" : ", not the system's: a memory card or another drive)");
    } else {
        std::string listed;
        for (const auto& place : places) listed += (listed.empty() ? "" : ", ") + narrow(place.storePath);
        line = volumes.empty() ? "app package: Windows did not list its package volumes (" + why + "): packages go where Windows puts new apps"
                               : "app package: none of the package volumes (" + listed + ") holds Megler's install folder " + narrow(install) +
                                     ": packages go where Windows puts new apps";
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (line != said) {
            said = line;
            shellLog::write(line);
        }
    }
    return at >= 0 ? volumes[at] : nullptr;
}

/*
 * AddPackageAsync onto `volume` (installVolume), or as before where there is none. Where Windows has not that call or will not
 * take a volume with it (E_NOINTERFACE, E_NOTIMPL or E_INVALIDARG thrown), as before too, said once; whatever else it throws
 * or answers is the caller's, as it was.
 */
winrt::Windows::Management::Deployment::DeploymentResult addPackage(winrt::Windows::Management::Deployment::PackageManager const& manager,
                                                                    winrt::Windows::Foundation::Uri const& package,
                                                                    winrt::Windows::Management::Deployment::DeploymentOptions options,
                                                                    winrt::Windows::Management::Deployment::PackageVolume const& volume) {
    if (volume) {
        try {
            return manager.AddPackageAsync(package, nullptr, options, volume).get();
        } catch (hresult_error const& e) {
            const int32_t code = e.code();
            if (code != E_NOINTERFACE && code != E_NOTIMPL && code != E_INVALIDARG) throw;
            static std::atomic<bool> said{false};
            if (!said.exchange(true))
                shellLog::write(format("app package: Windows would not take a package volume with an install (0x%08x): packages go where "
                                       "Windows puts new apps",
                                       unsigned(code)));
        }
    }
    return manager.AddPackageAsync(package, nullptr, options).get();
}

}  // namespace

std::string describeCapabilities(int capabilities) {
    if (capabilities < 0) return "every capability of Megler's";
    static const struct {
        int cap;
        const char* what;
    } names[] = {{MEGLER_APPX_CAP_NETWORK, "the network"},          {MEGLER_APPX_CAP_PICTURES, "the pictures library"},
                 {MEGLER_APPX_CAP_VIDEOS, "the videos library"},     {MEGLER_APPX_CAP_MUSIC, "the music library"},
                 {MEGLER_APPX_CAP_WEBCAM, "the camera"},             {MEGLER_APPX_CAP_MICROPHONE, "the microphone"},
                 {MEGLER_APPX_CAP_LOCATION, "the location"},         {MEGLER_APPX_CAP_CHAT, "text messages"},
                 {MEGLER_APPX_CAP_CALL_HISTORY, "the call history"}};
    std::string out;
    for (const auto& n : names)
        if (capabilities & n.cap) out += (out.empty() ? "" : ", ") + std::string(n.what);
    return out.empty() ? "none of the phone's" : out;
}

namespace {

/// The logos of a package (Megler's are Assets\*.png, drawn by Assets\make-assets.ps1): each file, its size, and how much of
/// its shorter side the icon takes. The same names and sizes as Megler's, which resources.pri's template lists.
struct Logo {
    const char* name;
    int width, height;
    double iconFraction;
};
constexpr Logo kLogos[] = {
    {"Assets/StoreLogo.png", 50, 50, 0.92},
    {"Assets/Square44x44Logo.png", 44, 44, 0.92},
    {"Assets/Square150x150Logo.png", 150, 150, 0.66},
    {"Assets/Wide310x150Logo.png", 310, 150, 0.66},
    {"Assets/SplashScreen.png", 620, 300, 0.6},
};

/// An icon of `width` x `height` fitted in a square of `side`, its proportions kept.
void fit(uint32_t width, uint32_t height, int side, int& w, int& h) {
    const double scale = double(side) / double(std::max(width, height));
    w = std::max(1, int(width * scale + 0.5));
    h = std::max(1, int(height * scale + 0.5));
}

/// The app's icon (megler_apk_icon's bytes: PNG, or WebP, which WIC on the phone has no codec for and the core decodes)
/// drawn in the middle of each of a package's logos, on nothing: the tile's colour shows round it. Each as a PNG, named as
/// in the package; none when the icon is no picture this reads.
std::vector<std::pair<std::string, std::string>> drawLogos(const std::string& icon) {
    using namespace winrt::Windows::Graphics::Imaging;
    using namespace winrt::Windows::Storage::Streams;
    std::vector<std::pair<std::string, std::string>> out;
    const auto* bytes = reinterpret_cast<const uint8_t*>(icon.data());
    try {
        int webpWidth = 0, webpHeight = 0;
        const bool webp = megler_webp_size(bytes, icon.size(), &webpWidth, &webpHeight) && webpWidth > 0 && webpHeight > 0;
        BitmapDecoder decoder{nullptr};
        if (!webp) {
            InMemoryRandomAccessStream stream;
            DataWriter writer(stream);
            writer.WriteBytes(array_view<const uint8_t>(bytes, bytes + icon.size()));
            writer.StoreAsync().get();
            writer.DetachStream();
            stream.Seek(0);
            decoder = BitmapDecoder::CreateAsync(stream).get();
        }
        const uint32_t iconWidth = webp ? uint32_t(webpWidth) : decoder.PixelWidth(), iconHeight = webp ? uint32_t(webpHeight) : decoder.PixelHeight();
        for (const Logo& logo : kLogos) {
            // The icon at its size, B, G, R, A premultiplied.
            int w = 0, h = 0;
            fit(iconWidth, iconHeight, int(std::min(logo.width, logo.height) * logo.iconFraction + 0.5), w, h);
            std::vector<uint8_t> pixels(size_t(w) * h * 4);
            if (webp) {
                if (!megler_webp_decode(bytes, icon.size(), pixels.data(), w, h, w * 4)) return {};
            } else {
                BitmapTransform transform;
                transform.ScaledWidth(uint32_t(w));
                transform.ScaledHeight(uint32_t(h));
                transform.InterpolationMode(BitmapInterpolationMode::Fant);
                const auto data = decoder.GetPixelDataAsync(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Premultiplied, transform,
                                                            ExifOrientationMode::IgnoreExifOrientation, ColorManagementMode::DoNotColorManage)
                                      .get()
                                      .DetachPixelData();
                if (data.size() < pixels.size()) return {};
                memcpy(pixels.data(), data.data(), pixels.size());
            }
            // In the middle of the logo, the rest transparent; and its alpha made straight again, as a PNG has it.
            std::vector<uint8_t> canvas(size_t(logo.width) * logo.height * 4, 0);
            const int x0 = (logo.width - w) / 2, y0 = (logo.height - h) / 2;
            for (int y = 0; y < h; y++) {
                const uint8_t* from = &pixels[size_t(y) * w * 4];
                uint8_t* to = &canvas[(size_t(y0 + y) * logo.width + x0) * 4];
                for (int x = 0; x < w; x++, from += 4, to += 4) {
                    const unsigned a = from[3];
                    for (int c = 0; c < 3; c++) to[c] = a ? uint8_t(std::min(255u, (from[c] * 255u + a / 2) / a)) : 0;
                    to[3] = uint8_t(a);
                }
            }
            InMemoryRandomAccessStream png;
            auto encoder = BitmapEncoder::CreateAsync(BitmapEncoder::PngEncoderId(), png).get();
            encoder.SetPixelData(BitmapPixelFormat::Bgra8, BitmapAlphaMode::Straight, uint32_t(logo.width), uint32_t(logo.height), 96.0,
                                 96.0, array_view<const uint8_t>(canvas.data(), canvas.data() + canvas.size()));
            encoder.FlushAsync().get();
            std::string file(size_t(png.Size()), '\0');
            DataReader reader(png.GetInputStreamAt(0));
            reader.LoadAsync(uint32_t(file.size())).get();
            reader.ReadBytes(array_view<uint8_t>(reinterpret_cast<uint8_t*>(file.data()), reinterpret_cast<uint8_t*>(file.data()) + file.size()));
            out.emplace_back(logo.name, std::move(file));
        }
    } catch (hresult_error const& e) {
        shellLog::write(format("app package: the app's icon could not be drawn on its tiles: 0x%08x ", unsigned(e.code())) + narrow(e.message()));
        return {};
    }
    return out;
}

}  // namespace

std::wstring sharedFolder() {
    static const std::wstring path = [] {
        try {
            return std::wstring(ApplicationData::Current().GetPublisherCacheFolder(L"shared").Path());
        } catch (...) {
            return std::wstring();
        }
    }();
    return path;
}

const SingleApp& single() {
    static const SingleApp app = [] {
        SingleApp s;
        std::string text;
        const std::wstring install = platform::installDirectory();
        if (!files::read(install + L"\\MeglerApp.ini", text)) return s;
        s.package = iniValue(text, "package");
        s.label = iniValue(text, "label");
        const std::string apk = iniValue(text, "apk"), root = iniValue(text, "root");
        const std::wstring shared = sharedFolder();
        // Since 645 the app's APK is in the shared folder (make: apk=shared:apks\<identity>\<token>); before, in the package.
        // Where Windows gave the package no shared folder (one on a memory card may have none), neither is anywhere: both left
        // empty, and nothing is looked for at the drive's root ("" + "\apks\...").
        if (apk.rfind("shared:", 0) == 0)
            s.apkFolder = shared.empty() ? std::wstring() : shared + L"\\" + widen(apk.substr(7));
        else
            s.apkFolder = install + L"\\" + widen(apk);
        if (!shared.empty() && !root.empty()) s.androidRoot = shared + L"\\" + widen(root);
        s.megler = widen(iniValue(text, "megler"));
        shellLog::write("app package: this is " + s.package + "'s own package; " +
                        (shared.empty() ? std::string("Windows gave it no shared folder, where its APK and Android's files are")
                                        : "Android's files in " + narrow(s.androidRoot) + ", its APK in " + narrow(s.apkFolder)));
        return s;
    }();
    return app;
}

const std::wstring& linkScheme() {
    static const std::wstring scheme = [] {
        std::string manifest;
        const std::string tag = "<uap:Protocol Name=\"";
        if (files::read(platform::installDirectory() + L"\\AppxManifest.xml", manifest)) {
            const size_t at = manifest.find(tag), end = at == std::string::npos ? at : manifest.find('"', at + tag.size());
            if (end != std::string::npos && end > at + tag.size()) return widen(manifest.substr(at + tag.size(), end - at - tag.size()));
        }
        return std::wstring(L"megler");
    }();
    return scheme;
}

std::wstring meglerFamilyName() {
    const auto id = Package::Current().Id();
    if (!isSingleApp()) return std::wstring(id.FamilyName());
    // (A test identity's app was handed to megler: as Megler_<publisher id>, which was not there: Windows asked which of the
    // release's Megler and its apps should open it, and the one picked looked for the APK in its own shared folder.)
    return !single().megler.empty() ? single().megler : L"Megler_" + std::wstring(id.PublisherId());
}

void takeHandover() {
    const SingleApp& app = single();
    const std::wstring shared = sharedFolder();
    if (app.package.empty() || shared.empty()) return;
    const std::wstring package = widen(app.package), handover = shared + L"\\handover\\" + package;
    if (!files::exists(handover + L"\\complete")) return;
    // The app's private files where the core keeps them (core/src/run/mg_run.c: <data>\apps\<package>\data); its own
    // until now put aside, and put back if the copy fails.
    const std::wstring own = platform::localStateDirectory() + L"\\apps\\" + package + L"\\data", aside = own + L".before-megler";
    const bool hadOwn = files::exists(own);
    if (hadOwn) {
        files::removeTree(aside);
        if (!files::move(own, aside)) {
            shellLog::write(format("app package: the app's own private files could not be put aside (error %lu): Megler's not taken", GetLastError()));
            return;
        }
    }
    uint64_t copied = 0;
    if (!copyTree(handover + L"\\data", own, copied)) {
        shellLog::write(format("app package: the private files Megler handed over could not be copied (error %lu): taken at the next start", GetLastError()));
        files::removeTree(own);
        if (hadOwn) files::move(aside, own);
        return;
    }
    // "complete" first: once it is gone, nothing is taken again, whatever of the rest stays.
    files::remove(handover + L"\\complete");
    files::removeTree(handover);
    shellLog::write("app package: the private files Megler handed over taken, " + formattedByteCount(copied) +
                    (hadOwn ? "; the app's own are in " + narrow(aside) : std::string()));
}

namespace {

/// The name of this build's folder of Android's files in the shared folder (android\<name>): its version, and on any Windows
/// but a phone's the ABI of its native code too. The shared folder is the publisher's, which an x86 and an x64 Megler of one
/// version both have on one PC: an x64 645's apps found the x86 one's files there ("libnativehelper.so: not a shared object
/// of x86_64"). A phone's (32-bit ARM) keeps the name it always had.
std::string androidRootName() {
#if defined(_M_ARM64)
    return versionString() + "-arm64-v8a";
#elif defined(_M_X64)
    return versionString() + "-x86_64";
#elif defined(_M_IX86)
    return versionString() + "-x86";
#else
    return versionString();
#endif
}

/// The folder of the shared folder `entry`'s files are kept in for its package (make), in `folder`: the entry's own when it is
/// there already (an app whose package was made before: nothing is copied), else a new one, apks\<identity>\<token>, its APK
/// and splits copied into it (`picked`: read where it was picked), byte for byte (files::mend), and put in place whole; then
/// its report, its icon and the build its report is of, as the library has them now. Empty, or why not.
std::string keepInShared(const ApkEntry& entry, const std::string& identity, const std::vector<std::wstring>& splits,
                         const PickedFile* picked, const std::function<void(const std::string&)>& progress, std::wstring& folder) {
    if (sharedFolder().empty()) return "Windows gives this Megler no shared folder to keep the APK in.";
    const std::wstring apks = sharedFolder() + L"\\apks\\" + widen(identity);
    if (entry.path.size() > apks.size() + 1 && _wcsnicmp(entry.path.c_str(), (apks + L"\\").c_str(), apks.size() + 1) == 0 &&
        files::exists(entry.path)) {
        folder = entry.path.substr(0, entry.path.find_last_of(L'\\'));
    } else {
        progress("Copying the APK to the shared folder…");
        FILETIME time;
        GetSystemTimeAsFileTime(&time);
        folder = apks + L"\\" + widen(format("%08lx%08lx-%08lx", time.dwHighDateTime, time.dwLowDateTime, GetCurrentProcessId()));
        const std::wstring part = folder + L".part";
        files::removeTree(part);
        if (!files::makeDirectories(part)) return format("The APK's folder in the shared folder could not be made (error %lu).", GetLastError());
        uint64_t mended = 0;
        const auto put = [&mended](const std::wstring& from, const std::wstring& to) {
            uint64_t more = 0;
            const bool whole = files::copyWhole(from, to, more);
            mended += more;
            return whole;
        };
        // The library's copy of an APK Megler's package brings (bitgapps) is not copied on: the package's own is. A Lumia 650's
        // copy of Google Play Store, made before copies were compared with what they were made from, had bytes of its own,
        // and its app's APK in the shared folder took them along byte for byte (shell/BundledApks.h).
        const std::wstring bundled = picked ? std::wstring() : bundledApks::originalOf(entry.path);
        std::string error;
        if (picked)
            error = picked->copyTo(part + L"\\app.apk");
        else if (!put(bundled.empty() ? entry.path : bundled, part + L"\\app.apk"))
            error = format("The APK could not be copied to the shared folder (error %lu).", GetLastError());
        if (error.empty() && !splits.empty() && !files::makeDirectory(part + L"\\app.apk.splits"))
            error = format("The folder of the split APKs in the shared folder could not be made (error %lu).", GetLastError());
        for (size_t i = 0; error.empty() && i < splits.size(); i++)
            if (!put(entry.splitsDir + L"\\" + splits[i], part + L"\\app.apk.splits\\" + splits[i]))
                error = format("The split APK %s could not be copied to the shared folder (error %lu).", narrow(splits[i]).c_str(), GetLastError());
        if (error.empty() && !files::move(part, folder))
            error = format("The APK's folder in the shared folder could not be put in place (error %lu).", GetLastError());
        if (!error.empty()) {
            files::removeTree(part);
            return error;
        }
        if (mended)
            shellLog::write(format("app package: %llu bytes of the APK's copy in the shared folder differed after the copy, and were written again",
                                   (unsigned long long)mended));
        shellLog::write("app package: the APK copied to " + narrow(folder) + ", " + formattedByteCount(entry.size) +
                        (bundled.empty() ? std::string() : ", from Megler's package's " + narrow(bundled.substr(bundled.find_last_of(L'\\') + 1))));
    }
    // Small: each written beside and moved into place.
    const std::wstring cached = Library::instance().directory() + L"\\" + widen(entry.id);
    const std::wstring icon = !entry.iconPath.empty() && files::exists(entry.iconPath) ? entry.iconPath : entry.path + L".icon";
    const std::pair<std::wstring, const wchar_t*> besides[] = {{files::exists(cached + L".json") ? cached + L".json" : entry.path + L".json", L"app.apk.json"},
                                                             {icon, L"app.apk.icon"},
                                                             {Library::instance().directory() + L"\\reports.version", L"reports.version"}};
    for (const auto& [from, name] : besides) {
        const std::wstring to = folder + L"\\" + name;
        if (_wcsicmp(from.c_str(), to.c_str()) == 0 || !files::exists(from)) continue;
        if (!files::copy(from, to + L".part") || !files::move(to + L".part", to))
            shellLog::write(format("app package: %s could not be put beside the APK in the shared folder (error %lu)", narrow(name).c_str(), GetLastError()));
    }
    return std::string();
}

}  // namespace

Made make(const ApkEntry& entry, const std::function<void(const std::string&)>& progress, const PickedFile* picked) {
    Made made;
    auto fail = [&made](std::string why) {
        made.error = std::move(why);
        shellLog::write("app package: " + made.error);
        return made;
    };
    const ApkReport& r = entry.report;
    if (!entry.analysed || !r.valid || r.package.empty()) return fail("The APK hasn't been read.");
    // An app in several APKs (an .apks or .xapk unpacked) has its splits beside the base, in app\app.apk.splits, where the
    // library looks for them (Library::splitsDirFor). Their names are the unpacking's (core/src/apk/mg_bundle.c).
    std::vector<std::wstring> splits;
    if (!entry.splitsDir.empty()) {
        splits = files::list(entry.splitsDir, L"*");
        for (const auto& name : splits)
            if (!plainName(narrow(name))) return fail("A split APK's name can't go in a package as it is: " + narrow(name));
        if (splits.empty()) return fail("The app's split APKs are missing (" + narrow(entry.splitsDir) + ").");
    }
    const std::wstring shared = sharedFolder();
    // (Its manifest declares the publisher cache folder: Windows gives none where Megler is installed on a memory card.)
    if (shared.empty())
        return fail(platform::isMobile()
                        ? "Windows gives this Megler no shared folder, where an app's own package finds Android's files and its APK: as a rule "
                          "because Megler is installed on the memory card. Move Megler to the phone's memory (Settings › System › Storage), "
                          "then install the app."
                        : "Windows gives this Megler no shared folder, where an app's own package finds Android's files and its APK: as a rule "
                          "because Megler is installed on another drive than Windows's. Install Megler on the system drive, then install the app.");
    const std::wstring install = platform::installDirectory(), local = platform::localStateDirectory();
    const std::string version = versionString();

    // Android's libcore and framework, once for each build: every app's package of this build reads them there.
    const std::wstring root = shared + L"\\android\\" + widen(androidRootName());
    if (!files::exists(root + L"\\complete")) {
        if (!files::exists(install + L"\\android")) return fail("This Megler carries no Android libcore and framework (built without --libcore).");
        progress("Copying Android's files to the shared folder (once for this build of Megler)…");
        files::removeTree(root);
        uint64_t copied = 0;
        files::makeDirectory(shared + L"\\android");
        if (!copyTree(install + L"\\android", root, copied))
            return fail(format("Android's files could not be copied to the shared folder (error %lu).", GetLastError()));
        files::write(root + L"\\complete", version);
        shellLog::write("app package: Android's files copied to " + narrow(root) + ", " + formattedByteCount(copied));
    }

    // The app's data in Megler, once for each app: the first package made of it takes it along, so that the app goes on
    // where it was. Its part of Android's storage (Android/data, Android/obb, whatever else it has there) is copied to
    // the shared folder's, which is the separate apps' storage; its private files (/data/data) to handover\<package> in
    // the shared folder, which the separate app takes into its own LocalState at its next start (takeHandover). What the
    // separate app had in Android/data is kept aside, not removed. Megler keeps its own. Later packages leave the separate
    // app's data alone, but for the expansion files (an .xapk's OBB), copied each time where they are missing. Once Megler's
    // own storage is the shared folder's (meglersStorageShared), its part of it is the app's already: only the private files go.
    const bool sameStorage = meglersStorageShared();
    const std::wstring package = widen(r.package);
    const std::wstring inMegler = local + L"\\apps\\" + package, inShared = shared + L"\\apps\\" + package;
    const std::wstring handedOver = inShared + L"\\megler-data.txt", handover = shared + L"\\handover\\" + package;
    const bool handOver = !files::exists(handedOver);
    if (handOver) {
        const session::Snapshot running = session::snapshot();
        if (running.apkId == entry.id && (running.state == session::State::starting || running.state == session::State::running))
            return fail("The app is running in Megler, and its data can't be copied while it writes it. Quit it first.");
        uint64_t copied = 0;
        if (!sameStorage && files::exists(inMegler + L"\\sdcard")) {
            progress("Copying the app's files in Android's storage to the shared storage…");
            const std::wstring data = inShared + L"\\sdcard\\Android\\data\\" + package, aside = inShared + L"\\Android-data.before-megler";
            // (A make that failed after this has left a copy of Megler's there and the separate app's own aside: that stays.)
            if (files::exists(data) && !files::exists(aside) && !files::move(data, aside))
                return fail(format("The separate app's own Android/data could not be put aside (error %lu).", GetLastError()));
            files::removeTree(data);
            if (!copyTree(inMegler + L"\\sdcard", inShared + L"\\sdcard", copied, true))
                return fail(format("The app's files could not be copied to the shared storage (error %lu).", GetLastError()));
        }
        if (files::exists(inMegler + L"\\data")) {
            progress("Copying the app's private files for the separate app…");
            files::removeTree(handover);
            if (!copyTree(inMegler + L"\\data", handover + L"\\data", copied) || !files::write(handover + L"\\complete", version))
                return fail(format("The app's private files could not be copied to the shared folder (error %lu).", GetLastError()));
        }
        shellLog::write("app package: the app's data in Megler copied for the separate app, " + formattedByteCount(copied));
    } else if (!sameStorage) {
        const std::wstring obb = L"\\sdcard\\Android\\obb\\" + package;
        if (files::exists(inMegler + obb)) {
            progress("Copying the app's expansion files to the shared storage…");
            uint64_t copied = 0;
            if (!copyTree(inMegler + obb, inShared + obb, copied, true))
                return fail(format("The app's expansion files could not be copied to the shared storage (error %lu).", GetLastError()));
            shellLog::write("app package: the expansion files in " + narrow(inShared + obb) + ", " + formattedByteCount(copied) + " copied");
        }
    }

    progress("Signing key…");
    const std::wstring signing = local + L"\\signing";
    files::makeDirectory(signing);
    char err[400] = {};
    const std::string publisher = narrow(Package::Current().Id().Publisher());
    megler_appx_key* key = megler_appx_key_open(narrow(signing + L"\\key.bin").c_str(), narrow(signing + L"\\MeglerApps.cer").c_str(),
                                            publisher.c_str(), err, sizeof(err));
    if (!key) return fail(std::string("The signing key could not be made or read: ") + err);
    struct KeyCloser {
        megler_appx_key* key;
        ~KeyCloser() { megler_appx_key_close(key); }
    } closer{key};

    // The app's icon on its tiles and splash screen, the tiles in the phone's own colour round it; or, where the APK has no
    // icon that can be drawn, Megler's logos as they are.
    progress("Drawing the app's icon on its tiles…");
    std::string icon;
    std::vector<std::pair<std::string, std::string>> logos;
    if (!entry.iconPath.empty() && files::read(entry.iconPath, icon)) logos = drawLogos(icon);
    if (logos.empty()) {
        // No icon kept for it (the library's cache gone, or never written for an APK installed where it was picked): the
        // icon looked for in the APK itself, which is being packaged and so can be read. A reinstalled Telegram got
        // Megler's logos this way.
        uint8_t* bytes = nullptr;
        size_t n = 0;
        const std::string splitsDir = entry.splitsDir.empty() ? std::string() : narrow(entry.splitsDir);
        if (megler_apk_icon_splits(narrow(entry.path).c_str(), splitsDir.empty() ? nullptr : splitsDir.c_str(), &bytes, &n) && bytes && n > 0) {
            icon.assign(reinterpret_cast<const char*>(bytes), n);
            logos = drawLogos(icon);
        }
        if (bytes) megler_free(bytes);
    }
    shellLog::write(logos.empty() ? std::string("app package: Megler's own logos, the app's icon is not drawn")
                                  : "app package: the app's icon drawn on " + std::to_string(logos.size()) + " logos");

    std::string meglerManifest;
    if (!files::read(install + L"\\AppxManifest.xml", meglerManifest)) return fail("Megler's own manifest could not be read.");
    const std::string label = oneLine(entry.title());
    // What the package declares to Windows: what the app's permissions need and Megler's own, not everything Megler declares
    // (megler_appx_capabilities). A report an older core made says nothing of the permissions: Megler's, as before.
    const int capabilities = r.capabilities();
    shellLog::write("app package: " + r.package + " declares " + describeCapabilities(capabilities) + " to Windows, from " +
                    (r.permissionsKnown ? std::to_string(r.permissions.size()) + " permissions of its manifest" : std::string("a report that names no permissions")));
    char* manifest = megler_appx_app_manifest(meglerManifest.c_str(), r.package.c_str(), label.c_str(), logos.empty() ? nullptr : "transparent",
                                            nullptr, capabilities, err, sizeof(err));
    if (!manifest) return fail(std::string("The manifest could not be made: ") + err);
    const std::string manifestText(manifest);
    megler_free(manifest);
    char identity[64] = {};
    megler_appx_app_identity(r.package.c_str(), identity, sizeof(identity));
    made.identity = identity;
    {
        // Its full name as Megler's is made: "<name>_<version>_<processor>__<publisher id>", the version Megler's.
        const auto id = Package::Current().Id();
        const std::string meglerFull = narrow(std::wstring(id.FullName())), meglerName = narrow(std::wstring(id.Name()));
        made.fullName = identity + std::string("_") + version + meglerFull.substr(std::min(meglerFull.size(), meglerName.size() + 1 + version.size()));
    }

    // resources.pri: XAML wants one whose resource map has the package's name. Megler carries one made for a placeholder
    // as long as every app package's name, and the name goes in its place (core/src/appx/mg_appx_pri.c).
    std::string priTemplate, priFacts;
    if (!files::read(install + L"\\appx-template\\resources.pri", priTemplate) || !files::read(install + L"\\appx-template\\resources.txt", priFacts))
        return fail("This Megler carries no template of an app package's resources.pri (appx-template).");
    std::string pri(priTemplate.size(), '\0');
    if (!megler_appx_pri_rename(reinterpret_cast<const uint8_t*>(priTemplate.data()), priTemplate.size(), iniValue(priFacts, "placeholder").c_str(),
                              uint32_t(strtoul(iniValue(priFacts, "tail1").c_str(), nullptr, 10)),
                              uint32_t(strtoul(iniValue(priFacts, "tail2").c_str(), nullptr, 10)), identity,
                              reinterpret_cast<uint8_t*>(pri.data()), err, sizeof(err)))
        return fail(std::string("The package's resources.pri could not be made: ") + err);

    const std::wstring outDir = local + L"\\packages";
    files::makeDirectory(outDir);
    made.appx = outDir + L"\\" + widen(identity) + L".appx";
    made.certificate = outDir + L"\\MeglerApps.cer";
    files::copy(signing + L"\\MeglerApps.cer", made.certificate);

    std::vector<std::pair<std::string, std::wstring>> parts;
    std::vector<std::string> skipped;
    collect(install, L"", parts, skipped);
    for (const auto& s : skipped) shellLog::write("app package: left out, its name would need escaping: " + s);

    // The app's APK, its splits, its report and its icon in the shared folder, where the package's app reads them
    // (MeglerApp.ini: apk=shared:...) and Megler does too. In the package itself, as before 645, Megler could not read them
    // once Windows installed it as the Store installs an app: such a package shows another package none of its folders and
    // refuses it its files (error 5 on a Lumia 650, 644), and Megler kept a second copy of each such app's APK. One copy
    // now, which the app's next packages take as it is: Update All packs Megler's own files alone.
    std::wstring folder;
    if (const std::string error = keepInShared(entry, identity, splits, picked, progress, folder); !error.empty()) return fail(error);
    made.apkFolder = folder.substr(shared.size() + 1);

    progress(format("Packing %u files of Megler…", unsigned(parts.size())));
    megler_appx* a = megler_appx_begin(narrow(made.appx).c_str(), err, sizeof(err));
    if (!a) return fail(std::string("The package could not be begun: ") + err);
    // (A failure on the way makes the rest fail at once; finish says what it was.)
    for (const auto& [name, path] : parts) {
        bool drawn = false;
        for (const auto& logo : logos) drawn = drawn || _stricmp(logo.first.c_str(), name.c_str()) == 0;
        if (!drawn) megler_appx_add_file(a, name.c_str(), narrow(path).c_str());
    }
    for (const auto& [name, png] : logos) megler_appx_add_data(a, name.c_str(), png.data(), png.size());
    megler_appx_add_data(a, "AppxManifest.xml", manifestText.data(), manifestText.size());
    megler_appx_add_data(a, "resources.pri", pri.data(), pri.size());
    // (megler=: the Megler the app hands APKs to and starts others through, by its package family name -- a test identity's is
    // not the release's.)
    const std::string ini = "package=" + r.package + "\r\nlabel=" + label + "\r\napk=shared:" + narrow(made.apkFolder) + "\r\nroot=android\\" +
                            androidRootName() + "\r\nmegler=" + narrow(std::wstring(Package::Current().Id().FamilyName())) + "\r\n";
    megler_appx_add_data(a, "MeglerApp.ini", ini.data(), ini.size());
    progress("Signing the package…");
    if (!megler_appx_finish(a, key, err, sizeof(err))) return fail(std::string("The package could not be made: ") + err);
    shellLog::write("app package: " + narrow(made.appx) + " made, " + formattedByteCount(files::size(made.appx)) + ", for " + r.package);
    // The data handed over once: said only now, so that a make that failed hands it over again.
    if (handOver && (!files::makeDirectories(inShared) || !files::write(handedOver, "Megler " + version + " handed over " + r.package + "'s data in Megler\r\n")))
        shellLog::write("app package: " + narrow(handedOver) + " could not be written: the next package of the app hands its data over again");
    return made;
}

/*
 * Windows's deployment engine installs a package as the Store installs an app (ForceApplicationShutdown, which closes the
 * app the package updates -- Megler itself, for its own update -- or None), or, on Windows 10 Mobile, as a developer's
 * package (the option 0x8000, its MinDeploy's packaged developer deployment; the public DevelopmentMode, 2, is for a loose
 * manifest and refused for a package). A phone takes only so many developer's packages and refuses one more (0x80073d11,
 * ERROR_DEV_SIDELOAD_LIMIT_EXCEEDED: a Megler 575 could not install Aurora Store through the Device Portal, which installs
 * only so; a Lumia 650 had 20 of them, Megler's and its four apps' among them, each installed so by 612 to 639). The
 * Store's way is not a developer's deployment: it goes first, as WinArchive's developer, whose way of installing this is,
 * advised, and a developer's deployment only where it is refused. Each set of options is tried while the one before was
 * refused at once; a deployment that has begun is waited for.
 *
 * This is the only way Megler installs a package (since 0.1.0.612 its own update, since 0.1.0.614 the apps' packages). The
 * phone's Device Portal, which Megler sent packages to before and then where the engine refused, is asked nothing any more
 * (2026-11-07, the owner's wish): it installed only as a developer's deployment, which the engine's last tries are too, and
 * it took the package through Megler's memory, which a phone of 1 GB could not give a package of Megler's own size. What it
 * did besides -- installing the certificate an app's package is signed with, the first time -- is the user's to do once
 * where Windows does not trust it (untrustedCertificate: the app's page offers the certificate to Windows's own installer).
 *
 * Every try refused at once for access (E_ACCESSDENIED, thrown by the call or in its result) is Windows withholding the
 * restricted capability packageManagement, which Windows 10 Mobile grants a sideloaded app only with the capability part of
 * the phone's interop unlock (Interop Tools, Unlock: Interop/Cap Unlock and New Capability Engine Unlock, then a restart). A
 * user's phone with the basic unlock alone -- codeGeneration granted, Android code running -- refused all four tries so, and
 * the app's page said no more than "0x80070005 Access is denied." four times (Megler 0.1.0.671): the result says it apart
 * (InstallResult::accessDenied), and the shell says what to do (accessDeniedText).
 */
namespace {

/// One deployment at a time, whoever asks (an app's install, Update All's, Megler's own update).
std::mutex gDeployMutex;
/// Whether Megler's packages were seen installed on this phone when Windows refused an install for access: Megler has had
/// its packageManagement capability then, and the refusal is not the interop unlock's (accessDeniedText).
std::atomic<bool> gInstalledBefore{false};

/// A PackageManager made from an activation factory of its own, past C++/WinRT's cache of them; null where it cannot be.
winrt::Windows::Management::Deployment::PackageManager freshPackageManager() {
    try {
        winrt::com_ptr<::IActivationFactory> factory;
        const winrt::hstring name = L"Windows.Management.Deployment.PackageManager";
        if (FAILED(RoGetActivationFactory(static_cast<HSTRING>(winrt::get_abi(name)), __uuidof(::IActivationFactory), factory.put_void())))
            return nullptr;
        winrt::com_ptr<::IInspectable> made;
        if (FAILED(factory->ActivateInstance(made.put()))) return nullptr;
        return made.as<winrt::Windows::Management::Deployment::PackageManager>();
    } catch (hresult_error const&) {
        return nullptr;
    }
}

/// Said once a run: why Windows refuses Megler every install, which each try's own line does not say.
void sayRefusedForAccess() {
    static std::atomic<bool> said{false};
    if (said.exchange(true)) return;
    if (gInstalledBefore) {
        shellLog::write("app package: Windows refuses Megler every install for access (0x80070005 at every try), though Megler's packages "
                        "are installed on this phone: Windows has granted it packageManagement before, and the refusal is this run's "
                        "(it came after Megler was suspended and resumed, on a Lumia 950 whose interop unlock was whole); a new start "
                        "of Megler installs again");
        return;
    }
    shellLog::write(platform::isMobile()
                        ? "app package: Windows refuses Megler every install for access (0x80070005 at every try): Megler has not got its "
                          "packageManagement capability, which Windows 10 Mobile grants a sideloaded app only with the capability part of the "
                          "phone's interop unlock (Interop Tools, Unlock: Interop/Cap Unlock and New Capability Engine Unlock, then a "
                          "restart); nothing is installed until then"
                        : "app package: Windows refuses Megler every install for access (0x80070005 at every try): it has refused Megler its "
                          "packageManagement capability");
}

/// The last refusal written down, LocalState\install-refusal.txt, in place of the one before: Save All Logs takes it along
/// and its summary says it (shell/LogArchive.h), whatever run it was in.
void rememberRefusal(const std::wstring& path, const InstallResult& result) {
    const std::string text = localDateTime() + ", Megler " + versionString() + ": " + narrow(path.substr(path.find_last_of(L'\\') + 1)) +
                             " was not installed. " + result.said + "\r\n" +
                             (result.accessDenied ? "Every try was refused for access: Megler has not got its packageManagement capability.\r\n"
                                                  : std::string());
    files::write(platform::localStateDirectory() + L"\\install-refusal.txt", text);
}

}  // namespace

std::string accessDeniedText(const std::string& meanwhile) {
    // Megler has installed packages here before: Windows has granted it the capability, and this run's refusal goes with a new
    // start (a Lumia 950's installs were refused after Megler was suspended and resumed, its interop unlock whole).
    if (gInstalledBefore)
        return "Windows refused Megler this install (0x80070005, access denied), though Megler has installed apps on this phone "
               "before. Close Megler fully (in the task view, swipe it away) and open it again: a new start installs. If every start "
               "is refused, check the phone's interop unlock in Interop Tools \u203a Unlock (Interop/Cap Unlock and New Capability "
               "Engine Unlock, then a restart). Until then, " +
               meanwhile;
    if (platform::isMobile())
        return "Windows does not let Megler install apps on this phone (0x80070005, access denied): Megler's right to install packages "
               "is blocked. The phone's interop unlock is not complete: in Interop Tools › Unlock, turn on Interop/Cap Unlock and New "
               "Capability Engine Unlock, then restart the phone. Until then, " +
               meanwhile;
    // (Desktop Windows grants a sideloaded package its restricted capabilities as a rule: no unlock to name, only the refusal.)
    return "Windows does not let Megler install apps here (0x80070005, access denied): it refused Megler the packageManagement "
           "capability, the right to install packages. Instead, " +
           meanwhile;
}

InstallResult installWithPackageManager(const std::wstring& path, const std::function<void(const std::string&)>& progress) {
    using namespace winrt::Windows::Management::Deployment;
    const DeploymentOptions mobileDevelopment = static_cast<DeploymentOptions>(0x8000);
    const DeploymentOptions tries[] = {DeploymentOptions::ForceApplicationShutdown, DeploymentOptions::None,
                                       mobileDevelopment | DeploymentOptions::ForceApplicationShutdown, mobileDevelopment};
    std::wstring uri = L"file:///" + path;
    std::replace(uri.begin(), uri.end(), L'\\', L'/');
    std::string said, asTheStore;
    bool denied = true;  // every try so far refused for access
    std::lock_guard<std::mutex> one(gDeployMutex);
    // (Onto the volume Megler itself is on, where there is one to name: installVolume, addPackage.)
    const PackageVolume volume = installVolume();
    bool first = true;
    for (const DeploymentOptions options : tries) {
        int32_t code = 0;
        /*
         * Each set of options asked of C++/WinRT's PackageManager, and where that is refused for access, of one made from an
         * activation factory of its own, at once and (the first set) again after 3 s: on a Lumia 650 resumed from suspension
         * the first was refused (0x80070005) where the second, asked a moment later, took the call (the flag diag-deploy's
         * probe, 0.1.0.674), and a Lumia 950's installs were refused at every try after a resume, each through the first.
         */
        for (int way = 0; way < (first ? 3 : 2); way++) {
            try {
                if (way == 2) std::this_thread::sleep_for(std::chrono::seconds(3));
                PackageManager manager = way == 0 ? PackageManager() : freshPackageManager();
                if (!manager) {
                    shellLog::write("app package: no PackageManager from an activation factory of its own");
                    break;
                }
                progress(format("Installing it through Windows's deployment engine (options 0x%x)…", unsigned(options)));
                // (Said before it goes: an update of Megler's own package ends Megler as soon as it is installed.)
                shellLog::flush();
                const DeploymentResult result = addPackage(manager, winrt::Windows::Foundation::Uri(uri), options, volume);
                if (result.ErrorText().empty()) {
                    shellLog::write(format("app package: %s installed by Windows's deployment engine (options 0x%x%s)",
                                           narrow(path.substr(path.find_last_of(L'\\') + 1)).c_str(), unsigned(options),
                                           way ? ", asked of a PackageManager of a fresh activation factory after a refusal for access" : ""));
                    return {true, "Installed."};
                }
                code = result.ExtendedErrorCode();
                said = format("0x%08x ", unsigned(code)) + narrow(result.ErrorText());
                shellLog::write(format("app package: the deployment engine refused it (options 0x%x%s): ", unsigned(options), way ? ", fresh" : "") +
                                said);
            } catch (hresult_error const& e) {
                code = e.code();
                said = format("0x%08x ", unsigned(code)) + narrow(e.message());
                shellLog::write(format("app package: the deployment engine could not be asked (options 0x%x%s): ", unsigned(options), way ? ", fresh" : "") +
                                said);
            }
            if (code != E_ACCESSDENIED) break;
        }
        first = false;
        denied = denied && code == E_ACCESSDENIED;
        if (!(unsigned(options) & 0x8000) && asTheStore.empty()) asTheStore = said;
    }
    // Refused for access every time: whether Megler's packages are installed here all the same (Windows has granted it the
    // capability before, and the refusal is this run's), for what is said (accessDeniedText).
    if (denied && !gInstalledBefore) {
        std::vector<InstalledApp> apps;
        std::string error;
        if (installedApps(apps, error) && !apps.empty()) gInstalledBefore = true;
    }
    // What the Store's way was refused for first: a developer's deployment is refused for their number alone.
    const InstallResult refused{false, "Windows's deployment engine did not install it: as the Store installs, " + asTheStore +
                                           "; as a developer's package, " + said,
                                denied};
    if (denied) sayRefusedForAccess();
    rememberRefusal(path, refused);
    return refused;
}

namespace {

/// One way of asking the deployment engine, said: FindPackagesForUser's count, and AddPackageAsync's answer to a package
/// that is not there -- an error of its own where the engine takes Megler's call, 0x80070005 where it refuses Megler.
std::string probeOne(winrt::Windows::Management::Deployment::PackageManager const& manager) {
    using namespace winrt::Windows::Management::Deployment;
    std::string said;
    try {
        unsigned count = 0;
        for (auto const& p : manager.FindPackagesForUser(L"")) {
            (void)p;
            count++;
        }
        said += format("lists %u packages", count);
    } catch (hresult_error const& e) {
        said += format("list refused 0x%08x", unsigned(e.code()));
    }
    try {
        const std::wstring missing = L"file:///" + platform::localStateDirectory() + L"/no-such-package.appx";
        std::wstring uri = missing;
        std::replace(uri.begin(), uri.end(), L'\\', L'/');
        const DeploymentResult result = manager.AddPackageAsync(winrt::Windows::Foundation::Uri(uri), nullptr, DeploymentOptions::None).get();
        said += format("; a missing package: 0x%08x ", unsigned(result.ExtendedErrorCode())) + narrow(result.ErrorText()).substr(0, 80);
    } catch (hresult_error const& e) {
        said += format("; a missing package: thrown 0x%08x ", unsigned(e.code())) + narrow(e.message()).substr(0, 80);
    }
    return said;
}

}  // namespace

void probeDeployment(const char* when) {
    if (!files::exists(platform::localStateDirectory() + L"\\flags\\diag-deploy")) return;
    const std::string label = when;
    std::thread([label] {
        using namespace winrt::Windows::Management::Deployment;
        try {
            winrt::init_apartment();
        } catch (...) {
        }
        std::string line = "deploy probe (" + label + "): ";
        try {
            line += "C++/WinRT's: " + probeOne(PackageManager());
        } catch (hresult_error const& e) {
            line += format("C++/WinRT's: not made 0x%08x", unsigned(e.code()));
        }
        // A factory of its own, past C++/WinRT's cache of them.
        try {
            winrt::com_ptr<::IActivationFactory> factory;
            const winrt::hstring name = L"Windows.Management.Deployment.PackageManager";
            winrt::check_hresult(RoGetActivationFactory(static_cast<HSTRING>(winrt::get_abi(name)), __uuidof(::IActivationFactory), factory.put_void()));
            winrt::com_ptr<::IInspectable> made;
            winrt::check_hresult(factory->ActivateInstance(made.put()));
            line += "; a fresh factory's: " + probeOne(made.as<PackageManager>());
        } catch (hresult_error const& e) {
            line += format("; a fresh factory's: not made 0x%08x", unsigned(e.code()));
        }
        // One made on a thread of its own, new now.
        std::string other;
        std::thread([&other] {
            try {
                winrt::init_apartment();
            } catch (...) {
            }
            try {
                other = probeOne(PackageManager());
            } catch (hresult_error const& e) {
                other = format("not made 0x%08x", unsigned(e.code()));
            }
        }).join();
        line += "; a new thread's: " + other;
        shellLog::write(line);
    }).detach();
}

#if defined(MEGLER_XBOX)
namespace {

std::atomic<bool> gPortalWantsSignIn{false};

/// The console's own portal, with the user name and password Megler has for it.
devicePortal::Client consolePortal() { return devicePortal::Client(devicePortal::consoleAddress(), devicePortal::savedCredentials()); }

/// What the portal answered, said for the Library: whether it wants a user name and password (those Megler had are
/// forgotten then: the Library asks again), and where it was not reached at all, the way to it after its own words.
std::string portalSaid(const devicePortal::Answer& answer) {
    gPortalWantsSignIn = answer.wantsSignIn();
    if (answer.wantsSignIn()) devicePortal::forgetCredentials();
    return answer.reached ? answer.said : answer.said + " " + portalHelp();
}

/*
 * A package installed through the Xbox's Device Portal, as DesktopMode_uwp's App Installer installs one on a console: the
 * package alone, no certificate, for a console in Developer Mode takes a package whatever signed it (DesktopMode's sends
 * none). Should the console refuse it for its certificate all the same, it is sent again with `certificate`, which the
 * portal installs with it, as a phone's did. `reached`, where given, says whether the portal answered at all.
 */
InstallResult installThroughPortal(const std::wstring& appx, const std::wstring& certificate, const std::function<void(const std::string&)>& progress,
                                   bool* reached = nullptr) {
    devicePortal::Client portal = consolePortal();
    progress("Asking the Xbox's Device Portal…");
    devicePortal::Answer answer = portal.install(appx, {}, progress);
    if (!answer.ok && answer.reached && untrustedCertificate(answer.said) && !certificate.empty() && files::exists(certificate)) {
        shellLog::write("app package: the Xbox refused " + narrow(appx.substr(appx.find_last_of(L'\\') + 1)) +
                        " for its certificate: sent again with " + narrow(certificate.substr(certificate.find_last_of(L'\\') + 1)));
        answer = portal.install(appx, {certificate}, progress);
    }
    if (reached) *reached = answer.reached;
    if (!answer.ok) return {false, portalSaid(answer)};
    gPortalWantsSignIn = false;
    return {true, "Installed."};
}

/*
 * After the portal installed `fullName`: whether Windows has it for the user Megler runs as, and where not, registered for
 * them. DesktopMode_uwp found the portal, which deploys in a service's context, to register a package for the console's
 * DefaultAccount alone, and its installer registers it for its own user so (RegisterPackageByFullNameAsync) -- unproven on a
 * console, its notes say, and so here: where Windows will not answer (packageQuery, packageManagement), the log says so and
 * nothing else is done.
 */
void registerForThisUser(const std::string& fullName) {
    using namespace winrt::Windows::Management::Deployment;
    try {
        PackageManager manager;
        if (manager.FindPackageForUser(L"", widen(fullName))) return;
        shellLog::write("app package: " + fullName + " is installed, but not for the user Megler runs as: registered for them");
        auto registering = manager.RegisterPackageByFullNameAsync(widen(fullName), std::vector<winrt::hstring>(), DeploymentOptions::None);
        if (registering.wait_for(std::chrono::seconds(60)) == winrt::Windows::Foundation::AsyncStatus::Started) {
            registering.Cancel();
            shellLog::write("app package: its registration did not end within a minute");
            return;
        }
        const DeploymentResult result = registering.get();
        shellLog::write(result.ErrorText().empty() ? "app package: " + fullName + " registered for this user"
                                                   : format("app package: its registration was refused: 0x%08x ", unsigned(result.ExtendedErrorCode())) +
                                                         narrow(result.ErrorText()));
    } catch (hresult_error const& e) {
        shellLog::write(format("app package: whether %s is this user's could not be asked: 0x%08x ", fullName.c_str(), unsigned(e.code())) +
                        narrow(e.message()));
    }
}

}  // namespace

bool portalWantsSignIn() { return gPortalWantsSignIn; }

std::string portalHelp() {
    std::string family;
    try {
        family = narrow(std::wstring(Package::Current().Id().FamilyName()));
    } catch (hresult_error const&) {
        family = "Megler_7p434m16tbgc6";
    }
    return "Megler installs its apps on an Xbox through the console's own Device Portal. Turn it on in Dev Home (Remote access "
           "settings: Enable Xbox Device Portal). Windows lets an app reach the console's own address only when it is told to: from "
           "a PC on the same network, open an SSH session to the console (user DevToolsUser, password the Visual Studio PIN Dev Home "
           "shows) and run CheckNetIsolation.exe LoopbackExempt -a -n=" +
           family + " -- again after every start of the console.";
}
#endif

InstallResult install(const Made& made, const std::function<void(const std::string&)>& progress) {
#if defined(MEGLER_XBOX)
    const InstallResult result = installThroughPortal(made.appx, made.certificate, progress);
    if (result.ok) registerForThisUser(made.fullName);
    return result;
#else
    return installWithPackageManager(made.appx, progress);
#endif
}

bool untrustedCertificate(const std::string& said) {
    // CERT_E_UNTRUSTEDROOT and CERT_E_CHAINING, as the deployment engine says them ("0x800b0109 ...").
    std::string lower(said);
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return lower.find("0x800b0109") != std::string::npos || lower.find("0x800b010a") != std::string::npos;
}

bool registerServiceApk(const ApkEntry& entry, const std::string& installedFullName, bool replace) {
    if (entry.report.package != "com.google.android.gms") return true;
    if (!entry.report.ok || installedFullName.empty() || installedFullName.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos || !canRead(entry.path)) return false;
    const auto shared = sharedFolder();
    if (shared.empty()) return false;
    // (One at a time: Google Play services' own package asks after its start and again when its library reads it, and a
    // second asking meanwhile copied the APK a second time, beside the first.)
    static std::mutex registering;
    std::lock_guard<std::mutex> lock(registering);
    // microG's and Google's own are each registered apart: an app takes the one it is set to (ui/SettingsScreen.cpp).
    const auto record = shared + (entry.report.microg ? L"\\services\\microg-package.txt" : L"\\services\\gms-package.txt");
    std::string previous;
    if (!replace && files::read(record, previous) && iniValue(previous, "package") == installedFullName) {
        const auto token = iniValue(previous, "cache");
        if (!token.empty() && token.find_first_not_of("0123456789abcdef-") == std::string::npos &&
            canRead(shared + L"\\services\\" + widen(token) + L"\\GmsCore.apk")) return true;
    }
    // One shared service copy per installation. Fresh paths also preserve an older client's mapped APK.
    FILETIME time; GetSystemTimeAsFileTime(&time);
    const auto token = format("%08lx%08lx-%08lx", time.dwHighDateTime, time.dwLowDateTime, GetCurrentProcessId());
    const auto folder = shared + L"\\services\\" + widen(token);
    if (!files::makeDirectories(folder)) return false;
    const auto apk = folder + L"\\GmsCore.apk";
    // On the disk, and whole, before its name says it is there: a Lumia 650 that restarted by itself soon after an Update All
    // kept the copy's name and size, and one of its DEX files no longer summed to its checksum, so no app could have the
    // classes in it (package 625: TikTok Lite's advertising ID failed).
    // And byte for byte: the next copy, made so, came out with a byte wrong in the middle of another DEX file of it (632); what
    // differs is written again from the APK (files::mend).
    // (From Megler's package's own APK where the entry is a copy of one: shell/BundledApks.h.)
    const std::wstring bundled = bundledApks::originalOf(entry.path);
    uint64_t mended = 0;
    if (!files::copyWhole(bundled.empty() ? entry.path : bundled, apk + L".part", mended) || !files::move(apk + L".part", apk)) {
        shellLog::write("microG: the shared copy of " + narrow(entry.path) + " could not be made whole");
        files::remove(apk + L".part");
        return false;
    }
    if (mended) shellLog::write(format("microG: %llu bytes of the shared copy of Google Play services differed from its APK after the copy, and were written again",
                                       (unsigned long long)mended));
    if (!files::write(record + L".part", "package=" + installedFullName + "\ncache=" + token + "\n") || !files::flush(record + L".part") ||
        !files::move(record + L".part", record)) return false;
    shellLog::write(std::string(entry.report.microg ? "microG: registered microG's " : "microG: registered Google's own ") + installedFullName);
    removeUnusedServiceCopies();
    return true;
}

void unregisterServiceApk(const std::string& installedFullName) {
    const auto shared = sharedFolder();
    if (shared.empty()) return;
    for (const wchar_t* kind : {L"\\services\\gms-package.txt", L"\\services\\microg-package.txt"}) {
        const auto record = shared + kind;
        std::string registered;
        if (files::read(record, registered) && iniValue(registered, "package") == installedFullName) files::remove(record);
    }
    // Existing clients can finish with their mapped version. The cache is not an installation.
}

namespace {

/// Whether the registered service package is still installed, asked on a thread of its own, once a run (installedApps).
/// The Device Portal's list of the phone's packages, which it was asked of until 0.1.0.614, took seconds to come, and every
/// app's start waited for it: 9 to 10 s between "session: starting" and the app's own start, in each app's log (packages
/// 484 to 491). An app does not need the answer to start. A service package uninstalled outside Megler (Megler's own uninstall unregisters it at once) is forgotten
/// here, and the next start is without it.
void verifyServiceApkLater(const std::string& name) {
    static std::atomic<bool> asked{false};
    if (asked.exchange(true)) return;
    std::thread([name] {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded);
        } catch (...) {
        }
        std::vector<InstalledApp> apps;
        std::string error;
        if (!installedApps(apps, error)) {
            shellLog::write("microG: cannot verify service package installation: " + error);
            return;
        }
        if (std::none_of(apps.begin(), apps.end(), [&](const InstalledApp& a) { return a.fullName == name; })) {
            unregisterServiceApk(name);
            shellLog::write("microG: " + name + " is no longer installed: apps start without it from the next start on");
        }
    }).detach();
}

}  // namespace

namespace {

/// The registered APK of a record (gms-package.txt, microg-package.txt), or nothing.
std::wstring registeredApk(const wchar_t* recordName) {
    const auto shared = sharedFolder();
    std::string record;
    if (shared.empty() || !files::read(shared + L"\\services\\" + recordName, record)) return {};
    const auto name = iniValue(record, "package"), token = iniValue(record, "cache");
    if (name.empty() || token.empty() || token.find_first_not_of("0123456789abcdef-") != std::string::npos) return {};
    if (name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos) return {};
    const auto apk = shared + L"\\services\\" + widen(token) + L"\\GmsCore.apk";
    if (!canRead(apk)) return {};
    verifyServiceApkLater(name);
    return apk;
}

}  // namespace

/// microG's: one installed as an app and registered, else the one Megler's package brings (library/Library.cpp publishes it).
std::wstring microgApk() {
    const auto registered = registeredApk(L"microg-package.txt");
    if (!registered.empty()) return registered;
    const auto shared = sharedFolder();
    const auto bundled = shared.empty() ? std::wstring() : shared + L"\\services\\microg\\GmsCore.apk";
    return !bundled.empty() && canRead(bundled) ? bundled : std::wstring();
}

std::wstring serviceApk(bool microg) {
    const auto own = registeredApk(L"gms-package.txt");
    const auto light = microgApk();
    if (microg && !light.empty()) return light;
    return !own.empty() ? own : light;
}

bool hasMicroG() { return !microgApk().empty(); }

bool hasGoogleServices() { return !registeredApk(L"gms-package.txt").empty(); }

std::wstring gsfApk() {
    const auto shared = sharedFolder();
    if (shared.empty()) return {};
    const auto path = shared + L"\\services\\gsf\\GoogleServicesFramework.apk";
    return canRead(path) ? path : std::wstring();
}

namespace {
std::atomic<int> gPackageManager{0}; // 1 Windows's package manager answered (packageQuery), -1 it would not, 0 not asked

/// Whether Windows counts `package` with the developer's packages, of which a phone takes only so many (0x80073d11:
/// installWithPackageManager), and what signed it: "developer's, signed Developer", for the log.
std::string howInstalled(const Package& package) {
    static const char* const kinds[] = {"none", "Developer", "Enterprise", "Store", "System"};
    try {
        const unsigned kind = unsigned(package.SignatureKind());
        return std::string(package.IsDevelopmentMode() ? "developer's" : "not a developer's") + ", signed " + (kind < 5 ? kinds[kind] : "?");
    } catch (hresult_error const&) {
        return "?";
    }
}
}

bool installedApps(std::vector<InstalledApp>& out, std::string& error, const std::function<void()>& reachable) {
    out.clear();
#if defined(MEGLER_XBOX)
    /*
     * On an Xbox, the console's Device Portal, which installs them: its list has every package of the console, whichever
     * user it was registered for (a package the portal installed may be the console's DefaultAccount's alone, which
     * Windows's package manager would not list for Megler's user). Installs may begin once it has answered at all.
     */
    devicePortal::Client portal = consolePortal();
    if (const devicePortal::Answer opened = portal.open(); !opened.ok) {
        error = portalSaid(opened);
        return false;
    }
    if (reachable) reachable();
    std::vector<devicePortal::Package> packages;
    if (const devicePortal::Answer listed = portal.packages(packages); !listed.ok) {
        error = portalSaid(listed);
        return false;
    }
    gPortalWantsSignIn = false;
    // Each package's full name is "<name>_<version>_<processor>_<resource>_<publisher id>": an app's own package's name is
    // "MeglerApp." and more (mg_appx_manifest.c), its publisher Megler's.
    const std::string publisherId = narrow(Package::Current().Id().PublisherId());
    for (const devicePortal::Package& p : packages) {
        const std::string& full = p.fullName;
        const size_t first = full.find('_'), second = first == std::string::npos ? std::string::npos : full.find('_', first + 1);
        if (second == std::string::npos || full.compare(0, 10, "MeglerApp.") != 0) continue;
        if (full.size() < publisherId.size() || full.compare(full.size() - publisherId.size(), publisherId.size(), publisherId) != 0) continue;
        out.push_back({full.substr(0, first), full.substr(first + 1, second - first - 1), full});
    }
    return true;
#else
    /*
     * Windows's package manager lists the user's packages at once (the restricted capability packageQuery), where the
     * Device Portal took 11 s on an HP Elite x3, more than a minute on a Lumia 650, and at times never answered on a Lumia
     * 950 XL. It is the only lister: the portal, asked where it would not answer before, is asked nothing any more
     * (installWithPackageManager), and a Megler whose package manager does not answer installs nothing either (the restricted
     * capabilities packageQuery and packageManagement go together in its manifest).
     */
    try {
        const std::wstring publisherId(Package::Current().Id().PublisherId());
        std::vector<InstalledApp> found;
        // Once a run, for the log: how many of the phone's packages Windows counts as a developer's, and how Megler's are.
        const bool tell = gPackageManager.load() != 1;
        unsigned all = 0, developers = 0;
        std::string own;
        for (auto const& package : winrt::Windows::Management::Deployment::PackageManager().FindPackagesForUser(L"")) {
            const auto id = package.Id();
            const std::wstring name(id.Name());
            if (tell) {
                all++;
                try {
                    if (package.IsDevelopmentMode()) developers++;
                } catch (hresult_error const&) {
                }
                if ((name == L"Megler" || name.compare(0, 10, L"MeglerApp.") == 0) && std::wstring(id.PublisherId()) == publisherId)
                    own += "; " + narrow(name) + " " + howInstalled(package);
            }
            if (name.compare(0, 10, L"MeglerApp.") != 0 || std::wstring(id.PublisherId()) != publisherId) continue;
            const auto v = id.Version();
            found.push_back({narrow(name), format("%u.%u.%u.%u", v.Major, v.Minor, v.Build, v.Revision), narrow(std::wstring(id.FullName()))});
        }
        if (gPackageManager.exchange(1) != 1)
            shellLog::write(format("app package: Windows's package manager lists the apps' packages; %u of the %u packages are developer's",
                                   developers, all) + own);
        if (reachable) reachable();
        out = std::move(found);
        return true;
    } catch (hresult_error const& e) {
        error = format("Windows's package manager did not list the packages (0x%08x %s)", unsigned(e.code()), narrow(e.message()).c_str());
        if (gPackageManager.exchange(-1) != -1) shellLog::write("app package: " + error);
        return false;
    }
#endif
}

std::string packageLister() {
#if defined(MEGLER_XBOX)
    return "the Xbox's Device Portal";
#else
    return "Windows's package manager";
#endif
}

InstallResult uninstall(const InstalledApp& app) {
#if defined(MEGLER_XBOX)
    // On an Xbox, through its Device Portal, which installed it.
    const devicePortal::Answer answer = consolePortal().uninstall(app.fullName);
    if (!answer.ok) return {false, portalSaid(answer)};
    unregisterServiceApk(app.fullName);
    shellLog::write("app package: " + app.fullName + " uninstalled through the Xbox's Device Portal");
    return {true, "Uninstalled."};
#else
    // Windows's package manager (packageManagement), as installWithPackageManager installs; refused for access where Megler
    // has not got that capability, as an install is (InstallResult::accessDenied).
    try {
        const auto result = winrt::Windows::Management::Deployment::PackageManager().RemovePackageAsync(widen(app.fullName)).get();
        if (result.ErrorText().empty()) {
            unregisterServiceApk(app.fullName);
            shellLog::write("app package: " + app.fullName + " uninstalled by Windows's package manager");
            return {true, "Uninstalled."};
        }
        const int32_t code = result.ExtendedErrorCode();
        const std::string said = format("0x%08x ", unsigned(code)) + narrow(result.ErrorText());
        shellLog::write("app package: Windows's package manager did not uninstall " + app.fullName + ": " + said);
        if (code == E_ACCESSDENIED) sayRefusedForAccess();
        return {false, "Windows's package manager did not uninstall it: " + said, code == E_ACCESSDENIED};
    } catch (hresult_error const& e) {
        const int32_t code = e.code();
        const std::string said = format("0x%08x ", unsigned(code)) + narrow(e.message());
        shellLog::write("app package: Windows's package manager could not be asked to uninstall " + app.fullName + ": " + said);
        if (code == E_ACCESSDENIED) sayRefusedForAccess();
        return {false, "Windows's package manager could not be asked to uninstall it: " + said, code == E_ACCESSDENIED};
    }
#endif
}

std::string meglerVersion() { return versionString(); }

bool olderVersion(const std::string& a, const std::string& b) {
    unsigned x[4] = {}, y[4] = {};
    sscanf_s(a.c_str(), "%u.%u.%u.%u", &x[0], &x[1], &x[2], &x[3]);
    sscanf_s(b.c_str(), "%u.%u.%u.%u", &y[0], &y[1], &y[2], &y[3]);
    for (int i = 0; i < 4; i++)
        if (x[i] != y[i]) return x[i] < y[i];
    return false;
}

namespace {

uint64_t treeSize(const std::wstring& dir) {
    uint64_t total = 0;
    for (const auto& name : files::list(dir, L"*")) total += files::size(dir + L"\\" + name);
    for (const auto& name : files::listFolders(dir, L"*")) total += treeSize(dir + L"\\" + name);
    return total;
}

} // namespace

void removeUnusedAndroidRoots(const std::vector<InstalledApp>& installed) {
    if (isSingleApp()) return;
    const std::wstring shared = sharedFolder();
    if (shared.empty()) return;
    const std::string current = versionString();
    for (const auto& name : files::listFolders(shared + L"\\android", L"*")) {
        // <version>, or on desktop Windows <version>-<ABI> (androidRootName): every ABI's of a version is kept alike.
        const std::string version = narrow(name).substr(0, narrow(name).find('-'));
        // This build's, a newer one's (a Megler put back to an older build), and those packages are still of, stay.
        if (!olderVersion(version, current) ||
            std::any_of(installed.begin(), installed.end(), [&](const InstalledApp& a) { return a.version == version; }))
            continue;
        const std::wstring root = shared + L"\\android\\" + name;
        const uint64_t size = treeSize(root);
        // "complete" first: a root without it is never read, whatever of the rest stays.
        files::remove(root + L"\\complete");
        if (files::removeTree(root))
            shellLog::write("app package: Android's files of " + version + " removed from the shared folder (" + formattedByteCount(size) +
                            "): no app's package is of that build any more");
        else
            shellLog::write("app package: Android's files of " + version + " could not all be removed from the shared folder");
    }
}

void removeUnusedServiceCopies() {
    if (isSingleApp()) return;
    const std::wstring shared = sharedFolder();
    if (shared.empty()) return;
    std::string record, other;
    const std::string keep = files::read(shared + L"\\services\\gms-package.txt", record) ? iniValue(record, "cache") : std::string();
    const std::string keepMicroG = files::read(shared + L"\\services\\microg-package.txt", other) ? iniValue(other, "cache") : std::string();
    uint64_t freed = 0;
    int removed = 0, left = 0;
    for (const auto& name : files::listFolders(shared + L"\\services", L"*")) {
        // Only the copies' folders, named by their tokens (hexadecimal and '-'): gsf and the registered copy stay.
        const std::string token = narrow(name);
        if (token.empty() || token == keep || token == keepMicroG || token.find_first_not_of("0123456789abcdef-") != std::string::npos) continue;
        const std::wstring folder = shared + L"\\services\\" + name;
        const uint64_t size = treeSize(folder);
        if (files::removeTree(folder)) {
            freed += size;
            removed++;
        } else {
            left++;
        }
    }
    if (removed || left)
        shellLog::write(format("microG: %d older copies of Google Play services' APK removed from the shared folder (%s)", removed,
                               formattedByteCount(freed).c_str()) +
                        (left ? format("; %d still open in an app, left for the next time", left) : std::string()));
}

void removeOldHandedApks() {
    if (isSingleApp()) return;
    const std::wstring shared = sharedFolder();
    if (shared.empty()) return;
    const std::wstring incoming = shared + L"\\incoming";
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    const uint64_t nowTicks = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime, hour = 36000000000ull;
    uint64_t freed = 0;
    int removed = 0;
    for (const auto& name : files::list(incoming, L"*")) {
        const std::wstring path = incoming + L"\\" + name;
        WIN32_FILE_ATTRIBUTE_DATA data = {};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) continue;
        const uint64_t written = (uint64_t(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
        // A fresh one may still wait for its request's answer.
        if (written + hour > nowTicks) continue;
        const uint64_t size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        if (files::remove(path)) {
            freed += size;
            removed++;
        }
    }
    if (removed)
        shellLog::write(format("import: %d APKs apps had handed over removed from the shared folder's incoming (%s)", removed,
                               formattedByteCount(freed).c_str()));
}

void logSharedSpace() {
    static std::atomic<bool> said{false};
    if (isSingleApp() || said.exchange(true)) return;
    const std::wstring shared = sharedFolder();
    if (shared.empty()) return;
    uint64_t total = files::folderSize(shared);
    std::string parts;
    for (const auto& name : files::listFolders(shared, L"*")) {
        const uint64_t size = treeSize(shared + L"\\" + name);
        total += size;
        parts += format("%s%s %s", parts.empty() ? "" : ", ", narrow(name).c_str(), formattedByteCount(size).c_str());
    }
    shellLog::write("storage: the shared folder holds " + formattedByteCount(total) + ": " + parts);
    // Its apps' own storage, the largest five.
    std::vector<std::pair<uint64_t, std::string>> apps;
    for (const auto& name : files::listFolders(shared + L"\\apps", L"*")) apps.push_back({treeSize(shared + L"\\apps\\" + name), narrow(name)});
    std::sort(apps.begin(), apps.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string largest;
    for (size_t i = 0; i < apps.size() && i < 5; i++)
        largest += format("%s%s %s", largest.empty() ? "" : ", ", apps[i].second.c_str(), formattedByteCount(apps[i].first).c_str());
    if (!largest.empty()) shellLog::write("storage: the apps' storage there, the largest: " + largest);
    int copies = 0;
    for (const auto& name : files::listFolders(shared + L"\\services", L"*"))
        if (narrow(name).find_first_not_of("0123456789abcdef-") == std::string::npos) copies++;
    shellLog::write(format("storage: %d copies of Google Play services' APK in the shared folder's services", copies));
}

std::wstring installedApk(const std::string& fullName) {
    const std::wstring install = platform::installDirectory();
    const size_t slash = install.find_last_of(L'\\');
    return (slash == std::wstring::npos ? install : install.substr(0, slash)) + L"\\" + widen(fullName) + L"\\app\\app.apk";
}

bool canRead(const std::wstring& apk) {
    // (Another's writing allowed: the once-a-build check of the bundled APKs holds each copy open for writing while it compares
    // it (bundledApks::checkLater, files::mend), and Google Play services' registration after Update All's install of it,
    // asking here at that moment, failed: no app had it until GMS's own package was opened.)
    winrt::file_handle h{CreateFile2(apk.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING, nullptr)};
    char head[4] = {};
    DWORD got = 0;
    return h && ReadFile(h.get(), head, sizeof(head), &got, nullptr) && got == 4 && !memcmp(head, "PK\3\4", 4);
}

std::vector<std::wstring> reportedSplits(const std::wstring& apk) {
    using namespace winrt::Windows::Data::Json;
    std::vector<std::wstring> splits;
    // The app's report, beside its APK (make: app.apk.json), names the splits it was read with; a bundle's are unpacked as
    // split_<name>.apk (core/src/apk/mg_bundle.c). (A package's block map would list them, but Windows refuses another package
    // the package's own files: AppxBlockMap.xml gave error 5 on a Lumia 650.)
    std::string json;
    if (!files::read(apk + L".json", json)) {
        shellLog::write(format("app package: the report beside %s could not be read (error %lu)", narrow(apk).c_str(), GetLastError()));
        return splits;
    }
    JsonObject o{nullptr};
    if (!JsonObject::TryParse(widen(json), o) || !o.HasKey(L"splits") || o.GetNamedValue(L"splits").ValueType() != JsonValueType::Array)
        return splits;
    for (IJsonValue const& value : o.GetNamedArray(L"splits")) {
        if (value.ValueType() != JsonValueType::String) continue;
        const std::string name = "split_" + narrow(value.GetString()) + ".apk";
        if (plainName(name) && name.find('/') == std::string::npos) splits.push_back(widen(name));
    }
    return splits;
}

bool inSharedApks(const std::wstring& path) {
    const std::wstring shared = sharedFolder();
    if (shared.empty()) return false;
    const std::wstring apks = shared + L"\\apks\\";
    return path.size() > apks.size() && _wcsnicmp(path.c_str(), apks.c_str(), apks.size()) == 0;
}

void sharedApkInstalled(const Made& made) {
    const size_t slash = made.apkFolder.find_last_of(L'\\');
    const std::wstring shared = sharedFolder();
    if (made.apkFolder.empty() || slash == std::wstring::npos || shared.empty()) return;
    const std::wstring dir = shared + L"\\" + made.apkFolder.substr(0, slash), record = dir + L"\\installed.txt";
    if (!files::write(record + L".part", narrow(made.apkFolder.substr(slash + 1))) || !files::move(record + L".part", record))
        shellLog::write(format("app package: %s could not be written (error %lu): the APK's folder stays while Megler's library reads it",
                               narrow(record).c_str(), GetLastError()));
}

void removeUnusedSharedApks(const std::vector<InstalledApp>& installed, const std::vector<std::wstring>& used) {
    const std::wstring shared = sharedFolder();
    if (shared.empty()) return;
    const std::wstring apks = shared + L"\\apks";
    FILETIME time;
    GetSystemTimeAsFileTime(&time);
    const uint64_t now = (uint64_t(time.dwHighDateTime) << 32) | time.dwLowDateTime, hour = 36000000000ull;
    uint64_t freed = 0;
    unsigned open = 0;
    for (const auto& identity : files::listFolders(apks, L"*")) {
        const std::wstring dir = apks + L"\\" + identity;
        const bool isInstalled = std::any_of(installed.begin(), installed.end(), [&](const InstalledApp& a) { return widen(a.identity) == identity; });
        std::string current;
        if (!isInstalled) files::remove(dir + L"\\installed.txt");
        else if (!files::read(dir + L"\\installed.txt", current)) current.clear();
        for (const auto& token : files::listFolders(dir, L"*")) {
            const std::wstring folder = dir + L"\\" + token;
            if ((!current.empty() && narrow(token) == current) ||
                std::any_of(used.begin(), used.end(), [&](const std::wstring& u) { return _wcsicmp(u.c_str(), folder.c_str()) == 0; }))
                continue;
            // A folder of the last hour may be a package's being made, which nothing names yet: its name begins with the time
            // it was begun at (keepInShared).
            const uint64_t begun = token.size() >= 16 ? std::wcstoull(token.substr(0, 16).c_str(), nullptr, 16) : 0;
            if (begun && (begun > now || now - begun < hour)) continue;
            const uint64_t size = files::size(folder + L"\\app.apk") + files::folderSize(folder + L"\\app.apk.splits");
            if (files::removeTree(folder))
                freed += size;
            else
                open++;
        }
        RemoveDirectoryW(dir.c_str()); // (only when nothing is left in it)
    }
    if (freed || open)
        shellLog::write("app package: apps' APKs no package and no library entry uses removed from the shared folder, " + formattedByteCount(freed) +
                        (open ? format("; %u still open in an app, left for the next time", open) : std::string()));
}

bool meglersStorageShared() {
    static const bool shared = !sharedFolder().empty() && files::exists(platform::localStateDirectory() + L"\\storage-in-shared.txt");
    return shared;
}

void shareMeglersStorage() {
    const std::wstring shared = sharedFolder(), local = platform::localStateDirectory(), marker = local + L"\\storage-in-shared.txt";
    if (isSingleApp() || shared.empty() || files::exists(marker)) return;
    shellLog::write("storage: Megler's storage of before being copied to the shared folder");
    uint64_t copied = 0;
    bool ok = !files::exists(local + L"\\sdcard") || copyTree(local + L"\\sdcard", shared + L"\\sdcard", copied, true);
    for (const auto& name : files::listFolders(local + L"\\apps", L"*")) {
        const std::wstring part = local + L"\\apps\\" + name + L"\\sdcard";
        if (!files::exists(part) || files::exists(shared + L"\\apps\\" + name + L"\\megler-data.txt")) continue;
        if (!copyTree(part, shared + L"\\apps\\" + name + L"\\sdcard", copied, true)) {
            shellLog::write(format("storage: %s could not be copied (error %lu)", narrow(part).c_str(), GetLastError()));
            ok = false;
        }
    }
    if (ok && files::write(marker, "Megler " + versionString() + " copied its storage to " + narrow(shared) + "\r\n"))
        shellLog::write("storage: Megler's storage copied to the shared folder, " + formattedByteCount(copied) + "; Megler uses it from its next start");
    else
        shellLog::write("storage: Megler's storage was not all copied; tried again at the next start");
}

void openWithSystem(const std::wstring& path, std::function<void(bool)> done) {
    [](std::wstring path, std::function<void(bool)> done) -> winrt::fire_and_forget {
        bool ok = false;
        try {
            auto file = co_await StorageFile::GetFileFromPathAsync(path);
            ok = co_await winrt::Windows::System::Launcher::LaunchFileAsync(file);
        } catch (hresult_error const& e) {
            shellLog::write("app package: " + narrow(path) + " could not be opened: " + narrow(e.message()));
        }
        shellLog::write("app package: " + narrow(path) + (ok ? " handed to Windows" : " not taken by Windows"));
        if (done) done(ok);
    }(path, std::move(done));
}

std::wstring signingCertificate() { return platform::localStateDirectory() + L"\\signing\\MeglerApps.cer"; }

std::string trustCommand() {
    return "Import-Certificate -FilePath \"" + narrow(signingCertificate()) + "\" -CertStoreLocation Cert:\\LocalMachine\\TrustedPeople";
}

void showSigningCertificate(std::function<void(bool)> done) {
    [](std::function<void(bool)> done) -> winrt::fire_and_forget {
        bool ok = false;
        const std::wstring certificate = signingCertificate();
        try {
            auto folder = co_await StorageFolder::GetFolderFromPathAsync(certificate.substr(0, certificate.find_last_of(L'\\')));
            winrt::Windows::System::FolderLauncherOptions options;
            options.ItemsToSelect().Append(co_await folder.GetFileAsync(L"MeglerApps.cer"));
            ok = co_await winrt::Windows::System::Launcher::LaunchFolderAsync(folder, options);
        } catch (hresult_error const& e) {
            shellLog::write("app package: the folder of " + narrow(certificate) + " could not be shown: " + narrow(e.message()));
        }
        shellLog::write("app package: the folder of " + narrow(certificate) + (ok ? " shown in File Explorer" : " not shown by Windows"));
        if (done) done(ok);
    }(std::move(done));
}

static std::atomic<bool> gSelfUpdating{false};
static std::mutex gSelfUpdateMutex;
static std::string gSelfUpdateTo, gSelfUpdateFailure;

bool selfUpdating() { return gSelfUpdating; }

std::string selfUpdateVersion() {
    std::lock_guard<std::mutex> lock(gSelfUpdateMutex);
    return gSelfUpdating ? gSelfUpdateTo : std::string();
}

std::string selfUpdateFailure() {
    std::lock_guard<std::mutex> lock(gSelfUpdateMutex);
    return gSelfUpdateFailure;
}

namespace {
/*
 * Whether Windows refused a package for its data, as a copy damaged on its way to the phone is: invalid data, a block map
 * or a block's hash that does not match, a CRC, a digest the signature does not match. A Lumia 650's copy of 650 made over
 * USB had the right size and other bytes; Windows's installer said 0x8007000d, and Megler, which had tried it, said nothing.
 */
bool damagedCopy(std::string said) {
    std::transform(said.begin(), said.end(), said.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    for (const char* code : {"0x8007000d", "0x80080205", "0x80080206", "0x80080207", "0x80070017", "0x80096010"})
        if (said.find(code) != std::string::npos) return true;
    return false;
}

/// The newer Megler in Pictures\Megler as `file` not installed for `said` (`denied`: refused for access, InstallResult::
/// accessDenied): what the Library says (selfUpdateFailure).
void rememberFailure(const std::string& file, const std::string& version, const std::string& said, bool denied = false) {
    const std::string text = denied ? ownUpdateRefusedText(version, file, true)
                             : damagedCopy(said)
                                 ? "Megler " + version + " was not installed: Windows found the data of Pictures\\Megler\\" + file +
                                       " invalid, as in a copy damaged on its way to the phone. Delete it there, copy it to the phone "
                                       "again, and open Megler."
                                 : "Megler " + version + " in Pictures\\Megler was not installed: " + said;
    std::lock_guard<std::mutex> lock(gSelfUpdateMutex);
    gSelfUpdateFailure = text;
}
}

std::string ownUpdateRefusedText(const std::string& version, const std::string& file, bool inPictures) {
    // (Windows's own installer takes a package the file manager hands it whatever Megler's capabilities: the way round.)
    const std::string opener = platform::isMobile() ? "the phone's file manager" : "File Explorer";
    return "Megler " + version + " was not installed. " +
           accessDeniedText(inPictures ? "open " + file + " from Pictures\\Megler in " + opener + "."
                                       : "download " + file + " from Megler's releases on GitHub and open it in " + opener + ".");
}

bool keepRefusedUpdate(const std::wstring& appx, const std::wstring& certificate, const std::string& version) {
    const std::string file = narrow(appx.substr(appx.find_last_of(L'\\') + 1));
    bool moved = false;
    try {
        // LocalState is out of the file manager's reach; Pictures\Megler is where a phone's owner copies Megler's updates to.
        auto folder = KnownFolders::PicturesLibrary().CreateFolderAsync(L"Megler", CreationCollisionOption::OpenIfExists).get();
        StorageFile::GetFileFromPathAsync(appx).get().MoveAsync(folder, widen(file), NameCollisionOption::ReplaceExisting).get();
        moved = true;
        // (installCopiedUpdate tries a package only with the certificate beside it.)
        if (files::exists(certificate))
            StorageFile::GetFileFromPathAsync(certificate).get().MoveAsync(folder, L"Megler.cer", NameCollisionOption::ReplaceExisting).get();
        shellLog::write("update: " + file + " moved to Pictures\\Megler, where the file manager opens it and Megler tries it again at its next start");
    } catch (hresult_error const& e) {
        shellLog::write(format("update: %s could not be moved to Pictures\\Megler: 0x%08x ", file.c_str(), unsigned(e.code())) + narrow(e.message()));
    }
    std::lock_guard<std::mutex> lock(gSelfUpdateMutex);
    gSelfUpdateFailure = ownUpdateRefusedText(version, file, moved);
    return moved;
}

bool updateRefusedForAccess(const std::string& version) {
    std::string record;
    if (!files::read(platform::localStateDirectory() + L"\\update-tried-" + widen(version), record)) return false;
    // The last try's result (installOwnUpdate writes "denied: " for it), with no try begun after it.
    const size_t denied = record.rfind("denied: ");
    return denied != std::string::npos && (denied == 0 || record[denied - 1] == '\n') && record.find(" installs ", denied) == std::string::npos;
}

InstallResult installOwnUpdate(const std::wstring& appx, const std::wstring& certificate, const std::string& version, const std::string& from) {
    const std::wstring tried = platform::localStateDirectory() + L"\\update-tried-" + widen(version);
    files::append(tried, "Megler " + versionString() + " installs " + version + "\r\n");
#if defined(MEGLER_XBOX)
    shellLog::write("update: Megler " + version + " " + from + " (this is " + versionString() +
                    "): installed through the Xbox's Device Portal, which closes this Megler");
#else
    // (The certificate is not handed on: Windows's deployment engine trusts what the machine trusts.)
    (void)certificate;
    shellLog::write("update: Megler " + version + " " + from + " (this is " + versionString() +
                    "): installed by Windows's deployment engine, which closes this Megler");
#endif
    // The screen stays on while the package goes: a phone that locks suspends Megler, and the try has no result.
    {
        std::lock_guard<std::mutex> lock(gSelfUpdateMutex);
        gSelfUpdateTo = version;
    }
    struct Updating {
        Updating() { gSelfUpdating = true; }
        ~Updating() { gSelfUpdating = false; }
    } updating;
    /*
     * Windows's deployment engine reads the package where it lies (installWithPackageManager). The phone's Device Portal,
     * which was asked where the engine refused until 2026-11-07, was sent it from Megler's memory: a phone of 1 GB lets an
     * app have 390 MB, and a Lumia 650's Megler 574 sending 610's 356 MB was ended by Windows with nothing said, three times.
     */
    const auto step = [](const std::string& said) { shellLog::write("update: " + said); };
#if defined(MEGLER_XBOX)
    // On an Xbox the console's Device Portal installs it, as it installs the apps' packages: sent through Megler's memory as
    // it went to a phone's portal, which a console's app has room for (the package is some 60 MB, Google's APKs being the
    // phone's alone). A portal that could not be reached said nothing of the package: that try is one without a result,
    // made again.
    bool reached = false;
    const InstallResult result = installThroughPortal(appx, certificate, step, &reached);
    const bool unreached = !result.ok && !reached;
#else
    const InstallResult result = installWithPackageManager(appx, step);
    const bool unreached = false;
#endif
    // A damaged copy is not a result either: it is tried again at the next start (Windows refuses it at once), in case a good
    // one was copied in its place. Nor is a refusal for access (Megler has not got packageManagement: accessDeniedText):
    // Windows refuses it at once too, and the next start after the phone's interop unlock is complete installs it.
    const bool damaged = !result.ok && damagedCopy(result.said);
    const bool denied = !result.ok && result.accessDenied;
    shellLog::write("update: " + std::string(result.ok ? "installed: " : "not installed: ") + result.said);
    files::append(tried, std::string(unreached ? "no result: " : damaged ? "damaged: " : denied ? "denied: " : "result: ") +
                             (result.ok ? "installed: " : "not installed: ") + result.said + "\r\n");
    return result;
}

void installCopiedUpdate() {
    if (isSingleApp()) return;
    std::thread([] {
        {
            std::lock_guard<std::mutex> lock(gSelfUpdateMutex);
            gSelfUpdateFailure.clear();
        }
        try {
            auto item = KnownFolders::PicturesLibrary().TryGetItemAsync(L"Megler").get();
            auto folder = item ? item.try_as<StorageFolder>() : nullptr;
            if (!folder) return;
            // "Megler_0.1.0.476_ARM.appx": the newest there.
            std::string best;
            StorageFile bestFile{nullptr};
            for (auto const& file : folder.GetFilesAsync().get()) {
                const std::string name = narrow(file.Name());
                const std::string head = "Megler_", tail = "_ARM.appx";
                if (name.size() <= head.size() + tail.size() || name.compare(0, head.size(), head) || name.compare(name.size() - tail.size(), tail.size(), tail)) continue;
                const std::string version = name.substr(head.size(), name.size() - head.size() - tail.size());
                unsigned v[4];
                if (sscanf_s(version.c_str(), "%u.%u.%u.%u", &v[0], &v[1], &v[2], &v[3]) != 4) continue;
                if (best.empty() || olderVersion(best, version)) {
                    best = version;
                    bestFile = file;
                }
            }
            if (best.empty() || !olderVersion(versionString(), best)) return;
            const std::string name = narrow(std::wstring(bestFile.Name()));
            // The file's size and times: what tells a file copied there again from the one tried.
            std::string file;
            try {
                const auto properties = bestFile.GetBasicPropertiesAsync().get();
                file = format("%llu bytes, made %llu, changed %llu", static_cast<unsigned long long>(properties.Size()),
                              static_cast<unsigned long long>(bestFile.DateCreated().time_since_epoch().count()),
                              static_cast<unsigned long long>(properties.DateModified().time_since_epoch().count()));
            } catch (hresult_error const&) {
            }
            /*
             * A try is written down before it is made, and its result after: a package the installer refused is not tried again.
             * A try that has no result -- this Megler was closed, or suspended before Windows had taken the package (the phone's
             * Device Portal, which installed it until 612, took minutes) -- is made again at the next start, three times in all.
             * 508's first try had no result, and every start after it said "tried once already" (the second, made by hand,
             * ended when the owner went to another app).
             * The tries are the file's: one copied there again since (of another size or time) is tried anew, and one Windows
             * found damaged at every start, in case a good copy was put in its place with the same size and times. A Lumia
             * 650's copy of 650 was damaged on its way over USB, Windows refused it (0x8007000d), and a good copy put in its
             * place would not have been tried.
             */
            const std::wstring tried = platform::localStateDirectory() + L"\\update-tried-" + widen(best);
            std::string record;
            if (files::read(tried, record)) {
                const size_t line = record.rfind("\nfile: ");
                const size_t at = line != std::string::npos ? line + 1 : record.rfind("file: ", 0) == 0 ? 0 : std::string::npos;
                const std::string triedFile = at == std::string::npos ? std::string() : record.substr(at + 6, record.find('\r', at) - (at + 6));
                const size_t damaged = record.rfind("\ndamaged: ");
                const size_t denied = record.rfind("\ndenied: ");
                if (damaged != std::string::npos && record.find(" installs ", damaged) == std::string::npos) {
                    shellLog::write("update: " + name + " in Pictures\\Megler was damaged when tried: tried again, in case it was copied again");
                    files::remove(tried);
                    record.clear();
                } else if (denied != std::string::npos && record.find(" installs ", denied) == std::string::npos) {
                    // (Refused for access: Megler had not got packageManagement. The phone's interop unlock may be complete now.)
                    shellLog::write("update: " + name + " in Pictures\\Megler was refused for access when tried: tried again, in case "
                                    "Megler may install packages now");
                    files::remove(tried);
                    record.clear();
                } else if (!file.empty() && triedFile != file) {
                    shellLog::write("update: " + name + " in Pictures\\Megler is not the file tried before (" +
                                    (triedFile.empty() ? std::string("that one's not written down") : triedFile) + "): tried anew");
                    files::remove(tried);
                    record.clear();
                }
            }
            if (!record.empty()) {
                int tries = 0;
                for (size_t at = record.find(" installs "); at != std::string::npos; at = record.find(" installs ", at + 1)) tries++;
                const size_t result = record.rfind("\nresult: ");
                if (result != std::string::npos || tries >= 3) {
                    shellLog::write(format("update: Megler %s in Pictures\\Megler was tried %d time%s already (%s)", best.c_str(), tries,
                                           tries == 1 ? "" : "s", narrow(tried).c_str()));
                    // Said in the Library too, at every start: the owner saw an update begin "for a second", and nothing more.
                    const size_t from = result == std::string::npos ? result : result + 9;
                    const std::string outcome = from == std::string::npos ? std::string() : record.substr(from, record.find('\r', from) - from);
                    if (outcome.rfind("not installed: ", 0) == 0)
                        rememberFailure(name, best, outcome.substr(15));
                    else if (outcome.empty())
                        rememberFailure(name, best, "none of its three tries ended (Megler was closed or suspended meanwhile)");
                    return;
                }
                shellLog::write(format("update: the try %d of Megler %s did not end (this Megler was closed or suspended while it sent it): tried again",
                                       tries, best.c_str()));
            }
            auto certificate = folder.TryGetItemAsync(L"Megler.cer").get();
            if (!certificate) {
                shellLog::write("update: Megler " + best + " is in Pictures\\Megler without Megler.cer beside it: not installed");
                return;
            }
            if (!file.empty()) files::append(tried, "file: " + file + "\r\n");
            const InstallResult result =
                installOwnUpdate(std::wstring(bestFile.Path()), std::wstring(certificate.Path()), best, "found in Pictures\\Megler");
            if (!result.ok) rememberFailure(name, best, result.said, result.accessDenied);
        } catch (hresult_error const& e) {
            shellLog::write(format("update: Pictures\\Megler could not be read: 0x%08x ", unsigned(e.code())) + narrow(e.message()));
        }
    }).detach();
}

}  // namespace megler::app::appPackage
