// What an app's own package shows until its app is open (shell/AppPackage.h): the splash screen's picture on its black,
// as Windows showed it a moment before, in place of Megler's library, which such a package has no use for. AppShell
// opens the app over it once the library has read the APK (ui/AppShell.cpp, poll).
#include "pch.h"

#include "ui/AppShell.h"
#include "ui/Theme.h"

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;

namespace {

class StartScreen final : public Page {
public:
    using Page::Page;

    std::string title() override { return {}; }
    bool fullScreen() override { return true; }
    bool upright() override { return true; }

    UIElement view() override {
        if (view_) return view_;
        Grid root;
        root.Background(brush(rgb(0x000000)));
        // The package's own splash screen picture (the app's icon in its middle: shell/AppPackage.cpp), as large as Windows
        // shows it: the width of the screen, at most its own.
        Image picture;
        picture.Source(Media::Imaging::BitmapImage(winrt::Windows::Foundation::Uri(L"ms-appx:///Assets/SplashScreen.png")));
        picture.Stretch(Media::Stretch::Uniform);
        picture.MaxWidth(620);
        picture.HorizontalAlignment(HorizontalAlignment::Center);
        picture.VerticalAlignment(VerticalAlignment::Center);
        root.Children().Append(picture);
        view_ = root;
        return view_;
    }

    /// Nothing to go back to: Back leaves, as on the app's own screen.
    bool handleBack() override {
        Application::Current().Exit();
        return true;
    }

private:
    UIElement view_{nullptr};
};

} // namespace

std::shared_ptr<Page> makeStartScreen(AppShell& shell) { return std::make_shared<StartScreen>(shell); }

} // namespace megler::app::ui
