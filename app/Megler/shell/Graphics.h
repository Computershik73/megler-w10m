// Which OpenGL ES an app draws with, where Megler's package carries two: the Windows 10 Mobile package (tools/build-appx.sh
// arm; not Windows RT's, the desktop's or the Xbox's) has
//
//   ANGLE          libEGL.dll, libGLESv2.dll (with mozglue.dll and the C runtime): OpenGL ES over Direct3D 11, the default;
//   Mesa freedreno libEGL_mesa.dll, libGLESv2_mesa.dll and their driver libgallium_wgl.dll, some 20 MB: OpenGL ES on the
//                  phone's Adreno itself, through its kernel-mode graphics driver (D3DKMT); experimental.
//
// Mesa's are named apart from ANGLE's so that both are at the package's root, where Windows finds the driver they import.
// The setting Graphics (settings::gles, ui/SettingsScreen.cpp) chooses, Megler's for every app and each app's own over it.
// An app's process takes its choice at its start (player/Session.cpp, megler_run_config.gles) and the core loads that
// library, or ANGLE where Mesa cannot be had (core/src/ndk/mg_egl.c, mesa_load), saying why in one line of the log.
//
// An app's own package carries Megler's installed files (shell/AppPackage.cpp, make): Mesa's 20 MB go into it only when the
// app goes by Mesa when its package is made, and a package made so is written down in the shared folder
// (apks\<identity>\graphics.txt). An installed app whose package has Mesa where its setting no longer asks for it, or lacks
// it where its setting does, is to be updated (ui/SeparateApps.cpp): its Update, or Update All, makes its package again. Until
// then an app set to Mesa whose package lacks it draws with ANGLE, and its log says that its package is to be made again.
//
// What is here is the decision, from what the callers read (files, settings): tested on a PC (core/tests/mg_shell_windows.cpp).
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace megler::app::graphics {

/// Mesa's three DLLs as the package has them at its root: the driver first, which the other two import.
extern const wchar_t* const mesaFiles[3];
/// Whether a file of the package's root, by name, is one of Mesa's (case-blind).
bool isMesaFile(const std::wstring& name);
/// The folder of the package's root with Mesa's licence texts (tools/build-appx.sh, stage_mesa: drivers/mesa-arm32/
/// LICENSES), which goes wherever its DLLs go: into an app's own package only with them.
extern const wchar_t* const mesaLicenses;
/// Whether a folder of the package's root, by name, is Mesa's (case-blind).
bool isMesaFolder(const std::wstring& name);
/// Whether the folder (a package's install folder) has all of Mesa's DLLs; `missing` gets those it lacks, ", " between.
bool hasMesaFiles(const std::wstring& folder, std::string* missing = nullptr);

/// The choice where none is set, the package's default: "mesa" where it carries gles-default.txt saying so (a package built
/// with MEGLER_GLES=mesa), else "angle".
std::string packageDefault(const std::wstring& installFolder);

/// What an app goes by: its own setting, else Megler's, else the package's default -- "mesa" only where Mesa can be chosen
/// at all (`mesaChoosable`), "angle" for anything else.
std::string resolve(const std::optional<std::string>& own, const std::optional<std::string>& megler, const std::string& fallback,
                    bool mesaChoosable);

/// What an app's process starts with: "mesa" or "angle", and, where it was set to Mesa and gets ANGLE, why, in words for
/// the log.
struct Start {
    std::string gles;
    std::string why;
};
/// `chosen` the app's choice (resolve), `packageHasMesa` whether this process's package has Mesa's DLLs, `missing` those it
/// lacks, `ownPackage` whether this is the app's own package (Megler's own carries Mesa whenever it can be chosen).
Start atStart(const std::string& chosen, bool packageHasMesa, const std::string& missing, bool ownPackage);

/// The record of the packages made of an app (`identity`, its package's identity name) and whether each carries Mesa:
/// <shared>\apks\<identity>\graphics.txt, a line "<full name> mesa" or "<full name> angle" for each, the newest last, the
/// last 16 kept. Written by Megler once the package is made.
bool record(const std::wstring& shared, const std::string& identity, const std::string& fullName, bool mesa);
/// Whether the package of that full name carries Mesa, as its record says; false where none says (a package made before
/// the record was kept, or by a Megler without Mesa: neither has it).
bool recordedMesa(const std::wstring& shared, const std::string& identity, const std::string& fullName);

}  // namespace megler::app::graphics
