// One app: what megler_apk_report says about its APK -- what it is, which
// engine drives it, its native libraries and what they need -- and what to do
// with it: install it as an app of the phone's own, update or uninstall it
// (ui/SeparateApps.h), its own settings, and Remove. Megler runs an app itself
// only in an auto run (auto/AutoRun.h).
#include "pch.h"

#include "library/Library.h"
#include "player/Session.h"
#include "shell/AppPackage.h"
#include "shell/Files.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Components.h"
#include "ui/SeparateApps.h"

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {

std::string joined(const std::vector<std::string>& items, const char* separator) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : separator) + item;
    return out;
}

/// A library's row: its name, then its size, how its code is encoded, what
/// it imports that the layer lacks, and what it needs.
UIElement libraryRow(const ApkLibrary& library) {
    const auto& p = palette();
    auto column = vstack(3);
    column.Padding(ThicknessHelper::FromLengths(16, 10, 16, 10));
    add(column, monospace(library.name, font::subheadline, p.label));
    std::string facts = formattedByteCount(library.size);
    if (library.thumbFunctions || library.armFunctions)
        facts += format(" · %d Thumb, %d ARM functions", library.thumbFunctions, library.armFunctions);
    add(column, text(facts, font::caption, weightNormal(), p.secondaryLabel));
    if (library.missingImports > 0)
        add(column, text(format("%d imports the layer doesn't have yet", library.missingImports), font::caption, weightMedium(),
                         p.orange));
    if (!library.needs.empty())
        add(column, text("needs " + joined(library.needs, ", "), font::caption, weightNormal(), p.tertiaryLabel));
    return column;
}

class AppDetailScreen final : public Page {
public:
    AppDetailScreen(AppShell& shell, std::string apkId) : Page(shell), id_(std::move(apkId)) {}

    std::string title() override {
        const auto entry = Library::instance().find(id_);
        return entry ? entry->title() : "App";
    }

    UIElement view() override {
        if (view_) return view_;
        host_ = ContentControl();
        host_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        view_ = scrollingPage(host_);
        return view_;
    }

    void appeared() override { separateApps::refresh(shell_); }

    void refresh() override {
        // Rebuilt when the library changes (the report arriving) or what is known of the app on the phone does.
        const unsigned generation = Library::instance().generation(), phone = separateApps::generation();
        if (generation == generation_ && phone == phoneGeneration_) return;
        generation_ = generation;
        phoneGeneration_ = phone;
        rebuild();
    }

private:
    void rebuild() {
        const auto& p = palette();
        const auto found = Library::instance().find(id_);
        if (!found) {
            host_.Content(contentUnavailable(glyphs::box, "Removed", "This app is no longer in the library."));
            return;
        }
        const ApkEntry& entry = *found;
        const ApkReport& r = entry.report;
        auto column = vstack(0);
        column.Padding(ThicknessHelper::FromLengths(16, 16, 16, 28));

        // Icon, name, package, status.
        Grid header;
        ColumnDefinition c0, c1;
        c0.Width(GridLengthHelper::Auto());
        c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
        header.ColumnDefinitions().Append(c0);
        header.ColumnDefinitions().Append(c1);
        header.Children().Append(apkIcon(entry, 76));
        auto names = vstack(3);
        names.VerticalAlignment(VerticalAlignment::Center);
        names.Margin(ThicknessHelper::FromLengths(16, 0, 0, 0));
        add(names, text(entry.title(), font::title3, weightBold(), p.label));
        if (!r.package.empty()) add(names, text(r.package, font::footnote, weightNormal(), p.secondaryLabel));
        add(names, apkStatus(entry, font::subheadline));
        Grid::SetColumn(names, 1);
        header.Children().Append(names);
        header.Margin(ThicknessHelper::FromLengths(0, 0, 0, 20));
        add(column, header);

        if (auto warnings = platformWarnings()) add(column, warnings);

        if (!appPackage::isSingleApp() && entry.analysed && r.valid && r.ok) add(column, phoneSection(entry));

        if (entry.analysed && r.valid && r.ok) {
            std::vector<UIElement> facts;
            std::string version = r.versionName.empty() ? std::string("—") : r.versionName;
            if (r.versionCode) version += format(" (%lld)", (long long)r.versionCode);
            facts.push_back(labeledContent("Version", version));
            facts.push_back(labeledContent("ABI", r.abi.empty() ? std::string("none usable") : r.abi));
            if (r.abis.size() > 1 || (r.abis.size() == 1 && r.abis[0] != r.abi))
                facts.push_back(labeledContent("In the APK", joined(r.abis, ", ")));
            facts.push_back(labeledContent("Engine", r.engineName() + (r.supported ? "" : " (no driver yet)")));
            if (r.minSdk || r.targetSdk)
                facts.push_back(labeledContent("Android SDK", format("min %d, target %d", r.minSdk, r.targetSdk)));
            if (!r.activity.empty()) facts.push_back(labeledContent("Activity", r.activity, true));
            if (!r.nativeLibrary.empty()) facts.push_back(labeledContent("Native Library", r.nativeLibrary, true));
            // An app that came as a bundle: the splits it is run with, and what of the bundle was not kept.
            if (!r.splits.empty()) facts.push_back(labeledContent("Split APKs", joined(r.splits, ", "), true));
            if (!r.splitsLeftOut.empty()) facts.push_back(labeledContent("Left Out of the Bundle", joined(r.splitsLeftOut, ", "), true));
            facts.push_back(labeledContent("File", entry.id + (entry.splitsDir.empty() ? "" : format(" and %u splits", unsigned(r.splits.size()))) +
                                                       " · " + formattedByteCount(entry.size)));
            add(column, listSection("About", facts));

            if (!r.libraries.empty()) {
                std::vector<UIElement> rows;
                for (const auto& library : r.libraries) rows.push_back(libraryRow(library));
                add(column, listSection(format("Libraries (%s)", r.abi.empty() ? "none usable" : r.abi.c_str()), rows,
                                        "Imports the layer doesn't have yet are calls the app would make into nothing."));
            }
            if (!r.notes.empty()) {
                std::vector<UIElement> rows;
                for (const auto& note : r.notes) {
                    auto t = text(note, font::subheadline, weightNormal(), p.label);
                    t.Padding(ThicknessHelper::FromLengths(16, 11, 16, 11));
                    rows.push_back(t);
                }
                add(column, listSection("Notes", rows));
            }
        } else if (entry.analysed) {
            auto t = text(r.status(), font::subheadline, weightNormal(), p.label);
            t.Padding(ThicknessHelper::FromLengths(16, 11, 16, 11));
            add(column, listSection("This APK Couldn't Be Read", {t, labeledContent("File", entry.id + " · " +
                                                                                              formattedByteCount(entry.size))}));
        }

        auto remove = button(label("Remove from Library", glyphs::trash, font::body, weightNormal(), p.red), [this] { askRemove(); },
                             ButtonKind::rounded, "Remove from Library");
        remove.Background(brush(p.card));
        remove.Padding(ThicknessHelper::FromLengths(16, 13, 16, 13));
        // The copy of a running app is open in the core.
        remove.IsEnabled(session::snapshot().apkId != id_ || !session::spent());
        add(column, remove);
        auto note = text(std::string(!entry.installedPackage.empty() ? "Takes the app out of Megler's list."
                                     : entry.splitsDir.empty()          ? "Removes Megler's copy of the APK."
                                                                        : "Removes Megler's copy of the app's APKs.") +
                             " The app installed on the phone stays, but Megler can no longer update it; its files and saves stay too.",
                         font::footnote, weightNormal(), p.secondaryLabel);
        note.Margin(ThicknessHelper::FromLengths(16, 7, 16, 0));
        add(column, note);

        host_.Content(column);
    }

    /// The app as an app of the phone's own (ui/SeparateApps.h): whether it is installed and from which Megler, Install or
    /// Update, its own settings, Uninstall; and, when the portal could not install a package made, Windows's installer.
    UIElement phoneSection(const ApkEntry& entry) {
        const auto& p = palette();
        const ApkReport& r = entry.report;
        const separateApps::Status s = separateApps::status(r.package);
        std::vector<UIElement> rows;
        auto row = [&](const std::string& title, wchar_t icon, std::function<void()> onClick, bool enabled, std::optional<Color> color = std::nullopt) {
            auto b = button(label(title, icon, font::body, weightNormal(), color.value_or(p.accent)), std::move(onClick), ButtonKind::rounded, title);
            b.Padding(ThicknessHelper::FromLengths(16, 13, 16, 13));
            b.HorizontalAlignment(HorizontalAlignment::Stretch);
            b.HorizontalContentAlignment(HorizontalAlignment::Left);
            b.IsEnabled(enabled && !s.queued);
            rows.push_back(b);
        };
        std::string state;
        if (s.installed == separateApps::Installed::yes)
            state = s.outdated ? "Installed, made by Megler " + s.version + ". This is Megler " + appPackage::meglerVersion() + ": Update makes it again with this one."
                               : "Installed, made by this Megler (" + s.version + ").";
        else if (s.installed == separateApps::Installed::no)
            state = "Not installed.";
        else
            state = separateApps::portalProblem().empty() ? "Asking the Device Portal whether it is installed…"
                                                          : "Megler can't tell whether it is installed: " + separateApps::portalProblem();
        auto stateText = text(state, font::subheadline, weightNormal(), s.outdated ? p.orange : p.label);
        stateText.TextWrapping(TextWrapping::Wrap);
        stateText.Padding(ThicknessHelper::FromLengths(16, 11, 16, 11));
        rows.push_back(stateText);

        const std::string id = id_, package = r.package, title = entry.title();
        const bool installed = s.installed == separateApps::Installed::yes;
        // An APK that was the installed app's, which went (uninstalled elsewhere than here), is nowhere now.
        const bool apkThere = files::exists(entry.path);
        row(installed ? (s.outdated ? "Update" : "Install Again") : "Install", installed ? glyphs::refresh : glyphs::add,
            [this, id] { separateApps::install(shell_, id); }, r.ready() && apkThere);
        row("App Settings", glyphs::settings, [this, package, title] { shell_.push(makeAppSettingsScreen(shell_, package, title)); }, true);
        if (installed)
            row("Uninstall", glyphs::trash, [this, package, title] {
                shell_.confirm("Uninstall " + title + "?",
                               "The app goes from the phone, with its private files. What it has in Android's storage stays in the shared folder, "
                               "and Megler keeps the APK: Install puts it back.",
                               "Uninstall", true, [this, package] { separateApps::uninstall(shell_, package); });
            }, true, p.red);
        // The package made, when the portal did not install it: Windows's own installer, as a file manager opens a package.
        if (!s.made.appx.empty() && !installed) {
            const std::wstring appx = s.made.appx, certificate = s.made.certificate;
            row("Open the Package (Windows Installer)", glyphs::play, [this, appx] { openFile(appx); }, true);
            row("Open the Certificate", glyphs::info, [this, certificate] { openFile(certificate); }, true);
        }
        const std::string step = opened_.empty() ? s.step : opened_;
        if (!step.empty()) {
            auto t = text(step, font::subheadline, weightNormal(), p.secondaryLabel);
            t.TextWrapping(TextWrapping::Wrap);
            t.Padding(ThicknessHelper::FromLengths(16, 11, 16, 11));
            rows.push_back(t);
        }
        std::string footer = r.ready() ? std::string("An app of the phone's own: Megler and this ") + (entry.splitsDir.empty() ? "APK" : "app's APKs") +
                                             " in a package with the app's name and icon, made here and installed through the phone's Device "
                                             "Portal. It starts straight into the app, in a process of its own with its own memory; its /sdcard is "
                                             "shared with the other apps. The first time, the app's data in Megler goes along with it."
                                       : "This app can't run here: " + r.status();
        footer += " App Settings are this app's own: render size, language and region, location.";
        if (!entry.installedPackage.empty())
            footer += apkThere ? " Megler keeps no copy of its APK: it reads the installed app's, and updates from it."
                               : " Its APK was the installed app's, which is gone: to install it again, install the APK again.";
        return listSection("On This Phone", rows, footer);
    }

    void openFile(const std::wstring& path) {
        const std::weak_ptr<Page> weak = shared_from_this();
        appPackage::openWithSystem(path, [this, weak](bool ok) {
            if (!weak.lock()) return;
            opened_ = ok ? "Handed to Windows." : "Windows didn't open it.";
            rebuild();
        });
    }

    void askRemove() {
        const auto entry = Library::instance().find(id_);
        if (!entry) return;
        const std::string id = id_;
        AppShell& shell = shell_;
        shell.confirm("Remove " + entry->title() + "?", "Megler's copy of the APK is deleted. To install or update the app again you would add it again.",
                      "Remove", true, [&shell, id] {
                          Library::instance().remove(id);
                          shell.pop();
                      });
    }

    std::string id_;
    UIElement view_{nullptr};
    ContentControl host_{nullptr};
    unsigned generation_ = 0, phoneGeneration_ = 0;
    std::string opened_; // what Windows did with a file handed to it
};

} // namespace

std::shared_ptr<Page> makeAppDetailScreen(AppShell& shell, std::string apkId) {
    return std::make_shared<AppDetailScreen>(shell, std::move(apkId));
}

} // namespace megler::app::ui
