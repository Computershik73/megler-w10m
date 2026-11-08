// What an app's own package shows until its app is open (shell/AppPackage.h): the splash screen's picture on its black,
// as Windows showed it a moment before, in place of Megler's library, which such a package has no use for. AppShell
// opens the app over it once the library has read the APK (ui/AppShell.cpp, poll).
//
// Where what Megler put in the shared folder for the app is not there -- no shared folder, no APK, no Android files, as when
// the app's package and Megler are on different storage (a memory card and the phone's memory) -- the app is not opened, and
// this says why and what to do instead of showing the picture for ever (appPackage::singleAppProblem).
#include "pch.h"

#include "player/SystemHost.h"
#include "shell/AppPackage.h"
#include "ui/AppShell.h"
#include "ui/Components.h"
#include "ui/Theme.h"

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {

class StartScreen final : public Page {
public:
    using Page::Page;

    std::string title() override { return problem_.empty() ? std::string() : "Can't Start"; }
    // (The phone's bars and Megler's tabs come back with the problem: its Log tab saves the logs.)
    bool fullScreen() override { return problem_.empty(); }
    unsigned orientations() override { return 2; } // DisplayOrientations::Portrait

    UIElement view() override {
        if (view_) return view_;
        root_ = Grid();
        root_.Background(brush(rgb(0x000000)));
        // The package's own splash screen picture (the app's icon in its middle: shell/AppPackage.cpp), as large as Windows
        // shows it: the width of the screen, at most its own.
        Image picture;
        picture.Source(Media::Imaging::BitmapImage(winrt::Windows::Foundation::Uri(L"ms-appx:///Assets/SplashScreen.png")));
        picture.Stretch(Media::Stretch::Uniform);
        picture.MaxWidth(620);
        picture.HorizontalAlignment(HorizontalAlignment::Center);
        picture.VerticalAlignment(VerticalAlignment::Center);
        root_.Children().Append(picture);
        view_ = root_;
        return view_;
    }

    void refresh() override {
        if (!problem_.empty() || !root_) return;
        const std::string& problem = appPackage::singleAppProblem();
        if (problem.empty()) return;
        problem_ = problem;
        showProblem();
    }

    /// Nothing to go back to: Back leaves, as on the app's own screen.
    bool handleBack() override {
        Application::Current().Exit();
        return true;
    }

private:
    /// Why the app cannot start, and Megler, where it can be installed again, a button away.
    void showProblem() {
        const auto& p = palette();
        root_.Children().Clear();
        root_.Background(brush(p.surface));
        auto column = vstack(0);
        column.Padding(ThicknessHelper::FromLengths(16, 8, 16, 24));
        add(column, warningCard(problem_));
        auto open = primaryButton("Open Megler", glyphs::openExternal, [] { systemHost::openMegler(L"library"); });
        open.HorizontalAlignment(HorizontalAlignment::Right);
        open.Margin(ThicknessHelper::FromLengths(0, 8, 0, 0));
        add(column, open);
        root_.Children().Append(scrollingPage(column));
    }

    UIElement view_{nullptr};
    Grid root_{nullptr};
    std::string problem_;
};

} // namespace

std::shared_ptr<Page> makeStartScreen(AppShell& shell) { return std::make_shared<StartScreen>(shell); }

} // namespace megler::app::ui
