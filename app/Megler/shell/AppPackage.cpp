#include "pch.h"

#include "shell/AppPackage.h"

#include "megler.h"
#include "library/Library.h"
#include "player/Session.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Graphics.Imaging.h>

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
        s.apkFolder = install + L"\\" + widen(iniValue(text, "apk"));
        const std::wstring shared = sharedFolder();
        if (!shared.empty()) s.androidRoot = shared + L"\\" + widen(iniValue(text, "root"));
        shellLog::write("app package: this is " + s.package + "'s own package; Android's files in " + narrow(s.androidRoot));
        return s;
    }();
    return app;
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

Made make(const ApkEntry& entry, const std::function<void(const std::string&)>& progress) {
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
    if (shared.empty()) return fail("Windows gives this Megler no shared folder (its manifest lacks the publisher cache folder).");
    const std::wstring install = platform::installDirectory(), local = platform::localStateDirectory();
    const std::string version = versionString();

    // Android's libcore and framework, once for each build: every app's package of this build reads them there.
    const std::wstring root = shared + L"\\android\\" + widen(version);
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
    char* manifest = megler_appx_app_manifest(meglerManifest.c_str(), r.package.c_str(), label.c_str(), logos.empty() ? nullptr : "transparent",
                                            nullptr, err, sizeof(err));
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

    progress(format("Packing %u files of Megler and the APK (%s)…", unsigned(parts.size()), formattedByteCount(entry.size).c_str()));
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
    const std::string ini = "package=" + r.package + "\r\nlabel=" + label + "\r\napk=app\r\nroot=android\\" + version + "\r\n";
    megler_appx_add_data(a, "MeglerApp.ini", ini.data(), ini.size());
    megler_appx_add_file(a, "app/app.apk", narrow(entry.path).c_str());
    for (const auto& name : splits)
        megler_appx_add_file(a, ("app/app.apk.splits/" + narrow(name)).c_str(), narrow(entry.splitsDir + L"\\" + name).c_str());
    if (files::exists(entry.path + L".json")) megler_appx_add_file(a, "app/app.apk.json", narrow(entry.path + L".json").c_str());
    // The icon as the library keeps it (beside Megler's copy, or in the library for an APK installed where it was picked).
    const std::wstring iconFile = !entry.iconPath.empty() && files::exists(entry.iconPath) ? entry.iconPath : entry.path + L".icon";
    if (files::exists(iconFile)) megler_appx_add_file(a, "app/app.apk.icon", narrow(iconFile).c_str());
    const std::wstring stamp = Library::instance().directory() + L"\\reports.version";
    if (files::exists(stamp)) megler_appx_add_file(a, "app/reports.version", narrow(stamp).c_str());
    progress("Signing the package…");
    if (!megler_appx_finish(a, key, err, sizeof(err))) return fail(std::string("The package could not be made: ") + err);
    shellLog::write("app package: " + narrow(made.appx) + " made, " + formattedByteCount(files::size(made.appx)) + ", for " + r.package);
    // The data handed over once: said only now, so that a make that failed hands it over again.
    if (handOver && (!files::makeDirectories(inShared) || !files::write(handedOver, "Megler " + version + " handed over " + r.package + "'s data in Megler\r\n")))
        shellLog::write("app package: " + narrow(handedOver) + " could not be written: the next package of the app hands its data over again");
    return made;
}

namespace {

/// The phone's own Device Portal, https://127.0.0.1, asked once for its cookies: what every call to it starts with.
struct Portal {
    winrt::Windows::Web::Http::HttpClient client{nullptr};
    std::wstring cookies, token;

    static std::wstring base() { return L"https://127.0.0.1"; }

    /// Empty when the portal answered, else why not, as a sentence.
    std::string open() {
        using namespace winrt::Windows::Web::Http;
        using namespace winrt::Windows::Web::Http::Filters;
        using winrt::Windows::Foundation::Uri;
        using winrt::Windows::Security::Cryptography::Certificates::ChainValidationResult;
        // The portal's certificate is one the phone made for itself, which nothing vouches for: taken as it is, here only,
        // where the address is the phone's own.
        HttpBaseProtocolFilter filter;
        filter.AllowUI(false);
        for (auto e : {ChainValidationResult::Untrusted, ChainValidationResult::InvalidName, ChainValidationResult::Expired,
                       ChainValidationResult::IncompleteChain})
            filter.IgnorableServerCertificateErrors().Append(e);
        filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::MostRecent);
        // Cookies by hand, as tools/phone.py sends them, which the portal takes: the token it hands out as a cookie goes
        // back in the Cookie header and in X-CSRF-Token. (Left to the filter's own cookies, the install was refused 403 on
        // the phone with package 353.)
        filter.CookieUsageBehavior(HttpCookieUsageBehavior::NoCookies);
        client = HttpClient(filter);
        auto info = client.GetAsync(Uri(base() + L"/api/os/info")).get();
        if (info.StatusCode() == HttpStatusCode::Unauthorized) return "The Device Portal asks for a user name and password, which Megler doesn't send yet.";
        if (!info.IsSuccessStatusCode()) return format("The Device Portal answered %d.", int(info.StatusCode()));
        for (auto const& header : info.Headers()) {
            if (_wcsicmp(header.Key().c_str(), L"Set-Cookie")) continue;
            // One or more cookies, "name=value; attributes", several joined by ", ".
            std::wstring all(header.Value());
            size_t at = 0;
            while (at < all.size()) {
                size_t end = all.find(L',', at);
                std::wstring one = all.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
                at = end == std::wstring::npos ? all.size() : end + 1;
                while (!one.empty() && one[0] == L' ') one.erase(0, 1);
                const std::wstring pair = one.substr(0, one.find(L';'));
                const size_t eq = pair.find(L'=');
                if (eq == std::wstring::npos || pair.find(L' ') != std::wstring::npos) continue;  // (an attribute's date, cut at its comma)
                cookies += (cookies.empty() ? L"" : L"; ") + pair;
                if (pair.compare(0, eq, L"CSRF-Token") == 0) token = pair.substr(eq + 1);
            }
        }
        return {};
    }

    /// A request with the cookies and the token.
    winrt::Windows::Web::Http::HttpRequestMessage request(winrt::Windows::Web::Http::HttpMethod const& method, const std::wstring& path) {
        winrt::Windows::Web::Http::HttpRequestMessage r(method, winrt::Windows::Foundation::Uri(base() + path));
        if (!cookies.empty()) r.Headers().TryAppendWithoutValidation(L"Cookie", cookies);
        if (!token.empty()) r.Headers().TryAppendWithoutValidation(L"X-CSRF-Token", token);
        return r;
    }
};

std::string unreachable(hresult_error const& e) {
    shellLog::write(format("app package: the Device Portal: 0x%08x ", unsigned(e.code())) + narrow(e.message()));
    return "The Device Portal could not be reached on this phone: " + narrow(e.message()) + format(" (0x%08x)", unsigned(e.code()));
}

}  // namespace

PortalResult installThroughPortal(const Made& made, const std::function<void(const std::string&)>& progress) {
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Web::Http::Headers;
    using winrt::Windows::Foundation::Uri;
    try {
        Portal portal;
        progress("Asking the Device Portal…");
        if (std::string why = portal.open(); !why.empty()) return {false, why};
        // (The cookies' names alone: their values are the portal's session, which a log sent on is no place for.)
        std::string names;
        for (size_t at = 0; at < portal.cookies.size();) {
            const size_t end = portal.cookies.find(L"; ", at), eq = portal.cookies.find(L'=', at);
            names += (names.empty() ? "" : ", ") + narrow(portal.cookies.substr(at, (eq < end ? eq : end) - at));
            at = end == std::wstring::npos ? portal.cookies.size() : end + 2;
        }
        shellLog::write("app package: the Device Portal's cookies: " + (names.empty() ? std::string("none") : names) +
                        (portal.token.empty() ? ", no CSRF token" : ", a CSRF token"));

        HttpMultipartFormDataContent form;
        for (const std::wstring& path : {made.appx, made.certificate}) {
            auto file = StorageFile::GetFileFromPathAsync(path).get();
            HttpStreamContent part(file.OpenAsync(FileAccessMode::Read).get());
            part.Headers().ContentType(HttpMediaTypeHeaderValue(L"application/octet-stream"));
            form.Add(part, file.Name(), file.Name());
        }
        const std::wstring appxName = made.appx.substr(made.appx.find_last_of(L'\\') + 1);
        auto request = portal.request(HttpMethod::Post(), L"/api/app/packagemanager/package?package=" + std::wstring(Uri::EscapeComponent(appxName)));
        request.Content(form);
        progress("Sending the package (" + formattedByteCount(files::size(made.appx)) + ") to the Device Portal…");
        auto sent = portal.client.SendRequestAsync(request).get();
        if (!sent.IsSuccessStatusCode()) {
            const std::string body = narrow(sent.Content().ReadAsStringAsync().get());
            shellLog::write(format("app package: the Device Portal refused the package: %d ", int(sent.StatusCode())) + body);
            return {false, format("The Device Portal refused the package (%d): ", int(sent.StatusCode())) + body};
        }

        // It answers the state 204 while it is still installing.
        progress("Installing…");
        for (int i = 0; i < 300; i++) {
            Sleep(2000);
            auto state = portal.client.GetAsync(Uri(Portal::base() + L"/api/app/packagemanager/state")).get();
            if (state.StatusCode() == HttpStatusCode::NoContent) continue;
            const hstring body = state.Content().ReadAsStringAsync().get();
            winrt::Windows::Data::Json::JsonObject json{nullptr};
            if (winrt::Windows::Data::Json::JsonObject::TryParse(body, json) && json.GetNamedBoolean(L"Success", false)) {
                shellLog::write("app package: " + made.identity + " installed through the Device Portal");
                return {true, "Installed."};
            }
            const std::string said = narrow(body);
            shellLog::write("app package: the Device Portal said " + said);
            return {false, format("Not installed (%d): ", int(state.StatusCode())) + said};
        }
        return {false, "The Device Portal was still installing after ten minutes."};
    } catch (hresult_error const& e) {
        return {false, unreachable(e)};
    }
}

bool registerServiceApk(const ApkEntry& entry, const std::string& installedFullName, bool replace) {
    if (entry.report.package != "com.google.android.gms") return true;
    if (!entry.report.ok || installedFullName.empty() || installedFullName.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos || !canRead(entry.path)) return false;
    const auto shared = sharedFolder();
    if (shared.empty()) return false;
    const auto record = shared + L"\\services\\gms-package.txt";
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
    if (!files::copy(entry.path, apk + L".part") || !files::move(apk + L".part", apk)) return false;
    if (!files::write(record + L".part", "package=" + installedFullName + "\ncache=" + token + "\n") ||
        !files::move(record + L".part", record)) return false;
    shellLog::write("microG: registered separately installed " + installedFullName);
    removeUnusedServiceCopies();
    return true;
}

void unregisterServiceApk(const std::string& installedFullName) {
    const auto record = sharedFolder() + L"\\services\\gms-package.txt";
    std::string registered;
    if (files::read(record, registered) && iniValue(registered, "package") == installedFullName) files::remove(record);
    // Existing clients can finish with their mapped version. The cache is not an installation.
}

namespace {

/// Whether the registered service package is still installed, asked of the Device Portal on a thread of its own, once a run.
/// The portal's list of the phone's packages takes seconds to come, and every app's start waited for it: 9 to 10 s between
/// "session: starting" and the app's own start, in each app's log (packages 484 to 491). An app does not need the answer
/// to start. A service package uninstalled outside Megler (Megler's own uninstall unregisters it at once) is forgotten
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

std::wstring serviceApk() {
    const auto shared = sharedFolder();
    std::string record;
    if (shared.empty() || !files::read(shared + L"\\services\\gms-package.txt", record)) return {};
    const auto name = iniValue(record, "package"), token = iniValue(record, "cache");
    if (name.empty() || token.empty() || token.find_first_not_of("0123456789abcdef-") != std::string::npos) return {};
    if (name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-") != std::string::npos) return {};
    const auto apk = shared + L"\\services\\" + widen(token) + L"\\GmsCore.apk";
    if (!canRead(apk)) return {};
    verifyServiceApkLater(name);
    return apk;
}

std::wstring gsfApk() {
    const auto shared = sharedFolder();
    if (shared.empty()) return {};
    const auto path = shared + L"\\services\\gsf\\GoogleServicesFramework.apk";
    return canRead(path) ? path : std::wstring();
}

bool installedApps(std::vector<InstalledApp>& out, std::string& error) {
    using namespace winrt::Windows::Data::Json;
    out.clear();
    try {
        Portal portal;
        if (std::string why = portal.open(); !why.empty()) {
            error = why;
            return false;
        }
        auto answer = portal.client.SendRequestAsync(portal.request(winrt::Windows::Web::Http::HttpMethod::Get(), L"/api/app/packagemanager/packages")).get();
        if (!answer.IsSuccessStatusCode()) {
            error = format("The Device Portal answered %d to the list of packages.", int(answer.StatusCode()));
            return false;
        }
        JsonObject json{nullptr};
        if (!JsonObject::TryParse(answer.Content().ReadAsStringAsync().get(), json)) {
            error = "The Device Portal's list of packages could not be read.";
            return false;
        }
        // Each package's full name is "<name>_<version>_<processor>_<resource>_<publisher id>": an app's own package's name
        // is "MeglerApp." and more (mg_appx_manifest.c), its publisher Megler's.
        const std::string publisherId = narrow(Package::Current().Id().PublisherId());
        for (auto const& value : json.GetNamedArray(L"InstalledPackages", JsonArray())) {
            if (value.ValueType() != JsonValueType::Object) continue;
            const std::string full = narrow(std::wstring(value.GetObject().GetNamedString(L"PackageFullName", L"")));
            const size_t first = full.find('_'), second = first == std::string::npos ? std::string::npos : full.find('_', first + 1);
            if (second == std::string::npos || full.compare(0, 10, "MeglerApp.") != 0) continue;
            if (full.size() < publisherId.size() || full.compare(full.size() - publisherId.size(), publisherId.size(), publisherId) != 0) continue;
            out.push_back({full.substr(0, first), full.substr(first + 1, second - first - 1), full});
        }
        return true;
    } catch (hresult_error const& e) {
        error = unreachable(e);
        return false;
    }
}

PortalResult uninstallThroughPortal(const InstalledApp& app) {
    using winrt::Windows::Foundation::Uri;
    try {
        Portal portal;
        if (std::string why = portal.open(); !why.empty()) return {false, why};
        auto answer = portal.client
                          .SendRequestAsync(portal.request(winrt::Windows::Web::Http::HttpMethod::Delete(),
                                                           L"/api/app/packagemanager/package?package=" + std::wstring(Uri::EscapeComponent(widen(app.fullName)))))
                          .get();
        if (!answer.IsSuccessStatusCode()) {
            const std::string body = narrow(answer.Content().ReadAsStringAsync().get());
            shellLog::write(format("app package: the Device Portal did not uninstall %s: %d ", app.fullName.c_str(), int(answer.StatusCode())) + body);
            return {false, format("The Device Portal did not uninstall it (%d): ", int(answer.StatusCode())) + body};
        }
        unregisterServiceApk(app.fullName);
        shellLog::write("app package: " + app.fullName + " uninstalled through the Device Portal");
        return {true, "Uninstalled."};
    } catch (hresult_error const& e) {
        return {false, unreachable(e)};
    }
}

std::string meglerVersion() { return versionString(); }

namespace {

/// "0.1.0.354" before "0.1.0.358".
bool olderVersion(const std::string& a, const std::string& b) {
    unsigned x[4] = {}, y[4] = {};
    sscanf_s(a.c_str(), "%u.%u.%u.%u", &x[0], &x[1], &x[2], &x[3]);
    sscanf_s(b.c_str(), "%u.%u.%u.%u", &y[0], &y[1], &y[2], &y[3]);
    for (int i = 0; i < 4; i++)
        if (x[i] != y[i]) return x[i] < y[i];
    return false;
}

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
        const std::string version = narrow(name);
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
    std::string record;
    const std::string keep = files::read(shared + L"\\services\\gms-package.txt", record) ? iniValue(record, "cache") : std::string();
    uint64_t freed = 0;
    int removed = 0, left = 0;
    for (const auto& name : files::listFolders(shared + L"\\services", L"*")) {
        // Only the copies' folders, named by their tokens (hexadecimal and '-'): gsf and the registered copy stay.
        const std::string token = narrow(name);
        if (token.empty() || token == keep || token.find_first_not_of("0123456789abcdef-") != std::string::npos) continue;
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
    winrt::file_handle h{CreateFile2(apk.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, OPEN_EXISTING, nullptr)};
    char head[4] = {};
    DWORD got = 0;
    return h && ReadFile(h.get(), head, sizeof(head), &got, nullptr) && got == 4 && !memcmp(head, "PK\3\4", 4);
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

void installCopiedUpdate() {
    if (isSingleApp()) return;
    std::thread([] {
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
            /*
             * A try is written down before it is made, and its result after: a package the installer refused is not tried again.
             * A try that has no result -- this Megler was closed, or suspended while it sent the package, which the portal
             * takes a minute for -- is made again at the next start, three times in all. 508's first try had no result, and
             * every start after it said "tried once already" (the second, made by hand, ended when the owner went to another app).
             */
            const std::wstring tried = platform::localStateDirectory() + L"\\update-tried-" + widen(best);
            std::string record;
            if (files::read(tried, record)) {
                int tries = 0;
                for (size_t at = record.find(" installs "); at != std::string::npos; at = record.find(" installs ", at + 1)) tries++;
                if (record.find("\nresult: ") != std::string::npos || tries >= 3) {
                    shellLog::write(format("update: Megler %s in Pictures\\Megler was tried %d time%s already (%s)", best.c_str(), tries,
                                           tries == 1 ? "" : "s", narrow(tried).c_str()));
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
            files::append(tried, "Megler " + versionString() + " installs " + best + "\r\n");
            Made made;
            made.appx = std::wstring(bestFile.Path());
            made.certificate = std::wstring(certificate.Path());
            made.identity = "Megler " + best;
            shellLog::write("update: Megler " + best + " found in Pictures\\Megler (this is " + versionString() +
                            "): installed through the Device Portal, which closes this Megler");
            const PortalResult result = installThroughPortal(made, [](const std::string& step) { shellLog::write("update: " + step); });
            shellLog::write("update: " + std::string(result.ok ? "installed: " : "not installed: ") + result.said);
            // A portal that could not be reached, or dropped the connection while the package went (0x80072eff, 512), said nothing
            // of the package: that try is one without a result, made again.
            const bool unreached = !result.ok && result.said.rfind("The Device Portal could not be reached", 0) == 0;
            files::append(tried, std::string(unreached ? "no result: " : "result: ") + (result.ok ? "installed: " : "not installed: ") + result.said + "\r\n");
        } catch (hresult_error const& e) {
            shellLog::write(format("update: Pictures\\Megler could not be read: 0x%08x ", unsigned(e.code())) + narrow(e.message()));
        }
    }).detach();
}

}  // namespace megler::app::appPackage
