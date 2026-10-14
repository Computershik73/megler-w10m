// Settings: the size of the surface apps draw to, the phone's folders that
// are in Android's storage, and what this build and this phone are.
#include "pch.h"

#include "player/Session.h"
#include "player/StorageHost.h"
#include "shell/Platform.h"
#include "shell/Settings.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
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

class SettingsScreen final : public Page {
public:
    using Page::Page;

    std::string title() override { return "Settings"; }

    UIElement view() override {
        if (view_) return view_;
        host_ = ContentControl();
        host_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        view_ = scrollingPage(host_);
        rebuild();
        return view_;
    }

    void refresh() override {
        // The phone's folders are looked for while Megler starts: shown once they are known.
        if (view_ && !linksShown_ && StorageHost::shared().searched()) rebuild();
        // Memory moves; once a second is plenty.
        if (++ticks_ % 4 != 0 || !memory_) return;
        if (auto value = memory_.Children().GetAt(1).try_as<TextBlock>()) value.Text(hs(platform::memorySummary()));
    }

private:
    /// The section "Android Storage": every link with where it is, the
    /// added ones with a button to take them out again, and "Add a Folder".
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
        auto checkLabel = label(checking_ ? "Checking the Folders..." : "Check the Folders", glyphs::wrench, font::body, weightNormal(),
                                checking_ ? p.secondaryLabel : p.accentText);
        checkLabel.Margin(ThicknessHelper::FromLengths(16, 11, 16, 11));
        rows.push_back(button(checkLabel, [this] { checkFolders(); }, ButtonKind::plain, "Check the Folders"));
        return listSection("Android Storage", rows,
                           "Folders of the phone that every Android app finds in its storage. The music, pictures and videos are there by themselves. "
                           "Add a Folder puts any other folder there under a name you give it: the phone's Documents or Downloads, a folder of a "
                           "memory card. An app is given them when it starts. Check the Folders tries in each what an app does to files, in a "
                           "folder of its own that it removes again (megler-storage-check); every step is in the Log.");
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
        const std::weak_ptr<Page> weak = shared_from_this();
        shell_.runInBackground(
            [done] {
                shellLog::write("storage check: started from Settings");
                *done = StorageHost::shared().check([](const std::string& line) { shellLog::write("storage check: " + line); });
            },
            [this, weak, done] {
                if (!weak.lock()) return;
                checking_ = false;
                rebuild();
                shell_.alert("The Folders Were Checked", (done->empty() ? std::string("The check did not finish.") : *done) + "\nEvery step is in the Log.");
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
        const std::shared_ptr<Page> keep = shared_from_this();
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
        const auto& p = palette();
        auto column = vstack(0);
        column.Padding(ThicknessHelper::FromLengths(16, 16, 16, 28));

        const int current = settings::getInt(settings::renderCap, settings::renderCapDefault);
        std::vector<UIElement> choices;
        for (const RenderChoice& choice : renderChoices) {
            Grid row;
            ColumnDefinition c0, c1;
            c0.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            c1.Width(GridLengthHelper::Auto());
            row.ColumnDefinitions().Append(c0);
            row.ColumnDefinitions().Append(c1);
            auto titles = vstack(2);
            add(titles, text(choice.title, font::body, weightNormal(), p.label));
            add(titles, text(choice.detail, font::caption, weightNormal(), p.secondaryLabel));
            row.Children().Append(titles);
            if (choice.cap == current) {
                auto mark = glyph(glyphs::checkmark, font::body, p.accentText);
                Grid::SetColumn(mark, 1);
                row.Children().Append(mark);
            }
            row.Padding(ThicknessHelper::FromLengths(16, 10, 16, 10));
            const int cap = choice.cap;
            choices.push_back(button(row, [this, cap] {
                settings::set(settings::renderCap, std::to_string(cap));
                shellLog::write(format("settings: render size %d", cap));
                rebuild();
            }, ButtonKind::plain, choice.title));
        }
        add(column, listSection("Render Size", choices,
                                "The longer side of the picture an app draws, in pixels; the phone scales it to the screen. "
                                "Smaller is faster. It applies to the next app started."));

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

        auto paths = vstack(4);
        paths.Padding(ThicknessHelper::FromLengths(16, 11, 16, 11));
        add(paths, monospace(narrow(platform::localStateDirectory()), font::caption, p.secondaryLabel));
        add(paths, text("apks: the library · apps: each app's files and saves · megler.log, megler-probe.log, "
                        "megler-selftest.log: the logs",
                        font::caption, weightNormal(), p.tertiaryLabel));
        add(column, listSection("Files", {paths}));

        host_.Content(column);
    }

    UIElement view_{nullptr};
    ContentControl host_{nullptr};
    Grid memory_{nullptr};
    int ticks_ = 0;
    bool linksShown_ = false; // the Android Storage section was built after the search for the links was over
    bool adding_ = false;     // the folder picker or the name's dialog is up
    bool checking_ = false;   // StorageHost::check is going through the folders
};

} // namespace

std::shared_ptr<Page> makeSettingsScreen(AppShell& shell) { return std::make_shared<SettingsScreen>(shell); }

} // namespace megler::app::ui
