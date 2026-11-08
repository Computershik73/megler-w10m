// The log: the last lines of what the core reported through megler_config.log
// and the shell's own (shell/ShellLog.h), readable on the phone itself. The
// same lines are in LocalState\megler.log, the run before in megler.prev.log.
// Above them, Save All Logs: every package's logs, Megler's and each app's
// own package's, in one ZIP for a problem report (shell/LogArchive.h), with
// where it went and Share, which sends it from the phone itself.
#include "pch.h"

#include "shell/LogArchive.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Theme.h"

#include <winrt/Windows.ApplicationModel.DataTransfer.h>

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {

/// Whether Windows has Share here: an Xbox's says it has not (DataTransferManager.IsSupported, of 1803); a phone's 1703,
/// which has Share but not that question, has.
bool shareable() {
    static const bool can = [] {
        try {
            using winrt::Windows::Foundation::Metadata::ApiInformation;
            if (ApiInformation::IsMethodPresent(L"Windows.ApplicationModel.DataTransfer.DataTransferManager", L"IsSupported"))
                return winrt::Windows::ApplicationModel::DataTransfer::DataTransferManager::IsSupported();
            return true;
        } catch (...) {
            return false;
        }
    }();
    return can;
}

class LogScreen final : public Page {
public:
    using Page::Page;

    std::string title() override { return "Log"; }

    UIElement view() override {
        if (view_) return view_;
        const auto& p = palette();
        Grid root;
        root.Background(brush(p.background));
        for (int i = 0; i < 2; ++i) {
            RowDefinition row;
            row.Height(i == 1 ? GridLengthHelper::FromValueAndType(1, GridUnitType::Star) : GridLengthHelper::Auto());
            root.RowDefinitions().Append(row);
        }
        auto top = vstack(8);
        top.Margin(ThicknessHelper::FromLengths(16, 6, 16, 8));
        add(top, text("LocalState\\megler.log · the run before: megler.prev.log", font::caption, weightNormal(), p.secondaryLabel));
        auto buttons = hstack(8);
        saveButton_ = tonalButton("Save All Logs", glyphs::download, [this] { save(); });
        add(buttons, saveButton_);
        shareButton_ = textButton("Share", [this] { share(); });
        setVisible(shareButton_, false);
        add(buttons, shareButton_);
        add(top, buttons);
        saveText_ = text("Megler's logs and every app's in one ZIP file, for a problem report.", font::bodySmall, weightNormal(),
                         p.onSurfaceVariant);
        saveText_.TextWrapping(TextWrapping::Wrap);
        add(top, saveText_);
        saveBar_ = ProgressBar();
        saveBar_.Minimum(0);
        saveBar_.Maximum(1);
        saveBar_.Foreground(brush(p.primary));
        saveBar_.Background(brush(p.secondaryContainer));
        setVisible(saveBar_, false);
        add(top, saveBar_);
        root.Children().Append(top);

        lines_ = std::make_unique<LineList>(shellLog::ringCapacity);
        Grid::SetRow(lines_->view(), 1);
        root.Children().Append(lines_->view());

        // Following the newest line, or holding still to be read.
        followLabel_ = text("", font::body, weightNormal(), p.accentText);
        followGlyph_ = glyph(glyphs::pause, font::body, p.accentText);
        auto content = hstack(6);
        add(content, followGlyph_);
        add(content, followLabel_);
        followButton_ = button(content, [this] { setFollowing(!lines_->following()); }, ButtonKind::plain, "Follow the log");
        followButton_.Padding(ThicknessHelper::FromLengths(8, 6, 4, 6));
        setFollowing(true);

        view_ = root;
        return view_;
    }

    UIElement toolbarItem() override { return followButton_; }

    void refresh() override {
        std::vector<std::string> fresh;
        cursor_ = shellLog::take(cursor_, fresh);
        lines_->append(fresh);
        showSaving();
    }

private:
    void setFollowing(bool following) {
        lines_->setFollowing(following);
        followLabel_.Text(following ? L"Hold" : L"Follow");
        followGlyph_.Text(hstring(std::wstring(1, following ? glyphs::pause : glyphs::play)));
    }

    void save() {
        if (logArchive::start()) shellLog::write("logs: Save All Logs");
        showSaving();
    }

    /// Where Save All Logs stands: while it runs, its step and a bar; then where the ZIP went, or why there is none.
    void showSaving() {
        const logArchive::State s = logArchive::state();
        saveButton_.IsEnabled(!s.running);
        setVisible(saveBar_, s.running);
        setVisible(shareButton_, !s.running && s.saved && shareable() && logArchive::file());
        if (s.running) {
            saveBar_.Value(s.total ? double(s.done) / double(s.total) : 0);
            setSaveText(s.step + " " + formattedByteCount(s.done) + " of " + formattedByteCount(s.total), false);
            return;
        }
        if (s.generation == shownGeneration_) return;
        shownGeneration_ = s.generation;
        if (!s.error.empty()) {
            setSaveText(s.error, true);
        } else if (s.saved) {
            std::string said = format("Saved in %s: %s, %s, %u files.", s.where.c_str(), s.name.c_str(), formattedByteCount(s.size).c_str(), s.files);
            if (!s.note.empty())
                said += " " + s.note;
            else if (platform::isMobile())
                said += " The phone's file manager finds it there, and a PC over USB in the phone's Pictures.";
            else
                said += " That is this PC's Pictures folder.";
            setSaveText(said, false);
        }
    }

    void setSaveText(const std::string& said, bool error) {
        if (narrow(std::wstring(saveText_.Text())) != said) saveText_.Text(hs(said));
        saveText_.Foreground(brush(error ? palette().error : palette().onSurfaceVariant));
    }

    /// The ZIP to Windows's Share, which sends it on from the phone: mail, a messenger, OneDrive.
    void share() {
        using namespace winrt::Windows::ApplicationModel::DataTransfer;
        const auto file = logArchive::file();
        if (!file) return;
        try {
            // (Once a run: the view's DataRequested hands over whichever ZIP was saved last.)
            static bool registered = false;
            if (!registered) {
                DataTransferManager::GetForCurrentView().DataRequested([](DataTransferManager const&, DataRequestedEventArgs const& e) {
                    const auto saved = logArchive::file();
                    auto request = e.Request();
                    if (!saved) {
                        request.FailWithDisplayText(L"No logs have been saved yet.");
                        return;
                    }
                    request.Data().Properties().Title(L"Megler's logs");
                    request.Data().Properties().Description(saved.Name());
                    request.Data().SetStorageItems(winrt::single_threaded_vector<winrt::Windows::Storage::IStorageItem>({saved}));
                });
                registered = true;
            }
            DataTransferManager::ShowShareUI();
            shellLog::write("logs: " + narrow(std::wstring(file.Name())) + " handed to Share");
        } catch (hresult_error const& e) {
            shellLog::write("logs: Share could not be opened: " + narrow(e.message()));
            shell_.alert("Couldn't Share", "Windows did not open Share: " + narrow(e.message()));
        }
    }

    UIElement view_{nullptr};
    std::unique_ptr<LineList> lines_;
    Button followButton_{nullptr};
    TextBlock followLabel_{nullptr};
    TextBlock followGlyph_{nullptr};
    Button saveButton_{nullptr};
    Button shareButton_{nullptr};
    TextBlock saveText_{nullptr};
    ProgressBar saveBar_{nullptr};
    unsigned shownGeneration_ = 0;
    uint64_t cursor_ = 0;
};

} // namespace

std::shared_ptr<Page> makeLogScreen(AppShell& shell) { return std::make_shared<LogScreen>(shell); }

} // namespace megler::app::ui
