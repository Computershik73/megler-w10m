# Releases, and how Megler updates itself from them

Megler's releases are on GitHub: <https://github.com/Computershik73/megler-w10m/releases>. Since 0.1.0.638 Megler
updates itself from them over the air (`app/Megler/ui/Ota.cpp`): it asks GitHub's API for the releases when it starts, at
most once in six hours (Settings › Updates: at start, or only when asked; Check Now), and offers the newest that has a
package for it. Taken, the package is downloaded into `LocalState\update`, its size and its SHA-256 checked against what
GitHub says of the asset, and installed as an update copied to `Pictures\Megler` is: Windows's deployment engine (on an
Xbox, the console's Device Portal) closes Megler and installs the new one, and Megler is opened again.

Megler finds its package by name alone, so every release keeps to these rules.

## The packages' names

One release is one version, `vX.Y.Z.N` (the tag) and `Megler X.Y.Z.N` (the title), not a draft and not a pre-release:
Megler takes neither. Its assets:

| Asset | For |
|---|---|
| `Megler_X.Y.Z.N_ARM.appx` | Windows 10 Mobile, 32-bit ARM: the phones (1703 and later) |
| `Megler_X.Y.Z.N_ARM64.appx` | desktop Windows 10 and 11 on 64-bit ARM |
| `Megler_X.Y.Z.N_x64.appx` | desktop Windows 10 and 11 on x64 |
| `Megler_X.Y.Z.N_x86.appx` | desktop Windows 10 and 11 on 32-bit x86 |
| `Megler_X.Y.Z.N_Xbox_x64.appx` | an Xbox One or Xbox Series X\|S in Developer Mode (`tools/build-appx.sh x64 --xbox`, docs/XBOX.md) |
| `Megler_X.Y.Z.N_RT_ARM.appx` | desktop Windows 10 on 32-bit ARM: build 15035 on Windows RT's tablets (Surface RT, Surface 2) and on Lumias ([RT.md](RT.md)) |
| `Megler.cer` | the certificate every package is signed with (the same one for all) |

- `_ARM.appx` is Windows 10 Mobile's alone. A 32-bit ARM package of any other Windows has its Windows in its name:
  `Megler_X.Y.Z.N_RT_ARM.appx` is desktop Windows 10 on 32-bit ARM, the build 15035 that Windows RT's tablets run (the
  phone's package with another manifest, which that Windows takes: `tools/build-appx.sh arm --rt` with the phone's
  options, on the release's number like every package of it -- app/Megler/build-number.txt at N-1 before the build --
  [RT.md](RT.md)). Any further Windows likewise: `Megler_X.Y.Z.N_<Windows>_<processor>.<ext>`. A phone takes
  `_ARM.appx` only, and never one of those.
- Which one a Megler takes: on a phone, `_ARM`; on desktop Windows, its own processor's, `_ARM64`, `_x64` or `_x86`; a
  32-bit ARM Megler on desktop Windows `_RT_ARM` where that Windows is a 32-bit ARM one (15035), `_ARM64` where it is an
  ARM64 one, which runs it (IsWow64Process2 says which; a Windows without it is older than 1709, the first on ARM64); on
  an Xbox (the device family `Windows.Xbox`), `_Xbox_x64`, which it installs through the console's Device Portal, and
  never the PC's `_x64`, whose manifest is not the Xbox's.
- `.msix` in place of `.appx` is taken too, for the day the desktop's packages are MSIX.
- A release need not have every package: a phone's Megler skips a release without `_ARM.appx` and takes the newest
  release that has one. Packages of one version for several Windows go into the one release of that version.
- An asset's name is exactly that: no other words in it, no copies with other names (`Megler_X.Y.Z.N_ARM (1).appx`).

## Google's apps in each package

Every package brings Google Play services, Google Play Store, Google Services Framework and microG
(`tools/build-appx.sh --bitgapps --gms --play-store --microg`), each package Google Play services of its own processor:
Megler runs no other processor's code, and Google's variant for a 64-bit processor has a few of its libraries for the
32-bit one only (docs/PER-APP-PACKAGES.md, "Google's APKs for each processor"). `tools/bitgapps-check.py` refuses
any other.

| Package | Google Play services (APKMirror's variant) |
|---|---|
| `_ARM` | armeabi-v7a, Android 9.0+, nodpi (`26.41.31 (100300-...)`, 264131009) |
| `_ARM64` | arm64-v8a + armeabi-v7a, Android 9.0+, nodpi (`100400`, 264131013) |
| `_x64` | x86 + x86_64, Android 7.0+, nodpi (`040800`, 264131008) |
| `_x86` | x86, Android 7.0+, nodpi (`040700`, 264131007) |

Google Play Store 53.7.23 (universal, Android 7.0+: 11 libraries of each processor), microG 0.3.17 (6 of each) and the
BiTGApps archive's Google Services Framework (no native code) are the same file in every package. The APKs are not in
the repository: whoever builds a release keeps them (the builds of 2026-11-07 took them from
`Downloads\megler-google-apks\<abi>` and `...\universal`), and checks each one's signer before it goes in: Google's
APKs `CN=Android, OU=Android, O=Google Inc.`, certificate SHA-256 `f0fd6c5b...2d60db83`; microG's `O=NOGAPPS Project`,
`9bd06727...d14165`.

Google's sign-in is a web page in Android's System WebView, so each package's Android root carries the WebView of its
processor: `tools/webview-fetch.sh` (32-bit ARM's) and `tools/webview-fetch.sh x86_64|x86|arm64-v8a` before the
builds; `tools/build-appx.sh` refuses Google's apps without it. (0.1.0.669's desktop packages had none, and Play Store's
Sign In came back at once on them.)

A phone that installed Google Play services from an earlier package's has it made from the later one at its next
Update All (`bundledApks::replacementFor`): a later version, or another variant of the same release with more of the
processor's libraries. An older one never replaces a later one.

## Publishing one

1. Merge the work branch into `megler` with a merge commit, check that the merged tree is the branch's, tag
   `vX.Y.Z.N` (annotated) and push both.
2. `gh release create vX.Y.Z.N --latest --title "Megler X.Y.Z.N" --notes-file <notes>` with each package under its name
   above and `Megler.cer`.
3. Compare the assets' SHA-256 digests (`gh release view vX.Y.Z.N --json assets`) with the files': Megler checks what it
   downloads against them, and refuses a package whose digest is not GitHub's.
4. The notes say which file is for which Windows and processor, and the SHA-256 of each.
