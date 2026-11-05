#include "pch.h"

#include "library/PhoneAppsList.h"

#include "library/Library.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include "megler.h"

namespace megler::app::phoneAppsList {

namespace {

/// A field of a line: no tab or line end in it.
std::string field(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
}

struct Item {
    std::string line, package;
    std::wstring icon;
    std::wstring apk, splits; // the APK Megler reads and its splits' folder (empty for an app in one APK)
    int64_t versionCode = 0;
};

/// The app's activities in <dir>\<package> (megler_apk_describe_activities), written again only when the APK is another one:
/// its path, size and version are in source.txt there, after the description's form (2: with the APK's certificates; the
/// descriptions of before have none). False when they could not be read.
bool describe(const std::wstring& dir, const Item& item) {
    if (item.apk.empty()) return false;
    const std::wstring out = dir + L"\\" + widen(item.package);
    const std::string source = "description 2\n" + narrow(item.apk) + "\n" + std::to_string((unsigned long long)files::size(item.apk)) +
                               "\n" + std::to_string((long long)item.versionCode) + "\n";
    std::string had;
    if (files::read(out + L"\\source.txt", had) && had == source && files::exists(out + L"\\activities.json")) return true;
    // Described beside the one there ('~' is in no Android package's name), which goes only once the new one is whole: an APK
    // Megler could not read (in a package installed as the Store installs an app, by 640 to 644) had left the app with none,
    // and the other apps without its activities and its certificates.
    const std::wstring fresh = out + L"~new";
    files::removeTree(fresh);
    const std::string splits = item.splits.empty() || !files::exists(item.splits) ? std::string() : narrow(item.splits);
    if (!megler_apk_describe_activities(narrow(item.apk).c_str(), splits.empty() ? nullptr : splits.c_str(), narrow(fresh).c_str())) {
        files::removeTree(fresh);
        const bool kept = files::exists(out + L"\\activities.json");
        shellLog::write("phone apps: the activities of " + item.package + " could not be read" + (kept ? "; those described before are kept" : ""));
        return kept;
    }
    files::write(fresh + L"\\source.txt", source);
    files::removeTree(out);
    if (!files::move(fresh, out)) {
        shellLog::write(format("phone apps: the description of %s could not be put in place (error %lu)", item.package.c_str(), GetLastError()));
        files::removeTree(fresh);
        return false;
    }
    return true;
}

} // namespace

std::wstring folder() {
    const std::wstring shared = appPackage::sharedFolder();
    return shared.empty() ? std::wstring() : shared + L"\\phone-apps";
}

void installOutcome(const std::string& name, const std::string& what, const std::string& why) {
    const std::wstring dir = folder();
    if (dir.empty() || name.empty() || name.find_first_of("\\/:") != std::string::npos) return;
    files::makeDirectories(dir + L"\\installs");
    // when it was told, first: the same outcome told again is still news to the session that waits for it
    const std::string line = std::to_string((unsigned long long)GetTickCount64()) + "\t" + what + "\t" + field(why) + "\n";
    if (files::write(dir + L"\\installs\\" + widen(name) + L".txt", line))
        shellLog::write("phone apps: " + name + " " + what + " told to the app that asked for it");
    else
        shellLog::write("phone apps: " + name + " " + what + " could not be told to the app that asked for it");
}

void publish() {
    static unsigned published = ~0u;
    if (appPackage::isSingleApp()) return;
    const unsigned generation = Library::instance().generation();
    if (generation == published) return;
    published = generation;
    const std::wstring dir = folder();
    if (dir.empty()) return;
    std::vector<Item> items;
    for (const ApkEntry& e : Library::instance().entries()) {
        const ApkReport& r = e.report;
        if (!e.analysed || !r.ready() || r.package.empty()) continue;
        Item item;
        item.package = r.package;
        item.line = field(r.package) + "\t" + field(r.activity) + "\t" + field(e.title()) + "\t" + field(r.versionName) + "\t" +
                    std::to_string((long long)r.versionCode) + "\n";
        item.icon = e.iconPath;
        item.apk = e.path;
        item.splits = e.splitsDir;
        item.versionCode = r.versionCode;
        items.push_back(std::move(item));
    }
    std::thread([dir, items] {
        files::makeDirectories(dir);
        std::string text;
        unsigned described = 0;
        for (const Item& item : items) {
            text += item.line;
            if (!item.icon.empty()) files::copy(item.icon, dir + L"\\" + widen(item.package) + L".png");
            if (describe(dir, item)) described++;
        }
        const std::wstring temp = dir + L"\\apps.txt.new";
        if (files::write(temp, text) && files::move(temp, dir + L"\\apps.txt"))
            shellLog::write(format("phone apps: %u listed for launchers in the shared folder, the activities of %u", unsigned(items.size()), described));
        else
            shellLog::write("phone apps: the list could not be written");
    }).detach();
}

} // namespace megler::app::phoneAppsList
