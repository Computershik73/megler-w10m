// Megler's look, built for XAML in code (there is no markup compiler in this build): Material Design 3, as Android's own
// apps have it, and the builders every screen is made of.
//
// The colours are a Material 3 colour scheme made of the phone's accent colour (Windows' accent, the seed of Material
// You's palettes, as an Android phone takes its wallpaper's: megler_palette_tone, core/src/run/mg_palette.c), dark or
// light as the phone's theme is or Settings say (player/PhoneState.h). The text is Roboto, Android's own face, which the
// package carries in fonts\ (tools/build-appx.sh, from the Android root's fonts), with Segoe UI where it is not there.
// The icons are Google's Material icons, drawn from their paths (Apache License 2.0); Segoe MDL2 Assets' glyphs
// for the few there is no path for here.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace megler::app::ui {

using winrt::Windows::UI::Color;

struct Palette {
    bool dark = true;
    // Material 3's colour roles.
    Color primary, onPrimary, primaryContainer, onPrimaryContainer;
    Color secondaryContainer, onSecondaryContainer;
    Color tertiaryContainer, onTertiaryContainer;
    Color error, onError, errorContainer, onErrorContainer;
    Color surface, surfaceContainerLow, surfaceContainer, surfaceContainerHigh, surfaceContainerHighest;
    Color onSurface, onSurfaceVariant, outline, outlineVariant, inverseSurface, inverseOnSurface, scrim;
    // The names the screens were written with, each one of the roles.
    Color background;     // surface
    Color card;           // surfaceContainerHigh
    Color label;          // onSurface
    Color secondaryLabel; // onSurfaceVariant
    Color tertiaryLabel;  // outline
    Color separator;      // outlineVariant
    Color cardStroke;     // outlineVariant
    Color fill;           // surfaceContainerHighest
    Color accent;         // primary
    Color accentText;     // primary
    Color green, red, orange; // a good state, an error (the scheme's error), a warning
};

const Palette& palette();

/// Text sizes in effective pixels: Material 3's type scale (Android's sp, one to one), and the names the screens were
/// written with, each one of the scale's.
namespace font {
constexpr double displaySmall = 36, headlineMedium = 28, headlineSmall = 24, titleLarge = 22, titleMedium = 16, titleSmall = 14,
                 bodyLarge = 16, bodyMedium = 14, bodySmall = 12, labelLarge = 14, labelMedium = 12, labelSmall = 11;
constexpr double title2 = titleLarge, title3 = titleLarge, headline = titleMedium, body = bodyLarge, subheadline = bodyMedium,
                 footnote = bodySmall, caption = bodySmall, caption2 = labelSmall;
} // namespace font

/// The icons, by Segoe MDL2 Assets' code points: icon() draws Material's where it has its path, and glyph() the MDL2 glyph.
namespace glyphs {
constexpr wchar_t library = 0xE8F1, settings = 0xE713, device = 0xE8EA, log = 0xE9D9, warning = 0xE7BA, checkmark = 0xE73E,
                  trash = 0xE74D, add = 0xE710, play = 0xE768, chevronRight = 0xE76C, chevronLeft = 0xE76B, box = 0xE7B8,
                  wrench = 0xE90F, bug = 0xEBE8, refresh = 0xE72C, close = 0xE711, pause = 0xE769, game = 0xE7FC,
                  info = 0xE946, folder = 0xE8B7, contact = 0xE77B, send = 0xE724, heart = 0xEB51, back = 0xE72B, more = 0xE712,
                  search = 0xE721, download = 0xE896, openExternal = 0xE8A7, language = 0xE774, place = 0xE707,
                  renderSize = 0xE740, colors = 0xE790;
} // namespace glyphs

winrt::Windows::UI::Xaml::Media::SolidColorBrush brush(Color color);
Color withAlpha(Color color, double alpha);
Color rgb(unsigned value);

/// Loads the button styles (XamlReader, since there is no markup compiler) into the application's resources. Once, at
/// start-up.
void installStyles();

/// The buttons' shapes. Each has Material's state layer: the content's colour over the button at 8% under the pointer and
/// 12% while pressed; a disabled one at 38%.
///   plain    a rectangle: a list's row
///   rounded  corners of 12: a card
///   pill     a pill of 40 high: Material's buttons
///   circle   a circle: an icon button of 48 x 48
///   fab      corners of 16: the floating action button
enum class ButtonKind { plain, rounded, pill, circle, fab };
winrt::Windows::UI::Xaml::Style buttonStyle(ButtonKind kind);

// MARK: Builders

using winrt::Windows::UI::Text::FontWeight;
FontWeight weightNormal();
FontWeight weightMedium();
FontWeight weightSemibold(); // Material has none: Medium
FontWeight weightBold();

/// Roboto of that weight (Regular, Medium or Bold), with Segoe UI after it.
winrt::Windows::UI::Xaml::Media::FontFamily roboto(FontWeight weight = weightNormal());

winrt::Windows::UI::Xaml::Controls::TextBlock text(std::string_view content, double size,
                                                   FontWeight weight = weightNormal(),
                                                   std::optional<Color> color = std::nullopt);
/// A Segoe MDL2 Assets glyph as text.
winrt::Windows::UI::Xaml::Controls::TextBlock glyph(wchar_t code, double size,
                                                    std::optional<Color> color = std::nullopt);
/// The icon of that code point, `size` square: Material's where there is its path, else the MDL2 glyph. Without a colour
/// it takes the colour of what it is in, as text does (a button's foreground).
winrt::Windows::UI::Xaml::FrameworkElement icon(wchar_t code, double size, std::optional<Color> color = std::nullopt);
winrt::Windows::UI::Xaml::Controls::TextBlock monospace(std::string_view content, double size,
                                                        std::optional<Color> color = std::nullopt);
winrt::Windows::UI::Xaml::Controls::StackPanel vstack(double spacing);
winrt::Windows::UI::Xaml::Controls::StackPanel hstack(double spacing);
/// Material's filled card: the high surface container, corners of 12.
winrt::Windows::UI::Xaml::Controls::Border card(winrt::Windows::UI::Xaml::UIElement const& content,
                                                double cornerRadius = 12, double padding = 16);
/// Material's outlined card: the surface, a line of the outline variant around it.
winrt::Windows::UI::Xaml::Controls::Border outlinedCard(winrt::Windows::UI::Xaml::UIElement const& content, double padding = 16);
/// A divider: a line of the outline variant.
winrt::Windows::UI::Xaml::Controls::Border divider();
/// A button; `content` inherits the button's foreground unless it sets its own.
winrt::Windows::UI::Xaml::Controls::Button button(winrt::Windows::UI::Xaml::UIElement const& content,
                                                  std::function<void()> onClick, ButtonKind kind = ButtonKind::plain,
                                                  std::string_view accessibilityName = {});
/// Material's filled button: a pill of the primary colour (or `fill`), its label in the colour on it, an icon of 18
/// before it.
winrt::Windows::UI::Xaml::Controls::Button primaryButton(std::string_view title, wchar_t icon,
                                                         std::function<void()> onClick,
                                                         std::optional<Color> fill = std::nullopt);
/// Material's filled tonal button: the secondary container.
winrt::Windows::UI::Xaml::Controls::Button tonalButton(std::string_view title, wchar_t icon, std::function<void()> onClick);
/// Material's outlined button: a pill outlined, its label in the primary colour.
winrt::Windows::UI::Xaml::Controls::Button outlinedButton(std::string_view title, wchar_t icon, std::function<void()> onClick);
/// Material's text button: the label alone, in the primary colour (or `color`).
winrt::Windows::UI::Xaml::Controls::Button textButton(std::string_view title, std::function<void()> onClick,
                                                      std::optional<Color> color = std::nullopt);
/// Material's icon button: the icon of 24 in a circle of 48.
winrt::Windows::UI::Xaml::Controls::Button iconButton(wchar_t icon, std::function<void()> onClick, std::string_view accessibilityName,
                                                      std::optional<Color> color = std::nullopt);
/// Material's extended floating action button: the primary container, corners of 16, 56 high, an icon and a label.
winrt::Windows::UI::Xaml::Controls::Button fab(std::string_view title, wchar_t icon, std::function<void()> onClick);
/// An icon and a title side by side.
winrt::Windows::UI::Xaml::Controls::StackPanel label(std::string_view title, wchar_t icon, double size,
                                                     FontWeight weight = weightNormal(),
                                                     std::optional<Color> color = std::nullopt);

/// Material's filled text field: the highest surface container with its top corners rounded, a line under it (the primary
/// colour's, thicker, while it has the focus), and the placeholder while it is empty. `box` is set to its TextBox.
winrt::Windows::UI::Xaml::Controls::Grid textField(winrt::Windows::UI::Xaml::Controls::TextBox& box, std::string_view text,
                                                   std::string_view placeholder);
/// The same for a password: its characters not shown. `box` is set to its PasswordBox.
winrt::Windows::UI::Xaml::Controls::Grid passwordField(winrt::Windows::UI::Xaml::Controls::PasswordBox& box, std::string_view placeholder);

/// A list's subheader: the primary colour, Material's title small.
winrt::Windows::UI::Xaml::Controls::TextBlock sectionHeader(std::string_view title);

/// A list item of one line: its title, and its value at the end (the colour of supporting text).
winrt::Windows::UI::Xaml::Controls::Grid labeledContent(std::string_view title, std::string_view value,
                                                        bool monospacedValue = false);
/// A list item of two or three lines: its headline, its supporting text under it, and an icon before them (0: none).
winrt::Windows::UI::Xaml::Controls::Grid listItem(std::string_view headline, std::string_view supporting, wchar_t leading = 0);

/// A section of a list, as Material's: a subheader, the rows on the surface from edge to edge (the section reaches out
/// over the page's margins of 16 for it), and a line of supporting text under them.
winrt::Windows::UI::Xaml::Controls::StackPanel listSection(
    std::string_view header, std::vector<winrt::Windows::UI::Xaml::UIElement> const& rows,
    std::string_view footer = {});

/// A big icon, a headline and a description, centred: an empty screen.
winrt::Windows::UI::Xaml::Controls::StackPanel contentUnavailable(wchar_t icon, std::string_view title,
                                                                  std::string_view description = {});

/// A vertically scrolling page body, as wide as the phone and centred on anything wider.
winrt::Windows::UI::Xaml::Controls::ScrollViewer scrollingPage(winrt::Windows::UI::Xaml::UIElement const& content,
                                                               double maxWidth = 600);

/// Appends to a vstack/hstack, putting the stack's spacing before every child but the first (StackPanel.Spacing needs
/// 1803).
void add(winrt::Windows::UI::Xaml::Controls::StackPanel const& stack, winrt::Windows::UI::Xaml::UIElement const& child);

void setVisible(winrt::Windows::UI::Xaml::UIElement const& element, bool visible);

/// A scrolling list of monospace lines that grows at its end and keeps the newest in view: the log, and the device checks'
/// output. A ListView, so only the lines on screen cost anything, however many it holds.
class LineList {
public:
    explicit LineList(size_t capacity);
    winrt::Windows::UI::Xaml::Controls::ListView view() const { return list_; }
    void clear();
    void append(const std::vector<std::string>& lines);
    /// Whether append() scrolls to the newest line (it does at first).
    void setFollowing(bool following);
    bool following() const { return following_; }
    size_t size() const;

private:
    winrt::Windows::UI::Xaml::Controls::ListView list_;
    winrt::Windows::Foundation::Collections::IObservableVector<winrt::Windows::Foundation::IInspectable> items_{nullptr};
    size_t capacity_;
    bool following_ = true;
};

} // namespace megler::app::ui
