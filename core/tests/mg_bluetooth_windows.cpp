// The test program's way to the shell's Bluetooth: megler_bluetooth_host as app/Megler/player/BluetoothHost.cpp implements
// it over Windows.Devices.Bluetooth, for mg_tests --bluetooth-windows (mg_tests_main.c), which does two things through it
// and the core's.
//
// It connects to devices made up here. The host takes a device from this file where Windows would give one
// (BluetoothHost::devicesFrom), and what it takes answers as Windows' objects do -- BluetoothLEDevice, GattDeviceService,
// GattCharacteristic, GattDescriptor, GattSession and their results, each a C++ object that implements Windows' interfaces
// -- but with the interfaces of the Windows it stands for, and no more. A made-up device of Windows 10 Mobile's has no
// IBluetoothLEDevice4: asked for it, it says E_NOINTERFACE, which is what ended every app that connected (0.1.0.690). So
// the host's whole way with a connection runs on a PC without a radio: made, its MTU, its services, reads, writes,
// notifications, the link lost and made again; and every way Windows has of not answering -- a device that is not there,
// out of range, gone in the middle; no result at all; an error; an older interface only; a null in a list, null event
// arguments -- each of which the app is to hear of the way Android says it, and none of which may end the process.
//
// And it says what radio this PC has, and listens passively a moment: nothing is sent, nothing connected to, nothing
// paired with. That shows the host's work against this PC's Windows -- its adapter and radio found, the devices it is
// paired with, a watcher started and stopped, the advertisements handed over. A PC without Bluetooth says so.
//
// Only tools/build-tests.sh win builds this file, with BluetoothHost.cpp itself; the rest of the shell it uses is stood in
// for here (the system host: its question to the user and its links) and by mg_storage_windows.cpp (the log).
#include "pch.h"

#include "player/BluetoothHost.h"
#include "player/SystemHost.h"

#include <winrt/Windows.Devices.Bluetooth.GenericAttributeProfile.h>
#include <winrt/Windows.Devices.Bluetooth.h>
#include <winrt/Windows.Devices.Enumeration.h>

#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

namespace megler::app::systemHost {

// (No one to ask in the tests, and no settings to show.)
int askUser(const std::string&, const std::string&, const std::vector<std::string>&) { return -1; }
megler_system_host callbacks() { return megler_system_host{}; }

} // namespace megler::app::systemHost

namespace {

using namespace winrt::Windows::Devices::Bluetooth;
using namespace winrt::Windows::Devices::Bluetooth::GenericAttributeProfile;
using winrt::Windows::Devices::Enumeration::DeviceAccessStatus;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Foundation::IClosable;
using winrt::Windows::Foundation::IReference;
using winrt::Windows::Foundation::TypedEventHandler;
using winrt::Windows::Foundation::Collections::IVectorView;
using winrt::Windows::Storage::Streams::IBuffer;
using Object = winrt::Windows::Foundation::IInspectable;

// ------------------------------------------------------------------ Windows' objects, made up

/// How a made-up attribute answers a read or a write.
enum class Answer {
    Done,
    Refused,       // by the device, with ATT error 5 (insufficient authentication), which the result tells
    RefusedUntold, // by the device, on a result that has no interface to tell the error with
    NoResult,      // the operation ends with no result at all
    Throws,        // the operation fails (E_FAIL)
};

/// What the host does not ask of Windows' objects. Asked all the same, it is an error, which the tests show.
[[noreturn]] void notAsked() { throw winrt::hresult_not_implemented(); }

std::vector<uint8_t> bytesOf(const IBuffer& b) {
    if (!b || b.Length() == 0) return {};
    return std::vector<uint8_t>(b.data(), b.data() + b.Length());
}

IBuffer bufferOf(const std::vector<uint8_t>& v) {
    winrt::Windows::Storage::Streams::Buffer b(uint32_t(v.size() ? v.size() : 1));
    b.Length(uint32_t(v.size()));
    if (!v.empty()) memcpy(b.data(), v.data(), v.size());
    return b;
}

/// A made-up object as the class of Windows' it stands for.
template <class C, class D, class... A>
C makeAs(A&&... args) {
    return winrt::make<D>(std::forward<A>(args)...).template as<C>();
}

namespace madeup {

/// One of the made-up objects: what winrt::implements makes of it, less the interfaces in `lacks`. Asked for one of
/// those it answers as a Windows older than the interface does, with E_NOINTERFACE.
template <class D, class... I>
struct Made : winrt::implements<D, I...> {
    std::vector<winrt::guid> lacks;

    void* find_interface(const winrt::guid& id) const noexcept override {
        for (const winrt::guid& lacking : lacks)
            if (lacking == id) return nullptr;
        return winrt::implements<D, I...>::find_interface(id);
    }
};

struct ServicesResult : winrt::implements<ServicesResult, IGattDeviceServicesResult> {
    ServicesResult(GattCommunicationStatus status, std::vector<GattDeviceService> services) : status_(status), services_(std::move(services)) {}
    GattCommunicationStatus Status() const { return status_; }
    IReference<uint8_t> ProtocolError() const { return nullptr; }
    IVectorView<GattDeviceService> Services() const { return winrt::single_threaded_vector(std::vector<GattDeviceService>(services_)).GetView(); }

private:
    GattCommunicationStatus status_;
    std::vector<GattDeviceService> services_;
};

struct CharacteristicsResult : winrt::implements<CharacteristicsResult, IGattCharacteristicsResult> {
    CharacteristicsResult(GattCommunicationStatus status, std::vector<GattCharacteristic> list) : status_(status), list_(std::move(list)) {}
    GattCommunicationStatus Status() const { return status_; }
    IReference<uint8_t> ProtocolError() const { return nullptr; }
    IVectorView<GattCharacteristic> Characteristics() const { return winrt::single_threaded_vector(std::vector<GattCharacteristic>(list_)).GetView(); }

private:
    GattCommunicationStatus status_;
    std::vector<GattCharacteristic> list_;
};

struct DescriptorsResult : winrt::implements<DescriptorsResult, IGattDescriptorsResult> {
    DescriptorsResult(GattCommunicationStatus status, std::vector<GattDescriptor> list) : status_(status), list_(std::move(list)) {}
    GattCommunicationStatus Status() const { return status_; }
    IReference<uint8_t> ProtocolError() const { return nullptr; }
    IVectorView<GattDescriptor> Descriptors() const { return winrt::single_threaded_vector(std::vector<GattDescriptor>(list_)).GetView(); }

private:
    GattCommunicationStatus status_;
    std::vector<GattDescriptor> list_;
};

struct ReadResult : Made<ReadResult, IGattReadResult, IGattReadResult2> {
    GattCommunicationStatus status = GattCommunicationStatus::Success;
    IBuffer value{nullptr};
    IReference<uint8_t> error{nullptr};

    GattCommunicationStatus Status() const { return status; }
    IBuffer Value() const { return value; }
    IReference<uint8_t> ProtocolError() const { return error; }
};

struct ConfigurationResult : Made<ConfigurationResult, IGattReadClientCharacteristicConfigurationDescriptorResult,
                                  IGattReadClientCharacteristicConfigurationDescriptorResult2> {
    GattCommunicationStatus status = GattCommunicationStatus::Success;
    int value = 0;
    IReference<uint8_t> error{nullptr};

    GattCommunicationStatus Status() const { return status; }
    GattClientCharacteristicConfigurationDescriptorValue ClientCharacteristicConfigurationDescriptor() const {
        return static_cast<GattClientCharacteristicConfigurationDescriptorValue>(value);
    }
    IReference<uint8_t> ProtocolError() const { return error; }
};

struct WriteResult : winrt::implements<WriteResult, IGattWriteResult> {
    WriteResult(GattCommunicationStatus status, IReference<uint8_t> error) : status_(status), error_(std::move(error)) {}
    GattCommunicationStatus Status() const { return status_; }
    IReference<uint8_t> ProtocolError() const { return error_; }

private:
    GattCommunicationStatus status_;
    IReference<uint8_t> error_;
};

struct Changed : winrt::implements<Changed, IGattValueChangedEventArgs> {
    explicit Changed(IBuffer value) : value_(std::move(value)) {}
    IBuffer CharacteristicValue() const { return value_; }
    winrt::Windows::Foundation::DateTime Timestamp() const { return winrt::clock::now(); }

private:
    IBuffer value_;
};

IReference<uint8_t> attError(uint8_t error) { return winrt::box_value(error).as<IReference<uint8_t>>(); }

/// A read's result as an attribute that answers so gives it: null for none. (One that throws does so itself.)
GattReadResult readResult(Answer how, const std::vector<uint8_t>& value, uint8_t error) {
    if (how == Answer::NoResult) return nullptr;
    const auto r = winrt::make_self<ReadResult>();
    if (how == Answer::Done) {
        r->value = bufferOf(value);
    } else {
        r->status = GattCommunicationStatus::ProtocolError;
        if (how == Answer::Refused) r->error = attError(error);
        else r->lacks.push_back(winrt::guid_of<IGattReadResult2>());
    }
    return r.as<GattReadResult>();
}

/// A write's result, likewise. (Windows' result of a write always has the error's place: untold is none in it.)
GattWriteResult writeResult(Answer how, uint8_t error) {
    if (how == Answer::NoResult) return nullptr;
    if (how == Answer::Done) return makeAs<GattWriteResult, WriteResult>(GattCommunicationStatus::Success, nullptr);
    return makeAs<GattWriteResult, WriteResult>(GattCommunicationStatus::ProtocolError, how == Answer::Refused ? attError(error) : nullptr);
}

/// Windows' GATT session: what keeps the link up, and knows its MTU.
struct Session : Made<Session, IGattSession, IClosable> {
    int pdu = 247;  // the MTU the device and Windows agreed on
    int notYet = 0; // the first askings of it that still say 23, the two not having agreed yet
    std::atomic<int> askings{0};
    std::atomic<bool> maintained{false}, closed{false};
    winrt::event<TypedEventHandler<GattSession, Object>> pduEvent;
    winrt::event<TypedEventHandler<GattSession, GattSessionStatusChangedEventArgs>> statusEvent;

    BluetoothDeviceId DeviceId() const { return nullptr; }
    bool CanMaintainConnection() const { return true; }
    void MaintainConnection(bool value) { maintained = value; }
    bool MaintainConnection() const { return maintained; }
    uint16_t MaxPduSize() { return uint16_t(askings++ < notYet ? 23 : pdu); }
    GattSessionStatus SessionStatus() const { return GattSessionStatus::Active; }
    winrt::event_token MaxPduSizeChanged(TypedEventHandler<GattSession, Object> const& handler) { return pduEvent.add(handler); }
    void MaxPduSizeChanged(winrt::event_token const& token) noexcept { pduEvent.remove(token); }
    winrt::event_token SessionStatusChanged(TypedEventHandler<GattSession, GattSessionStatusChangedEventArgs> const& handler) { return statusEvent.add(handler); }
    void SessionStatusChanged(winrt::event_token const& token) noexcept { statusEvent.remove(token); }
    void Close() { closed = true; }
};

struct Descriptor : Made<Descriptor, IGattDescriptor, IGattDescriptor2> {
    uint16_t handle = 0;
    winrt::guid uuid{};
    std::vector<uint8_t> value; // what a read of it gives, and where a write of it goes
    Answer reads = Answer::Done, writes = Answer::Done;
    uint8_t error = 5;

    GattProtectionLevel ProtectionLevel() const { return GattProtectionLevel::Plain; }
    void ProtectionLevel(GattProtectionLevel const&) {}
    winrt::guid Uuid() const { return uuid; }
    uint16_t AttributeHandle() const { return handle; }
    IAsyncOperation<GattReadResult> ReadValueAsync() { return ReadValueAsync(BluetoothCacheMode::Cached); }
    IAsyncOperation<GattReadResult> ReadValueAsync(BluetoothCacheMode) {
        const auto keep = get_strong();
        co_await winrt::resume_background();
        if (reads == Answer::Throws) throw winrt::hresult_error(E_FAIL);
        co_return readResult(reads, value, error);
    }
    IAsyncOperation<GattCommunicationStatus> WriteValueAsync(IBuffer) { notAsked(); }
    IAsyncOperation<GattWriteResult> WriteValueWithResultAsync(IBuffer data) {
        const auto keep = get_strong();
        co_await winrt::resume_background();
        if (writes == Answer::Throws) throw winrt::hresult_error(E_FAIL);
        if (writes == Answer::Done) value = bytesOf(data);
        co_return writeResult(writes, error);
    }
};

struct Characteristic : Made<Characteristic, IGattCharacteristic, IGattCharacteristic3> {
    uint16_t handle = 0;
    winrt::guid uuid{};
    uint32_t properties = 0;
    std::vector<uint8_t> value;              // what a read of it gives
    std::vector<GattDescriptor> descriptors; // as Windows lists them: a null among them is one
    // A read of it, a write, and the same of its Client Characteristic Configuration, which are Windows' own calls.
    Answer reads = Answer::Done, writes = Answer::Done, configured = Answer::Done, configures = Answer::Done;
    uint8_t error = 5;
    /// What the device does with a write (with a response or without).
    std::function<void(const std::vector<uint8_t>&, bool)> wrote;
    std::atomic<int> listeners{0}, configuration{0};
    winrt::event<TypedEventHandler<GattCharacteristic, GattValueChangedEventArgs>> changed;

    /// A notification or an indication of the device's; with `nothing`, an event of Windows' that brings no arguments.
    void notify(const std::vector<uint8_t>& data, bool nothing = false) {
        GattValueChangedEventArgs args{nullptr};
        if (!nothing) args = makeAs<GattValueChangedEventArgs, Changed>(bufferOf(data));
        changed(get_strong().as<GattCharacteristic>(), args);
    }

    // IGattCharacteristic
    IVectorView<GattDescriptor> GetDescriptors(winrt::guid const&) { notAsked(); }
    GattCharacteristicProperties CharacteristicProperties() const { return static_cast<GattCharacteristicProperties>(properties); }
    GattProtectionLevel ProtectionLevel() const { return GattProtectionLevel::Plain; }
    void ProtectionLevel(GattProtectionLevel const&) {}
    winrt::hstring UserDescription() const { return {}; }
    winrt::guid Uuid() const { return uuid; }
    uint16_t AttributeHandle() const { return handle; }
    IVectorView<GattPresentationFormat> PresentationFormats() { notAsked(); }
    IAsyncOperation<GattReadResult> ReadValueAsync() { return ReadValueAsync(BluetoothCacheMode::Cached); }
    IAsyncOperation<GattReadResult> ReadValueAsync(BluetoothCacheMode) {
        const auto keep = get_strong();
        co_await winrt::resume_background();
        if (reads == Answer::Throws) throw winrt::hresult_error(E_FAIL);
        co_return readResult(reads, value, error);
    }
    IAsyncOperation<GattCommunicationStatus> WriteValueAsync(IBuffer) { notAsked(); }
    IAsyncOperation<GattCommunicationStatus> WriteValueAsync(IBuffer, GattWriteOption) { notAsked(); }
    IAsyncOperation<GattReadClientCharacteristicConfigurationDescriptorResult> ReadClientCharacteristicConfigurationDescriptorAsync() {
        const auto keep = get_strong();
        co_await winrt::resume_background();
        if (configured == Answer::Throws) throw winrt::hresult_error(E_FAIL);
        if (configured == Answer::NoResult) co_return GattReadClientCharacteristicConfigurationDescriptorResult{nullptr};
        const auto r = winrt::make_self<ConfigurationResult>();
        if (configured == Answer::Done) {
            r->value = configuration;
        } else {
            r->status = GattCommunicationStatus::ProtocolError;
            if (configured == Answer::Refused) r->error = attError(error);
            else r->lacks.push_back(winrt::guid_of<IGattReadClientCharacteristicConfigurationDescriptorResult2>());
        }
        co_return r.as<GattReadClientCharacteristicConfigurationDescriptorResult>();
    }
    IAsyncOperation<GattCommunicationStatus> WriteClientCharacteristicConfigurationDescriptorAsync(GattClientCharacteristicConfigurationDescriptorValue) {
        notAsked();
    }
    winrt::event_token ValueChanged(TypedEventHandler<GattCharacteristic, GattValueChangedEventArgs> const& handler) {
        listeners++;
        return changed.add(handler);
    }
    void ValueChanged(winrt::event_token const& token) noexcept {
        listeners--;
        changed.remove(token);
    }

    // IGattCharacteristic3
    IAsyncOperation<GattDescriptorsResult> GetDescriptorsAsync() { return GetDescriptorsAsync(BluetoothCacheMode::Cached); }
    IAsyncOperation<GattDescriptorsResult> GetDescriptorsAsync(BluetoothCacheMode) {
        const auto keep = get_strong();
        co_await winrt::resume_background();
        co_return makeAs<GattDescriptorsResult, DescriptorsResult>(GattCommunicationStatus::Success, descriptors);
    }
    IAsyncOperation<GattDescriptorsResult> GetDescriptorsForUuidAsync(winrt::guid) { notAsked(); }
    IAsyncOperation<GattDescriptorsResult> GetDescriptorsForUuidAsync(winrt::guid, BluetoothCacheMode) { notAsked(); }
    IAsyncOperation<GattWriteResult> WriteValueWithResultAsync(IBuffer data) { return WriteValueWithResultAsync(data, GattWriteOption::WriteWithResponse); }
    IAsyncOperation<GattWriteResult> WriteValueWithResultAsync(IBuffer data, GattWriteOption option) {
        const auto keep = get_strong();
        co_await winrt::resume_background();
        if (writes == Answer::Throws) throw winrt::hresult_error(E_FAIL);
        if (writes == Answer::Done && wrote) wrote(bytesOf(data), option == GattWriteOption::WriteWithResponse);
        co_return writeResult(writes, error);
    }
    IAsyncOperation<GattWriteResult> WriteClientCharacteristicConfigurationDescriptorWithResultAsync(GattClientCharacteristicConfigurationDescriptorValue to) {
        const auto keep = get_strong();
        co_await winrt::resume_background();
        if (configures == Answer::Throws) throw winrt::hresult_error(E_FAIL);
        if (configures == Answer::Done) configuration = int(to);
        co_return writeResult(configures, error);
    }
};

struct Service : Made<Service, IGattDeviceService, IClosable, IGattDeviceService3> {
    enum class Kind {
        Open,
        Kept, // Windows keeps it to itself: access denied, and its consent not given
        Gone, // the device leaves as its characteristics are asked for
    };

    uint16_t handle = 0;
    winrt::guid uuid{};
    Kind kind = Kind::Open;
    GattSession session{nullptr};
    std::vector<GattCharacteristic> characteristics; // as Windows lists them: a null among them is one
    Answer included = Answer::Done;                  // its included services: none, no result, or an error
    std::function<void()> leaves;                    // (Gone) the link's end
    std::atomic<bool> closed{false};

    // IGattDeviceService
    IVectorView<GattCharacteristic> GetCharacteristics(winrt::guid const&) { notAsked(); }
    IVectorView<GattDeviceService> GetIncludedServices(winrt::guid const&) { notAsked(); }
    winrt::hstring DeviceId() const { return {}; }
    winrt::guid Uuid() const { return uuid; }
    uint16_t AttributeHandle() const { return handle; }

    // IGattDeviceService3
    winrt::Windows::Devices::Enumeration::DeviceAccessInformation DeviceAccessInformation() { notAsked(); }
    GattSession Session() const { return session; }
    GattSharingMode SharingMode() const { return GattSharingMode::SharedReadAndWrite; }
    IAsyncOperation<DeviceAccessStatus> RequestAccessAsync() {
        const auto keep = get_strong();
        co_await winrt::resume_background();
        co_return kind == Kind::Kept ? DeviceAccessStatus::DeniedBySystem : DeviceAccessStatus::Allowed;
    }
    IAsyncOperation<GattOpenStatus> OpenAsync(GattSharingMode) { notAsked(); }
    IAsyncOperation<GattCharacteristicsResult> GetCharacteristicsAsync() { return GetCharacteristicsAsync(BluetoothCacheMode::Cached); }
    IAsyncOperation<GattCharacteristicsResult> GetCharacteristicsAsync(BluetoothCacheMode) {
        const auto keep = get_strong();
        co_await winrt::resume_background();
        if (kind == Kind::Kept)
            co_return makeAs<GattCharacteristicsResult, CharacteristicsResult>(GattCommunicationStatus::AccessDenied, std::vector<GattCharacteristic>{});
        if (kind == Kind::Gone) {
            if (leaves) leaves();
            co_return makeAs<GattCharacteristicsResult, CharacteristicsResult>(GattCommunicationStatus::Unreachable, std::vector<GattCharacteristic>{});
        }
        co_return makeAs<GattCharacteristicsResult, CharacteristicsResult>(GattCommunicationStatus::Success, characteristics);
    }
    IAsyncOperation<GattCharacteristicsResult> GetCharacteristicsForUuidAsync(winrt::guid) { notAsked(); }
    IAsyncOperation<GattCharacteristicsResult> GetCharacteristicsForUuidAsync(winrt::guid, BluetoothCacheMode) { notAsked(); }
    IAsyncOperation<GattDeviceServicesResult> GetIncludedServicesAsync() { return GetIncludedServicesAsync(BluetoothCacheMode::Cached); }
    IAsyncOperation<GattDeviceServicesResult> GetIncludedServicesAsync(BluetoothCacheMode) {
        const auto keep = get_strong();
        co_await winrt::resume_background();
        if (included == Answer::Throws) throw winrt::hresult_error(E_FAIL);
        if (included == Answer::NoResult) co_return GattDeviceServicesResult{nullptr};
        co_return makeAs<GattDeviceServicesResult, ServicesResult>(GattCommunicationStatus::Success, std::vector<GattDeviceService>{});
    }
    IAsyncOperation<GattDeviceServicesResult> GetIncludedServicesForUuidAsync(winrt::guid) { notAsked(); }
    IAsyncOperation<GattDeviceServicesResult> GetIncludedServicesForUuidAsync(winrt::guid, BluetoothCacheMode) { notAsked(); }

    void Close() { closed = true; }
};

/// Windows' device. IBluetoothLEDevice4 is the one interface of it that the phone's Windows lacks: what has it here is a
/// later Windows, whose device's id cannot be had (the session by that id would be Windows' own to make, and is not made up).
struct Device : Made<Device, IBluetoothLEDevice, IClosable, IBluetoothLEDevice3, IBluetoothLEDevice4> {
    uint64_t address = 0;
    std::vector<GattDeviceService> services; // as Windows lists them: a null among them is one
    Answer answers = Answer::Done;           // its services asked for: told, no result, or an error
    int outOfRange = 0;                      // the first askings that do not reach it (-1: none does)
    bool slow = false;                       // an asking is not answered until the device is closed (five seconds at most)
    std::atomic<int> askings{0}, listeners{0};
    std::atomic<bool> up{false}, closed{false};
    winrt::event<TypedEventHandler<BluetoothLEDevice, Object>> linkEvent, nameEvent, servicesEvent;

    /// The link made or lost: Windows' ConnectionStatus, and its event where that changed.
    void link(bool now) {
        if (now && closed) return;
        if (up.exchange(now) == now) return;
        linkEvent(get_strong().as<BluetoothLEDevice>(), nullptr);
    }
    winrt::weak_ref<Device> weak() { return get_weak(); }

    // IBluetoothLEDevice
    winrt::hstring DeviceId() const { return L"made-up"; }
    winrt::hstring Name() const { return L"Made up"; }
    IVectorView<GattDeviceService> GattServices() { notAsked(); }
    BluetoothConnectionStatus ConnectionStatus() const { return up ? BluetoothConnectionStatus::Connected : BluetoothConnectionStatus::Disconnected; }
    uint64_t BluetoothAddress() const { return address; }
    GattDeviceService GetGattService(winrt::guid const&) { notAsked(); }
    winrt::event_token NameChanged(TypedEventHandler<BluetoothLEDevice, Object> const& handler) { return nameEvent.add(handler); }
    void NameChanged(winrt::event_token const& token) noexcept { nameEvent.remove(token); }
    winrt::event_token GattServicesChanged(TypedEventHandler<BluetoothLEDevice, Object> const& handler) { return servicesEvent.add(handler); }
    void GattServicesChanged(winrt::event_token const& token) noexcept { servicesEvent.remove(token); }
    winrt::event_token ConnectionStatusChanged(TypedEventHandler<BluetoothLEDevice, Object> const& handler) {
        listeners++;
        return linkEvent.add(handler);
    }
    void ConnectionStatusChanged(winrt::event_token const& token) noexcept {
        listeners--;
        linkEvent.remove(token);
    }

    // IBluetoothLEDevice3
    winrt::Windows::Devices::Enumeration::DeviceAccessInformation DeviceAccessInformation() { notAsked(); }
    IAsyncOperation<DeviceAccessStatus> RequestAccessAsync() { notAsked(); }
    IAsyncOperation<GattDeviceServicesResult> GetGattServicesAsync() { return GetGattServicesAsync(BluetoothCacheMode::Cached); }
    IAsyncOperation<GattDeviceServicesResult> GetGattServicesAsync(BluetoothCacheMode) {
        const auto keep = get_strong();
        co_await winrt::resume_background();
        const int asking = ++askings;
        for (int waited = 0; slow && !closed && waited < 500; waited++) co_await winrt::resume_after(std::chrono::milliseconds(10));
        if (answers == Answer::Throws) throw winrt::hresult_error(E_FAIL);
        if (answers == Answer::NoResult) co_return GattDeviceServicesResult{nullptr};
        if (outOfRange < 0 || asking <= outOfRange)
            co_return makeAs<GattDeviceServicesResult, ServicesResult>(GattCommunicationStatus::Unreachable, std::vector<GattDeviceService>{});
        // (Its services asked for afresh are what Windows connects for: the link is up, and said to be, before they are told.)
        link(true);
        co_return makeAs<GattDeviceServicesResult, ServicesResult>(GattCommunicationStatus::Success, services);
    }
    IAsyncOperation<GattDeviceServicesResult> GetGattServicesForUuidAsync(winrt::guid) { notAsked(); }
    IAsyncOperation<GattDeviceServicesResult> GetGattServicesForUuidAsync(winrt::guid, BluetoothCacheMode) { notAsked(); }

    // IBluetoothLEDevice4
    winrt::Windows::Devices::Bluetooth::BluetoothDeviceId BluetoothDeviceId() { throw winrt::hresult_error(E_FAIL); }

    void Close() {
        closed = true;
        up = false;
    }
};

} // namespace madeup

// ------------------------------------------------------------------ the devices, by their addresses

/// A made-up device: the word that says what it is like (mg_tests_main.c lists them), and what was last made of it -- each
/// connection gets a device of its own, as each gets one of Windows'.
struct Entry {
    std::string what;
    winrt::com_ptr<madeup::Device> device;
    winrt::com_ptr<madeup::Session> session;
    std::map<int, winrt::com_ptr<madeup::Characteristic>> characteristics;
};

std::mutex gEntriesMutex;
std::map<uint64_t, Entry> gEntries;

winrt::guid uuid16(uint16_t of) { return winrt::guid{of, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5f, 0x9b, 0x34, 0xfb}}; }
/// Nordic's UART service and its two characteristics (6E400001, 2 and 3), which MeshCore's radios speak through.
winrt::guid uartUuid(uint32_t of) { return winrt::guid{0x6E400000u | of, 0xB5A3, 0xF393, {0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E}}; }

winrt::com_ptr<madeup::Service> serviceAt(uint16_t handle, const winrt::guid& uuid, const GattSession& session) {
    const auto s = winrt::make_self<madeup::Service>();
    s->handle = handle;
    s->uuid = uuid;
    s->session = session;
    return s;
}

winrt::com_ptr<madeup::Characteristic> characteristicAt(Entry& e, madeup::Service& of, uint16_t handle, const winrt::guid& uuid, uint32_t properties) {
    const auto c = winrt::make_self<madeup::Characteristic>();
    c->handle = handle;
    c->uuid = uuid;
    c->properties = properties;
    e.characteristics[handle] = c;
    of.characteristics.push_back(c.as<GattCharacteristic>());
    return c;
}

winrt::com_ptr<madeup::Descriptor> descriptorAt(madeup::Characteristic& of, uint16_t handle, const winrt::guid& uuid, const char* value) {
    const auto d = winrt::make_self<madeup::Descriptor>();
    d->handle = handle;
    d->uuid = uuid;
    d->value.assign(value, value + strlen(value));
    of.descriptors.push_back(d.as<GattDescriptor>());
    return d;
}

constexpr uint32_t canRead = 0x02, canWriteWithoutResponse = 0x04, canWrite = 0x08, notifies = 0x10, indicates = 0x20;

/*
 * The services of the word "phone": a radio as MeshCore's is -- Nordic's UART service, one characteristic written to and
 * one that notifies, which here says every write back (a first byte 1 for a write with a response, 0 for one without, then
 * what was written) -- and beside it an attribute for each way Windows has of not answering, and a null wherever a list of
 * Windows' can have one.
 */
void phoneServices(Entry& e, madeup::Device& device, const GattSession& session) {
    device.services.push_back(nullptr);

    const auto uart = serviceAt(0x0010, uartUuid(1), session);
    uart->included = Answer::NoResult;
    uart->characteristics.push_back(nullptr);
    const auto rx = characteristicAt(e, *uart, 0x0012, uartUuid(2), canWrite | canWriteWithoutResponse);
    const auto tx = characteristicAt(e, *uart, 0x0015, uartUuid(3), notifies);
    descriptorAt(*tx, 0x0016, uuid16(0x2902), "");
    rx->wrote = [tx](const std::vector<uint8_t>& data, bool response) {
        if (tx->listeners <= 0) return;
        std::vector<uint8_t> back{uint8_t(response ? 1 : 0)};
        back.insert(back.end(), data.begin(), data.end());
        tx->notify(back);
    };
    device.services.push_back(uart.as<GattDeviceService>());

    const auto info = serviceAt(0x0020, uuid16(0x180A), session);
    info->included = Answer::Throws;
    const auto name = characteristicAt(e, *info, 0x0022, uuid16(0x2A29), canRead);
    name->value = {'M', 'e', 'g', 'l', 'e', 'r'};
    characteristicAt(e, *info, 0x0024, uuid16(0x2A24), canRead)->reads = Answer::Refused;
    characteristicAt(e, *info, 0x0026, uuid16(0x2A25), canRead)->reads = Answer::RefusedUntold;
    const auto silent = characteristicAt(e, *info, 0x0028, uuid16(0x2A26), canRead | canWrite);
    silent->reads = silent->writes = Answer::NoResult;
    const auto failing = characteristicAt(e, *info, 0x002A, uuid16(0x2A27), canRead | canWrite);
    failing->reads = failing->writes = Answer::Throws;
    // (Indicates, and Windows lists no descriptor of it: its Client Characteristic Configuration is made up by the host.
    // Windows' call for it fails.)
    const auto unlisted = characteristicAt(e, *info, 0x002C, uuid16(0x2A28), indicates);
    unlisted->configures = Answer::Throws;
    unlisted->configured = Answer::RefusedUntold;
    // (An object of an older Windows: its descriptors cannot be asked for, nor can it be written to with a result.)
    characteristicAt(e, *info, 0x002E, uuid16(0x2A23), canWrite)->lacks.push_back(winrt::guid_of<IGattCharacteristic3>());
    const auto described = characteristicAt(e, *info, 0x0030, uuid16(0x2A50), canRead | canWrite);
    described->writes = Answer::Refused;
    described->error = 3; // write not permitted
    described->descriptors.push_back(nullptr);
    descriptorAt(*described, 0x0031, uuid16(0x2901), "about");
    const auto old = descriptorAt(*described, 0x0032, uuid16(0x2904), "");
    old->reads = Answer::Refused;
    old->error = 2; // read not permitted
    old->lacks.push_back(winrt::guid_of<IGattDescriptor2>());
    device.services.push_back(info.as<GattDeviceService>());

    const auto kept = serviceAt(0x0040, uuid16(0x1812), session);
    kept->kind = madeup::Service::Kind::Kept;
    device.services.push_back(kept.as<GattDeviceService>());
}

/// A device for the address, made anew: what the host is given where Windows would be asked. Null for none.
Object deviceOf(unsigned long long address) {
    std::lock_guard<std::mutex> lock(gEntriesMutex);
    const auto it = gEntries.find(address);
    if (it == gEntries.end()) return nullptr;
    Entry& e = it->second;
    const std::string& what = e.what;
    if (what == "none") return nullptr;
    // (Something of Windows' that is no device.)
    if (what == "not-a-device") return winrt::box_value(uint32_t(1));

    const auto device = winrt::make_self<madeup::Device>();
    const auto session = winrt::make_self<madeup::Session>();
    device->address = address;
    e.characteristics.clear();
    // Windows 10 Mobile (10.0.15254, UniversalApiContract 4.0) has every interface the made-up objects have but this one.
    if (what != "newer") device->lacks.push_back(winrt::guid_of<IBluetoothLEDevice4>());
    if (what == "old") device->lacks.push_back(winrt::guid_of<IBluetoothLEDevice3>());
    if (what == "unreachable") device->outOfRange = -1;
    if (what == "later") device->outOfRange = 2;
    if (what == "no-result") device->answers = Answer::NoResult;
    if (what == "throws") device->answers = Answer::Throws;
    if (what == "slow") device->slow = true;
    if (what == "phone") {
        // (The MTU: not agreed on yet the first two times it is asked for.)
        session->notYet = 2;
        phoneServices(e, *device, session.as<GattSession>());
    } else {
        // One service with one characteristic, read as "Megler".
        const auto s = serviceAt(0x0010, uuid16(0x180A), what == "no-session" ? GattSession{nullptr} : session.as<GattSession>());
        characteristicAt(e, *s, 0x0012, uuid16(0x2A29), canRead)->value = {'M', 'e', 'g', 'l', 'e', 'r'};
        if (what == "old-services") s->lacks.push_back(winrt::guid_of<IGattDeviceService3>());
        if (what == "leaves") {
            s->kind = madeup::Service::Kind::Gone;
            s->leaves = [weak = device->weak()] {
                if (const auto d = weak.get()) d->link(false);
            };
        }
        device->services.push_back(s.as<GattDeviceService>());
    }
    e.device = device;
    e.session = session;
    return device.as<Object>();
}

/// What was last made for the address; false for nothing.
bool entryOf(unsigned long long address, Entry& out) {
    std::lock_guard<std::mutex> lock(gEntriesMutex);
    const auto it = gEntries.find(address);
    if (it == gEntries.end()) return false;
    out = it->second;
    return true;
}

} // namespace

extern "C" {

const megler_bluetooth_host* mg_bluetooth_windows_host(void) {
    static megler_bluetooth_host host;
    host = megler::app::BluetoothHost::shared().callbacks();
    return &host;
}

/// What Windows has, in a line.
void mg_bluetooth_windows_summary(char* out, size_t size) { snprintf(out, size, "%s", megler::app::BluetoothHost::shared().summary().c_str()); }

/// The host's devices are the made-up ones from now on (a connection made `auto` tried again every `again_ms`), or
/// Windows' own again, the made-up ones forgotten.
void mg_bluetooth_windows_devices(int made_up, int again_ms) {
    if (made_up) {
        megler::app::BluetoothHost::shared().devicesFrom(&deviceOf, again_ms);
        return;
    }
    megler::app::BluetoothHost::shared().devicesFrom(nullptr);
    std::lock_guard<std::mutex> lock(gEntriesMutex);
    gEntries.clear();
}

/*
 * What the device of that address is like, from its next connection on:
 *   phone          as Windows 10 Mobile gives a radio like MeshCore's (phoneServices)
 *   none           Windows has no such device
 *   not-a-device   Windows hands over something that is no device
 *   old            a device without IBluetoothLEDevice3: its services cannot be asked for
 *   unreachable    never in range
 *   later          in range the third time its services are asked for
 *   no-result      its services' asking ends with no result
 *   throws         its services' asking fails
 *   slow           its services are not told until the device is closed
 *   no-session     its services have no GATT session
 *   old-services   its services are without IGattDeviceService3: no session, no characteristics
 *   leaves         it goes out of range as its characteristics are asked for
 *   newer          a device of a later Windows (IBluetoothLEDevice4), whose id cannot be had
 * All but `newer` are without IBluetoothLEDevice4, as the phone's Windows is; all but `phone` have one service with one
 * characteristic, and a session that says the MTU is 247 at once.
 */
void mg_bluetooth_windows_device(unsigned long long address, const char* what) {
    std::lock_guard<std::mutex> lock(gEntriesMutex);
    Entry& e = gEntries[address];
    e = Entry{};
    e.what = what ? what : "none";
}

/// How many times the services of the device last made for the address were asked for.
int mg_bluetooth_windows_asked(unsigned long long address) {
    Entry e;
    return entryOf(address, e) && e.device ? int(e.device->askings) : 0;
}

/// The device last made for the address: 1 its link is up, 2 its session is told to keep the link up, 4 its session is
/// closed, 8 it is closed itself, 16 its ConnectionStatusChanged is listened to.
int mg_bluetooth_windows_state(unsigned long long address) {
    Entry e;
    if (!entryOf(address, e) || !e.device) return 0;
    return (e.device->up ? 1 : 0) | (e.session->maintained ? 2 : 0) | (e.session->closed ? 4 : 0) | (e.device->closed ? 8 : 0) |
           (e.device->listeners > 0 ? 16 : 0);
}

/// How many listen to the changes of the characteristic at that handle; -1 for no such characteristic.
int mg_bluetooth_windows_listeners(unsigned long long address, int handle) {
    Entry e;
    if (!entryOf(address, e)) return -1;
    const auto it = e.characteristics.find(handle);
    return it == e.characteristics.end() ? -1 : int(it->second->listeners);
}

/// The characteristic notifies: those bytes, or with a size of -1 an event of Windows' without arguments. How many heard.
int mg_bluetooth_windows_notify(unsigned long long address, int handle, const unsigned char* data, int size) {
    Entry e;
    if (!entryOf(address, e)) return -1;
    const auto it = e.characteristics.find(handle);
    if (it == e.characteristics.end()) return -1;
    std::vector<uint8_t> bytes;
    if (data && size > 0) bytes.assign(data, data + size);
    it->second->notify(bytes, size < 0);
    return int(it->second->listeners);
}

/// The device's link made or lost, as Windows says it of itself.
void mg_bluetooth_windows_link(unsigned long long address, int up) {
    Entry e;
    if (entryOf(address, e) && e.device) e.device->link(up != 0);
}

} // extern "C"
