#include "pch.h"

#include "ui/Theme.h"

#include "shell/ShellLog.h"
#include "shell/Strings.h"

namespace meglet::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::Text::FontWeights;

namespace {

Palette makePalette() {
    Palette p;
    p.background = rgb(0x000000);
    p.card = rgb(0x1C1C1E);
    p.label = rgb(0xFFFFFF);
    p.secondaryLabel = rgb(0x98989F);
    p.tertiaryLabel = rgb(0x5A5A5F);
    p.separator = withAlpha(rgb(0xFFFFFF), 0.08);
    p.cardStroke = withAlpha(rgb(0xFFFFFF), 0.06);
    p.fill = withAlpha(rgb(0xFFFFFF), 0.06);
    p.accent = rgb(0x5A52F1);
    p.accentText = rgb(0x9A94FF);
    p.green = rgb(0x30D158);
    p.red = rgb(0xFF453A);
    p.orange = rgb(0xFF9F0A);
    return p;
}

// The three button templates: content over a (rounded) background, dimmed
// when pressed and when disabled.
const wchar_t* styleTemplate = LR"(
<ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
  <Style x:Key="MegletButton%NAME%" TargetType="Button">
    <Setter Property="Background" Value="Transparent"/>
    <Setter Property="BorderThickness" Value="0"/>
    <Setter Property="Padding" Value="0"/>
    <Setter Property="HorizontalAlignment" Value="Stretch"/>
    <Setter Property="HorizontalContentAlignment" Value="Stretch"/>
    <Setter Property="VerticalContentAlignment" Value="Center"/>
    <Setter Property="UseSystemFocusVisuals" Value="True"/>
    <Setter Property="Template">
      <Setter.Value>
        <ControlTemplate TargetType="Button">
          <Border x:Name="Root" Background="{TemplateBinding Background}"
                  BorderBrush="{TemplateBinding BorderBrush}" BorderThickness="{TemplateBinding BorderThickness}"
                  CornerRadius="%RADIUS%">
            <VisualStateManager.VisualStateGroups>
              <VisualStateGroup x:Name="CommonStates">
                <VisualState x:Name="Normal"/>
                <VisualState x:Name="PointerOver"/>
                <VisualState x:Name="Pressed">
                  <VisualState.Setters><Setter Target="Root.Opacity" Value="0.6"/></VisualState.Setters>
                </VisualState>
                <VisualState x:Name="Disabled">
                  <VisualState.Setters><Setter Target="Root.Opacity" Value="0.4"/></VisualState.Setters>
                </VisualState>
              </VisualStateGroup>
            </VisualStateManager.VisualStateGroups>
            <ContentPresenter Content="{TemplateBinding Content}" ContentTemplate="{TemplateBinding ContentTemplate}"
                              Padding="{TemplateBinding Padding}" Foreground="{TemplateBinding Foreground}"
                              HorizontalContentAlignment="{TemplateBinding HorizontalContentAlignment}"
                              VerticalContentAlignment="{TemplateBinding VerticalContentAlignment}"/>
          </Border>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
  </Style>
</ResourceDictionary>)";

// A line of a LineList, and the row around it: no minimum height, no
// selection chrome -- a page of text, not a list of things to tap.
const wchar_t* lineTemplate = LR"(
<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
  <TextBlock Text="{Binding}" FontFamily="Consolas" FontSize="11" TextWrapping="Wrap" Foreground="#FFC7C7CC"/>
</DataTemplate>)";

const wchar_t* lineContainerStyle = LR"(
<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ListViewItem">
  <Setter Property="MinHeight" Value="0"/>
  <Setter Property="Padding" Value="12,1,12,1"/>
  <Setter Property="HorizontalContentAlignment" Value="Stretch"/>
  <Setter Property="IsTabStop" Value="False"/>
</Style>)";

std::wstring replaceAll(std::wstring s, const std::wstring& what, const std::wstring& with) {
    for (size_t at = s.find(what); at != std::wstring::npos; at = s.find(what, at + with.size())) s.replace(at, what.size(), with);
    return s;
}

const wchar_t* styleKey(ButtonKind kind) {
    switch (kind) {
    case ButtonKind::plain: return L"MegletButtonPlain";
    case ButtonKind::rounded: return L"MegletButtonRounded";
    case ButtonKind::pill: return L"MegletButtonPill";
    }
    return L"MegletButtonPlain";
}

} // namespace

const Palette& palette() {
    static const Palette p = makePalette();
    return p;
}

SolidColorBrush brush(Color color) { return SolidColorBrush(color); }

Color withAlpha(Color color, double alpha) {
    color.A = uint8_t(std::max(0.0, std::min(1.0, alpha)) * 255 + 0.5);
    return color;
}

Color rgb(unsigned value) {
    return Color{255, uint8_t((value >> 16) & 0xFF), uint8_t((value >> 8) & 0xFF), uint8_t(value & 0xFF)};
}

void installStyles() {
    auto resources = Application::Current().Resources();
    const std::pair<const wchar_t*, const wchar_t*> kinds[] = {{L"Plain", L"0"}, {L"Rounded", L"12"}, {L"Pill", L"24"}};
    for (auto [name, radius] : kinds) {
        std::wstring xaml = replaceAll(replaceAll(styleTemplate, L"%NAME%", name), L"%RADIUS%", radius);
        try {
            auto dictionary = Markup::XamlReader::Load(xaml).as<ResourceDictionary>();
            resources.MergedDictionaries().Append(dictionary);
        } catch (const hresult_error& e) {
            shellLog::write("ui: couldn't load the button style " + narrow(name) + ": " + narrow(e.message()));
        }
    }
}

Style buttonStyle(ButtonKind kind) {
    auto resources = Application::Current().Resources();
    auto key = box_value(styleKey(kind));
    if (resources.HasKey(key)) return resources.Lookup(key).as<Style>();
    for (auto const& merged : resources.MergedDictionaries())
        if (merged.HasKey(key)) return merged.Lookup(key).as<Style>();
    return nullptr;
}

FontWeight weightNormal() { return FontWeights::Normal(); }
FontWeight weightMedium() { return FontWeights::Medium(); }
FontWeight weightSemibold() { return FontWeights::SemiBold(); }
FontWeight weightBold() { return FontWeights::Bold(); }

TextBlock text(std::string_view content, double size, FontWeight weight, std::optional<Color> color) {
    TextBlock t;
    t.Text(hs(content));
    t.FontSize(size);
    t.FontWeight(weight);
    t.TextWrapping(TextWrapping::Wrap);
    if (color) t.Foreground(brush(*color));
    return t;
}

TextBlock glyph(wchar_t code, double size, std::optional<Color> color) {
    TextBlock t;
    t.Text(hstring(std::wstring(1, code)));
    t.FontFamily(FontFamily(L"Segoe MDL2 Assets"));
    t.FontSize(size);
    t.VerticalAlignment(VerticalAlignment::Center);
    if (color) t.Foreground(brush(*color));
    return t;
}

TextBlock monospace(std::string_view content, double size, std::optional<Color> color) {
    TextBlock t = text(content, size, weightNormal(), color);
    t.FontFamily(FontFamily(L"Consolas"));
    t.IsTextSelectionEnabled(true);
    return t;
}

StackPanel vstack(double spacing) {
    StackPanel s;
    s.Orientation(Orientation::Vertical);
    // StackPanel.Spacing arrived in 1803; margins do the job on 1703 (add()).
    s.Tag(box_value(spacing));
    return s;
}

StackPanel hstack(double spacing) {
    StackPanel s;
    s.Orientation(Orientation::Horizontal);
    s.Tag(box_value(spacing));
    return s;
}

Border card(UIElement const& content, double cornerRadius, double padding) {
    Border b;
    b.Child(content);
    b.CornerRadius(CornerRadiusHelper::FromUniformRadius(cornerRadius));
    b.Padding(ThicknessHelper::FromUniformLength(padding));
    b.Background(brush(palette().card));
    b.BorderBrush(brush(palette().cardStroke));
    b.BorderThickness(ThicknessHelper::FromUniformLength(1));
    return b;
}

Border divider() {
    Border b;
    b.Height(1);
    b.Background(brush(palette().separator));
    b.HorizontalAlignment(HorizontalAlignment::Stretch);
    return b;
}

Button button(UIElement const& content, std::function<void()> onClick, ButtonKind kind, std::string_view name) {
    Button b;
    if (auto style = buttonStyle(kind)) b.Style(style);
    b.Content(content);
    b.Foreground(brush(palette().label));
    b.HorizontalAlignment(HorizontalAlignment::Stretch);
    b.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    if (!name.empty()) Automation::AutomationProperties::SetName(b, hs(name));
    b.Click([onClick](auto&&, auto&&) {
        if (onClick) onClick();
    });
    return b;
}

Button primaryButton(std::string_view title, wchar_t icon, std::function<void()> onClick, std::optional<Color> fill) {
    auto content = hstack(8);
    content.HorizontalAlignment(HorizontalAlignment::Center);
    add(content, glyph(icon, font::headline));
    add(content, text(title, font::headline, weightSemibold()));
    Button b = button(content, std::move(onClick), ButtonKind::rounded, title);
    b.Background(brush(fill ? *fill : palette().accent));
    b.Foreground(brush(rgb(0xFFFFFF)));
    b.Padding(ThicknessHelper::FromLengths(16, 14, 16, 14));
    return b;
}

StackPanel label(std::string_view title, wchar_t icon, double size, FontWeight weight, std::optional<Color> color) {
    auto s = hstack(8);
    add(s, glyph(icon, size, color));
    auto t = text(title, size, weight, color);
    t.VerticalAlignment(VerticalAlignment::Center);
    add(s, t);
    return s;
}

TextBlock sectionHeader(std::string_view title) {
    std::string upper(title);
    for (char& ch : upper) ch = char(toupper(static_cast<unsigned char>(ch)));
    auto t = text(upper, font::caption, weightSemibold(), palette().secondaryLabel);
    t.CharacterSpacing(40);
    return t;
}

Grid labeledContent(std::string_view title, std::string_view value, bool monospacedValue) {
    Grid g;
    ColumnDefinition c0, c1;
    c0.Width(GridLengthHelper::Auto());
    c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    g.ColumnDefinitions().Append(c0);
    g.ColumnDefinitions().Append(c1);
    auto left = text(title, font::body, weightNormal(), palette().label);
    left.TextWrapping(TextWrapping::NoWrap);
    left.Margin(ThicknessHelper::FromLengths(0, 0, 12, 0));
    auto right = monospacedValue ? monospace(value, font::subheadline, palette().secondaryLabel)
                                 : text(value, font::body, weightNormal(), palette().secondaryLabel);
    right.TextAlignment(TextAlignment::Right);
    right.HorizontalAlignment(HorizontalAlignment::Right);
    right.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(right, 1);
    g.Children().Append(left);
    g.Children().Append(right);
    g.Padding(ThicknessHelper::FromLengths(16, 11, 16, 11));
    return g;
}

StackPanel listSection(std::string_view header, std::vector<UIElement> const& rows, std::string_view footer) {
    auto section = vstack(0);
    if (!header.empty()) {
        auto h = sectionHeader(header);
        h.Margin(ThicknessHelper::FromLengths(16, 0, 16, 7));
        section.Children().Append(h);
    }
    auto list = vstack(0);
    for (size_t i = 0; i < rows.size(); ++i) {
        if (i > 0) {
            auto d = divider();
            d.Margin(ThicknessHelper::FromLengths(16, 0, 0, 0));
            list.Children().Append(d);
        }
        list.Children().Append(rows[i]);
    }
    Border b;
    b.Child(list);
    b.CornerRadius(CornerRadiusHelper::FromUniformRadius(10));
    b.Background(brush(palette().card));
    section.Children().Append(b);
    if (!footer.empty()) {
        auto f = text(footer, font::footnote, weightNormal(), palette().secondaryLabel);
        f.Margin(ThicknessHelper::FromLengths(16, 7, 16, 0));
        section.Children().Append(f);
    }
    section.Margin(ThicknessHelper::FromLengths(0, 0, 0, 26));
    return section;
}

StackPanel contentUnavailable(wchar_t icon, std::string_view title, std::string_view description) {
    auto s = vstack(10);
    s.HorizontalAlignment(HorizontalAlignment::Center);
    auto g = glyph(icon, 44, palette().secondaryLabel);
    g.HorizontalAlignment(HorizontalAlignment::Center);
    g.Margin(ThicknessHelper::FromLengths(0, 0, 0, 8));
    add(s, g);
    auto t = text(title, font::title3, weightBold(), palette().label);
    t.TextAlignment(TextAlignment::Center);
    t.HorizontalAlignment(HorizontalAlignment::Center);
    add(s, t);
    if (!description.empty()) {
        auto d = text(description, font::subheadline, weightNormal(), palette().secondaryLabel);
        d.TextAlignment(TextAlignment::Center);
        d.Margin(ThicknessHelper::FromLengths(0, 6, 0, 0));
        s.Children().Append(d);
    }
    return s;
}

ScrollViewer scrollingPage(UIElement const& content, double maxWidth) {
    ScrollViewer sv;
    sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    sv.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    sv.Background(brush(palette().background));
    if (auto fe = content.try_as<FrameworkElement>()) {
        fe.MaxWidth(maxWidth);
        fe.HorizontalAlignment(HorizontalAlignment::Stretch);
    }
    // Stretch with a MaxWidth: as wide as the phone, centred on anything wider.
    Grid centre;
    centre.Children().Append(content);
    sv.Content(centre);
    return sv;
}

void add(StackPanel const& stack, UIElement const& child) {
    double spacing = 0;
    if (auto tag = stack.Tag()) spacing = unbox_value_or<double>(tag, 0);
    if (spacing > 0 && stack.Children().Size() > 0) {
        if (auto fe = child.try_as<FrameworkElement>()) {
            Thickness m = fe.Margin();
            if (stack.Orientation() == Orientation::Vertical)
                m.Top += spacing;
            else
                m.Left += spacing;
            fe.Margin(m);
        }
    }
    stack.Children().Append(child);
}

void setVisible(UIElement const& element, bool visible) {
    if (element) element.Visibility(visible ? Visibility::Visible : Visibility::Collapsed);
}

// MARK: LineList

LineList::LineList(size_t capacity) : capacity_(capacity) {
    list_.SelectionMode(ListViewSelectionMode::None);
    list_.IsItemClickEnabled(false);
    list_.Background(brush(palette().background));
    try {
        list_.ItemTemplate(Markup::XamlReader::Load(lineTemplate).as<DataTemplate>());
        list_.ItemContainerStyle(Markup::XamlReader::Load(lineContainerStyle).as<Style>());
    } catch (const hresult_error& e) {
        shellLog::write("ui: couldn't load the line list's templates: " + narrow(e.message()));
    }
    clear();
}

void LineList::clear() {
    items_ = single_threaded_observable_vector<IInspectable>();
    list_.ItemsSource(items_);
}

size_t LineList::size() const { return items_.Size(); }

void LineList::append(const std::vector<std::string>& lines) {
    if (lines.empty()) return;
    if (lines.size() > 48) {
        // A page of lines at once (the screen just opened on a log already
        // long): one new list instead of a change notification per line.
        std::vector<IInspectable> all;
        all.reserve(items_.Size() + lines.size());
        for (auto const& item : items_) all.push_back(item);
        for (const auto& line : lines) all.push_back(box_value(hs(line)));
        if (all.size() > capacity_) all.erase(all.begin(), all.begin() + ptrdiff_t(all.size() - capacity_));
        items_ = single_threaded_observable_vector<IInspectable>(std::move(all));
        list_.ItemsSource(items_);
        // A list that has not been laid out yet has nowhere to scroll to.
        if (following_) list_.UpdateLayout();
    } else {
        for (const auto& line : lines) items_.Append(box_value(hs(line)));
        while (items_.Size() > capacity_) items_.RemoveAt(0);
    }
    if (following_ && items_.Size() > 0) {
        try {
            list_.ScrollIntoView(items_.GetAt(items_.Size() - 1));
        } catch (const hresult_error&) {
        }
    }
}

void LineList::setFollowing(bool following) {
    following_ = following;
    if (following_ && items_.Size() > 0) {
        try {
            list_.ScrollIntoView(items_.GetAt(items_.Size() - 1));
        } catch (const hresult_error&) {
        }
    }
}

} // namespace meglet::app::ui
