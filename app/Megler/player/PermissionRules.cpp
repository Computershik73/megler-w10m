// The part of player/PermissionHost.h that needs neither Windows' objects nor a dialog: Android's groups and who decides
// each, what the user said of them (the app's settings) and of Google Play services' own location (Megler's), and what an
// app is told. Built into the shell's tests too (core/tests/mg_shell_windows.cpp).
#include "pch.h"

#include "player/PermissionHost.h"

#include "library/Library.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"

#include <cstring>
#include <iterator>
#include <map>
#include <mutex>

namespace megler::app::permissionHost {

namespace {

/* Android 14's dangerous permissions by group, as the framework's megler.Permissions has them. */
const struct {
    const char* permission;
    const char* group;
} kPermissions[] = {
    {"READ_CALENDAR", "calendar"},           {"WRITE_CALENDAR", "calendar"},
    {"READ_CALL_LOG", "call_log"},           {"WRITE_CALL_LOG", "call_log"},
    {"PROCESS_OUTGOING_CALLS", "call_log"},  {"CAMERA", "camera"},
    {"READ_CONTACTS", "contacts"},           {"WRITE_CONTACTS", "contacts"},
    {"GET_ACCOUNTS", "contacts"},            {"ACCESS_FINE_LOCATION", "location"},
    {"ACCESS_COARSE_LOCATION", "location"},  {"ACCESS_BACKGROUND_LOCATION", "location"},
    {"RECORD_AUDIO", "microphone"},          {"READ_PHONE_STATE", "phone"},
    {"READ_PHONE_NUMBERS", "phone"},         {"CALL_PHONE", "phone"},
    {"ANSWER_PHONE_CALLS", "phone"},         {"ADD_VOICEMAIL", "phone"},
    {"USE_SIP", "phone"},                    {"ACCEPT_HANDOVER", "phone"},
    {"BODY_SENSORS", "sensors"},             {"BODY_SENSORS_BACKGROUND", "sensors"},
    {"SEND_SMS", "sms"},                     {"RECEIVE_SMS", "sms"},
    {"READ_SMS", "sms"},                     {"RECEIVE_WAP_PUSH", "sms"},
    {"RECEIVE_MMS", "sms"},                  {"READ_CELL_BROADCASTS", "sms"},
    {"READ_EXTERNAL_STORAGE", "storage"},    {"WRITE_EXTERNAL_STORAGE", "storage"},
    {"READ_MEDIA_IMAGES", "read_media_visual"}, {"READ_MEDIA_VIDEO", "read_media_visual"},
    {"READ_MEDIA_VISUAL_USER_SELECTED", "read_media_visual"}, {"ACCESS_MEDIA_LOCATION", "read_media_visual"},
    {"READ_MEDIA_AUDIO", "read_media_aural"}, {"BLUETOOTH_SCAN", "nearby_devices"},
    {"BLUETOOTH_CONNECT", "nearby_devices"}, {"BLUETOOTH_ADVERTISE", "nearby_devices"},
    {"NEARBY_WIFI_DEVICES", "nearby_devices"}, {"UWB_RANGING", "nearby_devices"},
    {"POST_NOTIFICATIONS", "notifications"}, {"ACTIVITY_RECOGNITION", "activity_recognition"},
};

/* The groups in Android's order: what each lets an app have, its title in Android's settings, who decides it by default, its
   icon, Megler's question (after "Allow <app> to ") and what it amounts to here. */
struct Kind {
    const char* group;
    const char* what;
    const char* title;
    Decider decider;
    wchar_t icon;
    const char* question;
    const char* explanation;
};
/* Google Location Accuracy's group, Megler's own and no permission of Android's: asked of an app that holds the location, as
   its Google Play services want it (megler.LocationAccuracy, which writes the answer into Google's settings, every app's). */
constexpr const char* kLocationAccuracy = "location_accuracy";

const Kind kKinds[] = {
    {"location", "the location", "Location", Decider::windows, 0xE707, "use this phone's location?",
     "Where the phone is, as Windows finds it: the satellites, Wi-Fi, the cell network."},
    {kLocationAccuracy, "Google Location Accuracy", "Google Location Accuracy", Decider::megler, 0xE81D, "use Google Location Accuracy?",
     "Google Play services in the app then finds the phone from the Wi-Fi networks around it, as on Android: it sends the networks "
     "it hears, their addresses and signals, to Google, which answers where they are -- within tens of metres, where Windows finds "
     "the phone by the cell network alone. It is one setting for every app, as Android's is."},
    {"camera", "the camera", "Camera", Decider::windows, 0xE722, "take pictures and record video?", "The phone's cameras."},
    {"microphone", "the microphone", "Microphone", Decider::windows, 0xE720, "record audio?", "The phone's microphone."},
    {"storage", "photos, media and files", "Files and media", Decider::megler, 0xE8B7, "have the photos, media and files on this phone?",
     "The phone's pictures, music and videos, and the files Android apps keep in their shared storage (/sdcard)."},
    {"read_media_visual", "photos and videos", "Photos and videos", Decider::megler, 0xE8B9, "have the photos and videos on this phone?",
     "The phone's Pictures, Videos and the camera's pictures."},
    {"read_media_aural", "music and audio", "Music and audio", Decider::megler, 0xE8D6, "have the music and audio on this phone?",
     "The phone's Music."},
    {"sms", "text messages", "SMS", Decider::megler, 0xE8BD, "read text messages?",
     "The text messages that come in while it runs: a code it waits for comes so. None that came before."},
    {"phone", "the phone's calls", "Phone", Decider::megler, 0xE717, "know of the phone's calls?",
     "That a call comes in or ends while it runs, and the phone's SIM."},
    {"call_log", "the call history", "Call logs", Decider::megler, 0xE81C, "read the call history?",
     "The calls of the phone's history that end while it runs, with their numbers."},
    {"contacts", "contacts", "Contacts", Decider::none, 0xE77B, "have your contacts?",
     "Megler has none of the phone's contacts to give: the app finds none, allowed or not. Its own accounts are its own."},
    {"calendar", "the calendar", "Calendar", Decider::none, 0xE787, "have your calendar?",
     "Megler has none of the phone's calendar to give: the app finds none, allowed or not."},
    {"sensors", "body sensors", "Body sensors", Decider::none, 0xE95E, "have the body sensors' readings?",
     "Megler has no body sensors to give: the app finds none, allowed or not."},
    {"nearby_devices", "nearby devices", "Nearby devices", Decider::none, 0xE702, "find and connect to nearby devices?",
     "Bluetooth LE's devices around the phone. Windows refuses them too where its privacy settings of other devices say so."},
    {"notifications", "notifications", "Notifications", Decider::none, 0xEA8F, "send you notifications?",
     "Its notifications: Megler shows none of them, allowed or not."},
    {"activity_recognition", "physical activity", "Physical activity", Decider::none, 0xE726, "have your physical activity?",
     "Megler has no activity recognition to give: the app finds none, allowed or not."},
};

const Kind* kindOf(const std::string& group) {
    for (const Kind& k : kKinds)
        if (group == k.group) return &k;
    return nullptr;
}

Group groupOf(const Kind& k) { return Group{k.group, k.what, k.title, k.decider, k.icon}; }

const char* keptName(int state) {
    switch (state) {
    case MEGLER_PERMISSION_GRANTED: return "granted";
    case MEGLER_PERMISSION_DENIED: return "denied";
    case MEGLER_PERMISSION_DENIED_FIXED: return "denied-fixed";
    case EVERY_TIME: return "ask";
    default: return "";
    }
}

/* This run's answers to the groups asked every time, by "<package>/<group>". */
std::mutex gRunMutex;
std::map<std::string, int> gThisRun;

} // namespace

std::vector<Group> groupsOf(const ApkReport& report) {
    std::vector<Group> out;
    if (!report.permissionsKnown) return out;
    // ACCESS_MEDIA_LOCATION goes with the storage where the app asks for that (an app of Android 10 to 12), as megler.Permissions
    // takes it.
    const bool storage = report.asks("android.permission.READ_EXTERNAL_STORAGE");
    for (const Kind& kind : kKinds) {
        bool asked = false;
        // (Google Location Accuracy is the app's to decide where it holds the location in the foreground: its Google Play
        // services find the phone so.)
        const bool accuracy = !strcmp(kind.group, kLocationAccuracy);
        for (const auto& p : kPermissions) {
            const bool mediaLocation = !strcmp(p.permission, "ACCESS_MEDIA_LOCATION");
            const char* group = mediaLocation && storage ? "storage" : p.group;
            if (accuracy && !strcmp(p.permission, "ACCESS_BACKGROUND_LOCATION")) continue;
            if (!strcmp(group, accuracy ? "location" : kind.group) && report.asks(std::string("android.permission.") + p.permission)) asked = true;
        }
        if (asked) out.push_back(groupOf(kind));
    }
    return out;
}

const Group* groupNamed(const std::string& name) {
    static const std::vector<Group> all = [] {
        std::vector<Group> groups;
        for (const Kind& k : kKinds) groups.push_back(groupOf(k));
        return groups;
    }();
    for (const Group& g : all)
        if (g.name == name) return &g;
    return nullptr;
}

std::string groupOfPermission(const std::string& permission) {
    const std::string android = "android.permission.";
    if (permission.compare(0, android.size(), android) != 0) return std::string();
    for (const auto& p : kPermissions)
        if (permission.compare(android.size(), std::string::npos, p.permission) == 0) return p.group;
    return std::string();
}

std::string questionOf(const std::string& group, const std::string& app) {
    const Kind* k = kindOf(group);
    return "Allow " + app + " to " + (k ? k->question : "have " + group + "?");
}

std::string explanationOf(const std::string& group) {
    const Kind* k = kindOf(group);
    return k ? k->explanation : std::string();
}

int kept(const std::string& package, const std::string& group) {
    if (package.empty()) return MEGLER_PERMISSION_UNDECIDED;
    const std::string value = settings::app(package, "megler.permission." + group).value_or("");
    if (value == "granted") return MEGLER_PERMISSION_GRANTED;
    if (value == "denied") return MEGLER_PERMISSION_DENIED;
    if (value == "denied-fixed") return MEGLER_PERMISSION_DENIED_FIXED;
    if (value == "ask") return EVERY_TIME;
    return MEGLER_PERMISSION_UNDECIDED;
}

void keep(const std::string& package, const std::string& group, int state) {
    if (package.empty()) return;
    const char* name = keptName(state);
    settings::setApp(package, "megler.permission." + group, name[0] ? std::optional<std::string>(name) : std::nullopt);
    // A question asked every time is asked again from now on: what this run said of it went with it.
    rememberThisRun(package, group, MEGLER_PERMISSION_UNDECIDED);
}

void forgetAll(const std::string& package) {
    if (package.empty()) return;
    const int forgotten = settings::eraseApp(package, "megler.permission.");
    {
        std::lock_guard<std::mutex> lock(gRunMutex);
        for (auto it = gThisRun.begin(); it != gThisRun.end();)
            it = it->first.compare(0, package.size() + 1, package + "/") == 0 ? gThisRun.erase(it) : std::next(it);
    }
    shellLog::write("permissions: " + package + "'s data is cleared: " +
                    (forgotten ? std::to_string(forgotten) + " answer(s) forgotten, the app asks again" : std::string("no answers kept to forget")));
}

int decide(Decider decider, int kept, int windows, int thisRun) {
    // The user's no stands whoever else decides: Don't allow (for good), or the first no to Megler's question.
    if (kept == MEGLER_PERMISSION_DENIED_FIXED || kept == MEGLER_PERMISSION_DENIED) return kept;
    if (kept == EVERY_TIME) {
        // Not this run's yet: the app is asked when it requests it, and has it not meanwhile.
        if (thisRun != MEGLER_PERMISSION_GRANTED) return thisRun;
        return decider == Decider::windows ? windows : MEGLER_PERMISSION_GRANTED;
    }
    // Allowed by the user, or nothing said: who decides it by default (Windows' yes and no stay Windows').
    if (decider == Decider::windows) return windows;
    if (kept == MEGLER_PERMISSION_GRANTED || decider == Decider::none) return MEGLER_PERMISSION_GRANTED;
    return MEGLER_PERMISSION_UNDECIDED;
}

int thisRun(const std::string& package, const std::string& group) {
    std::lock_guard<std::mutex> lock(gRunMutex);
    auto it = gThisRun.find(package + "/" + group);
    return it == gThisRun.end() ? MEGLER_PERMISSION_UNDECIDED : it->second;
}

void rememberThisRun(const std::string& package, const std::string& group, int state) {
    std::lock_guard<std::mutex> lock(gRunMutex);
    if (state == MEGLER_PERMISSION_UNDECIDED)
        gThisRun.erase(package + "/" + group);
    else
        gThisRun[package + "/" + group] = state;
}

bool refusedByUser(const std::string& package, const std::string& group) {
    const int said = kept(package, group);
    if (said == MEGLER_PERMISSION_DENIED || said == MEGLER_PERMISSION_DENIED_FIXED) return true;
    if (said != EVERY_TIME) return false;
    const int now = thisRun(package, group);
    return now == MEGLER_PERMISSION_DENIED || now == MEGLER_PERMISSION_DENIED_FIXED;
}

int gmsLocation() {
    const std::string value = settings::meglerNow(settings::gmsLocation).value_or("");
    return value == "granted" ? MEGLER_PERMISSION_GRANTED : value == "denied" ? MEGLER_PERMISSION_DENIED_FIXED : MEGLER_PERMISSION_UNDECIDED;
}

void keepGmsLocation(int state) {
    settings::setMegler(settings::gmsLocation, state == MEGLER_PERMISSION_GRANTED ? "granted" : state == MEGLER_PERMISSION_DENIED_FIXED ? "denied" : "");
    shellLog::write(std::string("permissions: Google Play services' own use of the location, for every app: ") +
                    (state == MEGLER_PERMISSION_GRANTED ? "allowed" : state == MEGLER_PERMISSION_DENIED_FIXED ? "not allowed" : "to be asked"));
}

} // namespace megler::app::permissionHost
