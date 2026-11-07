// The test program's way to the Xbox build's Device Portal client (app/Megler/shell/DevicePortal.cpp), for mg_tests
// --portal-windows (mg_tests_main.c): the client against tools/device-portal-mock.py, a portal of the tests' own that
// answers as an Xbox's does -- Basic authentication, the CSRF token handed out as a cookie and wanted back on every POST and
// DELETE, an install sent as a form of files and its state asked until it says, the list of the packages, an uninstall --
// over TLS with a certificate nothing vouches for, made for another name, as the console's own is.
//
// What it shows: the client's own work against a server that refuses what a portal refuses -- the handshake, the
// credentials and their refusal, the token, the package the server receives byte for byte, a refusal's reason, an install
// another client's holds up, a portal that is not there. What it cannot show: the console's portal itself (the mock is
// made from Microsoft's WindowsDevicePortalWrapper and DesktopMode_uwp's installer, not from an Xbox), nor an app container
// on an Xbox, which reaches the console's own address only with a loopback exemption.
//
// Only tools/build-tests.sh win builds this file, with DevicePortal.cpp itself; the shell's log it writes to, and Strings.cpp,
// are mg_storage_windows.cpp's company.
#include "pch.h"

#include "shell/DevicePortal.h"
#include "shell/Strings.h"

#include "megler.h"

#include <cstdio>
#include <fstream>
#include <sstream>

namespace {

using namespace megler::app;

int gChecks = 0, gFailed = 0;

void check(bool ok, const std::string& what, const std::string& detail = {}) {
    gChecks++;
    if (ok) return;
    gFailed++;
    printf("  FAIL: %s%s%s\n", what.c_str(), detail.empty() ? "" : ": ", detail.c_str());
    fflush(stdout);
}

bool readAll(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::ostringstream s;
    s << f.rdbuf();
    out = s.str();
    return true;
}

/// A package as Megler makes one on the device: the core's packer and signer (core/src/appx), a manifest of `identity` and
/// a file or two; its path, empty when it could not be made.
std::string makePackage(const std::string& scratch, const std::string& file, const std::string& identity, const std::string& version) {
    char err[400] = {};
    megler_appx_key* key = megler_appx_key_open((scratch + "\\key.bin").c_str(), (scratch + "\\MeglerApps.cer").c_str(), "CN=Computershik", err,
                                                sizeof(err));
    if (!key) {
        printf("  the signing key could not be made: %s\n", err);
        return {};
    }
    const std::string path = scratch + "\\" + file;
    megler_appx* a = megler_appx_begin(path.c_str(), err, sizeof(err));
    bool ok = a != nullptr;
    if (a) {
        const std::string manifest =
            "<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n"
            "<Package xmlns=\"http://schemas.microsoft.com/appx/manifest/foundation/windows10\">\r\n"
            "  <Identity Name=\"" + identity + "\" Publisher=\"CN=Computershik\" Version=\"" + version + "\" ProcessorArchitecture=\"x64\" />\r\n"
            "  <Properties><DisplayName>Portal test</DisplayName><PublisherDisplayName>Computershik</PublisherDisplayName>"
            "<Logo>Assets\\StoreLogo.png</Logo></Properties>\r\n"
            "</Package>\r\n";
        std::string payload(200000, '\0');
        for (size_t i = 0; i < payload.size(); i++) payload[i] = char((i * 7919u) >> 3);
        megler_appx_add_data(a, "AppxManifest.xml", manifest.data(), manifest.size());
        megler_appx_add_data(a, "payload.bin", payload.data(), payload.size());
        ok = megler_appx_finish(a, key, err, sizeof(err));
    }
    megler_appx_key_close(key);
    if (!ok) {
        printf("  the package could not be made: %s\n", err);
        return {};
    }
    return path;
}

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

const devicePortal::Package* find(const std::vector<devicePortal::Package>& list, const std::string& fullName) {
    for (const auto& p : list)
        if (p.fullName == fullName) return &p;
    return nullptr;
}

/// A POST with no token, by an HTTP client of its own that takes the mock's certificate as it is: refused 403, or the
/// client's success below would prove nothing of its token.
int postWithoutToken(const std::wstring& address, const std::string& user, const std::string& password) {
    using namespace winrt::Windows::Web::Http;
    using namespace winrt::Windows::Web::Http::Filters;
    using winrt::Windows::Security::Cryptography::Certificates::ChainValidationResult;
    try {
        HttpBaseProtocolFilter filter;
        filter.AllowUI(false);
        filter.IgnorableServerCertificateErrors().Append(ChainValidationResult::Untrusted);
        filter.IgnorableServerCertificateErrors().Append(ChainValidationResult::InvalidName);
        HttpClient client(filter);
        HttpRequestMessage r(HttpMethod::Post(), winrt::Windows::Foundation::Uri(address + L"/api/app/packagemanager/package?package=x.appx"));
        using namespace winrt::Windows::Security::Cryptography;
        r.Headers().Authorization(Headers::HttpCredentialsHeaderValue(
            L"Basic", CryptographicBuffer::EncodeToBase64String(CryptographicBuffer::ConvertStringToBinary(widen(user + ":" + password), BinaryStringEncoding::Utf8))));
        r.Content(HttpStringContent(L"x"));
        return int(client.SendRequestAsync(r).get().StatusCode());
    } catch (const winrt::hresult_error& e) {
        printf("  the POST without a token was not answered: 0x%08X\n", unsigned(e.code()));
        return 0;
    }
}

/// Whether a client that trusts what Windows trusts refuses the mock's certificate: the portal's own is one nothing vouches
/// for, and the client takes it as it is from the machine's own address alone.
bool certificateRefusedByDefault(const std::wstring& address) {
    using namespace winrt::Windows::Web::Http;
    try {
        HttpClient client;
        client.GetAsync(winrt::Windows::Foundation::Uri(address + L"/api/os/info")).get();
        return false;
    } catch (const winrt::hresult_error&) {
        return true;
    }
}

}  // namespace

extern "C" int mg_portal_windows_run(const char* address, const char* user, const char* password, const char* scratch, const char* received,
                                     const char* mockLog, int* checks) {
    using namespace megler::app::devicePortal;
    try {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    } catch (...) {
    }
    const std::wstring base = widen(address);
    const Credentials good{user, password};
    std::vector<std::string> said;
    const auto progress = [&said](const std::string& line) { said.push_back(line); };

    // A portal that is not there: nothing listens on port 1.
    {
        Client nowhere(L"https://127.0.0.1:1", good);
        const Answer a = nowhere.open();
        check(!a.ok && !a.reached && a.status == 0, "a portal that is not there is not reached", a.said);
        check(contains(a.said, "could not reach the Device Portal at https://127.0.0.1:1"), "it says it could not reach the portal", a.said);
    }
    const bool https = base.rfind(L"https:", 0) == 0;
    if (https) check(certificateRefusedByDefault(base), "the mock's certificate is one a client that checks it refuses");

    // No user name and password, and wrong ones: 401, which the Library turns into its question for them.
    {
        Client anonymous(base, {});
        const Answer a = anonymous.open();
        check(!a.ok && a.reached && a.wantsSignIn(), "without a user name and password the portal answers 401", format("%d %s", a.status, a.said.c_str()));
        check(contains(a.said, "asks for its user name and password"), "it says the portal asks for them", a.said);
        Client wrong(base, {user, std::string(password) + "x"});
        const Answer b = wrong.open();
        check(!b.ok && b.wantsSignIn(), "a wrong password is refused 401", format("%d", b.status));
        check(contains(b.said, "did not take the user name and password"), "it says the portal did not take them", b.said);
    }

    Client portal(base, good);
    const Answer opened = portal.open();
    check(opened.ok && opened.status == 200, "the portal answers with the right user name and password", opened.said);
    check(portal.hasToken(), "the portal handed out a CSRF token");
    check(contains(portal.cookieNames(), "CSRF-Token") && contains(portal.cookieNames(), "WMID"), "both its cookies are kept", portal.cookieNames());
    check(postWithoutToken(base, user, password) == 403, "the mock refuses a POST without the token (so the client's are proof of its token)");

    std::vector<Package> list;
    Answer listed = portal.packages(list);
    check(listed.ok, "the list of the packages comes", listed.said);
    check(list.size() == 2 && find(list, "Microsoft.Xbox.DevHome_100.2410.0.0_x64__8wekyb3d8bbwe"), "it holds the console's own two",
          format("%u packages", unsigned(list.size())));

    // A package of Megler's making, installed: the mock answers 204 to the first two questions of its state, as another
    // client's install would, and to two after the form, as an install that takes its time.
    const std::string identity = "MeglerApp.portal.test.00000000000000000000000000000Xy";
    const std::string package = makePackage(scratch, "MeglerApp.portal.test.appx", identity, "0.1.0.7");
    check(!package.empty(), "a package is made to send");
    if (!package.empty()) {
        const Answer installed = portal.install(widen(package), {}, progress, 200, 60);
        check(installed.ok && installed.said == "Installed.", "the package is installed", installed.said);
        bool waited = false, sending = false, installing = false;
        for (const auto& line : said) {
            waited = waited || contains(line, "installing another package: waiting");
            sending = sending || contains(line, "Sending the package (");
            installing = installing || line == "Installing…";
        }
        check(waited, "an install another client's holds up is waited for");
        check(sending && installing, "the steps are told: sending, installing");
        std::string sent, got;
        check(readAll(package, sent) && readAll(std::string(received) + "\\MeglerApp.portal.test.appx", got) && sent == got,
              "the portal received the package byte for byte", format("%u sent, %u received", unsigned(sent.size()), unsigned(got.size())));

        const std::string full = identity + "_0.1.0.7_x64__7p434m16tbgc6";
        listed = portal.packages(list);
        const Package* p = find(list, full);
        check(listed.ok && p, "the list has the installed package under its full name", full);
        if (p) check(p->version == "0.1.0.7" && p->origin == 5 && p->familyName == identity + "_7p434m16tbgc6", "its version, origin and family",
                     p->version + " " + std::to_string(p->origin) + " " + p->familyName);

        // One the portal refuses: its reason, as the deployment engine gave it, is what the user is told.
        const std::string refused = makePackage(scratch, "refuse-me.appx", "MeglerApp.portal.refused", "0.1.0.7");
        said.clear();
        const Answer no = portal.install(widen(refused), {widen(std::string(scratch) + "\\MeglerApps.cer")}, progress, 200, 60);
        check(!no.ok && contains(no.said, "the mock refuses it") && contains(no.said, "(0x80073cf9)"), "a refused install says the portal's reason and code",
              no.said);
        std::string certificate;
        check(readAll(std::string(received) + "\\MeglerApps.cer", certificate) && !certificate.empty(), "a file sent beside the package arrives too");

        const Answer gone = portal.uninstall(full);
        check(gone.ok, "the package is uninstalled", gone.said);
        listed = portal.packages(list);
        check(listed.ok && !find(list, full), "the list no longer has it");
        const Answer again = portal.uninstall(full);
        check(!again.ok && again.status == 404 && contains(again.said, "No package is installed by that name"),
              "an uninstall of what is not there says the portal's reason", again.said);
    }

    // What the mock saw: every request with Basic authentication but the anonymous one's, every POST and DELETE with the
    // token in its Cookie header and in X-CSRF-Token but the one sent without on purpose.
    std::string log;
    check(readAll(mockLog, log), "the mock's log is there");
    int requests = 0, changes = 0, withToken = 0;
    std::istringstream lines(log);
    for (std::string line; std::getline(lines, line);) {
        requests++;
        if (line.rfind("POST ", 0) == 0 || line.rfind("DELETE ", 0) == 0) {
            changes++;
            if (contains(line, "cookie=csrf csrf=yes")) withToken++;
        }
    }
    check(changes >= 4 && withToken == changes - 1, "every POST and DELETE of the client's carried the token, cookie and header",
          format("%d of %d", withToken, changes));
    *checks = gChecks;
    printf("  %d requests reached the mock\n", requests);
    return gFailed;
}
