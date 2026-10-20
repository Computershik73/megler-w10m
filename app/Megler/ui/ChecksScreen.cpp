// The device checks: what this phone does with the mechanisms the layer rests
// on (megler_probe), and the core's own tests run on it (megler_selftest). Each
// runs on a thread of its own (shell/Checks.h); its lines appear here as
// they arrive and are saved in LocalState, where tools/phone.py reads them.
#include "pch.h"

#include "player/Session.h"
#include "shell/Checks.h"
#include "shell/Strings.h"
#include "ui/AppShell.h"
#include "ui/Components.h"

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {

class ChecksScreen final : public Page {
public:
    using Page::Page;

    std::string title() override { return "Device"; }

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

        auto top = vstack(0);
        top.Padding(ThicknessHelper::FromLengths(16, 12, 16, 10));
        Grid buttons;
        for (int i = 0; i < 2; ++i) {
            ColumnDefinition c;
            c.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
            buttons.ColumnDefinitions().Append(c);
        }
        probeButton_ = primaryButton("Probe", glyphs::device, [this] { run(checks::Kind::probe); });
        probeButton_.Margin(ThicknessHelper::FromLengths(0, 0, 5, 0));
        selfTestButton_ = primaryButton("Self-test", glyphs::wrench, [this] { askSelfTest(); });
        selfTestButton_.Margin(ThicknessHelper::FromLengths(5, 0, 0, 0));
        Grid::SetColumn(selfTestButton_, 1);
        buttons.Children().Append(probeButton_);
        buttons.Children().Append(selfTestButton_);
        add(top, buttons);
        status_ = text("Probe: how Android code can run on this phone, measured; it ends with a tone that should be heard. "
                       "Self-test: the core's own tests, with real "
                       "Android libraries loaded and called and a small app started -- the one app a start of Megler can run, "
                       "so nothing can be played after it until Megler is opened again.",
                       font::footnote, weightNormal(), p.secondaryLabel);
        status_.Margin(ThicknessHelper::FromLengths(2, 10, 2, 0));
        add(top, status_);
        root.Children().Append(top);

        lines_ = std::make_unique<LineList>(4000);
        Grid::SetRow(lines_->view(), 1);
        root.Children().Append(lines_->view());

        view_ = root;
        return view_;
    }

    void refresh() override {
        const bool running = checks::running();
        // A check started elsewhere (the unattended run) shows here too.
        if (running && checks::current() != shown_) show(checks::current());

        std::vector<std::string> fresh;
        unsigned run = run_;
        const uint64_t next = checks::take(shown_, cursor_, fresh, run);
        if (run != run_) {
            run_ = run;
            lines_->clear();
        }
        cursor_ = next;
        lines_->append(fresh);

        const session::State state = session::snapshot().state;
        const bool appRunning = state == session::State::starting || state == session::State::running;
        // The self-test starts a small app of its own: once per start of
        // Megler, and not after another app has run.
        const bool selfTestSpent = checks::selfTestStarted() || session::spent();
        probeButton_.IsEnabled(!running && !appRunning);
        selfTestButton_.IsEnabled(!running && !selfTestSpent);
        std::string status;
        if (running)
            status = std::string("Running the ") + (checks::current() == checks::Kind::probe ? "probe" : "self-test") +
                     "… It takes a few seconds; the screen stays on.";
        else if (appRunning)
            status = "An app is running: the checks wait until it has ended.";
        else {
            if (run_ > 0) status = "Saved as LocalState\\" + narrow(checks::logName(shown_)) + ". ";
            if (checks::selfTestStarted())
                status += "The self-test has had its one run: it can run again, and apps can be played, once Megler has been "
                          "closed and opened.";
            else if (selfTestSpent)
                status += "An app has run since Megler was opened: the self-test, which starts a small app of its own, can run "
                          "once Megler has been closed and opened again.";
        }
        if (!status.empty() && status != statusShown_) {
            statusShown_ = status;
            status_.Text(hs(status));
        }
    }

private:
    void show(checks::Kind kind) {
        shown_ = kind;
        cursor_ = 0;
        run_ = 0;
        lines_->clear();
    }

    void run(checks::Kind kind) {
        if (!checks::start(kind)) return;
        show(kind);
        refresh();
    }

    void askSelfTest() {
        std::weak_ptr<Page> weak = weak_from_this();
        shell_.confirm("Run the Self-test?",
                       "It starts a small app of its own, and one app is all a start of Megler can run: afterwards Megler has to "
                       "be closed and opened again before anything can be played.",
                       "Run", false, [this, weak] {
                           if (weak.lock()) run(checks::Kind::selftest);
                       });
    }

    UIElement view_{nullptr};
    Button probeButton_{nullptr};
    Button selfTestButton_{nullptr};
    TextBlock status_{nullptr};
    std::unique_ptr<LineList> lines_;
    checks::Kind shown_ = checks::Kind::probe;
    uint64_t cursor_ = 0;
    unsigned run_ = 0;
    std::string statusShown_;
};

} // namespace

std::shared_ptr<Page> makeChecksScreen(AppShell& shell) { return std::make_shared<ChecksScreen>(shell); }

} // namespace megler::app::ui
