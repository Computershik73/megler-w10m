// Settings: the size of the surface apps draw to, the language and region and
// where apps are told the phone is, the phone's folders that are in Android's
// storage, and what this build and this phone are.
//
// Whose (shell/Settings.h): Megler's, on Megler's Settings tab, which every app
// goes by unless it has its own; or one app's own, on its page in Megler (App
// Settings) and, in the app's own package, from the player's menu -- there
// with the package's folders and what it is too. An app's own has "As in
// Megler" at the top of each choice.
#include "pch.h"

#include "player/PhoneState.h"
#include "player/Session.h"
#include "player/StorageHost.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/SharedFolders.h"
#include "ui/Theme.h"

#include "megler.h"

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {

struct RenderChoice {
    int cap;
    const char* title;
    const char* detail;
};

// The longer side of the surface, in pixels (shell/Settings.h).
const RenderChoice renderChoices[] = {
    {960, "960", "Lightest: for phones whose GPU struggles"},
    {1280, "1280", "The default: 720p on a 16:9 screen"},
    {1600, "1600", "Sharper, about half as many pixels again"},
    {0, "Native", "The screen's own pixels"},
};

int capOf(const std::string& value) {
    char* end = nullptr;
    const long parsed = strtol(value.c_str(), &end, 10);
    return !value.empty() && end && *end == 0 ? int(parsed) : settings::renderCapDefault;
}

std::string capTitle(int cap) {
    for (const RenderChoice& choice : renderChoices)
        if (choice.cap == cap) return choice.title;
    return std::to_string(cap);
}

class SettingsBody final : public SettingsView, public std::enable_shared_from_this<SettingsBody> {
public:
    SettingsBody(AppShell& shell, SettingsScope scope) : shell_(shell), scope_(std::move(scope)) {}

    UIElement view() override {
        if (!host_) {
            host_ = ContentControl();
            host_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
            rebuild();
        }
        return host_;
    }

    void refresh() override {
        if (!host_ || !showsPhone()) return;
        // The phone's folders are looked for while Megler starts: shown once they are known.
        if (!linksShown_ && StorageHost::shared().searched()) rebuild();
        // Memory moves; once a second is plenty.
        if (++ticks_ % 4 != 0 || !memory_) return;
        if (auto value = memory_.Children().GetAt(1).try_as<TextBlock>()) value.Text(hs(platform::memorySummary()));
    }

private:
    bool forApp() const { return !scope_.package.empty(); }
    /// The folders, what this is, its files: Megler's, or an app's own package's.
    bool showsPhone() const { return !forApp() || scope_.own; }

    /// The value of this scope's own file: Megler's, or the app's own (nothing: as in Megler).
    std::optional<std::string> own(const char* key) const { return forApp() ? settings::app(scope_.package, key) : settings::megler(key); }
    std::string meglerOr(const char* key, const std::string& fallback) const { return settings::megler(key).value_or(fallback); }
    void put(const char* key, const std::optional<std::string>& value) {
        if (forApp())
            settings::setApp(scope_.package, key, value);
        else
            settings::setMegler(key, value.value_or(std::string()));
        shellLog::write("settings: " + (forApp() ? scope_.package + "'s " : std::string("Megler's ")) + key +
                        (value ? " = " + *value : std::string(": as in Megler")));
    }
    std::string applies() const {
        return forApp() ? " It applies at the app's next start."
                        : " It applies to every app that has no setting of its own, at its next start.";
    }

    /// A row of a list of choices: its title, a line under it, and a checkmark when it is the one chosen.
    Grid choiceRow(const std::string& title, const std::string& detail, bool chosen) {
        const auto& p = palette();
        Grid row;
        ColumnDefinition c0, c1;
        c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        c1.Width(GridLengthHelper::Auto());
        row.ColumnDefinitions().Append(c0);
        row.ColumnDefinitions().Append(c1);
        auto titles = vstack(2);
        add(titles, text(title, font::body, weightNormal(), p.label));
        auto second = text(detail, font::caption, weightNormal(), p.secondaryLabel);
        second.TextWrapping(TextWrapping::Wrap);
        add(titles, second);
        row.Children().Append(titles);
        if (chosen) {
            auto mark = glyph(glyphs::checkmark, font::body, p.accentText);
            Grid::SetColumn(mark, 1);
            row.Children().Append(mark);
        }
        row.Padding(ThicknessHelper::FromLengths(16, 10, 16, 10));
        return row;
    }

    /// A choice: chosen when it is, and `choose` then a rebuild when tapped.
    UIElement choice(const std::string& title, const std::string& detail, bool chosen, std::function<void()> choose) {
        return button(choiceRow(title, detail, chosen), [this, choose] {
            choose();
            rebuild();
        }, ButtonKind::plain, title);
    }

    UIElement renderSection() {
        const auto mine = own(settings::renderCap);
        std::vector<UIElement> rows;
        if (forApp()) {
            rows.push_back(choice("As in Megler", capTitle(capOf(meglerOr(settings::renderCap, ""))), !mine,
                                  [this] { put(settings::renderCap, std::nullopt); }));
        }
        const bool set = !forApp() || mine.has_value();
        const int current = capOf(mine.value_or(""));
        for (const RenderChoice& c : renderChoices) {
            const int cap = c.cap;
            rows.push_back(choice(c.title, c.detail, set && c.cap == current, [this, cap] { put(settings::renderCap, std::to_string(cap)); }));
        }
        return listSection("Render Size", rows,
                           "The longer side of the picture an app draws, in pixels; the phone scales it to the screen. Smaller is faster." +
                               applies());
    }

    /// The language and region apps are told (player/PhoneState.h): the phone's, English (United States), or another tag.
    UIElement localeSection() {
        const auto mine = own(settings::localeTag);
        auto describe = [](const std::string& tag) { return tag.empty() ? "The phone's: " + phoneState::locale() : tag; };
        std::vector<UIElement> rows;
        if (forApp())
            rows.push_back(choice("As in Megler", describe(meglerOr(settings::localeTag, "")), !mine, [this] { put(settings::localeTag, std::nullopt); }));
        const bool set = !forApp() || mine.has_value();
        const std::string chosen = mine.value_or("");
        const bool other = !chosen.empty() && chosen != "en-US";
        rows.push_back(choice("The Phone's", phoneState::locale() + ", as in Windows' settings", set && chosen.empty(),
                              [this] { put(settings::localeTag, std::string()); }));
        rows.push_back(choice("English (United States)", "en-US", set && chosen == "en-US", [this] { put(settings::localeTag, std::string("en-US")); }));
        rows.push_back(button(choiceRow("Another", (set && other ? chosen : std::string("A tag such as de-DE")) + " (tap to change)", set && other),
                              [this] { setLocale(); }, ButtonKind::plain, "Another"));
        return listSection("Language and Region", rows,
                           "What apps are told the phone's language and region are: the texts they show, and the country they take it to be in "
                           "(a phone number's form, the SIM's country)." +
                               applies());
    }

    /// Another language and region: a BCP 47 tag typed in, checked as the core checks it, kept, and chosen.
    winrt::fire_and_forget setLocale() {
        if (adding_) co_return;
        adding_ = true;
        const auto keep = shared_from_this();
        std::string error;
        try {
            ContentDialog dialog;
            dialog.Title(box_value(hs("Language and Region")));
            auto content = vstack(10);
            auto about = text("A language and a region as BCP 47 writes them: de-DE, uk-UA, pt-BR, zh-Hant-TW.", font::subheadline, weightNormal(),
                              palette().secondaryLabel);
            about.TextWrapping(TextWrapping::Wrap);
            add(content, about);
            TextBox box;
            const std::string chosen = own(settings::localeTag).value_or(meglerOr(settings::localeTag, ""));
            box.Text(hs(chosen.empty() ? phoneState::locale() : chosen));
            box.MaxLength(35);
            box.IsSpellCheckEnabled(false);
            add(content, box);
            dialog.Content(content);
            dialog.PrimaryButtonText(L"Use This");
            dialog.SecondaryButtonText(L"Cancel");
            dialog.RequestedTheme(ElementTheme::Dark);
            dialog.Background(brush(palette().card));
            dialog.Foreground(brush(palette().label));
            if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
                std::string tag = narrow(std::wstring(box.Text()));
                while (!tag.empty() && tag.back() == ' ') tag.pop_back();
                while (!tag.empty() && tag.front() == ' ') tag.erase(tag.begin());
                for (char& ch : tag)
                    if (ch == '_') ch = '-';
                // As the core takes one (mg_locale_set): letters, digits and hyphens, beginning with a language of two or three letters.
                bool ok = !tag.empty() && tag.size() < 36;
                size_t first = tag.find('-');
                if (first == std::string::npos) first = tag.size();
                for (char ch : tag) ok = ok && (isalnum((unsigned char)ch) || ch == '-');
                ok = ok && first >= 2 && first <= 3;
                for (size_t i = 0; ok && i < first; i++) ok = isalpha((unsigned char)tag[i]) != 0;
                if (ok)
                    put(settings::localeTag, tag);
                else
                    error = "\"" + tag + "\" is not a language and region as BCP 47 writes them, such as de-DE.";
            }
        } catch (const hresult_error& e) {
            error = format("0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str());
        }
        adding_ = false;
        if (!error.empty()) shell_.alert("Not Changed", error);
        rebuild();
    }

    /// Where apps are told the phone is (player/LocationHost.h): the phone's location, or a place set here.
    UIElement locationSection() {
        const auto source = own(settings::locationSource);
        const bool set = !forApp() || source.has_value();
        const bool atPlace = set && source.value_or("phone") == "set";
        auto shown = [](const std::string& place) {
            double latitude = 0, longitude = 0;
            return settings::location::parsePlace(place, latitude, longitude) ? format("%.6f, %.6f", latitude, longitude) : place;
        };
        const std::string meglerPlace = meglerOr(settings::locationPlace, settings::locationPlaceDefault);
        const std::string place = own(settings::locationPlace).value_or(meglerPlace);
        std::vector<UIElement> rows;
        if (forApp()) {
            const bool meglerAtPlace = meglerOr(settings::locationSource, "phone") == "set";
            rows.push_back(choice("As in Megler", meglerAtPlace ? "A place: " + shown(meglerPlace) : std::string("The phone's location"), !source, [this] {
                put(settings::locationSource, std::nullopt);
                put(settings::locationPlace, std::nullopt);
            }));
        }
        rows.push_back(choice("The Phone's Location", "Where Windows finds it: satellites, Wi-Fi, the cell network", set && !atPlace,
                              [this] { put(settings::locationSource, std::string("phone")); }));
        rows.push_back(button(choiceRow("A Place Set Here", shown(place) + " (tap to change)", atPlace), [this] { setPlace(); }, ButtonKind::plain,
                              "A Place Set Here"));
        return listSection("Location",
                           rows,
                           "Where an app is told the phone is when it asks. Windows asks once whether the app may have the phone's location. "
                           "A place set here is what the app is told instead, the location on, for trying it somewhere else." +
                               applies());
    }

    /// The place set here: latitude and longitude typed in, checked, kept, and chosen.
    winrt::fire_and_forget setPlace() {
        // (A second dialog while one is up throws.)
        if (adding_) co_return;
        adding_ = true;
        const auto keep = shared_from_this();
        std::string error;
        try {
            ContentDialog dialog;
            dialog.Title(box_value(hs("A Place for Apps")));
            auto content = vstack(10);
            auto about = text("Latitude and longitude in degrees, north and east positive, as 55.751244, 37.618423.", font::subheadline,
                              weightNormal(), palette().secondaryLabel);
            about.TextWrapping(TextWrapping::Wrap);
            add(content, about);
            TextBox box;
            box.Text(hs(own(settings::locationPlace).value_or(meglerOr(settings::locationPlace, settings::locationPlaceDefault))));
            box.MaxLength(60);
            box.IsSpellCheckEnabled(false);
            add(content, box);
            dialog.Content(content);
            dialog.PrimaryButtonText(L"Use This Place");
            dialog.SecondaryButtonText(L"Cancel");
            dialog.RequestedTheme(ElementTheme::Dark);
            dialog.Background(brush(palette().card));
            dialog.Foreground(brush(palette().label));
            if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
                const std::string typed = narrow(std::wstring(box.Text()));
                double latitude = 0, longitude = 0;
                if (settings::location::parsePlace(typed, latitude, longitude)) {
                    put(settings::locationPlace, format("%.6f,%.6f", latitude, longitude));
                    put(settings::locationSource, std::string("set"));
                } else {
                    error = "\"" + typed + "\" is not a latitude (-90 to 90) and a longitude (-180 to 180) with a comma between them.";
                }
            }
        } catch (const hresult_error& e) {
            error = format("0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str());
        }
        adding_ = false;
        if (!error.empty()) shell_.alert("The Place Was Not Set", error);
        rebuild();
    }

    /// The section "Developer": who makes Megler, the profile, the channel and the donation link (as in gecko-w10m's Settings).
    StackPanel developerSection() {
        const auto& p = palette();
        std::vector<UIElement> rows;
        auto lines = [&](std::string_view title, std::string_view description) {
            auto both = vstack(2);
            both.Padding(ThicknessHelper::FromLengths(16, 11, 16, 11));
            add(both, text(title, font::body, weightNormal(), p.label));
            auto under = text(description, font::caption, weightNormal(), p.secondaryLabel);
            under.TextWrapping(TextWrapping::Wrap);
            add(both, under);
            rows.push_back(both);
        };
        auto link = [&](std::string_view title, wchar_t icon, const wchar_t* uri) {
            auto l = label(title, icon, font::body, weightNormal(), p.accentText);
            l.Margin(ThicknessHelper::FromLengths(16, 11, 16, 11));
            const std::wstring to = uri;
            rows.push_back(button(l, [to] { winrt::Windows::System::Launcher::LaunchUriAsync(winrt::Windows::Foundation::Uri(to)); },
                                  ButtonKind::plain, title));
        };
        lines("Developed by Computershik", "Development, building and debugging on the device.");
        link("Profile on 4pda", glyphs::contact, L"https://4pda.to/forum/index.php?showuser=4458524");
        link("Telegram channel @cmplog", glyphs::send, L"https://t.me/cmplog");
        link("Donate (within the CIS for now)", glyphs::heart, L"https://pay.cloudtips.ru/p/83821e32");
        lines("Outside the CIS and would like to help?", "Send a message to the Telegram channel.");
        return listSection("Developer", rows);
    }

    /// The section "Android Storage": every link with where it is, the
    /// added ones with a button to take them out again, and "Add a Folder";
    /// in an app's own package, Megler's folders it has not got.
    StackPanel storageSection() {
        const auto& p = palette();
        StorageHost& storage = StorageHost::shared();
        std::vector<UIElement> rows;
        linksShown_ = storage.searched();
        if (!linksShown_) {
            auto looking = text("Looking for the phone's folders...", font::body, weightNormal(), p.secondaryLabel);
            looking.Margin(ThicknessHelper::FromLengths(16, 11, 16, 11));
            rows.push_back(looking);
        }
        for (const StorageHost::Link& link : storage.links()) {
            Grid row;
            ColumnDefinition c0, c1;
            c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            c1.Width(GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);
            auto titles = vstack(2);
            add(titles, monospace("/sdcard/" + link.name, font::subheadline, p.label));
            auto where = text(link.path.empty() ? link.what : link.path, font::caption, weightNormal(), p.secondaryLabel);
            where.TextWrapping(TextWrapping::Wrap);
            add(titles, where);
            // What it is, where the line above is its path; and why an app will not see it, when it will not.
            const std::string state = link.answers ? (link.path.empty() ? link.note : link.what + ", " + link.note) : "Left out: " + link.note;
            auto note = text(state, font::caption, weightNormal(), link.answers ? p.tertiaryLabel : p.orange);
            note.TextWrapping(TextWrapping::Wrap);
            add(titles, note);
            row.Children().Append(titles);
            if (!link.token.empty()) {
                const std::string token = link.token, name = link.name;
                auto forget = button(glyph(glyphs::trash, font::body, p.red), [this, token, name] {
                    shell_.confirm("Take /sdcard/" + name + " Out?",
                                   "The folder and what is in it stay on the phone. Android apps no longer see it.", "Take Out", true, [this, token] {
                                       StorageHost::shared().remove(token);
                                       sharedFolders::publish();
                                       rebuild();
                                   });
                }, ButtonKind::plain, "Take " + name + " out");
                forget.HorizontalAlignment(HorizontalAlignment::Right);
                forget.VerticalAlignment(VerticalAlignment::Center);
                Grid::SetColumn(forget, 1);
                row.Children().Append(forget);
            }
            row.Padding(ThicknessHelper::FromLengths(16, 10, 8, 10));
            rows.push_back(row);
        }
        auto addLabel = label("Add a Folder", glyphs::add, font::body, weightNormal(), p.accentText);
        addLabel.Margin(ThicknessHelper::FromLengths(16, 11, 16, 11));
        rows.push_back(button(addLabel, [this] { addFolder(); }, ButtonKind::plain, "Add a Folder"));
        if (scope_.own) {
            auto meglerLabel = label("Folders Megler Has", glyphs::folder, font::body, weightNormal(), p.accentText);
            meglerLabel.Margin(ThicknessHelper::FromLengths(16, 11, 16, 11));
            rows.push_back(button(meglerLabel, [this] { offerMeglersFolders(); }, ButtonKind::plain, "Folders Megler Has"));
            return listSection("Android Storage", rows,
                               "Folders of the phone this app finds in its storage. The music, pictures and videos are there by themselves. "
                               "Windows lets a folder picked in the folder picker into one app only: Folders Megler Has asks for each of those "
                               "added in Megler's Settings that this app has not got, and Add a Folder adds any other. The app is given them "
                               "when it starts.");
        }
        auto checkLabel = label(checking_ ? "Checking the Folders..." : "Check the Folders", glyphs::wrench, font::body, weightNormal(),
                                checking_ ? p.secondaryLabel : p.accentText);
        checkLabel.Margin(ThicknessHelper::FromLengths(16, 11, 16, 11));
        rows.push_back(button(checkLabel, [this] { checkFolders(); }, ButtonKind::plain, "Check the Folders"));
        return listSection("Android Storage", rows,
                           "Folders of the phone that every Android app finds in its storage. The music, pictures and videos are there by themselves. "
                           "Add a Folder puts any other folder there under a name you give it: the phone's Documents or Downloads, a folder of a "
                           "memory card. An app is given them when it starts. A separate app asks for each of them once, as Windows "
                           "lets a picked folder into one app only. Check the Folders tries in each what an app does to files, in a "
                           "folder of its own that it removes again (megler-storage-check); every step is in the Log.");
    }

    /// An app's own package: Megler's folders it has not got, those said "Not Now" to as well (ui/SharedFolders.h).
    void offerMeglersFolders() {
        const std::weak_ptr<SettingsBody> weak = shared_from_this();
        AppShell& shell = shell_;
        sharedFolders::offer(true, [weak, &shell](int offered, int added) {
            if (offered < 0)
                shell.alert("No Folders in Megler", "Megler has no folders of the phone's added. Add them in Megler: Settings, Android Storage, Add a Folder; "
                                                  "then they are offered here.");
            else if (offered == 0)
                shell.alert("Every Folder Is Here", "This app has every folder Megler has.");
            else if (added > 0)
                shell.alert("Folders Added", "The app finds them from its next start.");
            if (auto self = weak.lock()) self->rebuild();
        });
    }

    /// StorageHost::check on a worker thread, its steps into the log, its
    /// last line shown. Not while an app runs or once one has: the layer's
    /// storage is that app's then.
    void checkFolders() {
        if (checking_) return;
        if (session::spent()) {
            shell_.alert("Not Now", "An app is running or has run since Megler was opened. Close Megler and open it again, and check the folders before playing.");
            return;
        }
        checking_ = true;
        rebuild();
        auto done = std::make_shared<std::string>();
        const std::weak_ptr<SettingsBody> weak = shared_from_this();
        shell_.runInBackground(
            [done] {
                shellLog::write("storage check: started from Settings");
                *done = StorageHost::shared().check([](const std::string& line) { shellLog::write("storage check: " + line); });
            },
            [weak, done] {
                auto self = weak.lock();
                if (!self) return;
                self->checking_ = false;
                self->rebuild();
                self->shell_.alert("The Folders Were Checked", (done->empty() ? std::string("The check did not finish.") : *done) + "\nEvery step is in the Log.");
            });
    }

    /// The folder picker, then a name for the folder in Android's storage,
    /// then StorageHost::add, which has Windows remember the folder.
    winrt::fire_and_forget addFolder() {
        using namespace winrt::Windows::Storage;
        using namespace winrt::Windows::Storage::Pickers;
        // (A second picker while one is up throws.)
        if (adding_) co_return;
        adding_ = true;
        const auto keep = shared_from_this();
        std::string error;
        try {
            FolderPicker picker;
            picker.ViewMode(PickerViewMode::List);
            picker.SuggestedStartLocation(PickerLocationId::DocumentsLibrary);
            // (A folder picker with no file type named throws; any type will do.)
            picker.FileTypeFilter().Append(L"*");
            const StorageFolder folder = co_await picker.PickSingleFolderAsync();
            if (folder) {
                const std::string own = narrow(std::wstring(folder.Name()));
                const std::vector<std::string> names = StorageHost::shared().namesFor(own);
                // The name to start from: Android's own for the phone's Documents and Downloads while it is free, else the folder's.
                std::string first = names.empty() ? own : names.back();
                for (const std::string& name : names) {
                    const bool documents = name == "Documents" && _stricmp(own.c_str(), "Documents") == 0;
                    const bool downloads = name == "Download" && (_stricmp(own.c_str(), "Downloads") == 0 || _stricmp(own.c_str(), "Download") == 0);
                    if (documents || downloads) first = name;
                }
                ContentDialog dialog;
                dialog.Title(box_value(hs("Name in Android's Storage")));
                auto content = vstack(10);
                auto about = text("Apps will find " + (folder.Path().empty() ? own : narrow(std::wstring(folder.Path()))) + " as /sdcard/ and this name.", font::subheadline,
                                  weightNormal(), palette().secondaryLabel);
                about.TextWrapping(TextWrapping::Wrap);
                add(content, about);
                TextBox box;
                box.Text(hs(first));
                box.MaxLength(60);
                box.IsSpellCheckEnabled(false);
                add(content, box);
                if (names.size() > 1) {
                    auto offers = hstack(8);
                    for (const std::string& name : names) {
                        auto offer = button(text(name, font::subheadline, weightNormal(), palette().accentText), [box, name] { box.Text(hs(name)); }, ButtonKind::pill, name);
                        offer.HorizontalAlignment(HorizontalAlignment::Left);
                        add(offers, offer);
                    }
                    add(content, offers);
                }
                dialog.Content(content);
                dialog.PrimaryButtonText(L"Add");
                dialog.SecondaryButtonText(L"Cancel");
                dialog.RequestedTheme(ElementTheme::Dark);
                dialog.Background(brush(palette().card));
                dialog.Foreground(brush(palette().label));
                if (co_await dialog.ShowAsync() == ContentDialogResult::Primary) {
                    std::string name = narrow(std::wstring(box.Text()));
                    while (!name.empty() && name.back() == ' ') name.pop_back();
                    while (!name.empty() && name.front() == ' ') name.erase(name.begin());
                    error = StorageHost::shared().add(folder, name);
                    if (error.empty()) sharedFolders::publish();
                }
            }
        } catch (const hresult_error& e) {
            error = format("0x%08X %s", unsigned(e.code()), narrow(e.message()).c_str());
        }
        adding_ = false;
        if (!error.empty()) {
            shellLog::write("settings: a folder was not added: " + error);
            shell_.alert("The Folder Was Not Added", error);
        }
        rebuild();
    }

    void rebuild() {
        if (!host_) return;
        const auto& p = palette();
        auto column = vstack(0);
        column.Padding(ThicknessHelper::FromLengths(16, 16, 16, 28));

        if (forApp()) {
            auto about = text("The settings of " + (scope_.label.empty() ? scope_.package : scope_.label) +
                                  " alone. As in Megler: what Megler's Settings say, now and when they change.",
                              font::subheadline, weightNormal(), p.secondaryLabel);
            about.TextWrapping(TextWrapping::Wrap);
            about.Margin(ThicknessHelper::FromLengths(4, 0, 4, 14));
            add(column, about);
        }

        add(column, renderSection());
        add(column, localeSection());
        add(column, locationSection());

        if (showsPhone()) {
            add(column, storageSection());

            memory_ = labeledContent("Memory", platform::memorySummary());
            const auto& angle = platform::angle();
            add(column, listSection("About",
                                    {labeledContent("Megler", platform::packageVersion()),
                                     labeledContent("Core", megler_version()),
                                     labeledContent("Build", platform::architecture()),
                                     labeledContent("Device", platform::deviceFamily()),
                                     labeledContent("Windows", platform::osVersion()),
                                     memory_,
                                     labeledContent("Android Code", platform::canRunAndroidCode() ? "Can run" : "Cannot run"),
                                     labeledContent("ANGLE", angle.present ? "In the package" : "Missing")},
                                    "Android code needs the codeGeneration capability, which an interop-unlocked phone grants; "
                                    "ANGLE is what turns an app's OpenGL ES into Direct3D 11."));
            add(column, developerSection());

            auto paths = vstack(4);
            paths.Padding(ThicknessHelper::FromLengths(16, 11, 16, 11));
            add(paths, monospace(narrow(platform::localStateDirectory()), font::caption, p.secondaryLabel));
            add(paths, text(scope_.own ? "apps: the app's private files · megler.log: the log"
                                       : "apks: the library · apps: each app's files and saves · megler.log, megler-probe.log, "
                                         "megler-selftest.log: the logs",
                            font::caption, weightNormal(), p.tertiaryLabel));
            add(column, listSection("Files", {paths}));
        }

        host_.Content(column);
    }

    AppShell& shell_;
    SettingsScope scope_;
    ContentControl host_{nullptr};
    Grid memory_{nullptr};
    int ticks_ = 0;
    bool linksShown_ = false; // the Android Storage section was built after the search for the links was over
    bool adding_ = false;     // the folder picker or a dialog is up
    bool checking_ = false;   // StorageHost::check is going through the folders
};

class SettingsScreen final : public Page {
public:
    SettingsScreen(AppShell& shell, SettingsScope scope)
        : Page(shell), title_(scope.package.empty() ? "Settings" : "App Settings"), body_(makeSettingsView(shell, std::move(scope))) {}

    std::string title() override { return title_; }

    UIElement view() override {
        if (!view_) view_ = scrollingPage(body_->view());
        return view_;
    }

    void refresh() override { body_->refresh(); }

private:
    std::string title_;
    std::shared_ptr<SettingsView> body_;
    UIElement view_{nullptr};
};

} // namespace

std::shared_ptr<SettingsView> makeSettingsView(AppShell& shell, SettingsScope scope) {
    return std::make_shared<SettingsBody>(shell, std::move(scope));
}

std::shared_ptr<Page> makeSettingsScreen(AppShell& shell) { return std::make_shared<SettingsScreen>(shell, SettingsScope{}); }

std::shared_ptr<Page> makeAppSettingsScreen(AppShell& shell, std::string package, std::string label) {
    return std::make_shared<SettingsScreen>(shell, SettingsScope{std::move(package), std::move(label), false});
}

} // namespace megler::app::ui
