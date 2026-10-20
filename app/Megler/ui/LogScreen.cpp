// The log: the last lines of what the core reported through megler_config.log
// and the shell's own (shell/ShellLog.h), readable on the phone itself. The
// same lines are in LocalState\megler.log, the run before in megler.prev.log.
#include "pch.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Theme.h"

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {

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
        auto where = text("LocalState\\megler.log · the run before: megler.prev.log", font::caption, weightNormal(), p.secondaryLabel);
        where.Margin(ThicknessHelper::FromLengths(16, 6, 16, 8));
        root.Children().Append(where);

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
    }

private:
    void setFollowing(bool following) {
        lines_->setFollowing(following);
        followLabel_.Text(following ? L"Hold" : L"Follow");
        followGlyph_.Text(hstring(std::wstring(1, following ? glyphs::pause : glyphs::play)));
    }

    UIElement view_{nullptr};
    std::unique_ptr<LineList> lines_;
    Button followButton_{nullptr};
    TextBlock followLabel_{nullptr};
    TextBlock followGlyph_{nullptr};
    uint64_t cursor_ = 0;
};

} // namespace

std::shared_ptr<Page> makeLogScreen(AppShell& shell) { return std::make_shared<LogScreen>(shell); }

} // namespace megler::app::ui
