// The views several screens share: an APK's icon, its status line, and the
// warnings about what this phone or this build cannot do.
#pragma once

#include "library/Library.h"
#include "ui/Theme.h"

namespace meglet::app::ui {

/// The APK's launcher icon, `size` square with rounded corners, over a
/// placeholder that stays when there is no icon or the phone cannot decode
/// it (WebP, which Windows 10 Mobile has no codec for).
winrt::Windows::UI::Xaml::FrameworkElement apkIcon(const ApkEntry& entry, double size);

/// "Ready" in green, a reason in orange, "Reading..." while the report is
/// being made.
winrt::Windows::UI::Xaml::Controls::TextBlock apkStatus(const ApkEntry& entry, double size);

/// An orange note in a card.
winrt::Windows::UI::Xaml::Controls::Border warningCard(std::string_view message);

/// The warnings every screen about running shows: the phone did not grant
/// code generation; the package has no ANGLE. Null when there is nothing to
/// warn about.
winrt::Windows::UI::Xaml::UIElement platformWarnings();

} // namespace meglet::app::ui
