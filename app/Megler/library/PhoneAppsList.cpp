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
/// its path, size and version are in source.txt there. False when they could not be read.
bool describe(const std::wstring& dir, const Item& item) {
    if (item.apk.empty()) return false;
    const std::wstring out = dir + L"\\" + widen(item.package);
    const std::string source = narrow(item.apk) + "\n" + std::to_string((unsigned long long)files::size(item.apk)) + "\n" +
                               std::to_string((long long)item.versionCode) + "\n";
    std::string had;
    if (files::read(out + L"\\source.txt", had) && had == source && files::exists(out + L"\\activities.json")) return true;
    files::removeTree(out);
    const std::string splits = item.splits.empty() || !files::exists(item.splits) ? std::string() : narrow(item.splits);
    if (!megler_apk_describe_activities(narrow(item.apk).c_str(), splits.empty() ? nullptr : splits.c_str(), narrow(out).c_str())) {
        shellLog::write("phone apps: the activities of " + item.package + " could not be read");
        return false;
    }
    files::write(out + L"\\source.txt", source);
    return true;
}

} // namespace

std::wstring folder() {
    const std::wstring shared = appPackage::sharedFolder();
    return shared.empty() ? std::wstring() : shared + L"\\phone-apps";
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
