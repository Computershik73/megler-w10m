#include "pch.h"

#include "ui/SharedFolders.h"

#include "player/StorageHost.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/Theme.h"

#include <algorithm>

namespace megler::app::ui::sharedFolders {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {

struct Folder {
    std::string name;  // under /sdcard
    std::string path;  // the phone's folder; may be empty (a folder of a provider has none)
};

/// The shared folder's folders.txt: a line for each, "name<TAB>path".
std::wstring listFile() {
    const std::wstring shared = appPackage::sharedFolder();
    return shared.empty() ? shared : shared + L"\\folders.txt";
}

std::vector<Folder> listed() {
    std::vector<Folder> out;
    std::string text;
    if (listFile().empty() || !files::read(listFile(), text)) return out;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t tab = line.find('\t');
        if (tab != std::string::npos && tab > 0) out.push_back({line.substr(0, tab), line.substr(tab + 1)});
    }
    return out;
}

/// The names the user said "Not Now" to, kept as "a|b|c" (a folder's name has no '|').
std::vector<std::string> notNow() {
    std::vector<std::string> out;
    const std::string all = settings::get(settings::foldersNotNow, "");
    size_t at = 0;
    while (at < all.size()) {
        size_t end = all.find('|', at);
        if (end == std::string::npos) end = all.size();
        if (end > at) out.push_back(all.substr(at, end - at));
        at = end + 1;
    }
    return out;
}

bool sameText(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

ContentDialog dialog(const std::string& title, const std::string& message) {
    ContentDialog d;
    d.Title(box_value(hs(title)));
    auto t = text(message, font::body, weightNormal(), palette().label);
    t.TextWrapping(TextWrapping::Wrap);
    d.Content(t);
    d.RequestedTheme(ElementTheme::Dark);
    d.Background(brush(palette().card));
    d.Foreground(brush(palette().label));
    return d;
}

/// A dialog and the folder picker for each folder, one after another.
winrt::fire_and_forget offerEach(std::vector<Folder> folders, std::function<void(int, int)> done) {
    using namespace winrt::Windows::Storage;
    using namespace winrt::Windows::Storage::Pickers;
    std::vector<std::string> declined = notNow();
    const std::string app = appPackage::single().label.empty() ? appPackage::single().package : appPackage::single().label;
    int added = 0;
    for (const Folder& f : folders) {
        bool chose = false;
        std::string error;
        try {
            const std::string where = f.path.empty() ? "a folder" : f.path;
            auto ask = dialog("/sdcard/" + f.name,
                              "Megler gives Android apps " + where + " as /sdcard/" + f.name + ". Windows lets " + app +
                                  " into it only once you choose it yourself: the folder picker opens next, choose " + where + " there.");
            ask.PrimaryButtonText(L"Choose the Folder");
            ask.SecondaryButtonText(L"Not Now");
            chose = co_await ask.ShowAsync() == ContentDialogResult::Primary;
            if (chose) {
                FolderPicker picker;
                picker.ViewMode(PickerViewMode::List);
                const bool downloads = f.path.find("Download") != std::string::npos;
                picker.SuggestedStartLocation(downloads ? PickerLocationId::Downloads : PickerLocationId::DocumentsLibrary);
                // (A folder picker with no file type named throws; any type will do.)
                picker.FileTypeFilter().Append(L"*");
                const StorageFolder folder = co_await picker.PickSingleFolderAsync();
                if (!folder) {
                    chose = false;
                } else {
                    const std::string picked = narrow(std::wstring(folder.Path()));
                    if (!f.path.empty() && !sameText(picked, f.path))
                        shellLog::write("shared folders: " + picked + " was chosen for /sdcard/" + f.name + ", which is " + f.path + " in Megler");
                    error = StorageHost::shared().add(folder, f.name);
                    if (error.empty()) {
                        added++;
                        shellLog::write("shared folders: /sdcard/" + f.name + " added, " + picked);
                    }
                }
            }
        } catch (const hresult_error& e) {
            error = format("0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str());
        }
        if (!error.empty()) {
            shellLog::write("shared folders: /sdcard/" + f.name + " was not added: " + error);
            try {
                auto said = dialog("The Folder Was Not Added", error);
                said.PrimaryButtonText(L"OK");
                co_await said.ShowAsync();
            } catch (const hresult_error&) {
            }
        }
        // "Not Now" (or the picker closed with nothing) is remembered, a folder added forgotten from it.
        declined.erase(std::remove_if(declined.begin(), declined.end(), [&f](const std::string& n) { return sameText(n, f.name); }),
                       declined.end());
        if (!chose) declined.push_back(f.name);
    }
    std::string kept;
    for (const auto& name : declined) kept += (kept.empty() ? "" : "|") + name;
    settings::set(settings::foldersNotNow, kept);
    if (done) done(int(folders.size()), added);
}

}  // namespace

void publish() {
    if (appPackage::isSingleApp() || listFile().empty() || !StorageHost::shared().searched()) return;
    std::string text;
    for (const auto& link : StorageHost::shared().links())
        if (!link.token.empty()) text += link.name + "\t" + link.path + "\r\n";
    std::string before;
    if (files::read(listFile(), before) && before == text) return;
    if (files::write(listFile(), text))
        shellLog::write("shared folders: Megler's added folders listed for the separate apps in " + narrow(listFile()));
}

void offer(bool all, std::function<void(int, int)> done) {
    const std::vector<Folder> megler = listed();
    if (megler.empty()) {
        if (done) done(-1, 0);
        return;
    }
    const auto links = StorageHost::shared().links();
    const auto declined = notNow();
    std::vector<Folder> missing;
    for (const Folder& f : megler) {
        bool have = false, refused = false;
        for (const auto& link : links)
            have = have || sameText(link.name, f.name) || (!f.path.empty() && sameText(link.path, f.path));
        for (const auto& name : declined) refused = refused || sameText(name, f.name);
        if (!have && (all || !refused)) missing.push_back(f);
    }
    shellLog::write(format("shared folders: Megler lists %u, %u to offer", unsigned(megler.size()), unsigned(missing.size())));
    if (missing.empty()) {
        if (done) done(0, 0);
        return;
    }
    offerEach(std::move(missing), std::move(done));
}

}  // namespace megler::app::ui::sharedFolders
