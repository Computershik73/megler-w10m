// Settings: the size of the surface apps draw to, and what this build and
// this phone are.
#include "pch.h"

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
        // Memory moves; once a second is plenty.
        if (++ticks_ % 4 != 0 || !memory_) return;
        if (auto value = memory_.Children().GetAt(1).try_as<TextBlock>()) value.Text(hs(platform::memorySummary()));
    }

private:
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
};

} // namespace

std::shared_ptr<Page> makeSettingsScreen(AppShell& shell) { return std::make_shared<SettingsScreen>(shell); }

} // namespace megler::app::ui
