#include "pch.h"

#include "library/PhoneAppsList.h"

#include "library/Library.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

namespace megler::app::phoneAppsList {

namespace {

/// A field of a line: no tab or line end in it.
std::string field(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
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
    struct Item {
        std::string line, package;
        std::wstring icon;
    };
    std::vector<Item> items;
    for (const ApkEntry& e : Library::instance().entries()) {
        const ApkReport& r = e.report;
        if (!e.analysed || !r.ready() || r.package.empty()) continue;
        Item item;
        item.package = r.package;
        item.line = field(r.package) + "\t" + field(r.activity) + "\t" + field(e.title()) + "\t" + field(r.versionName) + "\t" +
                    std::to_string((long long)r.versionCode) + "\n";
        item.icon = e.iconPath;
        items.push_back(std::move(item));
    }
    std::thread([dir, items] {
        files::makeDirectories(dir);
        std::string text;
        for (const Item& item : items) {
            text += item.line;
            if (!item.icon.empty()) files::copy(item.icon, dir + L"\\" + widen(item.package) + L".png");
        }
        const std::wstring temp = dir + L"\\apps.txt.new";
        if (files::write(temp, text) && files::move(temp, dir + L"\\apps.txt"))
            shellLog::write(format("phone apps: %u listed for launchers in the shared folder", unsigned(items.size())));
        else
            shellLog::write("phone apps: the list could not be written");
    }).detach();
}

} // namespace megler::app::phoneAppsList
