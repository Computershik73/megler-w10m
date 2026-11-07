#include "pch.h"

#include "shell/DevicePortal.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Security.Credentials.h>
#include <winrt/Windows.Security.Cryptography.h>

#include <chrono>

namespace megler::app::devicePortal {

using namespace winrt;
using namespace winrt::Windows::Data::Json;
using namespace winrt::Windows::Storage;
using namespace winrt::Windows::Web::Http;
using namespace winrt::Windows::Web::Http::Filters;
using namespace winrt::Windows::Web::Http::Headers;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Foundation::Uri;
using winrt::Windows::Security::Cryptography::Certificates::ChainValidationResult;

namespace {

/// Whether `address` is the machine's own: the only place whose certificate is taken as it is.
bool loopback(const std::wstring& address) {
    try {
        const std::wstring host(Uri(address).Host());
        return _wcsicmp(host.c_str(), L"localhost") == 0 || host == L"127.0.0.1" || host == L"::1" || host == L"[::1]";
    } catch (hresult_error const&) {
        return false;
    }
}

std::string base64(const std::string& text) {
    using namespace winrt::Windows::Security::Cryptography;
    return narrow(CryptographicBuffer::EncodeToBase64String(CryptographicBuffer::ConvertStringToBinary(widen(text), BinaryStringEncoding::Utf8)));
}

/// A body in a line, at most `most` characters: what a refusal said, for the log and the user.
std::string oneLine(std::string text, size_t most = 300) {
    for (char& c : text)
        if (c == '\r' || c == '\n' || c == '\t') c = ' ';
    if (text.size() > most) text = text.substr(0, most) + "…";
    return text;
}

/// What the portal's JSON of a result says of it: its Reason, else its CodeText, with its Code.
std::string reasonOf(JsonObject const& json) {
    std::string reason = narrow(json.GetNamedString(L"Reason", L""));
    if (reason.empty()) reason = narrow(json.GetNamedString(L"CodeText", L""));
    const double code = json.GetNamedNumber(L"Code", 0);
    if (code != 0) reason += format(" (0x%08x)", unsigned(int64_t(code)));
    return reason;
}

}  // namespace

std::wstring consoleAddress() { return L"https://localhost:11443"; }

Client::Client(std::wstring address, Credentials credentials) : address_(std::move(address)), credentials_(std::move(credentials)) {
    while (!address_.empty() && address_.back() == L'/') address_.pop_back();
}

HttpRequestMessage Client::request(HttpMethod const& method, const std::wstring& path) {
    HttpRequestMessage r(method, Uri(address_ + path));
    // Basic authentication on every request, as WindowsDevicePortalWrapper's and DesktopMode_uwp's clients send it: the
    // portal asks for it on every one where its authentication is on.
    if (!credentials_.empty())
        r.Headers().Authorization(HttpCredentialsHeaderValue(L"Basic", widen(base64(credentials_.user + ":" + credentials_.password))));
    if (!cookies_.empty()) r.Headers().TryAppendWithoutValidation(L"Cookie", cookies_);
    if (!token_.empty())
        r.Headers().TryAppendWithoutValidation(L"X-CSRF-Token", token_);
    else
        r.Headers().TryAppendWithoutValidation(L"CSRF-Token", L"Fetch");
    return r;
}

namespace {

/// The cookies of `response`'s Set-Cookie headers put into `jar`: one or more, "name=value; attributes", several joined by
/// ", " (an attribute's date has a comma of its own, and the piece it is cut into is no "name=value").
void takeCookies(HttpResponseMessage const& response, std::map<std::wstring, std::wstring>& jar) {
    for (auto const& header : response.Headers()) {
        if (_wcsicmp(header.Key().c_str(), L"Set-Cookie")) continue;
        const std::wstring all(header.Value());
        size_t at = 0;
        while (at < all.size()) {
            const size_t end = all.find(L',', at);
            std::wstring one = all.substr(at, end == std::wstring::npos ? std::wstring::npos : end - at);
            at = end == std::wstring::npos ? all.size() : end + 1;
            while (!one.empty() && one[0] == L' ') one.erase(0, 1);
            const std::wstring pair = one.substr(0, one.find(L';'));
            const size_t eq = pair.find(L'=');
            if (eq == std::wstring::npos || eq == 0 || pair.find(L' ') != std::wstring::npos) continue;
            jar[pair.substr(0, eq)] = pair.substr(eq + 1);
        }
    }
}

}  // namespace

void Client::remember(HttpResponseMessage const& response) {
    takeCookies(response, jar_);
    cookies_.clear();
    for (const auto& [name, value] : jar_) {
        cookies_ += (cookies_.empty() ? L"" : L"; ") + name + L"=" + value;
        if (name == L"CSRF-Token") token_ = value;
    }
}

Answer Client::open() {
    if (opened_) return {true, 200, true, {}};
    try {
        HttpBaseProtocolFilter filter;
        filter.AllowUI(false);
        filter.AllowAutoRedirect(false);
        // The portal's certificate is one the console made for itself, which nothing vouches for, for another name than
        // localhost: taken as it is, from the machine's own address alone.
        if (loopback(address_))
            for (auto e : {ChainValidationResult::Untrusted, ChainValidationResult::InvalidName, ChainValidationResult::Expired,
                           ChainValidationResult::IncompleteChain, ChainValidationResult::RevocationFailure,
                           ChainValidationResult::RevocationInformationMissing})
                filter.IgnorableServerCertificateErrors().Append(e);
        filter.CacheControl().ReadBehavior(HttpCacheReadBehavior::MostRecent);
        filter.CacheControl().WriteBehavior(HttpCacheWriteBehavior::NoCache);
        // Cookies by hand, as tools/phone.py sends them (left to the filter's own cookies, a phone's portal refused an install
        // with 403, package 353): those it hands out go back in the Cookie header, the CSRF token in X-CSRF-Token too.
        filter.CookieUsageBehavior(HttpCookieUsageBehavior::NoCookies);
        client_ = HttpClient(filter);
        const auto info = client_.SendRequestAsync(request(HttpMethod::Get(), L"/api/os/info")).get();
        remember(info);
        if (!info.IsSuccessStatusCode()) return refused(info, "Megler's first question");
        opened_ = true;
        shellLog::write("device portal: " + narrow(address_) + " answered; its cookies: " + cookieNames() + (token_.empty() ? ", no CSRF token" : ", a CSRF token"));
        return {true, int(info.StatusCode()), true, {}};
    } catch (hresult_error const& e) {
        return unreachable(e);
    }
}

std::string Client::cookieNames() const {
    // (Their names alone: their values are the portal's session, which a log sent on is no place for.)
    std::string names;
    for (size_t at = 0; at < cookies_.size();) {
        const size_t end = cookies_.find(L"; ", at), eq = cookies_.find(L'=', at);
        names += (names.empty() ? "" : ", ") + narrow(cookies_.substr(at, (eq < end ? eq : end) - at));
        at = end == std::wstring::npos ? cookies_.size() : end + 2;
    }
    return names.empty() ? std::string("none") : names;
}

Answer Client::refused(HttpResponseMessage const& response, const std::string& what) {
    Answer a;
    a.reached = true;
    a.status = int(response.StatusCode());
    std::string body;
    try {
        body = narrow(response.Content().ReadAsStringAsync().get());
    } catch (hresult_error const&) {
    }
    JsonObject json{nullptr};
    const std::string reason = JsonObject::TryParse(widen(body), json) ? reasonOf(json) : oneLine(body);
    if (a.status == 401)
        a.said = credentials_.empty() ? "The Device Portal asks for its user name and password."
                                      : "The Device Portal did not take the user name and password Megler gave it.";
    else if (a.status == 403)
        a.said = "The Device Portal refused " + what + " (403: it did not take the CSRF token it had handed out)" + (reason.empty() ? "." : ": " + reason);
    else
        a.said = format("The Device Portal answered %d to %s", a.status, what.c_str()) + (reason.empty() ? "." : ": " + reason);
    shellLog::write("device portal: " + a.said);
    return a;
}

Answer Client::unreachable(hresult_error const& e) {
    Answer a;
    a.said = "Megler could not reach the Device Portal at " + narrow(address_) + ": " + oneLine(narrow(e.message())) + format(" (0x%08x)", unsigned(e.code()));
    shellLog::write("device portal: " + a.said);
    return a;
}

Answer Client::install(const std::wstring& package, const std::vector<std::wstring>& others, const std::function<void(const std::string&)>& progress,
                       unsigned pollMs, unsigned timeoutSeconds) {
    if (Answer a = open(); !a.ok) return a;
    const auto started = std::chrono::steady_clock::now();
    const auto late = [&] { return std::chrono::steady_clock::now() - started > std::chrono::seconds(timeoutSeconds); };
    const std::wstring name = package.substr(package.find_last_of(L'\\') + 1);
    try {
        /*
         * The portal installs one package at a time, the console's, whoever asks, and answers its state 204 while one goes:
         * another client's install is waited for, a minute at most, as DesktopMode_uwp's installer checks before it sends.
         */
        for (int i = 0;; i++) {
            const auto state = client_.SendRequestAsync(request(HttpMethod::Get(), L"/api/app/packagemanager/state")).get();
            remember(state);
            if (state.StatusCode() != HttpStatusCode::NoContent) break;
            if (i >= 60) return {false, 204, true, "The Device Portal was installing another package for a minute: try again when it is done."};
            if (i == 0) progress("The Device Portal is installing another package: waiting…");
            Sleep(1000);
        }

        // The package and what goes with it, each a part of the form named by its file's name, read where it lies.
        HttpMultipartFormDataContent form;
        uint64_t total = 0;
        std::vector<std::wstring> files{package};
        files.insert(files.end(), others.begin(), others.end());
        for (const std::wstring& path : files) {
            const StorageFile file = StorageFile::GetFileFromPathAsync(path).get();
            total += file.GetBasicPropertiesAsync().get().Size();
            HttpStreamContent part(file.OpenAsync(FileAccessMode::Read).get());
            part.Headers().ContentType(HttpMediaTypeHeaderValue(L"application/octet-stream"));
            form.Add(part, file.Name(), file.Name());
        }
        auto post = request(HttpMethod::Post(), L"/api/app/packagemanager/package?package=" + std::wstring(Uri::EscapeComponent(name)));
        post.Content(form);
        progress("Sending the package (" + formattedByteCount(total) + ") to the Device Portal…");
        shellLog::write("device portal: sending " + narrow(name) + format(" and %u more files, %s", unsigned(others.size()), formattedByteCount(total).c_str()));
        auto sending = client_.SendRequestAsync(post);
        // How much has gone, said from this thread: the operation tells it on one of the pool's.
        const auto sent = std::make_shared<std::atomic<uint64_t>>(0);
        sending.Progress([sent](auto const&, HttpProgress const& p) { sent->store(p.BytesSent); });
        unsigned shown = 0;
        while (sending.wait_for(std::chrono::seconds(1)) == AsyncStatus::Started) {
            const unsigned share = total ? unsigned(std::min<uint64_t>(sent->load() * 100 / total, 99)) : 0;
            if (share >= shown + 5) {
                shown = share;
                progress(format("Sending the package to the Device Portal: %u%%", share));
            }
            if (late()) {
                sending.Cancel();
                return {false, 0, true, format("The Device Portal had not taken the package after %u minutes.", timeoutSeconds / 60)};
            }
        }
        const auto answer = sending.get();
        remember(answer);
        if (!answer.IsSuccessStatusCode()) return refused(answer, "the package");
        // (Its answer may be the result itself, a refusal among them.)
        {
            const std::string body = narrow(answer.Content().ReadAsStringAsync().get());
            JsonObject json{nullptr};
            if (JsonObject::TryParse(widen(body), json) && json.HasKey(L"Success") && !json.GetNamedBoolean(L"Success", true)) {
                Answer a{false, int(answer.StatusCode()), true, "The Device Portal did not install it: " + reasonOf(json)};
                shellLog::write("device portal: " + a.said);
                return a;
            }
        }

        // Its state, until it says how the install went: 204 while it goes on.
        progress("Installing…");
        while (!late()) {
            Sleep(pollMs);
            const auto state = client_.SendRequestAsync(request(HttpMethod::Get(), L"/api/app/packagemanager/state")).get();
            remember(state);
            if (state.StatusCode() == HttpStatusCode::NoContent) continue;
            const std::string body = narrow(state.Content().ReadAsStringAsync().get());
            JsonObject json{nullptr};
            const bool parsed = JsonObject::TryParse(widen(body), json);
            if (state.IsSuccessStatusCode() && parsed && json.GetNamedBoolean(L"Success", false)) {
                shellLog::write("device portal: " + narrow(name) + " installed");
                return {true, int(state.StatusCode()), true, "Installed."};
            }
            if (!state.IsSuccessStatusCode() && !parsed) return refused(state, "the question how the install went");
            Answer a{false, int(state.StatusCode()), true, "The Device Portal did not install it: " + (parsed ? reasonOf(json) : oneLine(body))};
            shellLog::write("device portal: " + a.said);
            return a;
        }
        return {false, 0, true,
                format("The Device Portal was still installing after %u minutes; the install may yet end, which the list of the apps will say.",
                       timeoutSeconds / 60)};
    } catch (hresult_error const& e) {
        return unreachable(e);
    }
}

Answer Client::uninstall(const std::string& fullName) {
    if (Answer a = open(); !a.ok) return a;
    try {
        const auto answer = client_.SendRequestAsync(request(HttpMethod::Delete(),
                                                             L"/api/app/packagemanager/package?package=" + std::wstring(Uri::EscapeComponent(widen(fullName)))))
                                .get();
        remember(answer);
        if (!answer.IsSuccessStatusCode()) return refused(answer, "the uninstall of " + fullName);
        shellLog::write("device portal: " + fullName + " uninstalled");
        return {true, int(answer.StatusCode()), true, "Uninstalled."};
    } catch (hresult_error const& e) {
        return unreachable(e);
    }
}

Answer Client::packages(std::vector<Package>& out, unsigned timeoutSeconds) {
    out.clear();
    if (Answer a = open(); !a.ok) return a;
    try {
        auto listing = client_.SendRequestAsync(request(HttpMethod::Get(), L"/api/app/packagemanager/packages"));
        if (listing.wait_for(std::chrono::seconds(timeoutSeconds)) == AsyncStatus::Started) {
            listing.Cancel();
            Answer a{false, 0, true, format("The Device Portal answered, but its list of the packages did not come within %u s.", timeoutSeconds)};
            shellLog::write("device portal: " + a.said);
            return a;
        }
        const auto answer = listing.get();
        remember(answer);
        if (!answer.IsSuccessStatusCode()) return refused(answer, "the list of the packages");
        JsonObject json{nullptr};
        if (!JsonObject::TryParse(answer.Content().ReadAsStringAsync().get(), json))
            return {false, int(answer.StatusCode()), true, "The Device Portal's list of the packages could not be read."};
        for (auto const& value : json.GetNamedArray(L"InstalledPackages", JsonArray())) {
            if (value.ValueType() != JsonValueType::Object) continue;
            const JsonObject item = value.GetObject();
            Package p;
            p.name = narrow(item.GetNamedString(L"Name", L""));
            p.fullName = narrow(item.GetNamedString(L"PackageFullName", L""));
            p.familyName = narrow(item.GetNamedString(L"PackageFamilyName", L""));
            p.origin = int(item.GetNamedNumber(L"PackageOrigin", 0));
            if (item.HasKey(L"Version") && item.GetNamedValue(L"Version").ValueType() == JsonValueType::Object) {
                const JsonObject v = item.GetNamedObject(L"Version");
                p.version = format("%u.%u.%u.%u", unsigned(v.GetNamedNumber(L"Major", 0)), unsigned(v.GetNamedNumber(L"Minor", 0)),
                                   unsigned(v.GetNamedNumber(L"Build", 0)), unsigned(v.GetNamedNumber(L"Revision", 0)));
            }
            if (!p.fullName.empty()) out.push_back(std::move(p));
        }
        return {true, int(answer.StatusCode()), true, {}};
    } catch (hresult_error const& e) {
        return unreachable(e);
    }
}

namespace {

std::mutex gCredentialsMutex;
Credentials gCredentials;
bool gCredentialsLoaded = false;
constexpr const wchar_t* kVaultResource = L"Megler: the Xbox's Device Portal";

}  // namespace

Credentials savedCredentials() {
    std::lock_guard<std::mutex> lock(gCredentialsMutex);
    if (!gCredentialsLoaded) {
        gCredentialsLoaded = true;
        try {
            using namespace winrt::Windows::Security::Credentials;
            // (FindAllByResource throws where there are none.)
            const auto all = PasswordVault().FindAllByResource(kVaultResource);
            if (all.Size()) {
                PasswordCredential c = all.GetAt(0);
                c.RetrievePassword();
                gCredentials = {narrow(c.UserName()), narrow(c.Password())};
            }
        } catch (hresult_error const&) {
        }
    }
    return gCredentials;
}

void saveCredentials(const Credentials& credentials) {
    std::lock_guard<std::mutex> lock(gCredentialsMutex);
    gCredentials = credentials;
    gCredentialsLoaded = true;
    try {
        using namespace winrt::Windows::Security::Credentials;
        PasswordVault vault;
        try {
            for (auto const& old : vault.FindAllByResource(kVaultResource)) vault.Remove(old);
        } catch (hresult_error const&) {
        }
        if (!credentials.user.empty() && !credentials.password.empty())
            vault.Add(PasswordCredential(kVaultResource, widen(credentials.user), widen(credentials.password)));
    } catch (hresult_error const& e) {
        shellLog::write(format("device portal: the user name and password are kept for this run alone: Windows's credential locker refused them (0x%08x)",
                               unsigned(e.code())));
    }
}

void forgetCredentials() {
    std::lock_guard<std::mutex> lock(gCredentialsMutex);
    gCredentials = Credentials();
    gCredentialsLoaded = true;
    try {
        using namespace winrt::Windows::Security::Credentials;
        PasswordVault vault;
        for (auto const& old : vault.FindAllByResource(kVaultResource)) vault.Remove(old);
    } catch (hresult_error const&) {
    }
}

}  // namespace megler::app::devicePortal
