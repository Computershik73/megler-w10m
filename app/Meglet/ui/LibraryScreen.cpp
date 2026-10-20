// The library: the APKs the user added, each with its icon, its name and
// package, in one line whether it can run here, and in another whether it is
// installed as an app of the phone's own and by which Meglet (ui/SeparateApps.h);
// above them, Update All when some were made by another Meglet.
#include "pch.h"

#include "library/Library.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Components.h"
#include "ui/SeparateApps.h"

namespace meglet::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {

UIElement apkRow(const ApkEntry& entry) {
    const auto& p = palette();
    Grid row;
    ColumnDefinition c0, c1, c2;
    c0.Width(GridLengthHelper::Auto());
    c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    c2.Width(GridLengthHelper::Auto());
    row.ColumnDefinitions().Append(c0);
    row.ColumnDefinitions().Append(c1);
    row.ColumnDefinitions().Append(c2);

    row.Children().Append(apkIcon(entry, 52));

    auto middle = vstack(2);
    middle.VerticalAlignment(VerticalAlignment::Center);
    middle.Margin(ThicknessHelper::FromLengths(12, 0, 0, 0));
    auto title = text(entry.title(), font::body, weightMedium(), p.label);
    title.TextWrapping(TextWrapping::NoWrap);
    title.TextTrimming(TextTrimming::CharacterEllipsis);
    add(middle, title);
    if (!entry.report.package.empty() && entry.report.package != entry.title()) {
        auto package = text(entry.report.package, font::caption, weightNormal(), p.secondaryLabel);
        package.TextWrapping(TextWrapping::NoWrap);
        package.TextTrimming(TextTrimming::CharacterEllipsis);
        add(middle, package);
    }
    auto status = apkStatus(entry, font::caption);
    status.TextWrapping(TextWrapping::NoWrap);
    status.TextTrimming(TextTrimming::CharacterEllipsis);
    add(middle, status);
    // On the phone: installed, from which Meglet, or what is being done.
    const separateApps::Status phone = separateApps::status(entry.report.package);
    std::string onPhone;
    std::optional<Color> color;
    if (phone.queued) {
        onPhone = phone.step;
        color = p.accentText;
    } else if (phone.installed == separateApps::Installed::yes) {
        onPhone = phone.outdated ? "Installed, made by Meglet " + phone.version + ": to update" : std::string("Installed");
        color = phone.outdated ? p.orange : p.green;
    } else if (phone.installed == separateApps::Installed::no && entry.report.ready()) {
        onPhone = "Not installed";
        color = p.tertiaryLabel;
    }
    if (!onPhone.empty()) {
        auto line = text(onPhone, font::caption, weightNormal(), color);
        line.TextWrapping(TextWrapping::NoWrap);
        line.TextTrimming(TextTrimming::CharacterEllipsis);
        add(middle, line);
    }
    Grid::SetColumn(middle, 1);
    row.Children().Append(middle);

    auto chevron = glyph(glyphs::chevronRight, font::caption, p.tertiaryLabel);
    chevron.Margin(ThicknessHelper::FromLengths(10, 0, 0, 0));
    Grid::SetColumn(chevron, 2);
    row.Children().Append(chevron);
    row.Padding(ThicknessHelper::FromLengths(14, 10, 14, 10));
    return row;
}

class LibraryScreen final : public Page {
public:
    using Page::Page;

    std::string title() override { return "Meglet"; }

    UIElement view() override {
        if (view_) return view_;
        const auto& p = palette();
        column_ = vstack(0);
        column_.Padding(ThicknessHelper::FromLengths(16, 12, 16, 24));
        if (auto warnings = platformWarnings()) add(column_, warnings);

        // The copy of a picked APK, while it runs.
        importTitle_ = text("", font::subheadline, weightMedium(), p.label);
        importBar_ = ProgressBar();
        importBar_.Minimum(0);
        importBar_.Maximum(1);
        importBar_.Margin(ThicknessHelper::FromLengths(0, 10, 0, 6));
        importDetail_ = text("", font::caption, weightNormal(), p.secondaryLabel);
        auto importColumn = vstack(0);
        add(importColumn, importTitle_);
        add(importColumn, importBar_);
        add(importColumn, importDetail_);
        importCard_ = card(importColumn, 12, 14);
        importCard_.Margin(ThicknessHelper::FromLengths(0, 0, 0, 14));
        setVisible(importCard_, false);
        add(column_, importCard_);

        // What is being done to the apps on the phone (ui/SeparateApps.h), from the moment an APK is picked: refreshed at
        // every poll, whatever the list.
        workText_ = text("", font::subheadline, weightNormal(), p.label);
        workText_.TextWrapping(TextWrapping::Wrap);
        ProgressBar workBar;
        workBar.IsIndeterminate(true);
        workBar.Margin(ThicknessHelper::FromLengths(0, 10, 0, 0));
        auto workColumn = vstack(0);
        add(workColumn, workText_);
        add(workColumn, workBar);
        workCard_ = card(workColumn, 12, 14);
        workCard_.Margin(ThicknessHelper::FromLengths(0, 0, 0, 14));
        setVisible(workCard_, false);
        add(column_, workCard_);

        listHost_ = ContentControl();
        listHost_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        add(column_, listHost_);

        addButton_ = button(label("Install APK", glyphs::add, font::body, weightNormal(), p.accentText), [this] { addApk(); },
                            ButtonKind::plain, "Install APK");
        addButton_.Padding(ThicknessHelper::FromLengths(8, 6, 4, 6));

        view_ = scrollingPage(column_);
        return view_;
    }

    UIElement toolbarItem() override { return addButton_; }

    void appeared() override { separateApps::refresh(shell_); }

    void refresh() override {
        const ImportProgress& import = shell_.importProgress();
        const bool copying = import.active && !import.name.empty();
        setVisible(importCard_, copying);
        if (copying) {
            importTitle_.Text(hs((import.unpacking ? "Unpacking " : import.copied ? "Copying " : "Opening ") + import.name));
            importBar_.Value(import.total ? double(import.copied) / double(import.total) : 0);
            importDetail_.Text(hs(formattedByteCount(import.copied) + " of " + formattedByteCount(import.total)));
        }
        addButton_.IsEnabled(!import.active);
        const std::string working = separateApps::working();
        setVisible(workCard_, !working.empty());
        if (!working.empty() && narrow(std::wstring(workText_.Text())) != working) workText_.Text(hs(working));

        const unsigned generation = Library::instance().generation(), phone = separateApps::generation();
        if (generation == generation_ && phone == phoneGeneration_) return;
        generation_ = generation;
        phoneGeneration_ = phone;
        rebuildList();
    }

private:
    void rebuildList() {
        const auto entries = Library::instance().entries();
        if (entries.empty()) {
            auto empty = contentUnavailable(glyphs::game, "No Apps Yet",
                                            "Pick an Android app's APK on this phone, or a bundle of split APKs (.apks, .xapk): "
                                            "Meglet reads it where it is, tells you whether it can run here, and installs it as an "
                                            "app of the phone's own.");
            empty.Margin(ThicknessHelper::FromLengths(8, 48, 8, 0));
            auto first = primaryButton("Install APK", glyphs::add, [this] { addApk(); });
            first.HorizontalAlignment(HorizontalAlignment::Center);
            first.Margin(ThicknessHelper::FromLengths(0, 22, 0, 0));
            empty.Children().Append(first);
            if (Library::hasDemo()) {
                // The package's own small app: the first thing to run on a
                // phone, since its picture says what is right and what is not.
                auto demo = button(label("Add the Demo", glyphs::game, font::body, weightNormal(), palette().accentText),
                                   [this] { addDemo(); }, ButtonKind::plain, "Add the Demo");
                demo.HorizontalAlignment(HorizontalAlignment::Center);
                demo.Margin(ThicknessHelper::FromLengths(0, 10, 0, 0));
                empty.Children().Append(demo);
            }
            if (Library::hasPackaged("hello.apk")) {
                // A package built with --java: two small apps whose code is Java alone, to see such an app drawn.
                auto java = button(label("Add the Java Test Apps", glyphs::game, font::body, weightNormal(), palette().accentText),
                                   [this] { addJavaApps(); }, ButtonKind::plain, "Add the Java Test Apps");
                java.HorizontalAlignment(HorizontalAlignment::Center);
                java.Margin(ThicknessHelper::FromLengths(0, 10, 0, 0));
                empty.Children().Append(java);
            }
            listHost_.Content(empty);
            return;
        }
        // The apps made by another Meglet, to update, while nothing is being done; and whether the portal could be asked.
        auto top = vstack(0);
        const auto outdated = separateApps::outdatedIds();
        if (separateApps::working().empty() && !outdated.empty()) {
            auto column = vstack(10);
            auto line = text(format("%u of the apps on the phone were made by another Meglet. A package carries the Meglet that made it: "
                                    "Update All makes and installs each again with this one.",
                                    unsigned(outdated.size())),
                             font::subheadline, weightNormal(), palette().label);
            line.TextWrapping(TextWrapping::Wrap);
            add(column, line);
            add(column, primaryButton("Update All", glyphs::refresh, [this] { separateApps::updateAll(shell_); }));
            auto box = card(column, 12, 14);
            box.Margin(ThicknessHelper::FromLengths(0, 0, 0, 14));
            add(top, box);
        }
        if (!separateApps::portalProblem().empty()) {
            auto warning = warningCard("Meglet can't tell which apps are installed: " + separateApps::portalProblem());
            warning.Margin(ThicknessHelper::FromLengths(0, 0, 0, 14));
            add(top, warning);
        }
        std::vector<UIElement> rows;
        for (const auto& entry : entries) {
            const std::string id = entry.id;
            rows.push_back(button(apkRow(entry), [this, id] { shell_.push(makeAppDetailScreen(shell_, id)); }, ButtonKind::plain,
                                  entry.title()));
        }
        // A package built with --java, on a phone whose library already has apps in it: the same offer as on the empty page.
        const bool hasHello = std::any_of(entries.begin(), entries.end(), [](const ApkEntry& e) { return e.id == "hello.apk"; });
        if (Library::hasPackaged("hello.apk") && !hasHello) {
            rows.push_back(button(label("Add the Java Test Apps", glyphs::game, font::body, weightNormal(), palette().accentText),
                                  [this] { addJavaApps(); }, ButtonKind::plain, "Add the Java Test Apps"));
        }
        add(top, listSection("Apps", rows));
        listHost_.Content(top);
    }

    void addDemo() {
        if (!Library::instance().addDemo()) shell_.alert("Couldn't Add the Demo", "The copy into Meglet's library failed.");
        refresh();
    }

    void addJavaApps() {
        bool added = Library::instance().addPackaged("hello.apk");
        if (Library::hasPackaged("demos.apk")) added = Library::instance().addPackaged("demos.apk") && added;
        // (OpenGL ES from Java: a GLSurfaceView, EGL14 and EGL10.)
        if (Library::hasPackaged("gl.apk")) added = Library::instance().addPackaged("gl.apk") && added;
        if (!added) shell_.alert("Couldn't Add the Java Test Apps", "The copy into Meglet's library failed.");
        refresh();
    }

    void addApk() {
        std::weak_ptr<Page> weak = weak_from_this();
        shell_.importApk([this, weak](std::string id, std::string error) {
            if (!weak.lock()) return;
            if (!error.empty()) shell_.alert("Couldn't Install This APK", error);
            refresh();
        });
        refresh();
    }

    UIElement view_{nullptr};
    StackPanel column_{nullptr};
    Border importCard_{nullptr};
    TextBlock importTitle_{nullptr};
    ProgressBar importBar_{nullptr};
    TextBlock importDetail_{nullptr};
    ContentControl listHost_{nullptr};
    Border workCard_{nullptr};
    TextBlock workText_{nullptr};
    Button addButton_{nullptr};
    unsigned generation_ = 0, phoneGeneration_ = 0;
};

} // namespace

std::shared_ptr<Page> makeLibraryScreen(AppShell& shell) { return std::make_shared<LibraryScreen>(shell); }

} // namespace meglet::app::ui
