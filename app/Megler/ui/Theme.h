// Megler's look, built for XAML in code (there is no markup compiler in this
// build): one dark palette around the indigo of Megler's icon, and the builders
// every screen is made of. podium-w10m's ui/Theme, trimmed to what a dark,
// phone-first app needs.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace megler::app::ui {

using winrt::Windows::UI::Color;

struct Palette {
    Color background;
    Color card;
    Color label;
    Color secondaryLabel;
    Color tertiaryLabel;
    Color separator;  // white at 8%
    Color cardStroke; // white at 6%
    Color fill;       // white at 6%
    Color accent;     // Megler's indigo: fills
    Color accentText; // the same hue, light enough to read as text on black
    Color green, red, orange;
};

const Palette& palette();

/// Text sizes in effective pixels.
namespace font {
constexpr double title2 = 22, title3 = 20, headline = 17, body = 17, subheadline = 15, footnote = 13, caption = 12,
                 caption2 = 11;
}

/// Segoe MDL2 Assets glyphs.
namespace glyphs {
constexpr wchar_t library = 0xE8F1, settings = 0xE713, device = 0xE8EA, log = 0xE9D9, warning = 0xE7BA, checkmark = 0xE73E,
                  trash = 0xE74D, add = 0xE710, play = 0xE768, chevronRight = 0xE76C, chevronLeft = 0xE76B, box = 0xE7B8,
                  wrench = 0xE90F, bug = 0xEBE8, refresh = 0xE72C, close = 0xE711, pause = 0xE769, game = 0xE7FC,
                  info = 0xE946, folder = 0xE8B7;
}

winrt::Windows::UI::Xaml::Media::SolidColorBrush brush(Color color);
Color withAlpha(Color color, double alpha);
Color rgb(unsigned value);

/// Loads the button styles (XamlReader, since there is no markup compiler)
/// into the application's resources. Once, at start-up.
void installStyles();

enum class ButtonKind { plain, rounded, pill };
winrt::Windows::UI::Xaml::Style buttonStyle(ButtonKind kind);

// MARK: Builders

using winrt::Windows::UI::Text::FontWeight;
FontWeight weightNormal();
FontWeight weightMedium();
FontWeight weightSemibold();
FontWeight weightBold();

winrt::Windows::UI::Xaml::Controls::TextBlock text(std::string_view content, double size,
                                                   FontWeight weight = weightNormal(),
                                                   std::optional<Color> color = std::nullopt);
winrt::Windows::UI::Xaml::Controls::TextBlock glyph(wchar_t code, double size,
                                                    std::optional<Color> color = std::nullopt);
winrt::Windows::UI::Xaml::Controls::TextBlock monospace(std::string_view content, double size,
                                                        std::optional<Color> color = std::nullopt);
winrt::Windows::UI::Xaml::Controls::StackPanel vstack(double spacing);
winrt::Windows::UI::Xaml::Controls::StackPanel hstack(double spacing);
/// A rounded card with a hairline stroke.
winrt::Windows::UI::Xaml::Controls::Border card(winrt::Windows::UI::Xaml::UIElement const& content,
                                                double cornerRadius = 14, double padding = 16);
/// The cards' 1-pixel divider.
winrt::Windows::UI::Xaml::Controls::Border divider();
/// A button; `content` inherits the button's foreground unless it sets its own.
winrt::Windows::UI::Xaml::Controls::Button button(winrt::Windows::UI::Xaml::UIElement const& content,
                                                  std::function<void()> onClick, ButtonKind kind = ButtonKind::plain,
                                                  std::string_view accessibilityName = {});
/// Accent fill (or `fill`), white headline text, full width.
winrt::Windows::UI::Xaml::Controls::Button primaryButton(std::string_view title, wchar_t icon,
                                                         std::function<void()> onClick,
                                                         std::optional<Color> fill = std::nullopt);
/// A glyph and a title side by side.
winrt::Windows::UI::Xaml::Controls::StackPanel label(std::string_view title, wchar_t icon, double size,
                                                     FontWeight weight = weightNormal(),
                                                     std::optional<Color> color = std::nullopt);

/// Small caps, secondary.
winrt::Windows::UI::Xaml::Controls::TextBlock sectionHeader(std::string_view title);

/// A list row: title left, value right (secondary).
winrt::Windows::UI::Xaml::Controls::Grid labeledContent(std::string_view title, std::string_view value,
                                                        bool monospacedValue = false);

/// A section of a grouped list: header, rounded card of rows with
/// separators, optional footer.
winrt::Windows::UI::Xaml::Controls::StackPanel listSection(
    std::string_view header, std::vector<winrt::Windows::UI::Xaml::UIElement> const& rows,
    std::string_view footer = {});

/// A big glyph, a title and a description, centred: an empty screen.
winrt::Windows::UI::Xaml::Controls::StackPanel contentUnavailable(wchar_t icon, std::string_view title,
                                                                  std::string_view description = {});

/// A vertically scrolling page body, as wide as the phone and centred on
/// anything wider.
winrt::Windows::UI::Xaml::Controls::ScrollViewer scrollingPage(winrt::Windows::UI::Xaml::UIElement const& content,
                                                               double maxWidth = 600);

/// Appends to a vstack/hstack, putting the stack's spacing before every
/// child but the first (StackPanel.Spacing needs 1803).
void add(winrt::Windows::UI::Xaml::Controls::StackPanel const& stack, winrt::Windows::UI::Xaml::UIElement const& child);

void setVisible(winrt::Windows::UI::Xaml::UIElement const& element, bool visible);

/// A scrolling list of monospace lines that grows at its end and keeps the
/// newest in view: the log, and the device checks' output. A ListView, so
/// only the lines on screen cost anything, however many it holds.
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
