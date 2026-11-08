#include "pch.h"

#include "player/BluetoothHost.h"

#include "player/SystemHost.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include <winrt/Windows.Devices.Bluetooth.Advertisement.h>
#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>
#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Security.ExchangeActiveSyncProvisioning.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

namespace megler::app {
namespace {

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::Advertisement;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using winrt::Windows::Devices::Enumeration::DeviceInformation;
using winrt::Windows::Devices::Enumeration::DeviceInformationCustomPairing;
using winrt::Windows::Devices::Enumeration::DevicePairingKinds;
using winrt::Windows::Devices::Enumeration::DevicePairingRequestedEventArgs;
using winrt::Windows::Devices::Enumeration::DevicePairingResultStatus;
using winrt::Windows::Devices::Enumeration::DeviceUnpairingResultStatus;
using winrt::Windows::Devices::Radios::Radio;
using winrt::Windows::Devices::Radios::RadioState;
using winrt::Windows::Foundation::DateTime;
using winrt::Windows::Foundation::IReference;
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Storage::Streams::Buffer;
using winrt::Windows::Storage::Streams::IBuffer;

/// How long the radio's state waits for the looking: far longer than it takes.
constexpr std::chrono::seconds lookLimit{15};
/// Android's statuses (BluetoothGatt.GATT_*; a connection's HCI reasons; BluetoothDevice.UNBOND_REASON_*; ScanCallback's).
constexpr int gattInvalidHandle = 1, gattError = 133, gattFailure = 257, linkLost = 8;
constexpr int unbondAuthFailed = 1, unbondAuthRejected = 2, unbondAuthCanceled = 3, unbondRemoteDown = 4, unbondDiscovery = 5, unbondTimeout = 6,
              unbondRepeated = 7, unbondRemoved = 9;
constexpr int scanFailedRegistration = 2, scanFailedInternal = 3, scanFailedUnsupported = 4, scanFailedResources = 5;

/// A thread of the multithreaded apartment that runs what is posted to it, in order, nobody waiting.
class Worker {
public:
    Worker() : thread_([this] { loop(); }) {}
    Worker(const Worker&) = delete;
    Worker& operator=(const Worker&) = delete;

    void post(std::function<void()> work) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(work));
        }
        wake_.notify_one();
    }

private:
    void loop() {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        for (;;) {
            std::function<void()> work;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return !queue_.empty(); });
                work = std::move(queue_.front());
                queue_.pop_front();
            }
            try {
                work();
            } catch (const winrt::hresult_error& e) {
                shellLog::write(format("bluetooth: an error of Windows' (0x%08X): %s", unsigned(e.code()), narrow(e.message()).c_str()));
            } catch (...) {
                shellLog::write("bluetooth: an unexpected exception");
            }
        }
        if (SUCCEEDED(com)) CoUninitialize();
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<std::function<void()>> queue_;
    std::thread thread_;
};

Worker& worker() {
    // Never destroyed: Windows' threads may hand an event over as the process ends.
    static Worker* const w = new Worker();
    return *w;
}

std::string addressText(uint64_t a) {
    return format("%02X:%02X:%02X:%02X:%02X:%02X", unsigned(a >> 40) & 0xff, unsigned(a >> 32) & 0xff, unsigned(a >> 24) & 0xff, unsigned(a >> 16) & 0xff,
                  unsigned(a >> 8) & 0xff, unsigned(a) & 0xff);
}

std::vector<uint8_t> bytesOf(const IBuffer& b) {
    if (!b || b.Length() == 0) return {};
    return std::vector<uint8_t>(b.data(), b.data() + b.Length());
}

IBuffer bufferOf(const std::vector<uint8_t>& v) {
    Buffer b(uint32_t(v.size()));
    b.Length(uint32_t(v.size()));
    if (!v.empty()) memcpy(b.data(), v.data(), v.size());
    return b;
}

/// A UUID as megler.h has it: as it is written, Data1 first.
void uuidBytes(const winrt::guid& g, unsigned char out[16]) {
    out[0] = uint8_t(g.Data1 >> 24);
    out[1] = uint8_t(g.Data1 >> 16);
    out[2] = uint8_t(g.Data1 >> 8);
    out[3] = uint8_t(g.Data1);
    out[4] = uint8_t(g.Data2 >> 8);
    out[5] = uint8_t(g.Data2);
    out[6] = uint8_t(g.Data3 >> 8);
    out[7] = uint8_t(g.Data3);
    memcpy(out + 8, g.Data4, 8);
}

int statusOf(GattCommunicationStatus s, const IReference<uint8_t>& error) {
    switch (s) {
    case GattCommunicationStatus::Success: return 0;
    case GattCommunicationStatus::ProtocolError: return error ? int(error.Value()) : gattFailure;
    case GattCommunicationStatus::Unreachable: return gattError;
    default: return gattFailure; // AccessDenied: Windows refused it to Megler
    }
}

const char* statusName(GattCommunicationStatus s) {
    switch (s) {
    case GattCommunicationStatus::Success: return "done";
    case GattCommunicationStatus::Unreachable: return "unreachable";
    case GattCommunicationStatus::ProtocolError: return "refused by the device";
    default: return "refused by Windows (Settings, Privacy: the app's access to Bluetooth devices)";
    }
}

long long timeNs(const DateTime& when) { return (when.time_since_epoch().count() - 116444736000000000LL) * 100; }

/// `f` on an event from Windows' thread: what it throws stays here.
template <class F>
void guarded(F&& f) {
    try {
        f();
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("bluetooth: an error of Windows' in its event (0x%08X)", unsigned(e.code())));
    } catch (...) {
    }
}

void pushEvent(int kind, int conn, int status, int handle = 0, int value = 0, const std::vector<uint8_t>* data = nullptr) {
    megler_bluetooth_event e{};
    e.kind = kind;
    e.conn = conn;
    e.status = status;
    e.handle = handle;
    e.value = value;
    if (data && !data->empty()) {
        e.data = data->data();
        e.size = int(data->size());
    }
    megler_bluetooth_push(&e);
}

void pushDevice(int kind, uint64_t address, int type, int value, int status, int extra = 0, const std::string& name = std::string()) {
    megler_bluetooth_event e{};
    e.kind = kind;
    e.address = address;
    e.address_type = type;
    e.value = value;
    e.status = status;
    e.extra = extra;
    if (!name.empty()) e.name = name.c_str();
    megler_bluetooth_push(&e);
}

// ------------------------------------------------------------------ the radio

struct Paired {
    uint64_t address;
    int type;
    std::string name;
};

std::mutex gMutex; // everything below, and the connections and pairings
std::condition_variable gLookedChanged;
bool gLooking = false, gLooked = false, gSaidPaired = false;
int gRadio = MEGLER_BT_RADIO_NONE;
std::string gName, gWhyNone;
BluetoothAdapter gAdapter{nullptr};
Radio gRadioObject{nullptr};
std::vector<Paired> gPaired;
bool gTyped = false; // BluetoothAddressType there (Windows 10 1703)

int radioOf(RadioState s) { return s == RadioState::On ? MEGLER_BT_RADIO_ON : MEGLER_BT_RADIO_OFF; }

int typeOf(BluetoothAddressType t) {
    return t == BluetoothAddressType::Random ? MEGLER_BT_ADDRESS_RANDOM : t == BluetoothAddressType::Public ? MEGLER_BT_ADDRESS_PUBLIC : MEGLER_BT_ADDRESS_UNKNOWN;
}

void radioChanged(RadioState s) {
    const int now = radioOf(s);
    std::string name;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (now == gRadio) return;
        gRadio = now;
        name = gName;
    }
    shellLog::write(std::string("bluetooth: the radio is ") + (now == MEGLER_BT_RADIO_ON ? "on" : "off"));
    megler_bluetooth_event e{};
    e.kind = MEGLER_BT_EVENT_STATE;
    e.value = now;
    e.name = name.c_str();
    megler_bluetooth_push(&e);
}

/// On the worker, once: Windows' adapter, whether it can be an LE central, its radio, the phone's name, the paired devices.
void look() {
    BluetoothAdapter adapter{nullptr};
    Radio radio{nullptr};
    std::string why, name;
    int state = MEGLER_BT_RADIO_NONE;
    std::vector<Paired> paired;
    gTyped = ApiInformation::IsPropertyPresent(L"Windows.Devices.Bluetooth.BluetoothLEDevice", L"BluetoothAddressType");
    try {
        // (Windows 10 1703's GATT client, which reaches devices it is not paired with: GattSession, GetGattServicesAsync.)
        if (!ApiInformation::IsTypePresent(L"Windows.Devices.Bluetooth.GenericAttributeProfile.GattSession"))
            why = "this Windows is older than 10 1703, whose GATT client reaches unpaired devices";
        else if (!(adapter = BluetoothAdapter::GetDefaultAsync().get()))
            why = "Windows has no Bluetooth adapter";
        else if (!adapter.IsLowEnergySupported())
            why = "its Bluetooth adapter has no Bluetooth LE";
        else if (!adapter.IsCentralRoleSupported())
            why = "its Bluetooth adapter cannot be an LE central";
        else if (!(radio = adapter.GetRadioAsync().get()))
            why = "Windows gave no radio of its Bluetooth adapter";
        else
            state = radioOf(radio.State());
    } catch (const winrt::hresult_error& e) {
        why = e.code() == E_ACCESSDENIED ? "Windows refused it this package, which does not declare Bluetooth (an app's own package made before Megler had "
                                           "it: Update All makes it again)"
                                         : format("Windows' Bluetooth could not be asked (0x%08X)", unsigned(e.code()));
        state = MEGLER_BT_RADIO_NONE;
    }
    try {
        name = narrow(winrt::Windows::Security::ExchangeActiveSyncProvisioning::EasClientDeviceInformation().FriendlyName());
    } catch (...) {
    }
    if (name.empty()) name = "Windows";
    if (state != MEGLER_BT_RADIO_NONE) {
        try {
            const auto all = DeviceInformation::FindAllAsync(BluetoothLEDevice::GetDeviceSelectorFromPairingState(true)).get();
            for (const auto& d : all) {
                try {
                    if (auto dev = BluetoothLEDevice::FromIdAsync(d.Id()).get()) {
                        paired.push_back({dev.BluetoothAddress(), gTyped ? typeOf(dev.BluetoothAddressType()) : MEGLER_BT_ADDRESS_UNKNOWN, narrow(dev.Name())});
                        dev.Close();
                    }
                } catch (const winrt::hresult_error&) {
                }
            }
        } catch (const winrt::hresult_error& e) {
            shellLog::write(format("bluetooth: the paired devices could not be listed (0x%08X)", unsigned(e.code())));
        }
    }
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gAdapter = adapter;
        gRadioObject = radio;
        gRadio = state;
        gName = name;
        gWhyNone = why;
        gPaired = paired;
    }
    if (radio) radio.StateChanged([](const Radio& r, const winrt::Windows::Foundation::IInspectable&) { guarded([&] { radioChanged(r.State()); }); });
}

std::string summarize() {
    std::lock_guard<std::mutex> lock(gMutex);
    if (gRadio == MEGLER_BT_RADIO_NONE) return "none: " + gWhyNone;
    std::string line = format("an LE central, the radio %s, the phone named \"%s\"; paired with %d LE device(s)", gRadio == MEGLER_BT_RADIO_ON ? "on" : "off",
                              gName.c_str(), int(gPaired.size()));
    for (const Paired& p : gPaired) line += format(" %s \"%s\"", addressText(p.address).c_str(), p.name.c_str());
    return line;
}

void lookOnce() {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gLooking) return;
        gLooking = true;
    }
    worker().post([] {
        try {
            look();
        } catch (...) {
        }
        shellLog::write("bluetooth: Windows has " + summarize());
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gLooked = true;
        }
        gLookedChanged.notify_all();
    });
}

bool waitLooked() {
    lookOnce();
    std::unique_lock<std::mutex> lock(gMutex);
    return gLookedChanged.wait_for(lock, lookLimit, [] { return gLooked; });
}

// ------------------------------------------------------------------ scanning

/// What a device sent last: its advertisement and its scan response, each's AD structures as they came.
struct Heard {
    std::vector<uint8_t> adv, rsp;
    bool connectable = false;
};

std::mutex gScanMutex; // gScan, gHeard
int gScan = MEGLER_BT_SCAN_OFF;
std::map<uint64_t, Heard> gHeard;
BluetoothLEAdvertisementWatcher gWatcher{nullptr}; // the worker's

void appendSections(std::vector<uint8_t>& out, const BluetoothLEAdvertisement& a) {
    bool flags = false;
    for (const auto& s : a.DataSections()) {
        const IBuffer data = s.Data();
        const uint32_t n = std::min<uint32_t>(data ? data.Length() : 0, 254);
        out.push_back(uint8_t(n + 1));
        out.push_back(s.DataType());
        if (n) out.insert(out.end(), data.data(), data.data() + n);
        flags |= s.DataType() == 0x01;
    }
    // (The flags, where Windows keeps them apart from the sections.)
    if (!flags)
        if (const auto f = a.Flags()) out.insert(out.begin(), {uint8_t(2), uint8_t(1), uint8_t(uint32_t(f.Value()))});
}

void received(const BluetoothLEAdvertisementReceivedEventArgs& e) {
    const uint64_t address = e.BluetoothAddress();
    const auto type = e.AdvertisementType();
    std::vector<uint8_t> sections, all;
    appendSections(sections, e.Advertisement());
    bool connectable;
    {
        std::lock_guard<std::mutex> lock(gScanMutex);
        if (gScan == MEGLER_BT_SCAN_OFF) return;
        if (gHeard.size() > 1024 && !gHeard.count(address)) gHeard.clear();
        Heard& h = gHeard[address];
        if (type == BluetoothLEAdvertisementType::ScanResponse) {
            h.rsp = sections;
        } else {
            h.adv = sections;
            h.connectable = type == BluetoothLEAdvertisementType::ConnectableUndirected || type == BluetoothLEAdvertisementType::ConnectableDirected;
        }
        all = h.adv;
        all.insert(all.end(), h.rsp.begin(), h.rsp.end());
        connectable = h.connectable;
    }
    megler_bluetooth_event ev{};
    ev.kind = MEGLER_BT_EVENT_ADVERTISEMENT;
    ev.address = address;
    ev.address_type = gTyped ? typeOf(e.BluetoothAddressType()) : MEGLER_BT_ADDRESS_UNKNOWN;
    ev.value = e.RawSignalStrengthInDBm();
    ev.extra = connectable ? 1 : 0;
    ev.data = all.data();
    ev.size = int(all.size());
    ev.time_ns = timeNs(e.Timestamp());
    megler_bluetooth_push(&ev);
}

/// Windows stopped the watcher: by the app's wish (nothing to say), or by an error of its (SCAN_FAILED, unless the radio went off).
void stopped(BluetoothError error) {
    bool wanted;
    {
        std::lock_guard<std::mutex> lock(gScanMutex);
        wanted = gScan != MEGLER_BT_SCAN_OFF && error != BluetoothError::Success;
        if (wanted) gScan = MEGLER_BT_SCAN_OFF;
    }
    if (!wanted) return;
    shellLog::write(format("bluetooth: Windows stopped the scan (its error %d)", int(error)));
    int radio;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        radio = gRadio;
    }
    // (With the radio off, Android's scans end unheard.)
    if (radio != MEGLER_BT_RADIO_ON || error == BluetoothError::RadioNotAvailable) return;
    int code = scanFailedInternal;
    if (error == BluetoothError::NotSupported) code = scanFailedUnsupported;
    else if (error == BluetoothError::ResourceInUse) code = scanFailedResources;
    else if (error == BluetoothError::DisabledByPolicy || error == BluetoothError::DisabledByUser || error == BluetoothError::ConsentRequired) code = scanFailedRegistration;
    megler_bluetooth_event e{};
    e.kind = MEGLER_BT_EVENT_SCAN_FAILED;
    e.status = code;
    megler_bluetooth_push(&e);
}

/// On the worker: the watcher as the app's scans want it.
void scanOn(int mode) {
    int was;
    {
        std::lock_guard<std::mutex> lock(gScanMutex);
        was = gScan;
        gScan = mode;
        if (mode == MEGLER_BT_SCAN_OFF) gHeard.clear();
    }
    if (mode == was) return;
    if (!gWatcher) {
        gWatcher = BluetoothLEAdvertisementWatcher();
        gWatcher.Received([](const BluetoothLEAdvertisementWatcher&, const BluetoothLEAdvertisementReceivedEventArgs& e) { guarded([&] { received(e); }); });
        gWatcher.Stopped([](const BluetoothLEAdvertisementWatcher&, const BluetoothLEAdvertisementWatcherStoppedEventArgs& e) { guarded([&] { stopped(e.Error()); }); });
    }
    if (gWatcher.Status() == BluetoothLEAdvertisementWatcherStatus::Started) gWatcher.Stop();
    if (mode == MEGLER_BT_SCAN_OFF) {
        shellLog::write("bluetooth: no longer scanning");
        return;
    }
    // (A watcher stopping cannot start again yet.)
    for (int i = 0; i < 100 && gWatcher.Status() == BluetoothLEAdvertisementWatcherStatus::Stopping; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    try {
        gWatcher.ScanningMode(mode == MEGLER_BT_SCAN_ACTIVE ? BluetoothLEScanningMode::Active : BluetoothLEScanningMode::Passive);
        gWatcher.Start();
    } catch (const winrt::hresult_error& e) {
        // Refused: the app's scans hear so (Windows' access refused, as an app's registration refused; else an inner error).
        shellLog::write(format("bluetooth: Windows would not scan (0x%08X)", unsigned(e.code())));
        {
            std::lock_guard<std::mutex> lock(gScanMutex);
            gScan = MEGLER_BT_SCAN_OFF;
        }
        megler_bluetooth_event ev{};
        ev.kind = MEGLER_BT_EVENT_SCAN_FAILED;
        ev.status = e.code() == E_ACCESSDENIED ? scanFailedRegistration : scanFailedInternal;
        megler_bluetooth_push(&ev);
        return;
    }
    shellLog::write(mode == MEGLER_BT_SCAN_ACTIVE ? "bluetooth: scanning, active" : "bluetooth: scanning, passive");
}

// ------------------------------------------------------------------ connections

const winrt::guid cccdUuid{0x00002902, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}};
/// A Client Characteristic Configuration descriptor Windows did not list stands at a handle no attribute can have.
constexpr int madeUpCccd = 0x10000;

struct Conn {
    int conn = 0;
    uint64_t address = 0;
    int type = MEGLER_BT_ADDRESS_UNKNOWN;
    bool autoConnect = false;
    // Under gMutex:
    bool connected = false, ended = false;
    BluetoothLEDevice device{nullptr};
    GattSession session{nullptr};
    winrt::event_token statusToken{};
    std::vector<GattDeviceService> services; // the connection's own, for the first discovery
    std::map<int, GattCharacteristic> chars;
    std::map<int, GattDescriptor> descs;
    std::map<int, GattCharacteristic> cccds;     // a Client Characteristic Configuration descriptor's handle: its characteristic
    std::map<int, winrt::event_token> notifying; // a characteristic's handle: its ValueChanged
};

std::map<int, std::shared_ptr<Conn>> gConns; // under gMutex

std::shared_ptr<Conn> connOf(int conn) {
    std::lock_guard<std::mutex> lock(gMutex);
    auto it = gConns.find(conn);
    return it == gConns.end() ? nullptr : it->second;
}

/// The connection let go: its subscriptions, its session (no longer maintained), its device; nothing said to the app.
void release(const std::shared_ptr<Conn>& c) {
    BluetoothLEDevice device{nullptr};
    GattSession session{nullptr};
    std::vector<GattDeviceService> services;
    std::map<int, GattCharacteristic> chars;
    std::map<int, winrt::event_token> notifying;
    winrt::event_token token{};
    {
        std::lock_guard<std::mutex> lock(gMutex);
        c->ended = true;
        c->connected = false;
        device = c->device;
        session = c->session;
        services.swap(c->services);
        chars.swap(c->chars);
        notifying.swap(c->notifying);
        c->descs.clear();
        c->cccds.clear();
        token = c->statusToken;
        c->device = nullptr;
        c->session = nullptr;
        auto it = gConns.find(c->conn);
        if (it != gConns.end() && it->second == c) gConns.erase(it);
    }
    for (auto& [handle, t] : notifying) {
        auto ch = chars.find(handle);
        if (ch != chars.end()) guarded([&] { ch->second.ValueChanged(t); });
    }
    for (auto& s : services) guarded([&] { s.Close(); });
    if (session) guarded([&] {
        session.MaintainConnection(false);
        session.Close();
    });
    if (device) guarded([&] {
        device.ConnectionStatusChanged(token);
        device.Close();
    });
}

/// The connection could not be made: CONNECTION 0 with Android's status, and it is let go.
void failed(const std::shared_ptr<Conn>& c, int status, const std::string& why) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (c->ended) return;
    }
    shellLog::write(format("bluetooth: connection %d to %s not made: %s", c->conn, addressText(c->address).c_str(), why.c_str()));
    release(c);
    pushEvent(MEGLER_BT_EVENT_CONNECTION, c->conn, status);
}

/// Windows says the device is connected or not: CONNECTION as it changes. Dropped, one not made `auto` is let go.
void statusChanged(const std::shared_ptr<Conn>& c, bool now) {
    bool made = false, dropped = false;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (c->ended) return;
        if (now && !c->connected) made = c->connected = true;
        else if (!now && c->connected) {
            c->connected = false;
            dropped = true;
        }
    }
    if (made) pushEvent(MEGLER_BT_EVENT_CONNECTION, c->conn, 0, 0, 1);
    if (dropped) {
        shellLog::write(format("bluetooth: connection %d to %s dropped%s", c->conn, addressText(c->address).c_str(), c->autoConnect ? ": Windows makes it again when it can" : ""));
        if (!c->autoConnect) release(c);
        pushEvent(MEGLER_BT_EVENT_CONNECTION, c->conn, linkLost);
    }
}

winrt::fire_and_forget connectAsync(std::shared_ptr<Conn> c) {
    try {
        BluetoothLEDevice dev{nullptr};
        if (gTyped && c->type != MEGLER_BT_ADDRESS_UNKNOWN)
            dev = co_await BluetoothLEDevice::FromBluetoothAddressAsync(c->address, c->type == MEGLER_BT_ADDRESS_RANDOM ? BluetoothAddressType::Random
                                                                                                                         : BluetoothAddressType::Public);
        else
            dev = co_await BluetoothLEDevice::FromBluetoothAddressAsync(c->address);
        if (!dev) {
            failed(c, gattError, "Windows has no such device");
            co_return;
        }
        std::weak_ptr<Conn> weak = c;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (c->ended) {
                dev.Close();
                co_return;
            }
            c->device = dev;
            c->statusToken = dev.ConnectionStatusChanged([weak](const BluetoothLEDevice& d, const winrt::Windows::Foundation::IInspectable&) {
                guarded([&] {
                    if (auto c = weak.lock()) statusChanged(c, d.ConnectionStatus() == BluetoothConnectionStatus::Connected);
                });
            });
        }
        GattSession session = co_await GattSession::FromDeviceIdAsync(dev.BluetoothDeviceId());
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (c->ended) {
                session.Close();
                co_return;
            }
            c->session = session;
        }
        session.MaintainConnection(true);
        // The device's services, asked for afresh: what makes Windows connect to it.
        const auto result = co_await dev.GetGattServicesAsync(BluetoothCacheMode::Uncached);
        if (result.Status() == GattCommunicationStatus::Success) {
            {
                std::lock_guard<std::mutex> lock(gMutex);
                if (c->ended) co_return;
                c->services.clear();
                for (const auto& s : result.Services()) c->services.push_back(s);
            }
            statusChanged(c, true);
        } else if (!c->autoConnect) {
            failed(c, gattError, std::string("its services: ") + statusName(result.Status()));
        } else {
            shellLog::write(format("bluetooth: connection %d to %s: not in range (%s); Windows makes it when it can", c->conn, addressText(c->address).c_str(),
                                   statusName(result.Status())));
        }
    } catch (const winrt::hresult_error& e) {
        failed(c, gattError, format("an error of Windows' (0x%08X)", unsigned(e.code())));
    }
}

/// On the worker: a connection made for `conn`, the one it had before let go.
void connectOn(int conn, uint64_t address, int type, bool autoConnect) {
    if (auto old = connOf(conn)) release(old);
    auto c = std::make_shared<Conn>();
    c->conn = conn;
    c->address = address;
    c->type = type;
    c->autoConnect = autoConnect;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gConns[conn] = c;
    }
    connectAsync(c);
}

void disconnectOn(int conn) {
    auto c = connOf(conn);
    if (!c) return;
    release(c);
    pushEvent(MEGLER_BT_EVENT_CONNECTION, conn, 0);
}

/// A characteristic's notifications and indications handed over as CHANGED, from now on.
void subscribe(const std::shared_ptr<Conn>& c, int handle, const GattCharacteristic& ch) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (c->ended || c->notifying.count(handle)) return;
    }
    const int conn = c->conn;
    const auto token = ch.ValueChanged([conn, handle](const GattCharacteristic&, const GattValueChangedEventArgs& e) {
        guarded([&] {
            const std::vector<uint8_t> value = bytesOf(e.CharacteristicValue());
            pushEvent(MEGLER_BT_EVENT_CHANGED, conn, 0, handle, 0, &value);
        });
    });
    std::lock_guard<std::mutex> lock(gMutex);
    c->notifying[handle] = token;
}

void unsubscribe(const std::shared_ptr<Conn>& c, int handle, const GattCharacteristic& ch) {
    winrt::event_token token{};
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto it = c->notifying.find(handle);
        if (it == c->notifying.end()) return;
        token = it->second;
        c->notifying.erase(it);
    }
    guarded([&] { ch.ValueChanged(token); });
}

int handleOf(const GattCharacteristic& ch) { return int(ch.AttributeHandle()); }

winrt::fire_and_forget discoverAsync(std::shared_ptr<Conn> c) {
    std::vector<megler_bluetooth_attribute> table;
    int status = 0;
    try {
        BluetoothLEDevice device{nullptr};
        std::vector<GattDeviceService> services;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            device = c->device;
            services.swap(c->services);
        }
        if (!device) {
            pushEvent(MEGLER_BT_EVENT_SERVICES, c->conn, gattError);
            co_return;
        }
        // (The connection's own, the first time; afresh after.)
        if (services.empty()) {
            const auto r = co_await device.GetGattServicesAsync(BluetoothCacheMode::Uncached);
            if (r.Status() != GattCommunicationStatus::Success) status = statusOf(r.Status(), r.ProtocolError());
            else
                for (const auto& s : r.Services()) services.push_back(s);
        }
        std::map<int, GattCharacteristic> chars;
        std::map<int, GattDescriptor> descs;
        std::map<int, GattCharacteristic> cccds;
        for (const auto& s : services) {
            megler_bluetooth_attribute a{};
            a.kind = MEGLER_BT_ATTR_SERVICE;
            a.handle = int(s.AttributeHandle());
            uuidBytes(s.Uuid(), a.uuid);
            auto cr = co_await s.GetCharacteristicsAsync(BluetoothCacheMode::Uncached);
            // (Refused: Windows' consent for the device is asked for -- the first time, of the user -- and it is tried again.)
            if (cr.Status() == GattCommunicationStatus::AccessDenied) {
                const auto access = co_await s.RequestAccessAsync();
                if (access == winrt::Windows::Devices::Enumeration::DeviceAccessStatus::Allowed) cr = co_await s.GetCharacteristicsAsync(BluetoothCacheMode::Uncached);
            }
            // (A service Windows keeps to itself -- the keyboard's, HID -- is not the app's.)
            if (cr.Status() != GattCommunicationStatus::Success) {
                shellLog::write(format("bluetooth: connection %d: the service at 0x%04x left out: %s", c->conn, a.handle, statusName(cr.Status())));
                continue;
            }
            table.push_back(a);
            try {
                const auto ir = co_await s.GetIncludedServicesAsync(BluetoothCacheMode::Cached);
                if (ir.Status() == GattCommunicationStatus::Success)
                    for (const auto& inc : ir.Services()) {
                        megler_bluetooth_attribute i{};
                        i.kind = MEGLER_BT_ATTR_INCLUDED;
                        i.handle = int(inc.AttributeHandle());
                        uuidBytes(inc.Uuid(), i.uuid);
                        table.push_back(i);
                    }
            } catch (const winrt::hresult_error&) {
            }
            for (const auto& ch : cr.Characteristics()) {
                megler_bluetooth_attribute k{};
                k.kind = MEGLER_BT_ATTR_CHARACTERISTIC;
                k.handle = handleOf(ch);
                k.properties = int(uint32_t(ch.CharacteristicProperties()) & 0xFF);
                uuidBytes(ch.Uuid(), k.uuid);
                table.push_back(k);
                chars.emplace(k.handle, ch);
                bool hasCccd = false;
                const auto dr = co_await ch.GetDescriptorsAsync(BluetoothCacheMode::Uncached);
                if (dr.Status() == GattCommunicationStatus::Success)
                    for (const auto& d : dr.Descriptors()) {
                        megler_bluetooth_attribute dd{};
                        dd.kind = MEGLER_BT_ATTR_DESCRIPTOR;
                        dd.handle = int(d.AttributeHandle());
                        uuidBytes(d.Uuid(), dd.uuid);
                        table.push_back(dd);
                        descs.emplace(dd.handle, d);
                        if (d.Uuid() == cccdUuid) {
                            cccds.emplace(dd.handle, ch);
                            hasCccd = true;
                        }
                    }
                // Windows' own call stands for a Client Characteristic Configuration descriptor it does not list.
                if (!hasCccd && (k.properties & 0x30)) {
                    megler_bluetooth_attribute dd{};
                    dd.kind = MEGLER_BT_ATTR_DESCRIPTOR;
                    dd.handle = madeUpCccd + k.handle;
                    uuidBytes(cccdUuid, dd.uuid);
                    table.push_back(dd);
                    cccds.emplace(dd.handle, ch);
                }
            }
        }
        {
            std::lock_guard<std::mutex> lock(gMutex);
            c->chars = chars;
            c->descs = descs;
            c->cccds = cccds;
        }
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("bluetooth: connection %d: its services could not be read (0x%08X)", c->conn, unsigned(e.code())));
        status = gattFailure;
        table.clear();
    }
    megler_bluetooth_event e{};
    e.kind = MEGLER_BT_EVENT_SERVICES;
    e.conn = c->conn;
    e.status = status;
    e.attributes = table.data();
    e.count = int(table.size());
    megler_bluetooth_push(&e);
}

/// What the handle is of the connection's: a characteristic, a Client Characteristic Configuration descriptor (its
/// characteristic), or another descriptor; all null for none.
void attributeOf(const std::shared_ptr<Conn>& c, int handle, GattCharacteristic& ch, GattCharacteristic& cccdOf, GattDescriptor& d) {
    std::lock_guard<std::mutex> lock(gMutex);
    if (auto it = c->chars.find(handle); it != c->chars.end()) ch = it->second;
    else if (auto it2 = c->cccds.find(handle); it2 != c->cccds.end()) cccdOf = it2->second;
    else if (auto it3 = c->descs.find(handle); it3 != c->descs.end()) d = it3->second;
}

winrt::fire_and_forget readAsync(std::shared_ptr<Conn> c, int handle) {
    int status = gattInvalidHandle;
    std::vector<uint8_t> value;
    try {
        GattCharacteristic ch{nullptr}, cccdOf{nullptr};
        GattDescriptor d{nullptr};
        attributeOf(c, handle, ch, cccdOf, d);
        if (ch) {
            const auto r = co_await ch.ReadValueAsync(BluetoothCacheMode::Uncached);
            status = statusOf(r.Status(), r.ProtocolError());
            if (!status) value = bytesOf(r.Value());
        } else if (cccdOf) {
            const auto r = co_await cccdOf.ReadClientCharacteristicConfigurationDescriptorAsync();
            status = statusOf(r.Status(), r.ProtocolError());
            if (!status) value = {uint8_t(r.ClientCharacteristicConfigurationDescriptor()), 0};
        } else if (d) {
            const auto r = co_await d.ReadValueAsync(BluetoothCacheMode::Uncached);
            status = statusOf(r.Status(), r.ProtocolError());
            if (!status) value = bytesOf(r.Value());
        }
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("bluetooth: connection %d: a read of 0x%04x failed (0x%08X)", c->conn, handle, unsigned(e.code())));
        status = gattError;
    }
    pushEvent(MEGLER_BT_EVENT_READ, c->conn, status, handle, 0, &value);
}

winrt::fire_and_forget writeAsync(std::shared_ptr<Conn> c, int handle, std::vector<uint8_t> data, int type) {
    int status = gattInvalidHandle;
    try {
        GattCharacteristic ch{nullptr}, cccdOf{nullptr};
        GattDescriptor d{nullptr};
        attributeOf(c, handle, ch, cccdOf, d);
        if (ch) {
            // (Windows has no signed write: such a write goes without a response.)
            const auto option = type == MEGLER_BT_WRITE_REQUEST ? GattWriteOption::WriteWithResponse : GattWriteOption::WriteWithoutResponse;
            const auto r = co_await ch.WriteValueWithResultAsync(bufferOf(data), option);
            status = statusOf(r.Status(), r.ProtocolError());
        } else if (cccdOf) {
            const uint8_t bits = data.empty() ? 0 : uint8_t(data[0] & 3);
            const auto value = static_cast<GattClientCharacteristicConfigurationDescriptorValue>(bits);
            const int of = handleOf(cccdOf);
            // Subscribed before the device is told, so that its first notification is not lost.
            if (bits) subscribe(c, of, cccdOf);
            const auto r = co_await cccdOf.WriteClientCharacteristicConfigurationDescriptorWithResultAsync(value);
            status = statusOf(r.Status(), r.ProtocolError());
            if (!bits || status) unsubscribe(c, of, cccdOf);
        } else if (d) {
            const auto r = co_await d.WriteValueWithResultAsync(bufferOf(data));
            status = statusOf(r.Status(), r.ProtocolError());
        }
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("bluetooth: connection %d: a write of 0x%04x failed (0x%08X)", c->conn, handle, unsigned(e.code())));
        status = gattError;
    }
    pushEvent(MEGLER_BT_EVENT_WRITTEN, c->conn, status, handle);
}

winrt::fire_and_forget mtuAsync(std::shared_ptr<Conn> c, int want) {
    int pdu = 23;
    try {
        GattSession session{nullptr};
        {
            std::lock_guard<std::mutex> lock(gMutex);
            session = c->session;
        }
        // (Windows exchanges the MTU as it connects: one still at the least is waited for a moment.)
        for (int i = 0; session && i < 10; i++) {
            pdu = int(session.MaxPduSize());
            if (pdu > 23) break;
            co_await winrt::resume_after(std::chrono::milliseconds(100));
        }
    } catch (const winrt::hresult_error&) {
    }
    shellLog::write(format("bluetooth: connection %d: Windows' MTU is %d, the app asked for %d", c->conn, pdu, want));
    pushEvent(MEGLER_BT_EVENT_MTU, c->conn, 0, 0, std::min(want, std::max(pdu, 23)));
}

// ------------------------------------------------------------------ pairing

/// A request of the device's during a pairing, waiting for its answer (pairing_reply).
struct Asked {
    DevicePairingRequestedEventArgs args{nullptr};
    winrt::Windows::Foundation::Deferral deferral{nullptr};
    DevicePairingKinds kind = DevicePairingKinds::None;
    std::string pin, name;
    bool answered = false;
};

std::map<uint64_t, std::shared_ptr<Asked>> gAsked; // under gMutex
std::map<uint64_t, bool> gAskWindows;              // a pairing whose PIN Windows' own dialog is to ask for, under gMutex

int variantOf(DevicePairingKinds kind) {
    switch (kind) {
    case DevicePairingKinds::ProvidePin: return MEGLER_BT_PAIRING_PIN;
    case DevicePairingKinds::ConfirmPinMatch: return MEGLER_BT_PAIRING_CONFIRMATION;
    case DevicePairingKinds::DisplayPin: return MEGLER_BT_PAIRING_DISPLAY_PASSKEY;
    default: return MEGLER_BT_PAIRING_CONSENT;
    }
}

/// The request's answer given: accepted (with the PIN where one is asked for) or not; the ceremony goes on.
void finish(const std::shared_ptr<Asked>& a, bool accept, const std::string& pin) {
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (a->answered) return;
        a->answered = true;
    }
    guarded([&] {
        if (accept) {
            if (a->kind == DevicePairingKinds::ProvidePin) a->args.Accept(hs(pin));
            else a->args.Accept();
        }
        a->deferral.Complete();
    });
}

/// Windows' thread: the device asks for something to pair. The app hears it (PAIRING) and answers; a minute without an answer is a no.
void requested(uint64_t address, int type, const std::string& name, const DevicePairingRequestedEventArgs& args) {
    auto a = std::make_shared<Asked>();
    a->args = args;
    a->deferral = args.GetDeferral();
    a->kind = args.PairingKind();
    a->pin = narrow(args.Pin());
    a->name = name;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gAsked[address] = a;
    }
    int key = 0;
    if (!a->pin.empty()) key = atoi(a->pin.c_str());
    shellLog::write(format("bluetooth: %s asks, to pair, for %s", addressText(address).c_str(),
                           a->kind == DevicePairingKinds::ProvidePin ? "a PIN" : a->kind == DevicePairingKinds::ConfirmPinMatch ? "its number confirmed"
                           : a->kind == DevicePairingKinds::DisplayPin                                                     ? "a number entered on it"
                                                                                                                             : "a consent"));
    pushDevice(MEGLER_BT_EVENT_PAIRING, address, type, variantOf(a->kind), 0, key, name);
    [](std::weak_ptr<Asked> weak) -> winrt::fire_and_forget {
        co_await winrt::resume_after(std::chrono::minutes(1));
        if (auto a = weak.lock()) finish(a, false, std::string());
    }(a);
}

/// The answer to a request: the app's (accept, with its PIN, or no), or the user's -- Megler's question for a confirmation or a
/// consent, Windows' own pairing for a PIN, its dialog asking for it.
void replyOn(uint64_t address, int answer, const std::string& pin) {
    std::shared_ptr<Asked> a;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        auto it = gAsked.find(address);
        if (it == gAsked.end()) return;
        a = it->second;
        gAsked.erase(it);
    }
    if (answer == MEGLER_BT_PAIR_ACCEPT) {
        finish(a, true, pin);
        return;
    }
    if (answer != MEGLER_BT_PAIR_ASK_USER) {
        finish(a, false, std::string());
        return;
    }
    if (a->kind == DevicePairingKinds::ProvidePin) {
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gAskWindows[address] = true;
        }
        finish(a, false, std::string());
        return;
    }
    // (The question waits for the user, on a thread of its own: never the worker's, nor the UI thread.)
    std::thread([a] {
        const std::string who = a->name.empty() ? std::string("the device") : a->name;
        std::string title = "Pair with " + who + "?", message;
        std::vector<std::string> buttons = {"Cancel", "Pair"};
        if (a->kind == DevicePairingKinds::ConfirmPinMatch) {
            message = "Does " + who + " show " + a->pin + "?";
            buttons = {"No", "Yes"};
        } else if (a->kind == DevicePairingKinds::DisplayPin) {
            message = "Enter " + a->pin + " on " + who + ", then pair.";
        } else {
            message = "An app asks to pair with " + who + ".";
        }
        finish(a, systemHost::askUser(title, message, buttons) == 1, std::string());
    }).detach();
}

int reasonOf(DevicePairingResultStatus s) {
    switch (s) {
    case DevicePairingResultStatus::ConnectionRejected:
    case DevicePairingResultStatus::AuthenticationNotAllowed:
    case DevicePairingResultStatus::AccessDenied: return unbondAuthRejected;
    case DevicePairingResultStatus::PairingCanceled:
    case DevicePairingResultStatus::RejectedByHandler: return unbondAuthCanceled;
    case DevicePairingResultStatus::NotReadyToPair:
    case DevicePairingResultStatus::NotPaired:
    case DevicePairingResultStatus::HardwareFailure: return unbondRemoteDown;
    case DevicePairingResultStatus::AuthenticationTimeout: return unbondTimeout;
    case DevicePairingResultStatus::TooManyConnections: return unbondRepeated;
    case DevicePairingResultStatus::OperationAlreadyInProgress: return unbondDiscovery;
    default: return unbondAuthFailed;
    }
}

/// Windows' own pairing, on the UI thread, where its dialog asks for what the device wants.
winrt::Windows::Foundation::IAsyncOperation<winrt::Windows::Devices::Enumeration::DevicePairingResult> pairOnUi(
    winrt::Windows::Devices::Enumeration::DeviceInformationPairing pairing) {
    auto dispatcher = winrt::Windows::ApplicationModel::Core::CoreApplication::MainView().Dispatcher();
    co_await winrt::resume_foreground(dispatcher);
    co_return co_await pairing.PairAsync();
}

winrt::fire_and_forget bondAsync(uint64_t address, int type, bool create) {
    BluetoothLEDevice dev{nullptr};
    bool own = false;
    try {
        {
            std::lock_guard<std::mutex> lock(gMutex);
            for (auto& [n, c] : gConns)
                if (c->address == address && c->device) dev = c->device;
        }
        if (!dev) {
            dev = co_await BluetoothLEDevice::FromBluetoothAddressAsync(address);
            own = true;
        }
        if (!dev) {
            if (create) pushDevice(MEGLER_BT_EVENT_BOND, address, type, MEGLER_BT_BOND_NONE, unbondRemoteDown);
            co_return;
        }
        const std::string name = narrow(dev.Name());
        auto pairing = dev.DeviceInformation().Pairing();
        if (!create) {
            bool removed = !pairing.IsPaired();
            if (!removed) {
                const auto r = co_await pairing.UnpairAsync();
                removed = r.Status() == DeviceUnpairingResultStatus::Unpaired || r.Status() == DeviceUnpairingResultStatus::AlreadyUnpaired;
            }
            shellLog::write("bluetooth: " + addressText(address) + (removed ? " is no longer paired" : " stays paired: Windows did not unpair it"));
            pushDevice(MEGLER_BT_EVENT_BOND, address, type, removed ? MEGLER_BT_BOND_NONE : MEGLER_BT_BOND_BONDED, removed ? unbondRemoved : 0, 0, name);
            if (own) dev.Close();
            co_return;
        }
        if (pairing.IsPaired()) {
            pushDevice(MEGLER_BT_EVENT_BOND, address, type, MEGLER_BT_BOND_BONDED, 0, 0, name);
            if (own) dev.Close();
            co_return;
        }
        pushDevice(MEGLER_BT_EVENT_BOND, address, type, MEGLER_BT_BOND_BONDING, 0, 0, name);
        {
            std::lock_guard<std::mutex> lock(gMutex);
            gAskWindows.erase(address);
        }
        auto custom = pairing.Custom();
        const auto token = custom.PairingRequested([address, type, name](const DeviceInformationCustomPairing&, const DevicePairingRequestedEventArgs& args) {
            guarded([&] { requested(address, type, name, args); });
        });
        const auto kinds = DevicePairingKinds::ConfirmOnly | DevicePairingKinds::DisplayPin | DevicePairingKinds::ProvidePin | DevicePairingKinds::ConfirmPinMatch;
        auto result = co_await custom.PairAsync(kinds);
        custom.PairingRequested(token);
        auto status = result.Status();
        bool askWindows;
        {
            std::lock_guard<std::mutex> lock(gMutex);
            askWindows = gAskWindows.count(address) != 0;
            gAskWindows.erase(address);
        }
        if (askWindows && status != DevicePairingResultStatus::Paired && status != DevicePairingResultStatus::AlreadyPaired) {
            shellLog::write("bluetooth: " + addressText(address) + "'s PIN is asked of the user by Windows' own pairing");
            result = co_await pairOnUi(pairing);
            status = result.Status();
        }
        const bool paired = status == DevicePairingResultStatus::Paired || status == DevicePairingResultStatus::AlreadyPaired;
        shellLog::write(format("bluetooth: %s %s (Windows' %d)", addressText(address).c_str(), paired ? "is paired" : "is not paired", int(status)));
        pushDevice(MEGLER_BT_EVENT_BOND, address, type, paired ? MEGLER_BT_BOND_BONDED : MEGLER_BT_BOND_NONE, paired ? 0 : reasonOf(status), 0, name);
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("bluetooth: the pairing with %s failed (0x%08X)", addressText(address).c_str(), unsigned(e.code())));
        if (create) pushDevice(MEGLER_BT_EVENT_BOND, address, type, MEGLER_BT_BOND_NONE, unbondAuthFailed);
    }
    if (own && dev) guarded([&] { dev.Close(); });
}

// ------------------------------------------------------------------ megler_bluetooth_host

int MEGLER_CB stateThunk(void*, char* name, int size) {
    if (!waitLooked()) {
        shellLog::write("bluetooth: Windows did not say within 15 seconds what radio there is: the app is told of none");
        return MEGLER_BT_RADIO_NONE;
    }
    int state;
    std::vector<Paired> paired;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        state = gRadio;
        if (name && size > 0) snprintf(name, size_t(size), "%s", gName.c_str());
        if (!gSaidPaired) paired = gPaired;
        gSaidPaired = true;
    }
    // The devices paired before the app ran, said to the app that runs.
    for (const Paired& p : paired) pushDevice(MEGLER_BT_EVENT_PAIRED, p.address, p.type, 0, 0, 0, p.name);
    return state;
}

void MEGLER_CB scanThunk(void*, int mode) {
    worker().post([mode] { scanOn(mode); });
}

void MEGLER_CB connectThunk(void*, int conn, unsigned long long address, int type, bool autoConnect) {
    worker().post([conn, address, type, autoConnect] { connectOn(conn, address, type, autoConnect); });
}

void MEGLER_CB disconnectThunk(void*, int conn) {
    worker().post([conn] { disconnectOn(conn); });
}

void MEGLER_CB discoverThunk(void*, int conn) {
    worker().post([conn] {
        if (auto c = connOf(conn)) discoverAsync(c);
        else pushEvent(MEGLER_BT_EVENT_SERVICES, conn, gattError);
    });
}

void MEGLER_CB readThunk(void*, int conn, int handle) {
    worker().post([conn, handle] {
        if (auto c = connOf(conn)) readAsync(c, handle);
        else pushEvent(MEGLER_BT_EVENT_READ, conn, gattError, handle);
    });
}

void MEGLER_CB writeThunk(void*, int conn, int handle, const unsigned char* data, int size, int type) {
    std::vector<uint8_t> bytes(data, data + std::max(size, 0));
    worker().post([conn, handle, bytes = std::move(bytes), type] {
        if (auto c = connOf(conn)) writeAsync(c, handle, bytes, type);
        else pushEvent(MEGLER_BT_EVENT_WRITTEN, conn, gattError, handle);
    });
}

void MEGLER_CB mtuThunk(void*, int conn, int mtu) {
    worker().post([conn, mtu] {
        if (auto c = connOf(conn)) mtuAsync(c, mtu);
        else pushEvent(MEGLER_BT_EVENT_MTU, conn, gattError, 0, 23);
    });
}

void MEGLER_CB bondThunk(void*, unsigned long long address, int type, bool create) {
    worker().post([address, type, create] { bondAsync(address, type, create); });
}

void MEGLER_CB pairingReplyThunk(void*, unsigned long long address, int answer, const char* pin) {
    const std::string p = pin ? pin : "";
    worker().post([address, answer, p] { replyOn(address, answer, p); });
}

void MEGLER_CB settingsThunk(void*) {
    const megler_system_host system = systemHost::callbacks();
    if (system.open_url) system.open_url(system.user, "ms-settings:bluetooth");
}

} // namespace

BluetoothHost& BluetoothHost::shared() {
    // Never destroyed: an app's threads may still be in a call when the process ends.
    static BluetoothHost* const host = new BluetoothHost();
    return *host;
}

megler_bluetooth_host BluetoothHost::callbacks() {
    lookOnce();
    {
        std::lock_guard<std::mutex> lock(gMutex);
        gSaidPaired = false;
    }
    megler_bluetooth_host host = {};
    host.user = this;
    host.state = &stateThunk;
    host.scan = &scanThunk;
    host.connect = &connectThunk;
    host.disconnect = &disconnectThunk;
    host.discover = &discoverThunk;
    host.read = &readThunk;
    host.write = &writeThunk;
    host.mtu = &mtuThunk;
    // (Windows tells no connection's signal strength: the core answers with the last advertisement's.)
    host.rssi = nullptr;
    host.bond = &bondThunk;
    host.pairing_reply = &pairingReplyThunk;
    host.settings = &settingsThunk;
    return host;
}

std::string BluetoothHost::summary() {
    if (!waitLooked()) return "not known: Windows did not answer in time";
    return summarize();
}

} // namespace megler::app
