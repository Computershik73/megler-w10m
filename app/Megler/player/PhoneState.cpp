#include "pch.h"

#include "player/PhoneState.h"

#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include "megler.h"

#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.System.Power.h>
#include <winrt/Windows.System.UserProfile.h>

#include <climits>

namespace megler::app::phoneState {
namespace {

using winrt::Windows::System::Power::BatteryStatus;
using winrt::Windows::System::Power::PowerManager;
using winrt::Windows::System::Power::PowerSupplyStatus;

// Android's numbers (BatteryManager.BATTERY_STATUS_*, BATTERY_PLUGGED_*).
constexpr int statusUnknown = 1, statusCharging = 2, statusDischarging = 3, statusNotCharging = 4, statusFull = 5;
constexpr int pluggedNone = 0, pluggedMains = 1, pluggedUsb = 2;

std::mutex gMutex;
bool gTold = false;
std::string gLast; // the battery as the core was last told, in the log's words

void tellBattery() {
    try {
        const BatteryStatus status = PowerManager::BatteryStatus();
        const PowerSupplyStatus supply = PowerManager::PowerSupplyStatus();
        const int percent = PowerManager::RemainingChargePercent();
        megler_battery now = {};
        now.present = status != BatteryStatus::NotPresent;
        now.level = percent < 0 ? 0 : percent > 100 ? 100 : percent;
        // (Windows does not say what the power comes from: enough of it is taken for a charger, too little for a USB port.)
        now.plugged = supply == PowerSupplyStatus::NotPresent ? pluggedNone : supply == PowerSupplyStatus::Adequate ? pluggedMains : pluggedUsb;
        now.status = status == BatteryStatus::Charging      ? statusCharging
                     : status == BatteryStatus::Discharging ? statusDischarging
                     : status == BatteryStatus::Idle        ? (now.level >= 100 ? statusFull : statusNotCharging)
                                                            : statusUnknown;
        now.voltage_mv = 0;
        now.temperature = INT_MIN;
        const std::string line = format("%d %%, %s, %s", now.level,
                                        now.status == statusCharging      ? "charging"
                                        : now.status == statusDischarging ? "discharging"
                                        : now.status == statusFull        ? "full"
                                        : now.status == statusNotCharging ? "not charging"
                                                                          : "state unknown",
                                        now.plugged == pluggedNone ? "on battery" : now.plugged == pluggedMains ? "plugged in" : "plugged in, weakly");
        {
            std::lock_guard<std::mutex> lock(gMutex);
            if (line == gLast) return;
            gLast = line;
        }
        megler_set_battery(&now);
        shellLog::write("phone: the battery is at " + line);
    } catch (const winrt::hresult_error& e) {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gLast == "?") return;
        gLast = "?";
        shellLog::write(format("phone: Windows does not say how the battery is (0x%08X): an app is told it is full", unsigned(e.code())));
    }
}

/// The phone's language and region as a BCP 47 tag: its first display language's language (and script), its home region.
std::string phoneLocale() {
    using winrt::Windows::System::UserProfile::GlobalizationPreferences;
    std::string language = "en", region;
    try {
        auto languages = GlobalizationPreferences::Languages();
        if (languages.Size() > 0) language = narrow(std::wstring(languages.GetAt(0)));
        region = narrow(std::wstring(GlobalizationPreferences::HomeGeographicRegion()));
    } catch (const winrt::hresult_error&) {
    }
    // "ru-RU" -> "ru", "zh-Hant-TW" -> "zh-Hant": the language's own subtags, without the region it names.
    std::string tag;
    size_t at = 0;
    for (int part = 0; at <= language.size(); part++) {
        size_t end = language.find('-', at);
        if (end == std::string::npos) end = language.size();
        const std::string sub = language.substr(at, end - at);
        if (part == 0) tag = sub;
        else if (sub.size() == 4) tag += "-" + sub;
        at = end + 1;
    }
    const bool letters = region.size() == 2 && isalpha((unsigned char)region[0]) && isalpha((unsigned char)region[1]);
    const bool digits = region.size() == 3 && isdigit((unsigned char)region[0]);
    if (letters || digits) tag += "-" + region;
    return tag;
}

} // namespace

std::string locale() { return phoneLocale(); }

/// The language and region: as set in Settings, else the phone's. At every start: Settings may have changed it.
void tellLocale() {
    const std::string chosen = settings::get(settings::localeTag, "");
    const std::string tag = chosen.empty() ? phoneLocale() : chosen;
    megler_set_locale(tag.c_str());
    shellLog::write("phone: the language and region are " + tag + (chosen.empty() ? " (the phone's)" : " (set in Settings)"));
}

void tell() {
    tellLocale();
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gTold) return;
        gTold = true;
    }
    try {
        const std::string zone = narrow(std::wstring(winrt::Windows::Globalization::Calendar().GetTimeZone()));
        megler_set_timezone(zone.c_str());
        shellLog::write("phone: the time zone is " + zone);
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("phone: Windows does not say the time zone (0x%08X): an app's clock is UTC", unsigned(e.code())));
    }
    tellBattery();
    // The handlers are never taken off: they are wanted until the process ends. They come on threads of Windows' own.
    try {
        PowerManager::RemainingChargePercentChanged([](auto&&, auto&&) { tellBattery(); });
        PowerManager::BatteryStatusChanged([](auto&&, auto&&) { tellBattery(); });
        PowerManager::PowerSupplyStatusChanged([](auto&&, auto&&) { tellBattery(); });
    } catch (const winrt::hresult_error& e) {
        shellLog::write(format("phone: no word of the battery's changes (0x%08X): an app is told how it was at its start", unsigned(e.code())));
    }
}

} // namespace megler::app::phoneState
