#include "pch.h"

#include "ui/Ota.h"

#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"

#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Security.Cryptography.Core.h>
#include <winrt/Windows.Security.Cryptography.h>

#include <ctime>
#include <optional>

namespace megler::app::ui::ota {

using namespace winrt;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Foundation;
using namespace winrt::Windows::Storage::Streams;
using namespace winrt::Windows::Web::Http;

namespace {

constexpr const wchar_t* releasesUrl = L"https://api.github.com/repos/Computershik73/megler-w10m/releases?per_page=30";
/// GitHub is asked at most this often at start (its API answers 60 asks an hour from one address without a sign-in).
constexpr int64_t askEverySeconds = 6 * 3600;
/// Room for Windows to install the package once it is here, besides the package itself.
constexpr uint64_t installRoom = uint64_t(1200) << 20;

/// The newest Megler for this one on GitHub, newer than it.
struct Found {
    std::string version, tag, file, url, certificateUrl, sha256, notes;
    uint64_t size = 0;
};

enum class Step { idle, asking, found, offering, downloading, installing };

std::mutex gMutex;
Step gStep = Step::idle;
std::optional<Found> gFound;
std::string gSummary;           // Settings' line
std::string gDeclined;          // the version "Later" was said to, this start
bool gAskNow = false;           // Check Now: ask at the next poll, and offer even what "Later" was said to
bool gForced = false;           // ... the ask under way is one of those
bool gTidied = false;
std::atomic<uint64_t> gGot{0}, gTotal{0};

std::wstring updateFolder() { return platform::localStateDirectory() + L"\\update"; }
std::wstring askedFile() { return platform::localStateDirectory() + L"\\ota-asked.txt"; }

std::string userAgent() { return "Megler/" + appPackage::meglerVersion() + " (" + platform::deviceFamily() + ")"; }

/// A client that keeps nothing it fetches in Megler's internet cache (its package's AC\INetCache): HttpClient keeps what it
/// can there, and the 373 MB package of 639 that the OTA downloaded stayed in that cache on a Lumia 650 (package 663).
HttpClient uncachedClient() {
    using namespace winrt::Windows::Web::Http::Filters;
    HttpBaseProtocolFilter filter;
    filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
    filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::MostRecent);
    return HttpClient(filter);
}

std::string clock() {
    const std::time_t now = std::time(nullptr);
    std::tm local = {};
    localtime_s(&local, &now);
    return format("%02d:%02d", local.tm_hour, local.tm_min);
}

void setSummary(const std::string& line) {
    std::lock_guard<std::mutex> lock(gMutex);
    gSummary = line;
}

/// When GitHub was last asked at start (seconds since 1970), 0 never.
int64_t lastAsked() {
    std::string text;
    return files::read(askedFile(), text) ? _atoi64(text.c_str()) : 0;
}

/// A release's tag, "v0.1.0.637", as its version, "0.1.0.637"; empty when it is not four numbers.
std::string versionOf(const std::string& tag) {
    const std::string v = !tag.empty() && (tag[0] == 'v' || tag[0] == 'V') ? tag.substr(1) : tag;
    unsigned n[4] = {};
    char extra = 0;
    return sscanf_s(v.c_str(), "%u.%u.%u.%u%c", &n[0], &n[1], &n[2], &n[3], &extra, 1) == 4 ? v : std::string();
}

/// The newest release (not a draft, not a pre-release) with a package for this Megler, from GitHub's list of them: its
/// asset named Megler_<version>_<suffix>.appx (or .msix), and Megler.cer beside it. Nothing where none has one.
std::optional<Found> newestFor(const std::string& json, const std::string& suffix, std::string& error) {
    JsonArray releases{nullptr};
    if (!JsonArray::TryParse(widen(json), releases)) {
        error = "GitHub's answer could not be read";
        return std::nullopt;
    }
    std::optional<Found> best;
    for (const IJsonValue& value : releases) {
        if (value.ValueType() != JsonValueType::Object) continue;
        const JsonObject release = value.GetObject();
        if (release.GetNamedBoolean(L"draft", false) || release.GetNamedBoolean(L"prerelease", false)) continue;
        Found f;
        f.tag = narrow(release.GetNamedString(L"tag_name", L""));
        f.version = versionOf(f.tag);
        if (f.version.empty() || (best && !appPackage::olderVersion(best->version, f.version))) continue;
        f.notes = narrow(release.GetNamedString(L"body", L""));
        const std::string appx = "Megler_" + f.version + "_" + suffix + ".appx", msix = "Megler_" + f.version + "_" + suffix + ".msix";
        for (const IJsonValue& item : release.GetNamedArray(L"assets", JsonArray())) {
            if (item.ValueType() != JsonValueType::Object) continue;
            const JsonObject asset = item.GetObject();
            const std::string name = narrow(asset.GetNamedString(L"name", L"")), url = narrow(asset.GetNamedString(L"browser_download_url", L""));
            if (name == "Megler.cer") f.certificateUrl = url;
            if (name != appx && name != msix) continue;
            f.file = name;
            f.url = url;
            f.size = uint64_t(asset.GetNamedNumber(L"size", 0));
            // GitHub's own SHA-256 of what was uploaded ("sha256:<hex>"); a release of before it said one has none.
            const IJsonValue digest = asset.GetNamedValue(L"digest", JsonValue::CreateNullValue());
            if (digest.ValueType() == JsonValueType::String) {
                const std::string d = narrow(digest.GetString());
                if (d.rfind("sha256:", 0) == 0) f.sha256 = d.substr(7);
            }
        }
        if (!f.file.empty() && !f.url.empty()) best = f;
    }
    return best;
}

/// What the release says is new: its "What's new" section, bold and list marks taken off, at most about 700 characters.
std::string whatsNew(const std::string& notes) {
    size_t at = notes.find("## What's new");
    if (at == std::string::npos) return {};
    at = notes.find('\n', at);
    if (at == std::string::npos) return {};
    size_t end = notes.find("\n## ", at);
    std::string part = notes.substr(at + 1, end == std::string::npos ? std::string::npos : end - at - 1), out;
    for (size_t i = 0; i < part.size(); i++) {
        if (part.compare(i, 2, "**") == 0) {
            i++;
            continue;
        }
        if (part[i] == '`' || part[i] == '\r') continue;
        out += part[i];
    }
    // Lines of a list item joined, the items one to a line.
    std::string joined;
    for (size_t i = 0; i < out.size(); i++) {
        if (out[i] == '\n') {
            size_t next = i + 1;
            while (next < out.size() && out[next] == ' ') next++;
            if (next < out.size() && out[next] == '-') {
                joined += "\n";
                i = next;
                continue;
            }
            joined += ' ';
            i = next - 1;
            continue;
        }
        joined += out[i];
    }
    while (!joined.empty() && (joined.front() == ' ' || joined.front() == '\n' || joined.front() == '-')) joined.erase(joined.begin());
    if (joined.size() > 700) joined = joined.substr(0, joined.rfind(' ', 700)) + "…";
    return joined;
}

/// GitHub asked (a worker thread): what it has for this Megler, newer than it, or why it could not be asked.
void ask() {
    const std::string suffix = packageSuffix(), current = appPackage::meglerVersion();
    std::string error;
    std::optional<Found> found;
    try {
        HttpClient client = uncachedClient();
        client.DefaultRequestHeaders().UserAgent().TryParseAdd(widen(userAgent()));
        client.DefaultRequestHeaders().Accept().TryParseAdd(L"application/vnd.github+json");
        HttpResponseMessage response = client.GetAsync(Uri(releasesUrl)).get();
        if (!response.IsSuccessStatusCode()) {
            error = format("GitHub answered %d", int(response.StatusCode()));
        } else {
            found = newestFor(narrow(response.Content().ReadAsStringAsync().get()), suffix, error);
        }
    } catch (hresult_error const& e) {
        error = format("GitHub could not be reached (0x%08x %s)", unsigned(e.code()), narrow(e.message()).c_str());
    }
    files::write(askedFile(), std::to_string(int64_t(std::time(nullptr))));
    std::lock_guard<std::mutex> lock(gMutex);
    if (!error.empty()) {
        shellLog::write("ota: " + error);
        gSummary = error + ", at " + clock();
        gStep = Step::idle;
        return;
    }
    if (!found || !appPackage::olderVersion(current, found->version)) {
        shellLog::write("ota: the newest Megler on GitHub for " + suffix + " is " + (found ? found->version : std::string("none")) + "; this is " + current);
        gSummary = "Megler " + current + " is the newest for " + suffix + ", asked GitHub at " + clock();
        gStep = Step::idle;
        return;
    }
    shellLog::write(format("ota: Megler %s is on GitHub for %s (%s, %s%s); this is %s", found->version.c_str(), suffix.c_str(), found->file.c_str(),
                           formattedByteCount(found->size).c_str(), found->sha256.empty() ? ", no SHA-256 said" : "", current.c_str()));
    gSummary = "Megler " + found->version + " is on GitHub, " + formattedByteCount(found->size) + " for " + suffix;
    gFound = found;
    gStep = Step::found;
}

/// The package downloaded into `path` (a worker thread), as it comes: its size and SHA-256 checked against GitHub's.
bool fetch(const std::string& url, const std::wstring& path, uint64_t size, const std::string& sha256, std::string& error) {
    try {
        HttpClient client = uncachedClient();
        client.DefaultRequestHeaders().UserAgent().TryParseAdd(widen(userAgent()));
        HttpResponseMessage response = client.GetAsync(Uri(widen(url)), HttpCompletionOption::ResponseHeadersRead).get();
        if (!response.IsSuccessStatusCode()) {
            error = format("GitHub answered %d for the file", int(response.StatusCode()));
            return false;
        }
        IInputStream input = response.Content().ReadAsInputStreamAsync().get();
        const std::wstring part = path + L".part";
        auto hash = winrt::Windows::Security::Cryptography::Core::HashAlgorithmProvider::OpenAlgorithm(
                        winrt::Windows::Security::Cryptography::Core::HashAlgorithmNames::Sha256())
                        .CreateHash();
        uint64_t got = 0;
        {
            file_handle out{CreateFile2(part.c_str(), GENERIC_WRITE, 0, CREATE_ALWAYS, nullptr)};
            if (!out) {
                error = format("%s could not be made (error %lu)", narrow(part).c_str(), GetLastError());
                return false;
            }
            Buffer buffer(1u << 20);
            for (;;) {
                IBuffer chunk = input.ReadAsync(buffer, buffer.Capacity(), InputStreamOptions::Partial).get();
                const uint32_t n = chunk.Length();
                if (!n) break;
                DWORD written = 0;
                if (!WriteFile(out.get(), chunk.data(), n, &written, nullptr) || written != n) {
                    error = format("the file could not be written (error %lu: is the phone full?)", GetLastError());
                    return false;
                }
                hash.Append(chunk);
                got += n;
                gGot = got;
            }
        }
        if (size && got != size) {
            files::remove(part);
            error = format("the file came short: %s of %s", formattedByteCount(got).c_str(), formattedByteCount(size).c_str());
            return false;
        }
        const std::string sum = narrow(winrt::Windows::Security::Cryptography::CryptographicBuffer::EncodeToHexString(hash.GetValueAndReset()));
        if (!sha256.empty() && _stricmp(sum.c_str(), sha256.c_str()) != 0) {
            files::remove(part);
            error = "the file's SHA-256 is " + sum + ", not GitHub's " + sha256;
            return false;
        }
        files::remove(path);
        if (!files::move(part, path)) {
            error = "the file could not be put in its place";
            return false;
        }
        shellLog::write(format("ota: %s downloaded, %s, SHA-256 %s%s", narrow(path).c_str(), formattedByteCount(got).c_str(), sum.c_str(),
                               sha256.empty() ? " (GitHub said none)" : ", as GitHub says"));
        return true;
    } catch (hresult_error const& e) {
        error = format("the download stopped (0x%08x %s)", unsigned(e.code()), narrow(e.message()).c_str());
        return false;
    }
}

/// The newer Megler downloaded, checked and installed (a worker thread). Windows's deployment engine closes this Megler.
void downloadAndInstall(Found f) {
    gGot = 0;
    gTotal = f.size;
    const std::wstring folder = updateFolder(), appx = folder + L"\\" + widen(f.file), certificate = folder + L"\\Megler.cer";
    std::string error;
    ULARGE_INTEGER free = {};
    files::makeDirectories(folder);
    if (GetDiskFreeSpaceExW(folder.c_str(), &free, nullptr, nullptr) && free.QuadPart < f.size + installRoom) {
        error = format("the phone has %s free: %s are needed for the file and for Windows to install it", formattedByteCount(free.QuadPart).c_str(),
                       formattedByteCount(f.size + installRoom).c_str());
    } else if (!fetch(f.url, appx, f.size, f.sha256, error)) {
        error = "Megler " + f.version + " was not downloaded: " + error;
    } else if (!f.certificateUrl.empty() && !fetch(f.certificateUrl, certificate, 0, {}, error)) {
        error = "Megler.cer was not downloaded: " + error;
    }
    if (!error.empty()) {
        shellLog::write("ota: " + error);
        std::lock_guard<std::mutex> lock(gMutex);
        gSummary = error;
        gStep = Step::idle;
        return;
    }
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gStep = Step::installing;
        gSummary = "Megler " + f.version + " is being installed";
    }
    const appPackage::InstallResult result = appPackage::installOwnUpdate(appx, certificate, f.version, "downloaded from GitHub (" + f.tag + ")");
    std::lock_guard<std::mutex> lock(gMutex);
    // (Installed, this Megler is closed by Windows before it gets here, as a rule.)
    gSummary = result.ok ? "Megler " + f.version + " installed: open Megler again" : "Megler " + f.version + " was not installed: " + result.said;
    gStep = Step::idle;
}

/// The files of 4 MB or more in Megler's internet cache (AC\INetCache, beside LocalState) removed: the packages HttpClient kept
/// there before the OTA asked it not to (uncachedClient). Windows never let 639's go: 373 MB of a Lumia 650's last 600.
void tidyInternetCache() {
    const std::wstring local = platform::localStateDirectory();
    const size_t slash = local.find_last_of(L'\\');
    if (slash == std::wstring::npos) return;
    const std::wstring cache = local.substr(0, slash) + L"\\AC\\INetCache";
    uint64_t freed = 0;
    int removed = 0;
    for (const auto& folder : files::listFolders(cache, L"*")) {
        const std::wstring dir = cache + L"\\" + folder;
        for (const auto& name : files::list(dir, L"*")) {
            const std::wstring path = dir + L"\\" + name;
            const uint64_t size = files::size(path);
            if (size >= (uint64_t(4) << 20) && files::remove(path)) {
                freed += size;
                removed++;
            }
        }
    }
    if (removed) shellLog::write(format("ota: %d files HttpClient had kept in Megler's internet cache removed, %s", removed, formattedByteCount(freed).c_str()));
}

/// What the last start's downloads left, of this Megler or older (the folder keeps nothing once its Megler is installed),
/// and what Megler's internet cache kept of them.
void tidy() {
    tidyInternetCache();
    const std::wstring folder = updateFolder();
    const std::string current = appPackage::meglerVersion();
    for (const auto& name : files::list(folder, L"*")) {
        const std::string n = narrow(name);
        const size_t under = n.find('_', 7);
        const std::string version = n.rfind("Megler_", 0) == 0 && under != std::string::npos ? n.substr(7, under - 7) : std::string();
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".part") == 0) {
            files::remove(folder + L"\\" + name);
        } else if (!version.empty() && !appPackage::olderVersion(current, version)) {
            files::remove(folder + L"\\" + name);
            shellLog::write("ota: " + n + " removed: this Megler is " + current);
        }
    }
}

/// Whether the phone's connection is one that is paid for by the byte.
bool metered() {
    try {
        using namespace winrt::Windows::Networking::Connectivity;
        const ConnectionProfile profile = NetworkInformation::GetInternetConnectionProfile();
        if (!profile) return false;
        const NetworkCostType cost = profile.GetConnectionCost().NetworkCostType();
        return cost == NetworkCostType::Fixed || cost == NetworkCostType::Variable;
    } catch (...) {
        return false;
    }
}

void offer(AppShell& shell, const Found& f) {
    const bool phone = platform::deviceFamily() == "Windows.Mobile";
    AppShell::DialogSpec spec;
    spec.title = "Megler " + f.version;
    const std::string news = whatsNew(f.notes);
    spec.message = format("A newer Megler is on GitHub: %s (this one is %s). Its package for this %s (%s) is %s%s: Megler downloads it, checks it and "
                          "installs it. Megler closes while Windows installs it, a few minutes%s; then open Megler again.",
                          f.version.c_str(), appPackage::meglerVersion().c_str(), phone ? "phone" : "computer", packageSuffix().c_str(),
                          formattedByteCount(f.size).c_str(), metered() ? ", over a connection paid for by the byte" : "",
                          phone ? " (leave the phone on)" : "") +
                   (news.empty() ? std::string() : "\n\nWhat's new:\n" + news);
    spec.buttons = {"Later", "Update"};
    std::weak_ptr<AppShell> weak = shell.weak_from_this();
    shell.dialog(std::move(spec), [weak, f](int button, std::string) {
        std::lock_guard<std::mutex> lock(gMutex);
        if (button != 1) {
            // Later (or dismissed): not offered again this start, unless Check Now asks.
            gDeclined = f.version;
            gStep = Step::idle;
            shellLog::write("ota: Megler " + f.version + " not now");
            return;
        }
        shellLog::write("ota: Megler " + f.version + " taken: downloading " + f.url);
        gStep = Step::downloading;
        gSummary = "Megler " + f.version + " is being downloaded";
        if (auto self = weak.lock()) self->runInBackground([f] { downloadAndInstall(f); });
    });
}

} // namespace

std::string packageSuffix() {
    static const std::string suffix = [] {
        using winrt::Windows::System::ProcessorArchitecture;
        const std::string family = platform::deviceFamily();
        ProcessorArchitecture arch = ProcessorArchitecture::Unknown;
        try {
            arch = winrt::Windows::ApplicationModel::Package::Current().Id().Architecture();
        } catch (...) {
        }
        switch (arch) {
        case ProcessorArchitecture::Arm:
            // The phone's own; a 32-bit ARM Megler on desktop Windows (an ARM64 computer runs it) takes the desktop's, ARM64.
            return std::string(family == "Windows.Mobile" ? "ARM" : family == "Windows.Desktop" ? "ARM64" : "");
        case ProcessorArchitecture::Arm64: return std::string("ARM64");
        case ProcessorArchitecture::X64: return std::string("x64");
        case ProcessorArchitecture::X86: return std::string("x86");
        default: return std::string();
        }
    }();
    return suffix;
}

bool atStart() { return settings::megler(settings::otaAtStart).value_or("") != "off"; }

void setAtStart(bool on) {
    settings::setMegler(settings::otaAtStart, on ? "" : "off");
    shellLog::write(std::string("ota: GitHub asked ") + (on ? "at start" : "only when asked"));
}

void checkNow() {
    std::lock_guard<std::mutex> lock(gMutex);
    gAskNow = true;
    if (gStep == Step::idle) gSummary = "Asking GitHub…";
}

std::string summary() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (!gSummary.empty()) return gSummary;
    return packageSuffix().empty() ? "GitHub has no Megler for this Windows and processor"
                                   : "Megler " + appPackage::meglerVersion() + " takes Megler_<version>_" + packageSuffix() + ".appx";
}

bool downloading(std::string& version, uint64_t& got, uint64_t& total) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gStep != Step::downloading || !gFound) return false;
    version = gFound->version;
    got = gGot;
    total = gTotal;
    return true;
}

bool busy() {
    std::lock_guard<std::mutex> lock(gMutex);
    return gStep == Step::downloading || gStep == Step::installing;
}

void tick(AppShell& shell) {
    if (appPackage::isSingleApp() || packageSuffix().empty()) return;
    if (!gTidied) {
        gTidied = true;
        shell.runInBackground([] { tidy(); });
    }
    std::unique_lock<std::mutex> lock(gMutex);
    switch (gStep) {
    case Step::idle: {
        const bool now = gAskNow;
        if (!now && (!atStart() || std::time(nullptr) - lastAsked() < askEverySeconds)) return;
        // (Not while Megler's own update from Pictures\Megler is being installed.)
        if (appPackage::selfUpdating()) return;
        gAskNow = false;
        gForced = now;
        gStep = Step::asking;
        lock.unlock();
        shell.runInBackground([] { ask(); });
        return;
    }
    case Step::found: {
        if (shell.dialogShown() || appPackage::selfUpdating() || !gFound) return;
        if (gFound->version == gDeclined && !gForced) {
            gStep = Step::idle;
            return;
        }
        gStep = Step::offering;
        const Found f = *gFound;
        lock.unlock();
        offer(shell, f);
        return;
    }
    default:
        return;
    }
}

} // namespace megler::app::ui::ota
