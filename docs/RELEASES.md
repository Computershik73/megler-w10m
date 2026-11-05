# Releases, and how Megler updates itself from them

Megler's releases are on GitHub: <https://github.com/Computershik73/megler-w10m/releases>. Since 0.1.0.638 Megler
updates itself from them over the air (`app/Megler/ui/Ota.cpp`): it asks GitHub's API for the releases when it starts, at
most once in six hours (Settings › Updates: at start, or only when asked; Check Now), and offers the newest that has a
package for it. Taken, the package is downloaded into `LocalState\update`, its size and its SHA-256 checked against what
GitHub says of the asset, and installed as an update copied to `Pictures\Megler` is: Windows's deployment engine closes
Megler and installs the new one, and Megler is opened again.

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
| `Megler.cer` | the certificate every package is signed with (the same one for all) |

- `_ARM.appx` is Windows 10 Mobile's alone. A 32-bit ARM package of any other Windows -- Windows RT's, should there be
  one, which is not Windows 10 and could not take the phone's -- has its Windows in its name: `Megler_X.Y.Z.N_RT_ARM.appx`
  (or `.zip`, whatever it is). Any further Windows likewise: `Megler_X.Y.Z.N_<Windows>_<processor>.<ext>`. A phone takes
  `_ARM.appx` only, and never one of those.
- Which one a Megler takes: on a phone, `_ARM`; on desktop Windows, its own processor's, `_ARM64`, `_x64` or `_x86` --
  and a 32-bit ARM Megler running on an ARM64 computer takes `_ARM64`, there being no 32-bit ARM package for the desktop.
- `.msix` in place of `.appx` is taken too, for the day the desktop's packages are MSIX.
- A release need not have every package: a phone's Megler skips a release without `_ARM.appx` and takes the newest
  release that has one. Packages of one version for several Windows go into the one release of that version.
- An asset's name is exactly that: no other words in it, no copies with other names (`Megler_X.Y.Z.N_ARM (1).appx`).

## Publishing one

1. Merge the work branch into `megler` with a merge commit, check that the merged tree is the branch's, tag
   `vX.Y.Z.N` (annotated) and push both.
2. `gh release create vX.Y.Z.N --latest --title "Megler X.Y.Z.N" --notes-file <notes>` with each package under its name
   above and `Megler.cer`.
3. Compare the assets' SHA-256 digests (`gh release view vX.Y.Z.N --json assets`) with the files': Megler checks what it
   downloads against them, and refuses a package whose digest is not GitHub's.
4. The notes say which file is for which Windows and processor, and the SHA-256 of each.
