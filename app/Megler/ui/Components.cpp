#include "pch.h"

#include "ui/Components.h"

#include "shell/Platform.h"
#include "shell/Strings.h"

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;

FrameworkElement apkIcon(const ApkEntry& entry, double size) {
    const auto& p = palette();
    const auto radius = CornerRadiusHelper::FromUniformRadius(size * 0.22);
    Grid holder;
    holder.Width(size);
    holder.Height(size);

    Border placeholder;
    placeholder.CornerRadius(radius);
    placeholder.Background(brush(p.fill));
    auto mark = glyph(glyphs::game, size * 0.46, p.tertiaryLabel);
    mark.HorizontalAlignment(HorizontalAlignment::Center);
    placeholder.Child(mark);
    holder.Children().Append(placeholder);

    if (!entry.iconPath.empty()) {
        try {
            // The cache file beside the copy (library/Library.h), through its
            // ms-appdata address: the decoder goes by the bytes, not the
            // name. Decoded at twice the size shown, not at the icon's own
            // 512 pixels.
            const hstring name = winrt::Windows::Foundation::Uri::EscapeComponent(hs(entry.id + ".icon"));
            Imaging::BitmapImage bitmap;
            bitmap.CreateOptions(Imaging::BitmapCreateOptions::IgnoreImageCache);
            bitmap.DecodePixelWidth(int(size * 2));
            bitmap.UriSource(winrt::Windows::Foundation::Uri(L"ms-appdata:///local/apks/" + name));
            ImageBrush image;
            image.ImageSource(bitmap);
            image.Stretch(Stretch::UniformToFill);
            Border picture;
            picture.CornerRadius(radius);
            picture.Background(image);
            // Rounded through the brush: a Border does not clip its child.
            image.ImageFailed([picture](auto&&, auto&&) { picture.Visibility(Visibility::Collapsed); });
            holder.Children().Append(picture);
        } catch (const hresult_error&) {
        }
    }
    return holder;
}

TextBlock apkStatus(const ApkEntry& entry, double size) {
    const auto& p = palette();
    if (!entry.analysed) return text("Reading…", size, weightNormal(), p.secondaryLabel);
    const bool ready = entry.report.ready();
    auto t = text(entry.report.status(), size, weightMedium(), ready ? p.green : p.orange);
    return t;
}

Border warningCard(std::string_view message) {
    auto mark = glyph(glyphs::warning, font::headline, palette().orange);
    mark.VerticalAlignment(VerticalAlignment::Top);
    mark.Margin(ThicknessHelper::FromLengths(0, 2, 0, 0));
    Grid g;
    ColumnDefinition c0, c1;
    c0.Width(GridLengthHelper::Auto());
    c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    g.ColumnDefinitions().Append(c0);
    g.ColumnDefinitions().Append(c1);
    g.Children().Append(mark);
    auto t = text(message, font::footnote, weightNormal(), palette().label);
    t.Margin(ThicknessHelper::FromLengths(10, 0, 0, 0));
    Grid::SetColumn(t, 1);
    g.Children().Append(t);
    auto c = card(g, 12, 12);
    c.Margin(ThicknessHelper::FromLengths(0, 0, 0, 14));
    return c;
}

UIElement platformWarnings() {
    const bool canRun = platform::canRunAndroidCode();
    const bool angle = platform::angle().present;
    if (canRun && angle) return nullptr;
    auto column = vstack(0);
    if (!canRun) {
#if defined(_M_ARM)
        column.Children().Append(warningCard(
            "This phone has not let Megler make code executable (the codeGeneration capability, which an "
            "interop-unlocked phone grants). APKs can be added and looked at, but none can run."));
#else
        column.Children().Append(warningCard(
            "This build is not for an ARM phone: Android code cannot run in it. APKs can be added and looked at."));
#endif
    }
    if (!angle)
        column.Children().Append(warningCard(platform::angle().summary +
                                             ". This build has nothing to turn OpenGL ES into Direct3D with, so no app can draw."));
    return column;
}

} // namespace megler::app::ui
