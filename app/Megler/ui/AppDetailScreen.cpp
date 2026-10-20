// One app: what megler_apk_report says about its APK -- what it is, which
// engine drives it, its native libraries and what they need -- and what to do
// with it: Play, make it an app of the phone's own (shell/AppPackage.h), and
// Remove.
#include "pch.h"

#include "library/Library.h"
#include "player/Session.h"
#include "shell/AppPackage.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Components.h"

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

/// Making the app's own package: what is going on, and what was made. Shared with the worker; `page` is touched on the UI
/// thread only, and is null once the page is gone.
struct PackState {
    bool busy = false;
    std::string status;
    appPackage::Made made;
    class AppDetailScreen* page = nullptr;
};

class AppDetailScreen final : public Page {
public:
    AppDetailScreen(AppShell& shell, std::string apkId) : Page(shell), id_(std::move(apkId)) { pack_->page = this; }
    ~AppDetailScreen() override { pack_->page = nullptr; }

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

    void refresh() override {
        // Rebuilt when the library changes (the report arriving) or Play
        // comes or goes (it is gone once an app, or the self-test's, has run).
        const unsigned generation = Library::instance().generation();
        std::string blocked = session::unavailable();
        if (generation == generation_ && blocked == blocked_) return;
        generation_ = generation;
        blocked_ = std::move(blocked);
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

        // Play, or why not.
        if (!blocked_.empty()) {
            add(column, warningCard(blocked_));
            auto close = primaryButton("Close Megler", glyphs::close, [] { Application::Current().Exit(); }, p.fill);
            close.Margin(ThicknessHelper::FromLengths(0, 0, 0, 26));
            add(column, close);
        } else {
            const std::string id = id_;
            auto play = primaryButton("Play", glyphs::play, [this, id] { shell_.push(makePlayerScreen(shell_, id)); });
            play.IsEnabled(entry.analysed && r.ready());
            play.Margin(ThicknessHelper::FromLengths(0, 0, 0, 26));
            add(column, play);
        }

        if (!appPackage::isSingleApp() && entry.analysed && r.valid && r.ok) add(column, packSection(entry));

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
        auto note = text(entry.splitsDir.empty() ? "Removes Megler's copy of the APK. The app's own files and saves (LocalState\\apps) stay."
                                                 : "Removes Megler's copy of the app's APKs. The app's own files and saves, and the expansion files "
                                                   "its bundle brought (LocalState\\apps), stay.",
                         font::footnote, weightNormal(), p.secondaryLabel);
        note.Margin(ThicknessHelper::FromLengths(16, 7, 16, 0));
        add(column, note);

        host_.Content(column);
    }

    /// The app as an app of the phone's own: a package of Megler and this APK (shell/AppPackage.h), made here and installed
    /// through the phone's Device Portal or by Windows's installer.
    UIElement packSection(const ApkEntry& entry) {
        const auto& p = palette();
        auto& s = *pack_;
        std::vector<UIElement> rows;
        auto row = [&](const std::string& title, wchar_t icon, std::function<void()> onClick, bool enabled) {
            auto b = button(label(title, icon, font::body, weightNormal(), p.accent), std::move(onClick), ButtonKind::rounded, title);
            b.Padding(ThicknessHelper::FromLengths(16, 13, 16, 13));
            b.HorizontalAlignment(HorizontalAlignment::Stretch);
            b.HorizontalContentAlignment(HorizontalAlignment::Left);
            b.IsEnabled(enabled && !s.busy);
            rows.push_back(b);
        };
        const bool canMake = entry.report.ready();
        row(s.made.appx.empty() ? "Make a Separate App" : "Make It Again", glyphs::box, [this] { makePackage(); }, canMake);
        if (!s.made.appx.empty()) {
            row("Install Through the Device Portal", glyphs::add, [this] { installPackage(); }, true);
            row("Open the Package (Windows Installer)", glyphs::play, [this] { openFile(pack_->made.appx); }, true);
            row("Open the Certificate", glyphs::info, [this] { openFile(pack_->made.certificate); }, true);
        }
        if (!s.status.empty()) {
            auto t = text(s.status, font::subheadline, weightNormal(), p.label);
            t.Padding(ThicknessHelper::FromLengths(16, 11, 16, 11));
            rows.push_back(t);
        }
        std::string footer = std::string("A package of Megler and this ") + (entry.splitsDir.empty() ? "APK" : "app's APKs") +
                             ", with the app's name and icon: it starts straight into the app, in a process of its own with its own "
                             "memory. Its /sdcard is shared with the other separate apps.";
        if (!s.made.appx.empty())
            footer += " The package and the certificate it is signed with are in Megler's LocalState\\packages (" + s.made.identity +
                      ".appx, MeglerApps.cer); the phone has to trust the certificate, which the Device Portal is given with the package.";
        return listSection("Separate App", rows, footer);
    }

    void makePackage() {
        const auto entry = Library::instance().find(id_);
        if (!entry || pack_->busy) return;
        auto state = pack_;
        AppShell& shell = shell_;
        state->busy = true;
        state->status = "Starting…";
        rebuild();
        const ApkEntry copy = *entry;
        auto made = std::make_shared<appPackage::Made>();
        shell.runInBackground(
            [state, made, copy, &shell] {
                *made = appPackage::make(copy, [state, &shell](const std::string& step) {
                    shell.runOnUiThread([state, step] {
                        state->status = step;
                        if (state->page) state->page->rebuild();
                    });
                });
            },
            [state, made] {
                state->busy = false;
                if (made->error.empty()) {
                    state->made = *made;
                    state->status = "Made: " + made->identity + ". Install it through the Device Portal, or open it with Windows's installer.";
                } else {
                    state->status = made->error;
                }
                if (state->page) state->page->rebuild();
            });
    }

    void installPackage() {
        if (pack_->busy || pack_->made.appx.empty()) return;
        auto state = pack_;
        AppShell& shell = shell_;
        state->busy = true;
        state->status = "Installing through the Device Portal…";
        rebuild();
        const appPackage::Made made = state->made;
        auto said = std::make_shared<std::string>();
        shell.runInBackground(
            [state, made, said, &shell] {
                *said = appPackage::installThroughPortal(made, [state, &shell](const std::string& step) {
                    shell.runOnUiThread([state, step] {
                        state->status = step;
                        if (state->page) state->page->rebuild();
                    });
                });
            },
            [state, said] {
                state->busy = false;
                state->status = *said;
                if (state->page) state->page->rebuild();
            });
    }

    void openFile(const std::wstring& path) {
        auto state = pack_;
        appPackage::openWithSystem(path, [state](bool ok) {
            state->status = ok ? "Handed to Windows." : "Windows didn't open it.";
            if (state->page) state->page->rebuild();
        });
    }

    void askRemove() {
        const auto entry = Library::instance().find(id_);
        if (!entry) return;
        const std::string id = id_;
        AppShell& shell = shell_;
        shell.confirm("Remove " + entry->title() + "?", "Megler's copy of the APK is deleted. To play it again you would add it again.",
                      "Remove", true, [&shell, id] {
                          Library::instance().remove(id);
                          shell.pop();
                      });
    }

    std::string id_;
    std::shared_ptr<PackState> pack_ = std::make_shared<PackState>();
    UIElement view_{nullptr};
    ContentControl host_{nullptr};
    unsigned generation_ = 0;
    std::string blocked_;
};

} // namespace

std::shared_ptr<Page> makeAppDetailScreen(AppShell& shell, std::string apkId) {
    return std::make_shared<AppDetailScreen>(shell, std::move(apkId));
}

} // namespace megler::app::ui
