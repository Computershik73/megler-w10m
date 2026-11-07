// The Xbox's own Windows Device Portal, as Megler reaches it on the console it runs on (docs/XBOX.md): the one way an app
// there has to install packages -- Megler's own update and the apps' own packages -- to list them and to uninstall them.
// Only the Xbox's build has this file (tools/build-appx.sh x64 --xbox, which defines MEGLER_XBOX); a phone's and a PC's
// Megler install through Windows's deployment engine (shell/AppPackage.h) and never ask a Device Portal anything.
//
// The portal of an Xbox in Developer Mode answers on https://localhost:11443 (DesktopMode_uwp's App Installer, whose way this
// is, and its developer). Its certificate is one the console made for itself, which nothing vouches for: taken as it is, and
// only from an address of the machine's own. An app container may not reach that address unless Windows is told to let it
// (CheckNetIsolation's loopback exemption, which only an SSH session on the console can give: Megler says so).
//
// Every call asks the portal again from the start, as tools/phone.py does: a GET of /api/os/info (with "CSRF-Token: Fetch",
// as Microsoft's WindowsDevicePortalWrapper asks) for the portal's cookies, among them the CSRF token, which goes back in
// the Cookie header and in X-CSRF-Token on every request that changes something; a user name and password, where the
// portal's authentication wants them (Dev Home, Remote access settings), as Basic authentication on every request.
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace megler::app::devicePortal {

/// Where the portal of the Xbox Megler runs on is: https://localhost:11443.
std::wstring consoleAddress();

struct Credentials {
    std::string user, password;
    bool empty() const { return user.empty(); }
};

/// What the portal answered.
struct Answer {
    bool ok = false;
    int status = 0;        // the HTTP status; 0 when no answer came
    bool reached = false;  // an answer came at all, whatever its status
    std::string said;      // what it said, or why nothing was done, as a sentence
    /// It wants a user name and password, or other ones than those sent (401).
    bool wantsSignIn() const { return status == 401; }
};

/// An installed package, as the portal lists it.
struct Package {
    std::string name;        // the portal's name of it, which may be its display name
    std::string fullName;    // "<name>_<version>_<processor>_<resource>_<publisher id>"
    std::string familyName;
    std::string version;     // "0.1.0.672"
    int origin = 0;          // PackageOrigin: 2 inbox, 3 Store, 5 developer signed, 6 line of business
};

class Client {
public:
    Client(std::wstring address, Credentials credentials);

    /// The portal asked for its cookies and its CSRF token, and whether it takes the user name and password; before anything
    /// else (each call below does it itself when it has not been done).
    Answer open();
    /// `package` sent to the portal with `others` (the packages it depends on, a certificate), as the form of its install
    /// wants them, and its state asked every `pollMs` until it says how the install went, `timeoutSeconds` in all. `progress`
    /// is told each step, and the share sent while the files go.
    Answer install(const std::wstring& package, const std::vector<std::wstring>& others, const std::function<void(const std::string&)>& progress,
                   unsigned pollMs = 1000, unsigned timeoutSeconds = 1200);
    /// The package of that full name uninstalled.
    Answer uninstall(const std::string& fullName);
    /// Every package installed on the console.
    Answer packages(std::vector<Package>& out, unsigned timeoutSeconds = 60);

    /// The names of the cookies the portal handed out (their values are its session, no line of a log's), and whether a
    /// CSRF token was among them.
    std::string cookieNames() const;
    bool hasToken() const { return !token_.empty(); }

private:
    std::wstring address_;
    Credentials credentials_;
    bool opened_ = false;
    std::wstring cookies_, token_;
    winrt::Windows::Web::Http::HttpClient client_{nullptr};

    winrt::Windows::Web::Http::HttpRequestMessage request(winrt::Windows::Web::Http::HttpMethod const& method, const std::wstring& path);
    Answer refused(winrt::Windows::Web::Http::HttpResponseMessage const& response, const std::string& what);
    Answer unreachable(winrt::hresult_error const& e);
};

/// The user name and password Megler has for the portal: those given this run, else those kept in Windows's credential
/// locker (PasswordVault) from an earlier one; empty when there are none.
Credentials savedCredentials();
/// Kept for this run, and in the credential locker for the next where Windows has one for the app.
void saveCredentials(const Credentials& credentials);
/// Forgotten, here and in the locker: the portal refused them.
void forgetCredentials();

}  // namespace megler::app::devicePortal
