#include "pch.h"

#include "shell/AppPackage.h"

#include "megler.h"
#include "library/Library.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

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

/// A folder and everything in it; false at the first failure.
bool copyTree(const std::wstring& from, const std::wstring& to, uint64_t& copied) {
    if (!files::makeDirectory(to)) return false;
    for (const auto& name : files::list(from, L"*")) {
        if (!files::copy(from + L"\\" + name, to + L"\\" + name)) return false;
        copied += files::size(to + L"\\" + name);
    }
    for (const auto& name : files::listFolders(from, L"*"))
        if (!copyTree(from + L"\\" + name, to + L"\\" + name, copied)) return false;
    return true;
}

/// A name a package's part may have as it is (core/src/appx/hk_appx.c): letters, digits, '.', '-', '_' and '/'.
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
    for (const auto& name : files::listFolders(dir, L"*")) {
        if (rel.empty() && (sameName(name, L"android") || sameName(name, L"selftest") || sameName(name, L"demo") ||
                            sameName(name, L"codec") || sameName(name, L"app") || sameName(name, L"AppxMetadata") ||
                            sameName(name, L"appx-template")))
            continue;
        collect(root, rel.empty() ? name : rel + L"\\" + name, out, skipped);
    }
}

std::string oneLine(std::string s) {
    for (char& c : s)
        if (c == '\r' || c == '\n') c = ' ';
    return s;
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

Made make(const ApkEntry& entry, const std::function<void(const std::string&)>& progress) {
    Made made;
    auto fail = [&made](std::string why) {
        made.error = std::move(why);
        shellLog::write("app package: " + made.error);
        return made;
    };
    const ApkReport& r = entry.report;
    if (!entry.splitsDir.empty()) return fail("An app of split APKs can't be made a package of its own yet.");
    if (!entry.analysed || !r.valid || r.package.empty()) return fail("The APK hasn't been read.");
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

    std::string meglerManifest;
    if (!files::read(install + L"\\AppxManifest.xml", meglerManifest)) return fail("Megler's own manifest could not be read.");
    const std::string label = oneLine(entry.title());
    char* manifest = megler_appx_app_manifest(meglerManifest.c_str(), r.package.c_str(), label.c_str(), err, sizeof(err));
    if (!manifest) return fail(std::string("The manifest could not be made: ") + err);
    const std::string manifestText(manifest);
    megler_free(manifest);
    char identity[64] = {};
    megler_appx_app_identity(r.package.c_str(), identity, sizeof(identity));
    made.identity = identity;

    // resources.pri: XAML wants one whose resource map has the package's name. Megler carries one made for a placeholder
    // as long as every app package's name, and the name goes in its place (core/src/appx/hk_appx_pri.c).
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
    for (const auto& [name, path] : parts) megler_appx_add_file(a, name.c_str(), narrow(path).c_str());
    megler_appx_add_data(a, "AppxManifest.xml", manifestText.data(), manifestText.size());
    megler_appx_add_data(a, "resources.pri", pri.data(), pri.size());
    const std::string ini = "package=" + r.package + "\r\nlabel=" + label + "\r\napk=app\r\nroot=android\\" + version + "\r\n";
    megler_appx_add_data(a, "MeglerApp.ini", ini.data(), ini.size());
    megler_appx_add_file(a, "app/app.apk", narrow(entry.path).c_str());
    if (files::exists(entry.path + L".json")) megler_appx_add_file(a, "app/app.apk.json", narrow(entry.path + L".json").c_str());
    if (files::exists(entry.path + L".icon")) megler_appx_add_file(a, "app/app.apk.icon", narrow(entry.path + L".icon").c_str());
    const std::wstring stamp = Library::instance().directory() + L"\\reports.version";
    if (files::exists(stamp)) megler_appx_add_file(a, "app/reports.version", narrow(stamp).c_str());
    progress("Signing the package…");
    if (!megler_appx_finish(a, key, err, sizeof(err))) return fail(std::string("The package could not be made: ") + err);
    shellLog::write("app package: " + narrow(made.appx) + " made, " + formattedByteCount(files::size(made.appx)) + ", for " + r.package);
    return made;
}

std::string installThroughPortal(const Made& made, const std::function<void(const std::string&)>& progress) {
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Web::Http::Filters;
    using namespace winrt::Windows::Web::Http::Headers;
    using winrt::Windows::Foundation::Uri;
    using winrt::Windows::Security::Cryptography::Certificates::ChainValidationResult;
    const std::wstring base = L"https://127.0.0.1";
    try {
        // The portal's certificate is one the phone made for itself, which nothing vouches for: taken as it is, here only,
        // where the address is the phone's own.
        HttpBaseProtocolFilter filter;
        filter.AllowUI(false);
        for (auto e : {ChainValidationResult::Untrusted, ChainValidationResult::InvalidName, ChainValidationResult::Expired,
                       ChainValidationResult::IncompleteChain})
            filter.IgnorableServerCertificateErrors().Append(e);
        filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::MostRecent);
        HttpClient client(filter);

        progress("Asking the Device Portal…");
        // The portal hands out its CSRF token as a cookie; whatever changes something has to echo it in a header.
        auto info = client.GetAsync(Uri(base + L"/api/os/info")).get();
        if (info.StatusCode() == HttpStatusCode::Unauthorized)
            return "The Device Portal asks for a user name and password, which Megler doesn't send yet.";
        if (!info.IsSuccessStatusCode()) return format("The Device Portal answered %d.", int(info.StatusCode()));
        std::wstring token;
        for (auto const& cookie : filter.CookieManager().GetCookies(Uri(base)))
            if (cookie.Name() == L"CSRF-Token") token = cookie.Value().c_str();

        HttpMultipartFormDataContent form;
        for (const std::wstring& path : {made.appx, made.certificate}) {
            auto file = StorageFile::GetFileFromPathAsync(path).get();
            HttpStreamContent part(file.OpenAsync(FileAccessMode::Read).get());
            part.Headers().ContentType(HttpMediaTypeHeaderValue(L"application/octet-stream"));
            form.Add(part, file.Name(), file.Name());
        }
        const std::wstring appxName = made.appx.substr(made.appx.find_last_of(L'\\') + 1);
        HttpRequestMessage request(HttpMethod::Post(),
                                   Uri(base + L"/api/app/packagemanager/package?package=" + std::wstring(Uri::EscapeComponent(appxName))));
        if (!token.empty()) request.Headers().Insert(L"X-CSRF-Token", token);
        request.Content(form);
        progress("Sending the package (" + formattedByteCount(files::size(made.appx)) + ") to the Device Portal…");
        auto sent = client.SendRequestAsync(request).get();
        if (!sent.IsSuccessStatusCode())
            return format("The Device Portal refused the package (%d): ", int(sent.StatusCode())) + narrow(sent.Content().ReadAsStringAsync().get());

        // It answers the state 204 while it is still installing.
        progress("Installing…");
        for (int i = 0; i < 300; i++) {
            Sleep(2000);
            auto state = client.GetAsync(Uri(base + L"/api/app/packagemanager/state")).get();
            if (state.StatusCode() == HttpStatusCode::NoContent) continue;
            const hstring body = state.Content().ReadAsStringAsync().get();
            winrt::Windows::Data::Json::JsonObject json{nullptr};
            if (winrt::Windows::Data::Json::JsonObject::TryParse(body, json) && json.GetNamedBoolean(L"Success", false)) {
                shellLog::write("app package: " + made.identity + " installed through the Device Portal");
                return "Installed.";
            }
            const std::string said = narrow(body);
            shellLog::write("app package: the Device Portal said " + said);
            return format("Not installed (%d): ", int(state.StatusCode())) + said;
        }
        return "The Device Portal was still installing after ten minutes.";
    } catch (hresult_error const& e) {
        shellLog::write(format("app package: the Device Portal: 0x%08x ", unsigned(e.code())) + narrow(e.message()));
        return "The Device Portal could not be reached on this phone: " + narrow(e.message()) + format(" (0x%08x)", unsigned(e.code()));
    }
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

}  // namespace megler::app::appPackage
