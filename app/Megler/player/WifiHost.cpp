#include "pch.h"

#include "player/WifiHost.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Devices.WiFi.h>
#include <winrt/Windows.Networking.Connectivity.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace megler::app {
namespace {

using winrt::Windows::Devices::Radios::Radio;
using winrt::Windows::Devices::Radios::RadioKind;
using winrt::Windows::Devices::Radios::RadioState;
using winrt::Windows::Devices::WiFi::WiFiAccessStatus;
using winrt::Windows::Devices::WiFi::WiFiAdapter;
using winrt::Windows::Devices::WiFi::WiFiAvailableNetwork;
using winrt::Windows::Devices::WiFi::WiFiNetworkKind;
using winrt::Windows::Devices::WiFi::WiFiNetworkReport;
using winrt::Windows::Foundation::AsyncStatus;
using winrt::Windows::Networking::Connectivity::ConnectionProfile;

/// How long a scan of Windows' is waited for, and how long what is known of the state and the connection is kept.
constexpr std::chrono::seconds scanLimit{10}, stateKept{10}, connectionKept{10};
/// How long a call of the core's waits for this host's thread at most: a scan's limit and a little.
constexpr std::chrono::seconds callLimit{15};

/// A thread of the multithreaded apartment that runs what is posted to it, in order.
class Worker {
public:
    Worker() : thread_([this] { loop(); }) {}
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    /// `f` run on this thread, the caller waiting for it up to `limit`: false when it had not ended by then (it still runs,
    /// and what it fills must be the caller's to share: a shared_ptr's).
    bool run(std::function<void()> f, std::chrono::seconds limit) {
        struct Done {
            std::mutex mutex;
            std::condition_variable cv;
            bool done = false;
        };
        auto done = std::make_shared<Done>();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back([f = std::move(f), done] {
                try {
                    f();
                } catch (const winrt::hresult_error& e) {
                    shellLog::write(format("wifi: an error of Windows' (0x%08X): %s", unsigned(e.code()), narrow(e.message()).c_str()));
                } catch (...) {
                    shellLog::write("wifi: an unexpected exception");
                }
                std::lock_guard<std::mutex> l(done->mutex);
                done->done = true;
                done->cv.notify_all();
            });
        }
        wake_.notify_one();
        std::unique_lock<std::mutex> lock(done->mutex);
        return done->cv.wait_for(lock, limit, [&done] { return done->done; });
    }

private:
    void loop() {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        for (;;) {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return !queue_.empty(); });
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            work();
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    std::thread thread_;
};

Worker& worker() {
    // Never destroyed: an app's thread may be in a call when the process ends.
    static Worker* const w = new Worker();
    return *w;
}

// What is known, under gMutex: the state as last looked at, the adapter, the last report's networks, the connection.
std::mutex gMutex;
int gState = MEGLER_WIFI_UNKNOWN;
bool gAsked = false;
std::chrono::steady_clock::time_point gStateAt{};
WiFiAdapter gAdapter{nullptr};
std::vector<megler_wifi_network> gLastReport;
bool gHaveConnection = false, gConnected = false;
megler_wifi_network gConnection = {};
std::chrono::steady_clock::time_point gConnectionAt{};
int gSaidState = -1;
int gScanErrors = 0;

const char* accessName(WiFiAccessStatus s) {
    switch (s) {
    case WiFiAccessStatus::Allowed: return "allowed";
    case WiFiAccessStatus::DeniedByUser: return "denied by the user";
    case WiFiAccessStatus::DeniedBySystem: return "denied by the system (the package's wiFiControl, or a policy)";
    default: return "not specified";
    }
}

const char* stateName(int s) {
    switch (s) {
    case MEGLER_WIFI_ON: return "on: it can be scanned";
    case MEGLER_WIFI_OFF: return "off: the phone's Wi-Fi is switched off";
    case MEGLER_WIFI_DENIED: return "denied: Windows does not let Megler have it";
    case MEGLER_WIFI_NONE: return "none: no Wi-Fi adapter";
    default: return "not known yet";
    }
}

/// Windows' Wi-Fi radio switched off, where Windows tells (false where it does not: the radio is taken to be on).
bool radioOff() {
    try {
        for (const Radio& r : Radio::GetRadiosAsync().get())
            if (r.Kind() == RadioKind::WiFi) return r.State() == RadioState::Off;
    } catch (const winrt::hresult_error&) {
    }
    return false;
}

/// On the worker: the access (asked with `ask`), the adapter and the radio, kept under gMutex; the state returned.
int lookNow(bool ask) {
    int state = MEGLER_WIFI_UNKNOWN;
    bool asked;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        asked = gAsked;
    }
    std::string said;
    if (ask || asked) {
        const WiFiAccessStatus access = WiFiAdapter::RequestAccessAsync().get();
        said = format("Windows' access: %s", accessName(access));
        if (access != WiFiAccessStatus::Allowed) {
            state = access == WiFiAccessStatus::Unspecified ? MEGLER_WIFI_UNKNOWN : MEGLER_WIFI_DENIED;
        } else {
            WiFiAdapter adapter{nullptr};
            const auto adapters = WiFiAdapter::FindAllAdaptersAsync().get();
            if (adapters.Size() > 0) adapter = adapters.GetAt(0);
            said += format(", %u Wi-Fi adapter%s", unsigned(adapters.Size()), adapters.Size() == 1 ? "" : "s");
            state = !adapter ? MEGLER_WIFI_NONE : radioOff() ? MEGLER_WIFI_OFF : MEGLER_WIFI_ON;
            std::lock_guard<std::mutex> lock(gMutex);
            gAdapter = adapter;
        }
    }
    bool say;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (ask || asked) {
            gAsked = true;
            gState = state;
            gStateAt = std::chrono::steady_clock::now();
        } else {
            state = gState;
        }
        say = gState != gSaidState && (ask || asked);
        if (say) gSaidState = gState;
    }
    if (say) shellLog::write("wifi: " + said + "; the Wi-Fi is " + stateName(state));
    return state;
}

/// The state, looked at again where it is old (or not ON and ten seconds old), on the worker.
int state(bool ask) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        const bool fresh = std::chrono::steady_clock::now() - gStateAt < stateKept;
        if (gAsked && (gState == MEGLER_WIFI_ON || fresh) && !(ask && gState == MEGLER_WIFI_UNKNOWN)) return gState;
        if (!gAsked && !ask) return MEGLER_WIFI_UNKNOWN;
    }
    auto out = std::make_shared<int>(MEGLER_WIFI_UNKNOWN);
    if (!worker().run([out, ask] { *out = lookNow(ask); }, callLimit)) {
        shellLog::write("wifi: Windows did not say within 15 seconds whether the Wi-Fi can be scanned");
        return MEGLER_WIFI_UNKNOWN;
    }
    return *out;
}

/// "aa:bb:cc:dd:ee:ff" as a number; 0 for text that is not one.
unsigned long long bssidOf(const winrt::hstring& text) {
    const std::string s = narrow(std::wstring_view(text));
    unsigned long long v = 0;
    int digits = 0;
    for (char c : s) {
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d >= 0) {
            v = v << 4 | unsigned(d);
            digits++;
        } else if (c != ':' && c != '-') {
            return 0;
        }
    }
    return digits == 12 ? v : 0;
}

/// Windows' security as megler.h's MEGLER_WIFI_SECURITY (NetworkAuthenticationType's and NetworkEncryptionType's numbers).
int securityOf(int auth, int cipher) {
    int a;
    switch (auth) {
    case 2: a = MEGLER_WIFI_AUTH_OPEN; break;          // Open80211
    case 3: a = MEGLER_WIFI_AUTH_SHARED_KEY; break;    // SharedKey80211
    case 4: a = MEGLER_WIFI_AUTH_WPA; break;           // Wpa
    case 5: a = MEGLER_WIFI_AUTH_WPA_PSK; break;       // WpaPsk
    case 7: a = MEGLER_WIFI_AUTH_RSNA; break;          // Rsna
    case 8: a = MEGLER_WIFI_AUTH_RSNA_PSK; break;      // RsnaPsk
    case 10: case 13: a = MEGLER_WIFI_AUTH_WPA3; break; // Wpa3 (Enterprise 192 bits), Wpa3Enterprise
    case 11: a = MEGLER_WIFI_AUTH_WPA3_SAE; break;     // Wpa3Sae
    case 12: a = MEGLER_WIFI_AUTH_OWE; break;          // Owe
    case 0: case 1: a = MEGLER_WIFI_AUTH_UNKNOWN; break;
    default: a = MEGLER_WIFI_AUTH_OTHER; break;        // WpaNone, Ihv
    }
    int c;
    switch (cipher) {
    case 0: c = MEGLER_WIFI_CIPHER_NONE; break;
    case 2: case 3: case 4: c = MEGLER_WIFI_CIPHER_WEP; break;   // Wep, Wep40, Wep104
    case 5: c = MEGLER_WIFI_CIPHER_TKIP; break;
    case 6: case 7: case 8: c = MEGLER_WIFI_CIPHER_CCMP; break;  // Ccmp, WpaUseGroup, RsnUseGroup
    case 10: case 11: c = MEGLER_WIFI_CIPHER_GCMP; break;        // Gcmp, Gcmp256
    default: c = MEGLER_WIFI_CIPHER_UNKNOWN; break;
    }
    return MEGLER_WIFI_SECURITY(a, c);
}

megler_wifi_network networkOf(const WiFiAvailableNetwork& n) {
    megler_wifi_network out = {};
    out.bssid = bssidOf(n.Bssid());
    const std::string ssid = narrow(std::wstring_view(n.Ssid()));
    out.ssid_size = int(std::min<size_t>(ssid.size(), sizeof(out.ssid)));
    memcpy(out.ssid, ssid.data(), size_t(out.ssid_size));
    const double rssi = n.NetworkRssiInDecibelMilliwatts();
    out.rssi_dbm = std::isfinite(rssi) ? int(std::lround(rssi)) : -127;
    out.frequency_mhz = int(n.ChannelCenterFrequencyInKilohertz() / 1000);
    try {
        const auto security = n.SecuritySettings();
        out.security = securityOf(int(security.NetworkAuthenticationType()), int(security.NetworkEncryptionType()));
    } catch (const winrt::hresult_error&) {
        out.security = MEGLER_WIFI_SECURITY(MEGLER_WIFI_AUTH_UNKNOWN, MEGLER_WIFI_CIPHER_UNKNOWN);
    }
    out.phy = int(n.PhyKind());
    out.adhoc = n.NetworkKind() == WiFiNetworkKind::Adhoc ? 1 : 0;
    return out;
}

/// On the worker: a scan and its report, into `out`; the time of the report (ns since 1970) into `when`. -1 when it failed.
int scanNow(std::vector<megler_wifi_network>& out, long long& when) {
    WiFiAdapter adapter{nullptr};
    {
        std::lock_guard<std::mutex> lock(gMutex);
        adapter = gAdapter;
    }
    if (!adapter) return -1;
    const auto t0 = std::chrono::steady_clock::now();
    auto scan = adapter.ScanAsync();
    const AsyncStatus status = scan.wait_for(scanLimit);
    if (status != AsyncStatus::Completed) {
        bool say;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            say = gScanErrors++ < 5;
            // (Looked at again: the radio may have been switched off, or the access withdrawn.)
            gStateAt = {};
            if (gState == MEGLER_WIFI_ON) gState = MEGLER_WIFI_UNKNOWN;
        }
        HRESULT hr = S_OK;
        if (status == AsyncStatus::Started) {
            scan.Cancel();
        } else {
            try {
                hr = scan.ErrorCode();
            } catch (...) {
            }
        }
        if (say)
            shellLog::write(status == AsyncStatus::Started ? "wifi: Windows' scan did not end within 10 seconds"
                                                           : format("wifi: Windows' scan failed (0x%08X)", unsigned(hr)));
        return -1;
    }
    const WiFiNetworkReport report = adapter.NetworkReport();
    when = (report.Timestamp().time_since_epoch().count() - 116444736000000000LL) * 100;
    out.clear();
    for (const WiFiAvailableNetwork& n : report.AvailableNetworks()) out.push_back(networkOf(n));
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gLastReport = out;
        // (The connection's access point is the strongest of its SSID in this report: asked again with it.)
        gConnectionAt = {};
    }
    static std::atomic<int> said{0};
    if (said++ < 1)
        shellLog::write(format("wifi: Windows' first scan: %u networks in %lld ms", unsigned(out.size()),
                               (long long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count()));
    return int(out.size());
}

/// On the worker: the network the phone is connected to by Wi-Fi, kept under gMutex.
void connectionNow() {
    WiFiAdapter adapter{nullptr};
    {
        std::lock_guard<std::mutex> lock(gMutex);
        adapter = gAdapter;
    }
    megler_wifi_network c = {};
    bool connected = false;
    if (adapter) {
        const ConnectionProfile profile = adapter.NetworkAdapter().GetConnectedProfileAsync().get();
        if (profile && profile.IsWlanConnectionProfile()) {
            const std::string ssid = narrow(std::wstring_view(profile.WlanConnectionProfileDetails().GetConnectedSsid()));
            connected = true;
            c.ssid_size = int(std::min<size_t>(ssid.size(), sizeof(c.ssid)));
            memcpy(c.ssid, ssid.data(), size_t(c.ssid_size));
            c.rssi_dbm = -50;
            if (const auto bars = profile.GetSignalBars()) c.rssi_dbm = -100 + 10 * int(bars.Value());
            std::lock_guard<std::mutex> lock(gMutex);
            const megler_wifi_network* best = nullptr;
            for (const megler_wifi_network& n : gLastReport)
                if (n.ssid_size == c.ssid_size && !memcmp(n.ssid, c.ssid, size_t(c.ssid_size)) && (!best || n.rssi_dbm > best->rssi_dbm)) best = &n;
            if (best) c = *best;
        }
    }
    std::lock_guard<std::mutex> lock(gMutex);
    gHaveConnection = true;
    gConnected = connected;
    gConnection = c;
    gConnectionAt = std::chrono::steady_clock::now();
}

int MEGLER_CB stateThunk(void*, bool ask) { return state(ask); }

int MEGLER_CB scanThunk(void*, megler_wifi_network* out, int max, long long* time_ns) {
    *time_ns = 0;
    if (state(true) != MEGLER_WIFI_ON) return -1;
    struct Result {
        std::vector<megler_wifi_network> networks;
        long long when = 0;
        int n = -1;
    };
    auto r = std::make_shared<Result>();
    if (!worker().run([r] { r->n = scanNow(r->networks, r->when); }, callLimit)) return -1;
    const int n = std::min(r->n, max);
    for (int i = 0; i < n; i++) out[i] = r->networks[size_t(i)];
    *time_ns = r->when;
    return r->n < 0 ? -1 : n;
}

bool MEGLER_CB connectionThunk(void*, megler_wifi_network* out) {
    // (Windows is asked for the access here too, which shows the user nothing: an app may ask for its connection before any scan.)
    if (state(true) != MEGLER_WIFI_ON) return false;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gHaveConnection && std::chrono::steady_clock::now() - gConnectionAt < connectionKept) {
            if (gConnected) *out = gConnection;
            return gConnected;
        }
    }
    worker().run([] { connectionNow(); }, callLimit);
    std::lock_guard<std::mutex> lock(gMutex);
    if (gHaveConnection && gConnected) *out = gConnection;
    return gHaveConnection && gConnected;
}

} // namespace

WifiHost& WifiHost::shared() {
    // Never destroyed: an app's threads may still be in a call when the process ends.
    static WifiHost* const host = new WifiHost();
    return *host;
}

megler_wifi_host WifiHost::callbacks() {
    megler_wifi_host host = {};
    host.user = this;
    host.state = &stateThunk;
    host.scan = &scanThunk;
    host.connection = &connectionThunk;
    return host;
}

std::string WifiHost::summary() {
    const int s = state(true);
    std::string out = std::string("the Wi-Fi is ") + stateName(s);
    megler_wifi_network c = {};
    if (s == MEGLER_WIFI_ON && connectionThunk(nullptr, &c)) out += format("; connected to \"%.*s\"", c.ssid_size, reinterpret_cast<const char*>(c.ssid));
    return out;
}

} // namespace megler::app
