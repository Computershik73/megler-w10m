#include "pch.h"

#include "ui/Theme.h"

#include "player/PhoneState.h"
#include "shell/Files.h"
#include "shell/Platform.h"
#include "shell/ShellLog.h"
#include "shell/Strings.h"

#include "megler.h"

#include <map>

namespace megler::app::ui {

using namespace winrt;
using namespace winrt::Windows::UI::Xaml;
using namespace winrt::Windows::UI::Xaml::Controls;
using namespace winrt::Windows::UI::Xaml::Media;
using winrt::Windows::Foundation::IInspectable;
using winrt::Windows::UI::Text::FontWeights;

namespace {

Color argb(uint32_t v) { return Color{uint8_t(v >> 24), uint8_t((v >> 16) & 0xFF), uint8_t((v >> 8) & 0xFF), uint8_t(v & 0xFF)}; }

/// Material 3's colour scheme of the accent: its roles as tones of the five palettes, the dark scheme's or the light's.
Palette makePalette() {
    const uint32_t seed = phoneState::accent();
    const bool dark = phoneState::dark();
    auto t = [seed](int palette, int tone) { return argb(megler_palette_tone(seed, palette, tone)); };
    constexpr int N1 = MEGLER_PALETTE_NEUTRAL1, N2 = MEGLER_PALETTE_NEUTRAL2, A1 = MEGLER_PALETTE_ACCENT1, A2 = MEGLER_PALETTE_ACCENT2,
                  A3 = MEGLER_PALETTE_ACCENT3, E = MEGLER_PALETTE_ERROR;
    Palette p;
    p.dark = dark;
    if (dark) {
        p.primary = t(A1, 80), p.onPrimary = t(A1, 20), p.primaryContainer = t(A1, 30), p.onPrimaryContainer = t(A1, 90);
        p.secondaryContainer = t(A2, 30), p.onSecondaryContainer = t(A2, 90);
        p.tertiaryContainer = t(A3, 30), p.onTertiaryContainer = t(A3, 90);
        p.error = t(E, 80), p.onError = t(E, 20), p.errorContainer = t(E, 30), p.onErrorContainer = t(E, 90);
        p.surface = t(N1, 6), p.surfaceContainerLow = t(N1, 10), p.surfaceContainer = t(N1, 12), p.surfaceContainerHigh = t(N1, 17),
        p.surfaceContainerHighest = t(N1, 22);
        p.onSurface = t(N1, 90), p.onSurfaceVariant = t(N2, 80), p.outline = t(N2, 60), p.outlineVariant = t(N2, 30);
        p.inverseSurface = t(N1, 90), p.inverseOnSurface = t(N1, 20);
        p.green = rgb(0x81C995), p.orange = rgb(0xFDD663);
    } else {
        p.primary = t(A1, 40), p.onPrimary = t(A1, 100), p.primaryContainer = t(A1, 90), p.onPrimaryContainer = t(A1, 10);
        p.secondaryContainer = t(A2, 90), p.onSecondaryContainer = t(A2, 10);
        p.tertiaryContainer = t(A3, 90), p.onTertiaryContainer = t(A3, 10);
        p.error = t(E, 40), p.onError = t(E, 100), p.errorContainer = t(E, 90), p.onErrorContainer = t(E, 10);
        p.surface = t(N1, 98), p.surfaceContainerLow = t(N1, 96), p.surfaceContainer = t(N1, 94), p.surfaceContainerHigh = t(N1, 92),
        p.surfaceContainerHighest = t(N1, 90);
        p.onSurface = t(N1, 10), p.onSurfaceVariant = t(N2, 30), p.outline = t(N2, 50), p.outlineVariant = t(N2, 80);
        p.inverseSurface = t(N1, 20), p.inverseOnSurface = t(N1, 95);
        p.green = rgb(0x146C2E), p.orange = rgb(0x8C5000);
    }
    p.scrim = withAlpha(rgb(0x000000), 0.32);
    p.background = p.surface;
    p.card = p.surfaceContainerHigh;
    p.label = p.onSurface;
    p.secondaryLabel = p.onSurfaceVariant;
    p.tertiaryLabel = p.outline;
    p.separator = p.outlineVariant;
    p.cardStroke = p.outlineVariant;
    p.fill = p.surfaceContainerHighest;
    p.accent = p.primary;
    p.accentText = p.primary;
    p.red = p.error;
    shellLog::write(format("ui: Material 3's %s scheme of the accent #%06X: primary #%06X, surface #%06X", dark ? "dark" : "light",
                           unsigned(seed & 0xFFFFFF), unsigned((p.primary.R << 16) | (p.primary.G << 8) | p.primary.B),
                           unsigned((p.surface.R << 16) | (p.surface.G << 8) | p.surface.B)));
    return p;
}

// The buttons' template: the content over a background, with Material's state layer -- the content's own colour, at 8%
// under the pointer and 12% while pressed, in the button's shape -- and the whole at 38% when disabled.
const wchar_t* styleTemplate = LR"(
<ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
  <Style x:Key="MeglerButton%NAME%" TargetType="Button">
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
                <VisualState x:Name="PointerOver">
                  <VisualState.Setters><Setter Target="StateLayer.Opacity" Value="0.08"/></VisualState.Setters>
                </VisualState>
                <VisualState x:Name="Pressed">
                  <VisualState.Setters><Setter Target="StateLayer.Opacity" Value="0.12"/></VisualState.Setters>
                </VisualState>
                <VisualState x:Name="Disabled">
                  <VisualState.Setters><Setter Target="Root.Opacity" Value="0.38"/></VisualState.Setters>
                </VisualState>
              </VisualStateGroup>
            </VisualStateManager.VisualStateGroups>
            <Grid>
              <Border x:Name="StateLayer" Background="{TemplateBinding Foreground}" Opacity="0" CornerRadius="%RADIUS%"/>
              <ContentPresenter Content="{TemplateBinding Content}" ContentTemplate="{TemplateBinding ContentTemplate}"
                                Padding="{TemplateBinding Padding}" Foreground="{TemplateBinding Foreground}"
                                HorizontalContentAlignment="{TemplateBinding HorizontalContentAlignment}"
                                VerticalContentAlignment="{TemplateBinding VerticalContentAlignment}"/>
            </Grid>
          </Border>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
  </Style>
</ResourceDictionary>)";

// A text box with none of the system's own chrome (whose focused field is white in the dark theme): its text alone, the
// field around it being textField's. A password box likewise (passwordField).
const wchar_t* textBoxStyle = LR"(
<ResourceDictionary xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
                    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml">
  <Style x:Key="MeglerTextBox" TargetType="TextBox">
    <Setter Property="Template">
      <Setter.Value>
        <ControlTemplate TargetType="TextBox">
          <ScrollViewer x:Name="ContentElement" Margin="{TemplateBinding Padding}" Foreground="{TemplateBinding Foreground}"
                        HorizontalScrollMode="Auto" VerticalScrollMode="Auto" HorizontalScrollBarVisibility="Hidden"
                        VerticalScrollBarVisibility="Hidden" IsTabStop="False" ZoomMode="Disabled"
                        AutomationProperties.AccessibilityView="Raw"/>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
  </Style>
  <Style x:Key="MeglerPasswordBox" TargetType="PasswordBox">
    <Setter Property="Template">
      <Setter.Value>
        <ControlTemplate TargetType="PasswordBox">
          <ScrollViewer x:Name="ContentElement" Margin="{TemplateBinding Padding}" Foreground="{TemplateBinding Foreground}"
                        HorizontalScrollMode="Auto" VerticalScrollMode="Auto" HorizontalScrollBarVisibility="Hidden"
                        VerticalScrollBarVisibility="Hidden" IsTabStop="False" ZoomMode="Disabled"
                        AutomationProperties.AccessibilityView="Raw"/>
        </ControlTemplate>
      </Setter.Value>
    </Setter>
  </Style>
</ResourceDictionary>)";

// A line of a LineList, and the row around it: no minimum height, no selection chrome -- a page of text, not a list of
// things to tap. (%COLOR% is the scheme's text colour.)
const wchar_t* lineTemplate = LR"(
<DataTemplate xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation">
  <TextBlock Text="{Binding}" FontFamily="%MONO%" FontSize="11" TextWrapping="Wrap" Foreground="%COLOR%"/>
</DataTemplate>)";

const wchar_t* lineContainerStyle = LR"(
<Style xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation" TargetType="ListViewItem">
  <Setter Property="MinHeight" Value="0"/>
  <Setter Property="Padding" Value="16,1,16,1"/>
  <Setter Property="HorizontalContentAlignment" Value="Stretch"/>
  <Setter Property="IsTabStop" Value="False"/>
</Style>)";

/*
 * The monospaced face of the log's lines and of code, from a copy in LocalState, not from Megler's package: what XAML's
 * text maps of a font file stays mapped, and on an Xbox a package Megler made then could not read the same file of Megler's
 * own ("reading ...\fonts\DroidSansMono.ttf failed"), and the same "invalid cross-partition private file/section access"
 * ended Megler as an unhandled XAML exception (0.1.0.681, Google Play services' package made while the install's lines were
 * shown). The package's own file where the copy cannot be made.
 */
const std::wstring& monoFamilyText() {
    static const std::wstring family = [] {
        const std::wstring local = platform::localStateDirectory() + L"\\fonts", copy = local + L"\\DroidSansMono.ttf";
        const std::wstring original = platform::installDirectory() + L"\\fonts\\DroidSansMono.ttf";
        if (files::size(copy) == 0 || files::size(copy) != files::size(original)) {
            files::makeDirectory(local);
            if (!files::copy(original, copy + L".part") || !files::move(copy + L".part", copy)) {
                shellLog::write("ui: the monospaced font could not be copied to LocalState: its package's file is read");
                return std::wstring(L"ms-appx:///fonts/DroidSansMono.ttf#Droid Sans Mono, Consolas");
            }
        }
        return std::wstring(L"ms-appdata:///local/fonts/DroidSansMono.ttf#Droid Sans Mono, Consolas");
    }();
    return family;
}

std::wstring replaceAll(std::wstring s, const std::wstring& what, const std::wstring& with) {
    for (size_t at = s.find(what); at != std::wstring::npos; at = s.find(what, at + with.size())) s.replace(at, what.size(), with);
    return s;
}

std::wstring hexColor(Color c) {
    wchar_t buf[16];
    swprintf(buf, 16, L"#%02X%02X%02X%02X", c.A, c.R, c.G, c.B);
    return buf;
}

const wchar_t* styleKey(ButtonKind kind) {
    switch (kind) {
    case ButtonKind::plain: return L"MeglerButtonPlain";
    case ButtonKind::rounded: return L"MeglerButtonRounded";
    case ButtonKind::pill: return L"MeglerButtonPill";
    case ButtonKind::circle: return L"MeglerButtonCircle";
    case ButtonKind::fab: return L"MeglerButtonFab";
    }
    return L"MeglerButtonPlain";
}

// Google's Material icons (github.com/google/material-design-icons, Apache License 2.0): each icon's path in its square
// of 24, by the Segoe MDL2 code point it stands in for.
const std::map<wchar_t, const wchar_t*>& materialPaths() {
    static const std::map<wchar_t, const wchar_t*> paths = {
        {glyphs::back, L"M20 11H7.83l5.59-5.59L12 4l-8 8 8 8 1.41-1.41L7.83 13H20v-2z"},
        {glyphs::chevronLeft, L"M20 11H7.83l5.59-5.59L12 4l-8 8 8 8 1.41-1.41L7.83 13H20v-2z"},
        {glyphs::close, L"M19 6.41L17.59 5 12 10.59 6.41 5 5 6.41 10.59 12 5 17.59 6.41 19 12 13.41 17.59 19 19 17.59 13.41 12z"},
        {glyphs::more, L"M12 8c1.1 0 2-.9 2-2s-.9-2-2-2-2 .9-2 2 .9 2 2 2zm0 2c-1.1 0-2 .9-2 2s.9 2 2 2 2-.9 2-2-.9-2-2-2zm0 6c-1.1 0-2 "
                       L".9-2 2s.9 2 2 2 2-.9 2-2-.9-2-2-2z"},
        {glyphs::add, L"M19 13h-6v6h-2v-6H5v-2h6V5h2v6h6v2z"},
        {glyphs::trash, L"M16 9v10H8V9h8m-1.5-6h-5l-1 1H5v2h14V4h-3.5l-1-1zM18 7H6v12c0 1.1.9 2 2 2h8c1.1 0 2-.9 2-2V7z"},
        {glyphs::play, L"M8 5v14l11-7z"},
        {glyphs::pause, L"M6 19h4V5H6v14zm8-14v14h4V5h-4z"},
        {glyphs::settings,
         L"M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58c.18-.14.23-.41.12-.61l-1.92-3.32c-.12-.22-.37-.29-.59-.22l"
         L"-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54c-.04-.24-.24-.41-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62"
         L".94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.09.63-.09.94s.02.64.07.94l-2.03 1.58c-.18"
         L".14-.23.41-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47"
         L"-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6c-1.98 "
         L"0-3.6-1.62-3.6-3.6s1.62-3.6 3.6-3.6 3.6 1.62 3.6 3.6-1.62 3.6-3.6 3.6z"},
        {glyphs::library, L"M4 8h4V4H4v4zm6 12h4v-4h-4v4zm-6 0h4v-4H4v4zm0-6h4v-4H4v4zm6 0h4v-4h-4v4zm6-10v4h4V4h-4zm-6 4h4V4h-4v4zm6 6h4v-4h"
                          L"-4v4zm0 6h4v-4h-4v4z"},
        {glyphs::device, L"M17 1.01L7 1c-1.1 0-2 .9-2 2v18c0 1.1.9 2 2 2h10c1.1 0 2-.9 2-2V3c0-1.1-.9-1.99-2-1.99zM17 19H7V5h10v14z"},
        {glyphs::log, L"M19 3H5c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h14c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm-5 14H7v-2h7v2zm3-4H7v-2h10v2zm0-4H7V7h"
                      L"10v2z"},
        {glyphs::folder, L"M10 4H4c-1.1 0-1.99.9-1.99 2L2 18c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2h-8l-2-2z"},
        {glyphs::info, L"M11 7h2v2h-2zm0 4h2v6h-2zm1-9C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm0 18c-4.41 0-8-3.59-8"
                       L"-8s3.59-8 8-8 8 3.59 8 8-3.59 8-8 8z"},
        {glyphs::warning, L"M1 21h22L12 2 1 21zm12-3h-2v-2h2v2zm0-4h-2v-4h2v4z"},
        {glyphs::checkmark, L"M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z"},
        {glyphs::refresh, L"M17.65 6.35C16.2 4.9 14.21 4 12 4c-4.42 0-7.99 3.58-7.99 8s3.57 8 7.99 8c3.73 0 6.84-2.55 7.73-6h-2.08c-.82 "
                          L"2.33-3.04 4-5.65 4-3.31 0-6-2.69-6-6s2.69-6 6-6c1.66 0 3.14.69 4.22 1.78L13 11h7V4l-2.35 2.35z"},
        {glyphs::wrench, L"M22.7 19l-9.1-9.1c.9-2.3.4-5-1.5-6.9-2-2-5-2.4-7.4-1.3L9 6 6 9 1.6 4.7C.4 7.1.9 10.1 2.9 12.1c1.9 1.9 4.6 2.4 "
                         L"6.9 1.5l9.1 9.1c.4.4 1 .4 1.4 0l2.3-2.3c.5-.4.5-1.1.1-1.4z"},
        {glyphs::chevronRight, L"M10 6L8.59 7.41 13.17 12l-4.58 4.59L10 18l6-6z"},
        {glyphs::game, L"M21 6H3c-1.1 0-2 .9-2 2v8c0 1.1.9 2 2 2h18c1.1 0 2-.9 2-2V8c0-1.1-.9-2-2-2zm-10 7H8v3H6v-3H3v-2h3V8h2v3h3v2zm4.5 "
                       L"2c-.83 0-1.5-.67-1.5-1.5s.67-1.5 1.5-1.5 1.5.67 1.5 1.5-.67 1.5-1.5 1.5zm4-3c-.83 0-1.5-.67-1.5-1.5S18.67 9 19.5 9s1.5"
                       L".67 1.5 1.5-.67 1.5-1.5 1.5z"},
        {glyphs::contact, L"M12 12c2.21 0 4-1.79 4-4s-1.79-4-4-4-4 1.79-4 4 1.79 4 4 4zm0 2c-2.67 0-8 1.34-8 4v2h16v-2c0-2.66-5.33-4-8-4z"},
        {glyphs::send, L"M2.01 21L23 12 2.01 3 2 10l15 2-15 2z"},
        {glyphs::heart, L"M12 21.35l-1.45-1.32C5.4 15.36 2 12.28 2 8.5 2 5.42 4.42 3 7.5 3c1.74 0 3.41.81 4.5 2.09C13.09 3.81 14.76 3 16.5 "
                        L"3 19.58 3 22 5.42 22 8.5c0 3.78-3.4 6.86-8.55 11.54L12 21.35z"},
        {glyphs::bug, L"M20 8h-2.81c-.45-.78-1.07-1.45-1.82-1.96L17 4.41 15.59 3l-2.17 2.17C12.96 5.06 12.49 5 12 5c-.49 0-.96.06-1.41.17L8"
                      L".41 3 7 4.41l1.62 1.63C7.88 6.55 7.26 7.22 6.81 8H4v2h2.09c-.05.33-.09.66-.09 1v1H4v2h2v1c0 .34.04.67.09 1H4v2h2.81c1"
                      L".04 1.79 2.97 3 5.19 3s4.15-1.21 5.19-3H20v-2h-2.09c.05-.33.09-.66.09-1v-1h2v-2h-2v-1c0-.34-.04-.67-.09-1H20V8zm-6 8h"
                      L"-4v-2h4v2zm0-4h-4v-2h4v2z"},
        {glyphs::download, L"M19 9h-4V3H9v6H5l7 7 7-7zM5 18v2h14v-2H5z"},
        {glyphs::openExternal, L"M19 19H5V5h7V3H5c-1.11 0-2 .9-2 2v14c0 1.1.89 2 2 2h14c1.1 0 2-.9 2-2v-7h-2v7zM14 3v2h3.59l-9.83 9.83 1"
                               L".41 1.41L19 6.41V10h2V3h-7z"},
        {glyphs::search, L"M15.5 14h-.79l-.28-.27C15.41 12.59 16 11.11 16 9.5 16 5.91 13.09 3 9.5 3S3 5.91 3 9.5 5.91 16 9.5 16c1.61 0 3"
                         L".09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11"
                         L".99 14 9.5 14z"},
        {glyphs::place, L"M12 2C8.13 2 5 5.13 5 9c0 5.25 7 13 7 13s7-7.75 7-13c0-3.87-3.13-7-7-7zm0 9.5c-1.38 0-2.5-1.12-2.5-2.5s1.12-2.5 "
                        L"2.5-2.5 2.5 1.12 2.5 2.5-1.12 2.5-2.5 2.5z"},
        {glyphs::renderSize, L"M21 3H3c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h18c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zm0 16H3V5h18v14zM5 15h2v2H5zm0"
                             L"-4h2v2H5zm4 4h2v2H9z"},
        {glyphs::language,
         L"M11.99 2C6.47 2 2 6.48 2 12s4.47 10 9.99 10C17.52 22 22 17.52 22 12S17.52 2 11.99 2zm6.93 6h-2.95c-.32-1.25-.78-2.45-1.38-3.56 "
         L"1.84.63 3.37 1.91 4.33 3.56zM12 4.04c.83 1.2 1.48 2.53 1.91 3.96h-3.82c.43-1.43 1.08-2.76 1.91-3.96zM4.26 14C4.1 13.36 4 12.69 4 "
         L"12s.1-1.36.26-2h3.38c-.08.66-.14 1.32-.14 2 0 .68.06 1.34.14 2H4.26zm.82 2h2.95c.32 1.25.78 2.45 1.38 3.56-1.84-.63-3.37-1.9-4.33"
         L"-3.56zm2.95-8H5.08c.96-1.66 2.49-2.93 4.33-3.56C8.81 5.55 8.35 6.75 8.03 8zM12 19.96c-.83-1.2-1.48-2.53-1.91-3.96h3.82c-.43 1.43"
         L"-1.08 2.76-1.91 3.96zM14.34 14H9.66c-.09-.66-.16-1.32-.16-2 0-.68.07-1.35.16-2h4.68c.09.65.16 1.32.16 2 0 .68-.07 1.34-.16 2zm.25"
         L" 5.56c.6-1.11 1.06-2.31 1.38-3.56h2.95c-.96 1.65-2.49 2.93-4.33 3.56zM16.36 14c.08-.66.14-1.32.14-2 0-.68-.06-1.34-.14-2h3.38c.16"
         L".64.26 1.31.26 2s-.1 1.36-.26 2h-3.38z"},
    };
    return paths;
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
    const std::pair<const wchar_t*, const wchar_t*> kinds[] = {
        {L"Plain", L"0"}, {L"Rounded", L"12"}, {L"Pill", L"20"}, {L"Circle", L"24"}, {L"Fab", L"16"}};
    for (auto [name, radius] : kinds) {
        std::wstring xaml = replaceAll(replaceAll(styleTemplate, L"%NAME%", name), L"%RADIUS%", radius);
        try {
            auto dictionary = Markup::XamlReader::Load(xaml).as<ResourceDictionary>();
            resources.MergedDictionaries().Append(dictionary);
        } catch (const hresult_error& e) {
            shellLog::write("ui: couldn't load the button style " + narrow(name) + ": " + narrow(e.message()));
        }
    }
    try {
        resources.MergedDictionaries().Append(Markup::XamlReader::Load(textBoxStyle).as<ResourceDictionary>());
    } catch (const hresult_error& e) {
        shellLog::write("ui: couldn't load the text box style: " + narrow(e.message()));
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
FontWeight weightSemibold() { return FontWeights::Medium(); }
FontWeight weightBold() { return FontWeights::Bold(); }

FontFamily roboto(FontWeight weight) {
    // Roboto's Medium is a family of its own in its file ("Roboto Medium"); Regular and Bold are "Roboto".
    static const FontFamily regular{L"ms-appx:///fonts/Roboto-Regular.ttf#Roboto, Segoe UI"};
    static const FontFamily medium{L"ms-appx:///fonts/Roboto-Medium.ttf#Roboto Medium, Segoe UI"};
    static const FontFamily bold{L"ms-appx:///fonts/Roboto-Bold.ttf#Roboto, Segoe UI"};
    return weight.Weight >= 700 ? bold : weight.Weight >= 500 ? medium : regular;
}

TextBlock text(std::string_view content, double size, FontWeight weight, std::optional<Color> color) {
    TextBlock t;
    t.Text(hs(content));
    t.FontFamily(roboto(weight));
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

FrameworkElement icon(wchar_t code, double size, std::optional<Color> color) {
    const auto& paths = materialPaths();
    const auto found = paths.find(code);
    if (found == paths.end()) return glyph(code, size, color);
    try {
        // A PathIcon's colour is its Foreground, which it takes from what it is in, as text does; its path is drawn in a
        // square of 24, which the Viewbox scales.
        auto pathIcon = Markup::XamlReader::Load(L"<PathIcon xmlns=\"http://schemas.microsoft.com/winfx/2006/xaml/presentation\" "
                                                 L"HorizontalAlignment=\"Left\" VerticalAlignment=\"Top\" Data=\"" +
                                                 std::wstring(found->second) + L"\"/>")
                            .as<PathIcon>();
        if (color) pathIcon.Foreground(brush(*color));
        Grid square;
        square.Width(24);
        square.Height(24);
        square.Children().Append(pathIcon);
        Viewbox box;
        box.Width(size);
        box.Height(size);
        box.Child(square);
        box.VerticalAlignment(VerticalAlignment::Center);
        return box;
    } catch (const hresult_error&) {
        return glyph(code, size, color);
    }
}

TextBlock monospace(std::string_view content, double size, std::optional<Color> color) {
    TextBlock t;
    t.Text(hs(content));
    t.FontFamily(FontFamily(monoFamilyText()));
    t.FontSize(size);
    t.TextWrapping(TextWrapping::Wrap);
    if (color) t.Foreground(brush(*color));
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
    b.Background(brush(palette().surfaceContainerHigh));
    return b;
}

Border outlinedCard(UIElement const& content, double padding) {
    Border b;
    b.Child(content);
    b.CornerRadius(CornerRadiusHelper::FromUniformRadius(12));
    b.Padding(ThicknessHelper::FromUniformLength(padding));
    b.Background(brush(palette().surface));
    b.BorderBrush(brush(palette().outlineVariant));
    b.BorderThickness(ThicknessHelper::FromUniformLength(1));
    return b;
}

Border divider() {
    Border b;
    b.Height(1);
    b.Background(brush(palette().outlineVariant));
    b.HorizontalAlignment(HorizontalAlignment::Stretch);
    return b;
}

Button button(UIElement const& content, std::function<void()> onClick, ButtonKind kind, std::string_view name) {
    Button b;
    if (auto style = buttonStyle(kind)) b.Style(style);
    b.Content(content);
    b.Foreground(brush(palette().onSurface));
    b.HorizontalAlignment(HorizontalAlignment::Stretch);
    b.HorizontalContentAlignment(HorizontalAlignment::Stretch);
    if (!name.empty()) Automation::AutomationProperties::SetName(b, hs(name));
    b.Click([onClick](auto&&, auto&&) {
        if (onClick) onClick();
    });
    return b;
}

namespace {

/// A pill button's content: an icon of 18 (0: none) and the label, Material's label large.
StackPanel pillContent(std::string_view title, wchar_t iconCode) {
    auto content = hstack(8);
    content.HorizontalAlignment(HorizontalAlignment::Center);
    content.VerticalAlignment(VerticalAlignment::Center);
    if (iconCode) add(content, icon(iconCode, 18));
    auto t = text(title, font::labelLarge, weightMedium());
    t.TextWrapping(TextWrapping::NoWrap);
    t.VerticalAlignment(VerticalAlignment::Center);
    add(content, t);
    return content;
}

Button pill(std::string_view title, wchar_t iconCode, std::function<void()> onClick, Color background, Color foreground) {
    Button b = button(pillContent(title, iconCode), std::move(onClick), ButtonKind::pill, title);
    b.Background(brush(background));
    b.Foreground(brush(foreground));
    b.MinHeight(40);
    b.Padding(ThicknessHelper::FromLengths(iconCode ? 16 : 24, 10, 24, 10));
    return b;
}

} // namespace

Button primaryButton(std::string_view title, wchar_t iconCode, std::function<void()> onClick, std::optional<Color> fill) {
    const auto& p = palette();
    // A fill of its own (the player's menu asks the highest surface container): the colour on the surface on it.
    return fill ? pill(title, iconCode, std::move(onClick), *fill, p.onSurface) : pill(title, iconCode, std::move(onClick), p.primary, p.onPrimary);
}

Button tonalButton(std::string_view title, wchar_t iconCode, std::function<void()> onClick) {
    const auto& p = palette();
    return pill(title, iconCode, std::move(onClick), p.secondaryContainer, p.onSecondaryContainer);
}

Button outlinedButton(std::string_view title, wchar_t iconCode, std::function<void()> onClick) {
    const auto& p = palette();
    Button b = pill(title, iconCode, std::move(onClick), withAlpha(p.surface, 0), p.primary);
    b.BorderBrush(brush(p.outline));
    b.BorderThickness(ThicknessHelper::FromUniformLength(1));
    return b;
}

Button textButton(std::string_view title, std::function<void()> onClick, std::optional<Color> color) {
    Button b = pill(title, 0, std::move(onClick), withAlpha(palette().surface, 0), color ? *color : palette().primary);
    b.Padding(ThicknessHelper::FromLengths(12, 10, 12, 10));
    b.HorizontalAlignment(HorizontalAlignment::Left);
    return b;
}

Button iconButton(wchar_t iconCode, std::function<void()> onClick, std::string_view name, std::optional<Color> color) {
    auto i = icon(iconCode, 24);
    i.HorizontalAlignment(HorizontalAlignment::Center);
    Button b = button(i, std::move(onClick), ButtonKind::circle, name);
    b.Width(48);
    b.Height(48);
    b.HorizontalAlignment(HorizontalAlignment::Center);
    b.HorizontalContentAlignment(HorizontalAlignment::Center);
    b.Foreground(brush(color ? *color : palette().onSurfaceVariant));
    return b;
}

Button fab(std::string_view title, wchar_t iconCode, std::function<void()> onClick) {
    const auto& p = palette();
    auto content = hstack(12);
    content.VerticalAlignment(VerticalAlignment::Center);
    add(content, icon(iconCode, 24));
    auto t = text(title, font::labelLarge, weightMedium());
    t.TextWrapping(TextWrapping::NoWrap);
    t.VerticalAlignment(VerticalAlignment::Center);
    add(content, t);
    Button b = button(content, std::move(onClick), ButtonKind::fab, title);
    b.Background(brush(p.primaryContainer));
    b.Foreground(brush(p.onPrimaryContainer));
    b.Height(56);
    b.Padding(ThicknessHelper::FromLengths(16, 0, 20, 0));
    b.HorizontalAlignment(HorizontalAlignment::Right);
    b.VerticalAlignment(VerticalAlignment::Bottom);
    return b;
}

StackPanel label(std::string_view title, wchar_t iconCode, double size, FontWeight weight, std::optional<Color> color) {
    auto s = hstack(12);
    add(s, icon(iconCode, 24, color));
    auto t = text(title, size, weight, color);
    t.VerticalAlignment(VerticalAlignment::Center);
    add(s, t);
    return s;
}

Grid textField(TextBox& box, std::string_view initial, std::string_view placeholder) {
    const auto& p = palette();
    Grid g;
    g.MinHeight(56);
    Border back;
    back.Background(brush(p.surfaceContainerHighest));
    back.CornerRadius(CornerRadius{4, 4, 0, 0});
    g.Children().Append(back);
    Border line;
    line.Height(1);
    line.VerticalAlignment(VerticalAlignment::Bottom);
    line.Background(brush(p.onSurfaceVariant));
    g.Children().Append(line);
    auto hint = text(placeholder, font::bodyLarge, weightNormal(), p.onSurfaceVariant);
    hint.TextWrapping(TextWrapping::NoWrap);
    hint.VerticalAlignment(VerticalAlignment::Center);
    hint.Margin(ThicknessHelper::FromLengths(16, 0, 16, 0));
    hint.IsHitTestVisible(false);
    setVisible(hint, initial.empty());
    g.Children().Append(hint);
    TextBox t;
    auto resources = Application::Current().Resources();
    for (auto const& merged : resources.MergedDictionaries())
        if (merged.HasKey(box_value(L"MeglerTextBox"))) t.Style(merged.Lookup(box_value(L"MeglerTextBox")).as<Style>());
    t.Text(hs(initial));
    t.FontFamily(roboto());
    t.FontSize(font::bodyLarge);
    t.Foreground(brush(p.onSurface));
    t.SelectionHighlightColor(brush(p.primary));
    t.Padding(ThicknessHelper::FromLengths(16, 0, 16, 0));
    t.VerticalAlignment(VerticalAlignment::Center);
    weak_ref<TextBox> weak = t;
    t.TextChanged([weak, hint](auto&&, auto&&) {
        if (auto s = weak.get()) setVisible(hint, s.Text().empty());
    });
    const Color focused = p.primary, idle = p.onSurfaceVariant;
    t.GotFocus([line, focused](auto&&, auto&&) {
        line.Height(2);
        line.Background(brush(focused));
    });
    t.LostFocus([line, idle](auto&&, auto&&) {
        line.Height(1);
        line.Background(brush(idle));
    });
    g.Children().Append(t);
    box = t;
    return g;
}

Grid passwordField(PasswordBox& box, std::string_view placeholder) {
    const auto& p = palette();
    Grid g;
    g.MinHeight(56);
    Border back;
    back.Background(brush(p.surfaceContainerHighest));
    back.CornerRadius(CornerRadius{4, 4, 0, 0});
    g.Children().Append(back);
    Border line;
    line.Height(1);
    line.VerticalAlignment(VerticalAlignment::Bottom);
    line.Background(brush(p.onSurfaceVariant));
    g.Children().Append(line);
    auto hint = text(placeholder, font::bodyLarge, weightNormal(), p.onSurfaceVariant);
    hint.TextWrapping(TextWrapping::NoWrap);
    hint.VerticalAlignment(VerticalAlignment::Center);
    hint.Margin(ThicknessHelper::FromLengths(16, 0, 16, 0));
    hint.IsHitTestVisible(false);
    g.Children().Append(hint);
    PasswordBox t;
    auto resources = Application::Current().Resources();
    for (auto const& merged : resources.MergedDictionaries())
        if (merged.HasKey(box_value(L"MeglerPasswordBox"))) t.Style(merged.Lookup(box_value(L"MeglerPasswordBox")).as<Style>());
    t.FontFamily(roboto());
    t.FontSize(font::bodyLarge);
    t.Foreground(brush(p.onSurface));
    t.SelectionHighlightColor(brush(p.primary));
    t.Padding(ThicknessHelper::FromLengths(16, 0, 16, 0));
    t.VerticalAlignment(VerticalAlignment::Center);
    weak_ref<PasswordBox> weak = t;
    t.PasswordChanged([weak, hint](auto&&, auto&&) {
        if (auto s = weak.get()) setVisible(hint, s.Password().empty());
    });
    const Color focused = p.primary, idle = p.onSurfaceVariant;
    t.GotFocus([line, focused](auto&&, auto&&) {
        line.Height(2);
        line.Background(brush(focused));
    });
    t.LostFocus([line, idle](auto&&, auto&&) {
        line.Height(1);
        line.Background(brush(idle));
    });
    g.Children().Append(t);
    box = t;
    return g;
}

TextBlock sectionHeader(std::string_view title) { return text(title, font::titleSmall, weightMedium(), palette().primary); }

Grid labeledContent(std::string_view title, std::string_view value, bool monospacedValue) {
    const auto& p = palette();
    Grid g;
    ColumnDefinition c0, c1;
    c0.Width(GridLengthHelper::Auto());
    c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    g.ColumnDefinitions().Append(c0);
    g.ColumnDefinitions().Append(c1);
    auto left = text(title, font::bodyLarge, weightNormal(), p.onSurface);
    left.TextWrapping(TextWrapping::NoWrap);
    left.VerticalAlignment(VerticalAlignment::Center);
    left.Margin(ThicknessHelper::FromLengths(0, 0, 16, 0));
    auto right = monospacedValue ? monospace(value, font::bodySmall, p.onSurfaceVariant) : text(value, font::bodyMedium, weightNormal(), p.onSurfaceVariant);
    right.TextAlignment(TextAlignment::Right);
    right.HorizontalAlignment(HorizontalAlignment::Right);
    right.VerticalAlignment(VerticalAlignment::Center);
    Grid::SetColumn(right, 1);
    g.Children().Append(left);
    g.Children().Append(right);
    g.MinHeight(56);
    g.Padding(ThicknessHelper::FromLengths(16, 8, 24, 8));
    return g;
}

Grid listItem(std::string_view headline, std::string_view supporting, wchar_t leading) {
    const auto& p = palette();
    Grid g;
    ColumnDefinition c0, c1;
    c0.Width(GridLengthHelper::Auto());
    c1.Width(GridLengthHelper::FromValueAndType(1, GridUnitType::Star));
    g.ColumnDefinitions().Append(c0);
    g.ColumnDefinitions().Append(c1);
    if (leading) {
        auto i = icon(leading, 24, p.onSurfaceVariant);
        i.Margin(ThicknessHelper::FromLengths(0, 0, 16, 0));
        g.Children().Append(i);
    }
    auto lines = vstack(0);
    lines.VerticalAlignment(VerticalAlignment::Center);
    add(lines, text(headline, font::bodyLarge, weightNormal(), p.onSurface));
    if (!supporting.empty()) add(lines, text(supporting, font::bodyMedium, weightNormal(), p.onSurfaceVariant));
    Grid::SetColumn(lines, 1);
    g.Children().Append(lines);
    g.MinHeight(supporting.empty() ? 56 : 72);
    g.Padding(ThicknessHelper::FromLengths(16, 8, 24, 8));
    return g;
}

StackPanel listSection(std::string_view header, std::vector<UIElement> const& rows, std::string_view footer) {
    auto section = vstack(0);
    if (!header.empty()) {
        auto h = sectionHeader(header);
        h.Margin(ThicknessHelper::FromLengths(16, 16, 16, 4));
        section.Children().Append(h);
    }
    for (const auto& row : rows) section.Children().Append(row);
    if (!footer.empty()) {
        auto f = text(footer, font::bodySmall, weightNormal(), palette().onSurfaceVariant);
        f.Margin(ThicknessHelper::FromLengths(16, 4, 16, 8));
        section.Children().Append(f);
    }
    // From edge to edge: the page's margin of 16 is the rows' own padding.
    section.Margin(ThicknessHelper::FromLengths(-16, 0, -16, 8));
    return section;
}

StackPanel contentUnavailable(wchar_t iconCode, std::string_view title, std::string_view description) {
    const auto& p = palette();
    auto s = vstack(8);
    s.HorizontalAlignment(HorizontalAlignment::Center);
    auto g = icon(iconCode, 48, p.onSurfaceVariant);
    g.HorizontalAlignment(HorizontalAlignment::Center);
    g.Margin(ThicknessHelper::FromLengths(0, 0, 0, 8));
    add(s, g);
    auto t = text(title, font::headlineSmall, weightNormal(), p.onSurface);
    t.TextAlignment(TextAlignment::Center);
    t.HorizontalAlignment(HorizontalAlignment::Center);
    add(s, t);
    if (!description.empty()) {
        auto d = text(description, font::bodyMedium, weightNormal(), p.onSurfaceVariant);
        d.TextAlignment(TextAlignment::Center);
        d.Margin(ThicknessHelper::FromLengths(0, 8, 0, 0));
        s.Children().Append(d);
    }
    return s;
}

ScrollViewer scrollingPage(UIElement const& content, double maxWidth) {
    ScrollViewer sv;
    sv.VerticalScrollBarVisibility(ScrollBarVisibility::Auto);
    sv.HorizontalScrollBarVisibility(ScrollBarVisibility::Disabled);
    sv.Background(brush(palette().surface));
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
    list_.Background(brush(palette().surface));
    try {
        list_.ItemTemplate(Markup::XamlReader::Load(replaceAll(replaceAll(lineTemplate, L"%COLOR%", hexColor(palette().onSurfaceVariant)),
                                                               L"%MONO%", monoFamilyText()))
                               .as<DataTemplate>());
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

} // namespace megler::app::ui
