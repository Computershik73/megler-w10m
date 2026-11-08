// The library: the APKs the user added, each a Material list item with its icon, its name and package, in one line whether
// it can run here, and in another whether it is installed as an app of the phone's own and by which Megler
// (ui/SeparateApps.h); above them, Update All when some were made by another Megler; Install APK, the floating action
// button.
#include "pch.h"

#include "library/Library.h"
#include "shell/AppPackage.h"
#include "shell/BundledApks.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Components.h"
#include "ui/GooglePlay.h"
#include "ui/Ota.h"
#include "ui/SeparateApps.h"

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {

UIElement apkRow(const ApkEntry& entry) {
    const auto& p = palette();
    Grid row;
    ColumnDefinition c0, c1;
    c0.Width(GridLengthHelper::Auto());
    c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    row.ColumnDefinitions().Append(c0);
    row.ColumnDefinitions().Append(c1);

    auto picture = apkIcon(entry, 48);
    picture.VerticalAlignment(VerticalAlignment::Top);
    row.Children().Append(picture);

    auto middle = vstack(0);
    middle.VerticalAlignment(VerticalAlignment::Center);
    middle.Margin(ThicknessHelper::FromLengths(16, 0, 0, 0));
    auto line = [&middle](const std::string& t, double size, Color color) {
        auto b = text(t, size, weightNormal(), color);
        b.TextWrapping(TextWrapping::NoWrap);
        b.TextTrimming(TextTrimming::CharacterEllipsis);
        add(middle, b);
    };
    line(entry.title(), font::bodyLarge, p.onSurface);
    if (!entry.report.package.empty() && entry.report.package != entry.title()) line(entry.report.package, font::bodyMedium, p.onSurfaceVariant);
    auto status = apkStatus(entry, font::bodyMedium);
    status.TextWrapping(TextWrapping::NoWrap);
    status.TextTrimming(TextTrimming::CharacterEllipsis);
    add(middle, status);
    // On the phone: installed, from which Megler, or what is being done.
    const separateApps::Status phone = separateApps::status(entry.report.package);
    std::string onPhone;
    std::optional<Color> color;
    if (phone.queued) {
        onPhone = phone.step;
        color = p.primary;
    } else if (phone.installed == separateApps::Installed::yes) {
        onPhone = phone.outdated ? "Installed, made by Megler " + phone.version + ": to update" : std::string("Installed");
        color = phone.outdated ? p.orange : p.green;
    } else if (phone.installed == separateApps::Installed::no && entry.report.ready()) {
        onPhone = "Not installed";
        color = p.onSurfaceVariant;
    }
    if (!onPhone.empty()) line(onPhone, font::bodyMedium, *color);
    Grid::SetColumn(middle, 1);
    row.Children().Append(middle);
    row.MinHeight(72);
    row.Padding(ThicknessHelper::FromLengths(16, 12, 24, 12));
    return row;
}

class LibraryScreen final : public Page {
public:
    using Page::Page;

    std::string title() override { return "Megler"; }

    UIElement view() override {
        if (view_) return view_;
        const auto& p = palette();
        column_ = vstack(0);
        // (Room at the end for the floating action button over the list.)
        column_.Padding(ThicknessHelper::FromLengths(16, 8, 16, 96));
        if (auto warnings = platformWarnings()) add(column_, warnings);

        // Megler's own update (from Pictures\Megler, or downloaded from GitHub: ui/Ota.h), while it is installed: Megler closes
        // once Windows takes it, and it went with nothing said, the phone's screen then going off as Windows installed it for
        // minutes (a Lumia 650's, from 574 to 610).
        updateText_ = text("", font::bodyMedium, weightNormal(), p.onSurface);
        updateText_.TextWrapping(TextWrapping::Wrap);
        updateBar_ = ProgressBar();
        updateBar_.IsIndeterminate(true);
        updateBar_.Foreground(brush(p.primary));
        updateBar_.Background(brush(p.secondaryContainer));
        updateBar_.Margin(ThicknessHelper::FromLengths(0, 12, 0, 0));
        auto updateColumn = vstack(0);
        add(updateColumn, updateText_);
        add(updateColumn, updateBar_);
        updateCard_ = card(updateColumn, 12, 16);
        updateCard_.Margin(ThicknessHelper::FromLengths(0, 8, 0, 8));
        setVisible(updateCard_, false);
        add(column_, updateCard_);

        // A newer Megler downloaded from GitHub (ui/Ota.h), while it comes.
        otaTitle_ = text("", font::titleMedium, weightMedium(), p.onSurface);
        otaBar_ = ProgressBar();
        otaBar_.Minimum(0);
        otaBar_.Maximum(1);
        otaBar_.Foreground(brush(p.primary));
        otaBar_.Background(brush(p.secondaryContainer));
        otaBar_.Margin(ThicknessHelper::FromLengths(0, 12, 0, 8));
        otaDetail_ = text("", font::bodyMedium, weightNormal(), p.onSurfaceVariant);
        auto otaColumn = vstack(0);
        add(otaColumn, otaTitle_);
        add(otaColumn, otaBar_);
        add(otaColumn, otaDetail_);
        otaCard_ = card(otaColumn, 12, 16);
        otaCard_.Margin(ThicknessHelper::FromLengths(0, 8, 0, 8));
        setVisible(otaCard_, false);
        add(column_, otaCard_);

        // The copy of a picked APK, while it runs.
        importTitle_ = text("", font::titleMedium, weightMedium(), p.onSurface);
        importBar_ = ProgressBar();
        importBar_.Minimum(0);
        importBar_.Maximum(1);
        importBar_.Foreground(brush(p.primary));
        importBar_.Background(brush(p.secondaryContainer));
        importBar_.Margin(ThicknessHelper::FromLengths(0, 12, 0, 8));
        importDetail_ = text("", font::bodyMedium, weightNormal(), p.onSurfaceVariant);
        auto importColumn = vstack(0);
        add(importColumn, importTitle_);
        add(importColumn, importBar_);
        add(importColumn, importDetail_);
        importCard_ = card(importColumn, 12, 16);
        importCard_.Margin(ThicknessHelper::FromLengths(0, 8, 0, 8));
        setVisible(importCard_, false);
        add(column_, importCard_);

        // Megler's copies of the Google APKs its package brings, made after the window is shown (shell/BundledApks.h): a
        // first start's are hundreds of megabytes, the better part of a minute on a slow phone.
        bundledTitle_ = text("", font::titleMedium, weightMedium(), p.onSurface);
        bundledBar_ = ProgressBar();
        bundledBar_.Minimum(0);
        bundledBar_.Maximum(1);
        bundledBar_.Foreground(brush(p.primary));
        bundledBar_.Background(brush(p.secondaryContainer));
        bundledBar_.Margin(ThicknessHelper::FromLengths(0, 12, 0, 8));
        bundledDetail_ = text("", font::bodyMedium, weightNormal(), p.onSurfaceVariant);
        bundledDetail_.TextWrapping(TextWrapping::Wrap);
        auto bundledColumn = vstack(0);
        add(bundledColumn, bundledTitle_);
        add(bundledColumn, bundledBar_);
        add(bundledColumn, bundledDetail_);
        bundledCard_ = card(bundledColumn, 12, 16);
        bundledCard_.Margin(ThicknessHelper::FromLengths(0, 8, 0, 8));
        setVisible(bundledCard_, false);
        add(column_, bundledCard_);

        // What is being done to the apps on the phone (ui/SeparateApps.h), from the moment an APK is picked: refreshed at
        // every poll, whatever the list.
        workText_ = text("", font::bodyMedium, weightNormal(), p.onSurface);
        workText_.TextWrapping(TextWrapping::Wrap);
        ProgressBar workBar;
        workBar.IsIndeterminate(true);
        workBar.Foreground(brush(p.primary));
        workBar.Background(brush(p.secondaryContainer));
        workBar.Margin(ThicknessHelper::FromLengths(0, 12, 0, 0));
        auto workColumn = vstack(0);
        add(workColumn, workText_);
        add(workColumn, workBar);
        workCard_ = card(workColumn, 12, 16);
        workCard_.Margin(ThicknessHelper::FromLengths(0, 8, 0, 8));
        setVisible(workCard_, false);
        add(column_, workCard_);

        listHost_ = ContentControl();
        listHost_.HorizontalContentAlignment(HorizontalAlignment::Stretch);
        add(column_, listHost_);

        // Install APK: Material's extended floating action button, at the end of the screen over the list.
        addButton_ = fab("Install APK", glyphs::add, [this] { addApk(); });
        addButton_.Margin(ThicknessHelper::FromLengths(16, 16, 16, 16));

        Grid layers;
        layers.Children().Append(scrollingPage(column_));
        layers.Children().Append(addButton_);
        view_ = layers;
        return view_;
    }

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
        const bundledApks::Progress bundled = bundledApks::progress();
        setVisible(bundledCard_, bundled.active);
        if (bundled.active) {
            const char* doing = bundled.stage == files::Stage::copying    ? "Copying "
                                : bundled.stage == files::Stage::flushing ? "Saving "
                                                                          : "Checking the copy of ";
            bundledTitle_.Text(hs(doing + bundled.what));
            bundledBar_.Value(bundled.total ? double(bundled.done) / double(bundled.total) : 0);
            bundledDetail_.Text(hs(formattedByteCount(bundled.done) + " of " + formattedByteCount(bundled.total) +
                                   (bundled.count > 1 ? format(", copy %d of %d", bundled.index, bundled.count) : std::string()) +
                                   ", from Megler's own package"));
        }
        const std::string updating = appPackage::selfUpdateVersion();
        // And why it was not installed, which went with nothing said: a Lumia 650's owner saw the card "for a second", and
        // nothing more (its copy of 650 in Pictures\Megler was damaged).
        const std::string failed = updating.empty() ? appPackage::selfUpdateFailure() : std::string();
        setVisible(updateCard_, !updating.empty() || !failed.empty());
        setVisible(updateBar_, !updating.empty());
        const std::string shown = updating.empty() ? failed : updating;
        if (!shown.empty() && shown != updateShown_) {
            updateShown_ = shown;
            // (Windows's deployment engine closed a Lumia 650's Megler 646 nine seconds after it was given 648. A few minutes,
            // as this said, were the Device Portal's, which installed Megler's updates before the engine did.)
            updateText_.Text(hs(updating.empty() ? failed
                                                 : "Megler " + updating + " is being installed. Windows closes Megler when it takes it, as a "
                                                   "rule within a minute: leave the phone on, then open Megler again."));
            updateText_.Foreground(brush(updating.empty() ? palette().error : palette().onSurface));
        }
        std::string otaVersion;
        uint64_t otaGot = 0, otaTotal = 0;
        const bool otaComing = ota::downloading(otaVersion, otaGot, otaTotal);
        setVisible(otaCard_, otaComing);
        if (otaComing) {
            otaTitle_.Text(hs("Downloading Megler " + otaVersion));
            otaBar_.Value(otaTotal ? double(otaGot) / double(otaTotal) : 0);
            otaDetail_.Text(hs(formattedByteCount(otaGot) + " of " + formattedByteCount(otaTotal) + " from GitHub; then it is checked and installed"));
        }
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
                                            "Pick an Android app's APK on this phone, or a bundle of split APKs (.apks, .xapk, .apkm): "
                                            "Megler reads it where it is, tells you whether it can run here, and installs it as an "
                                            "app of the phone's own.");
            empty.Margin(ThicknessHelper::FromLengths(8, 64, 8, 0));
            auto first = primaryButton("Install APK", glyphs::add, [this] { addApk(); });
            first.HorizontalAlignment(HorizontalAlignment::Center);
            first.Margin(ThicknessHelper::FromLengths(0, 24, 0, 0));
            empty.Children().Append(first);
            if (Library::hasDemo()) {
                // The package's own small app: the first thing to run on a
                // phone, since its picture says what is right and what is not.
                auto demo = textButton("Add the Demo", [this] { addDemo(); });
                demo.HorizontalAlignment(HorizontalAlignment::Center);
                demo.Margin(ThicknessHelper::FromLengths(0, 8, 0, 0));
                empty.Children().Append(demo);
            }
            if (Library::hasPackaged("hello.apk")) {
                // A package built with --java: two small apps whose code is Java alone, to see such an app drawn.
                auto java = textButton("Add the Java Test Apps", [this] { addJavaApps(); });
                java.HorizontalAlignment(HorizontalAlignment::Center);
                java.Margin(ThicknessHelper::FromLengths(0, 4, 0, 0));
                empty.Children().Append(java);
            }
            listHost_.Content(empty);
            return;
        }
        // The apps made by another Megler, to update, while nothing is being done; and whether the list could be had.
        auto top = vstack(0);
        // Which apps carry another Megler is known once Windows has listed the apps' packages, two seconds after the start on
        // a Lumia 650 (the Device Portal, which listed them before, took more than a minute there): said meanwhile, so that
        // Update All's coming is expected (the owner's wish, 2026-11-04).
        if (separateApps::checking()) {
            auto column = vstack(0);
            auto line = text("Please wait: checking whether the apps installed on the phone carry this Megler…", font::bodyMedium,
                             weightNormal(), palette().onSurfaceVariant);
            line.TextWrapping(TextWrapping::Wrap);
            add(column, line);
            ProgressBar bar;
            bar.IsIndeterminate(true);
            bar.Foreground(brush(palette().primary));
            bar.Background(brush(palette().secondaryContainer));
            bar.Margin(ThicknessHelper::FromLengths(0, 12, 0, 0));
            add(column, bar);
            auto box = card(column, 12, 16);
            box.Margin(ThicknessHelper::FromLengths(0, 8, 0, 8));
            add(top, box);
        }
        // Windows refused Megler's installs for access (Megler has not got packageManagement): why and what to do, once for all
        // the apps, Update All's included -- each app's page says it too, by its Open the Package.
        if (separateApps::installsRefused()) {
            auto warning = warningCard(appPackage::accessDeniedText("each app's page has Open the Package: Windows's own installer installs it."));
            add(top, warning);
        }
        // Google Play chosen at the first start and not installed yet: said, with microG meanwhile (ui/GooglePlay.h).
        if (const std::string waiting = googlePlay::pending(); !waiting.empty()) {
            auto line = text(waiting, font::bodyMedium, weightNormal(), palette().onSurfaceVariant);
            line.TextWrapping(TextWrapping::Wrap);
            auto box = card(line, 12, 16);
            box.Margin(ThicknessHelper::FromLengths(0, 8, 0, 8));
            add(top, box);
        }
        const auto outdated = separateApps::outdatedIds();
        if (separateApps::working().empty() && !outdated.empty()) {
            auto column = vstack(16);
            auto line = text(format("%u of the apps on the phone were made by another Megler. A package carries the Megler that made it: "
                                    "Update All makes and installs each again with this one.",
                                    unsigned(outdated.size())),
                             font::bodyMedium, weightNormal(), palette().onSurfaceVariant);
            line.TextWrapping(TextWrapping::Wrap);
            add(column, line);
            auto update = primaryButton("Update All", glyphs::refresh, [this] { separateApps::updateAll(shell_); });
            update.HorizontalAlignment(HorizontalAlignment::Right);
            add(column, update);
            auto box = card(column, 12, 16);
            box.Margin(ThicknessHelper::FromLengths(0, 8, 0, 8));
            add(top, box);
        }
        if (!separateApps::listProblem().empty()) {
            auto warning = warningCard("Megler can't tell which apps are installed: " + separateApps::listProblem());
            warning.Margin(ThicknessHelper::FromLengths(0, 8, 0, 8));
            add(top, warning);
#if defined(MEGLER_XBOX)
            // On an Xbox the console's Device Portal lists them: its user name and password asked for where it wants them,
            // else the list asked for again (and what to do said once more), without an install to try first.
            auto again = textButton(appPackage::portalWantsSignIn() ? "Sign In to the Device Portal" : "Try Again",
                                    [this] { separateApps::requireList(shell_); });
            again.HorizontalAlignment(HorizontalAlignment::Right);
            add(top, again);
#endif
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
            rows.push_back(button(listItem("Add the Java Test Apps", "hello, demos and gl, which the package carries", glyphs::add),
                                  [this] { addJavaApps(); }, ButtonKind::plain, "Add the Java Test Apps"));
        }
        add(top, listSection("", rows));
        listHost_.Content(top);
    }

    void addDemo() {
        if (!Library::instance().addDemo()) shell_.alert("Couldn't Add the Demo", "The copy into Megler's library failed.");
        refresh();
    }

    void addJavaApps() {
        bool added = Library::instance().addPackaged("hello.apk");
        if (Library::hasPackaged("demos.apk")) added = Library::instance().addPackaged("demos.apk") && added;
        // (OpenGL ES from Java: a GLSurfaceView, EGL14 and EGL10.)
        if (Library::hasPackaged("gl.apk")) added = Library::instance().addPackaged("gl.apk") && added;
        if (!added) shell_.alert("Couldn't Add the Java Test Apps", "The copy into Megler's library failed.");
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
    Border otaCard_{nullptr};
    TextBlock otaTitle_{nullptr};
    ProgressBar otaBar_{nullptr};
    TextBlock otaDetail_{nullptr};
    Border importCard_{nullptr};
    TextBlock importTitle_{nullptr};
    ProgressBar importBar_{nullptr};
    TextBlock importDetail_{nullptr};
    Border bundledCard_{nullptr};
    TextBlock bundledTitle_{nullptr};
    ProgressBar bundledBar_{nullptr};
    TextBlock bundledDetail_{nullptr};
    ContentControl listHost_{nullptr};
    Border workCard_{nullptr};
    Border updateCard_{nullptr};
    TextBlock updateText_{nullptr};
    ProgressBar updateBar_{nullptr};
    std::string updateShown_;
    TextBlock workText_{nullptr};
    Button addButton_{nullptr};
    unsigned generation_ = 0, phoneGeneration_ = 0;
};

} // namespace

std::shared_ptr<Page> makeLibraryScreen(AppShell& shell) { return std::make_shared<LibraryScreen>(shell); }

} // namespace megler::app::ui
