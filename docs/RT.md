# Megler on Windows RT's tablets: desktop Windows 10 on 32-bit ARM

Windows RT's tablets -- the Surface RT (NVIDIA Tegra 3: four Cortex-A9) and the Surface 2 (Tegra 4: Cortex-A15) --
and phones such as the Lumia 950 and 950 XL run, by their owners' hands, desktop Windows 10 for 32-bit ARM: build
10.0.15035 (`rs2_release.170209-1535`, armfre), a pre-release of 1703 that got out, and the only such Windows there is
(GitHub issue #3; images sold as "Windows RT 10" or as later builds are 15035 too, says Open-RT's
[list of fake builds](https://open-rt.gitbook.io/open-surfacert/common/windows-10/fake-builds-and-customized-releases.md)).
It is desktop Windows, device family `Windows.Desktop`, not Windows 10 Mobile, and older than any Windows 10 Mobile
Megler's phone package is for (1703, 10.0.15063): Windows refuses that package there twice over, for its families and
for its version.

## The package

`tools/build-appx.sh arm --rt`, with the options of the phone's package of the release (`--java --bitgapps ...
--gms ... --play-store ... --microg ...`), makes `Megler_X.Y.Z.N_RT_ARM.appx` ([RELEASES.md](RELEASES.md)): the
phone's build -- the same compile, Megler.exe, core, ANGLE, Android root and APKs -- under that name and with a
manifest that differs from the phone's in one place:

| | the phone's (`_ARM`) | Windows RT's (`_RT_ARM`) |
|---|---|---|
| `TargetDeviceFamily` | `Windows.Mobile` and `Windows.Universal`, `MinVersion` 10.0.15063.0 | `Windows.Desktop`, `MinVersion` 10.0.15035.0 |
| `MaxVersionTested` | 10.0.15254.0 | 10.0.15254.0 |

The rest is the phone's: the identity (`Megler`, `CN=Computershik`, `arm`), `mp:PhoneIdentity` (which the manifest of
an app's own package is made from: `core/src/appx/mg_appx_manifest.c`), the capabilities, the extensions.
`Windows.Desktop` alone, so that no phone takes the package, and the app packages a Megler of it makes say the same.
`MinVersion` is what decides the install; `MaxVersionTested` does not
([TargetDeviceFamily](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-targetdevicefamily)).

**Why the families are all it takes.** Windows checks a manifest against the schema of the Windows it is, and 15035
has 1607's (10.0.14393) at least. The RT manifest passes MakeAppx of the 14393 SDK (10.0.14393.795,
`Windows Kits\10\bin\x64\makeappx.exe`), which checks with 14393's schema: a capability of the uap4 namespace (1703's)
added as a test, not ignorable, is refused there -- "The schema for OSMinVersion specified does not recognize XML
fields with namespace .../uap/windows10/4", 0x80080204 -- and taken by 15063's MakeAppx. The manifest's namespaces
(foundation, uap, uap2, uap3, mp, rescap; uap3 is 1607's, by Microsoft's
[table of namespaces](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/what-s-changed-in-windows-10)),
its capabilities and `windows.publisherCacheFolders` are all in 14393's schema
(`Include\10.0.14393.0\winrt\AppxManifestTypes.xsd`: `codeGeneration`, `chat`, `phoneCallHistoryPublic`,
`backgroundMediaPlayback`, the libraries, `removableStorage`; a restricted capability is any name there). It depends
on no framework package (VCLibs, .NET Native): the C runtime is linked in, and ANGLE's is in the package.

## What people change in other apps' manifests

What the owners of these devices do to a UWP app before 15035 takes it, as they write it (no tool of its own for it
was found; the forums named were read through search engines' excerpts, which is all they gave):

- **The version, and the family.** `MinVersion` of `TargetDeviceFamily` lowered to 10.0.15035.0 or below, the package
  signed again on the device with an ARM signtool, its certificate put in Local Machine > Trusted People
  ([XDA, "Windows 10 build 15035 for Surface RT"](https://xdaforums.com/t/windows-10-build-15035-for-surface-rt.4011147/page-3));
  the package must be 32-bit ARM, for Desktop or Universal, with a minimum of 1607 or 15035 -- a Mobile or HoloLens
  one does not install ([WFAv7 Installer's wiki](https://github.com/RedGreenBlue09/WFAv7_Installer/wiki/Apps-on-Windows-10-ARM)).
  Unigram's builds, among others, are passed around with `MinVersion` 15035
  ([4PDA](https://4pda.to/forum/index.php?showtopic=990472)). A developer's own app for them says
  `Windows.Universal`, `MinVersion` 10.0.15035.0, built with Visual Studio told not to raise it
  (`AppxOSMinVersionReplaceManifestVersion=false`; [a gist](https://gist.github.com/WamWooWam/e72e5137606f7c59ed657db6587cd5e8)).
- **What 15035 does not know.** Namespaces of 1709 and later (uap5 on, desktop3 on, rescap4 on, com2 on) taken out
  or listed in `IgnorableNamespaces`, whose elements a Windows that does not know them skips
  ([Package](https://learn.microsoft.com/en-us/uwp/schemas/appxpackage/uapmanifestschema/element-package)); else
  0x80080204, the manifest not valid.
- **Frameworks.** The frameworks an app depends on put on first (.NET Native's framework and runtime, VCLibs) -- or
  the dependency taken out and the C++ runtime carried in the package, as the Bad Piggies repack for these tablets
  does ([BPRE_WinARM32](https://github.com/Max-RM/BPRE_WinARM32)); WinUI 2.8 wants 17763.
- **Signing.** Publisher equal to the certificate's subject (else 0x8007000B or 0x80073CF0), the block map's hash
  method signtool's `/fd`; an unsigned package installs on Windows 11 alone
  ([troubleshooting](https://learn.microsoft.com/en-us/windows/win32/appxpkg/troubleshooting)). The MSIX Packaging
  Tool's editor is used to change `MinVersion` and sign again, the .msix renamed .appx (15035 has no MSIX, which is
  1709's).

Megler's package needs none of it but the version and the family: the rest of its manifest is 1607's, it has no
frameworks, and it is signed as built.

## What Megler asks of 15035

- **Imports.** Megler.exe (`link /dump /imports`, 0.1.0.670): 254 functions of 38 DLLs. Every one is in the 14393
  SDK's import libraries for ARM (WindowsApp.lib, OneCoreUAP.lib) but two, VirtualAllocFromApp and
  VirtualProtectFromApp, which the 15063 libraries the phone's build links with name through
  `api-ms-win-core-memory-l1-1-5` where 14393's say `l1-1-3`: kernelbase's functions since 10240 either way. Windows
  10's loader finds an api-set by its name without the last number (on this PC `api-ms-win-core-memory-l1-1-99`
  loads kernelbase.dll), and the package has a forwarder DLL of that name besides (gecko-w10m's
  `gen-apiset-shims.py`, tools/build-appx.sh's "ANGLE"). Delay-loaded: d2d1, dwrite, xaudio2_9, mfplat, mfreadwrite and
  oleaut32, all of desktop Windows 10. Looked up by name as it runs: ntdll's NtQueryInformationProcess,
  NtSetTimerResolution and NtContinue, kernelbase's VirtualProtect, and kernel32's IsWow64Process2 (1709 on), whose
  absence is an answer too (the x64 core's stops, the OTA).
- **Windows Runtime.** The shell compiled for ARM against a C++/WinRT projection of the 14393 SDK's metadata (cppwinrt
  over `References\<contract>\<version>` at 14393's versions: UniversalApiContract 3.0) fails in three places only:
  `Windows.UI.ViewManagement.StatusBar` and `PhoneCallManager.CallStateChanged`, both behind
  `ApiInformation.IsTypePresent` (a phone's, which desktop Windows has not), and `ProcessorArchitecture::Arm64`, a
  constant in a switch. Nothing it calls is newer than 1607. Its XAML (ui/Theme.cpp's styles and templates, which
  XamlReader reads) is 10240's. (15035 is very likely 1703's API whole: the SDK of 15021 was said to be complete, and
  that of 15042 to have changed nothing since; no list of what 15035 lacks of 15063 was found.)
- **The desktop.** What the shell does otherwise on desktop Windows (`platform::isMobile()`: a phone-sized window, no
  status bar, the cameras' turning) is the x64 package's, which runs on desktop Windows now.

## Graphics

A Surface RT's and a Surface 2's graphics drivers offer Direct3D 11 at feature level **9_1**
([Chuck Walbourn's list](https://walbourn.github.io/direct3d-feature-levels/); BPRE_WinARM32, above; Tegra 4's
fragment shaders are FP20). The shell takes it: its devices ask for levels down to 9_1, and a Java app's pictures fall
back to WARP where there is no device at all (player/WindowHost.cpp, GfxHost.cpp, CodecHost.cpp); 9_1's textures stop
at 2048, which a Surface 2's 1920x1080 still fits.

**ANGLE does not.** gecko-w10m's ANGLE asks Direct3D for 11_1 down to 10_0 and then 9_3 (Renderer11.cpp, "GeckoW10m":
for the Adreno 3xx of the Lumia 550, 650 and 950), and Megler's EGL asks it for a display as Direct3D 11 and then at
9_3, and stops there: "software rendering is not worth trying on a phone" (core/src/ndk/mg_egl.c, make_display). On a
9_1 device neither is made, and an app that draws with OpenGL ES gets no display. Microsoft's ANGLE says the same of
the Surface RT -- it needs WARP, Direct3D in software
([Initializing ANGLE on D3D11 Feature Level 9](https://github.com/Microsoft/angle/wiki/Initializing-ANGLE-on-D3D11-Feature-Level-9))
-- and RetroArch for these devices draws through ANGLE on WARP at about 5 pictures a second
([RetroArch-ARM](https://github.com/basharast/RetroArch-ARM)). A third try in make_display, its device type
`EGL_PLATFORM_ANGLE_DEVICE_TYPE_D3D_WARP_ANGLE`, made only where the graphics processor's own level is below 9_3 (so
never on a phone), would give these tablets OpenGL ES in software; it is not made yet. A Lumia on 15035 without its
graphics driver (the WFAv7 installer leaves drivers out) should have the Basic Render Driver, which is WARP, and ANGLE
on it in software -- inferred, not seen. Java apps' own pictures (Direct2D) do not need ANGLE.

## The processor

The Tegra 3's Cortex-A9 is ARMv7 with NEON and VFPv3-D32 but has no integer division (SDIV, UDIV), no VFPv4 (fused
multiply-add) and no LPAE. The Tegra 4's Cortex-A15 and every phone's Snapdragon have them all. Windows on 32-bit ARM
needs VFPv3-D32 and NEON and emulates no floating point; SDIV and UDIV it traps and emulates where the processor has
none, "approximately 80 cycles" more per division on a Cortex-A9
([Overview of ARM ABI Conventions](https://learn.microsoft.com/en-us/cpp/build/overview-of-arm-abi-conventions)).

- **Compiled code** has none of them: llvm-objdump, decoding as a Cortex-A15 would, finds no SDIV, UDIV or VFMA in
  Megler.exe, libGLESv2.dll, libEGL.dll, mozglue.dll or the VCLibs' msvcp140.dll and vcruntime140.dll, but a handful
  decoded from data (beside an `mcr2` or a `bx sp`). MSVC divides by its runtime's `__rt_sdiv` and `__rt_udiv`
  (LIBCMT's divide.obj, in Megler.exe's map), a shift and subtract.
- **The JIT** wrote SDIV for an int's division and remainder: trapped by Windows at every division on a Cortex-A9,
  and an undefined instruction under Linux. It divides by a call of the runtime's where `mg_cpu_arm_divide()` says the
  processor cannot (`IsProcessorFeaturePresent(PF_ARM_DIVIDE_INSTRUCTION_AVAILABLE)`; HWCAP_IDIVT under Linux).
- **What an app is told**: `/proc/cpuinfo` and AT_HWCAP said idiva, idivt and vfpv4 (and cpuinfo lpae) whatever the
  processor. An app's code that chooses by them -- a library that takes VFPv4's fused multiply-add when it reads
  vfpv4; V8 in the WebView, which divides with SDIV when it reads idiva -- would fault on a Tegra 3 for the one and be
  trapped at every division for the other. They say them where the processor has SDIV, and the Cortex-A9's part
  number (0xc09) where it has not.
- **Checked** under qemu-arm playing a Cortex-A9 (`QEMU_CPU=cortex-a9` with `WSLENV=QEMU_CPU`; its AT_HWCAP 0x8b8d7
  has no IDIVA, IDIVT or VFPv4): `tools/build-tests.sh arm`, the Java test apps included, passes as on qemu's default
  processor; with the JIT made to write SDIV again the same run stops at the first compiled loop on an illegal
  instruction.

## Installing

On the device (from the guides above and [Open-RT's](https://open-rt.gitbook.io/open-surfacert/common/windows-10/software.md)):

1. Settings > Update & security > For developers: Sideload apps (or Developer mode).
2. `Megler.cer` from the release into Local Machine > Trusted People (as administrator: `certutil -addstore
   TrustedPeople Megler.cer`, or the certificate's Install Certificate).
3. The package opened in App Installer (1.0.32912.0 works on 15035, says Open-RT), or `Add-AppxPackage
   .\Megler_X.Y.Z.N_RT_ARM.appx` in PowerShell (15035's runs in Constrained Language Mode, which the cmdlet does not
   mind).
4. The true date on the clock. 15035 expired on 31 May 2017, and its owners silence the notice (WFAv7's
   [tweaks](https://github.com/RedGreenBlue09/WFAv7_Installer/wiki/More-tweaks)) rather than turn the clock back; a
   clock turned back would refuse Megler's signature, its certificate being valid from 18 October 2026 (to 18 October
   2027), and GitHub's TLS for the updates.

The packages Megler makes of single apps are signed with a key of the device's own, whose certificate
(`LocalState\signing\MeglerApps.cer`) goes in Trusted People as well, as on any PC
([PER-APP-PACKAGES.md](PER-APP-PACKAGES.md)).

## What only such a device can tell

Nothing here has run on 15035; there is no such device at hand. Still to be seen on one:

- that 15035's deployment engine takes the package, and grants a sideloaded package `codeGeneration` (without which
  no Android code runs: the library says so), `packageManagement` and `packageQuery` (Megler's own update and app
  packages) -- Microsoft says a sideloaded app may declare restricted capabilities with no approval
  ([capabilities](https://learn.microsoft.com/en-us/windows/apps/package-and-deploy/app-capability-declarations));
- that Megler starts: XAML and the shell on 15035, the core's executable memory, its vectored exception handler and
  its stops of threads (the collector's) on that kernel;
- the shell's pictures on the Tegra drivers at 9_1, and what OpenGL ES apps do with no ANGLE display there;
- that Windows says no to PF_ARM_DIVIDE_INSTRUCTION_AVAILABLE on a Tegra 3 (and yes on a phone, whose Self-test says
  "the processor has SDIV");
- memory: 2 GB in a Surface RT and a Surface 2 (3 GB in a Lumia 950 or 950 XL), and a 32-bit process's 2 GB of
  addresses;
- the OTA's choice there, Megler's own update, and app packages through the package manager (whether 15035 has the
  Device Portal, the fallback, nobody says).
