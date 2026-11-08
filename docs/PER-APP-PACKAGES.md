# A package for each Android app

Megler runs one Android app in its one process, and a process here has 2 GB: the
app memory limit Windows sets on the phone tried (`MemoryManager.
AppMemoryUsageLimit`, 2048 MB on the HP Elite x3, 4 GB of memory), which is
also the 32-bit user address space. Two apps at once would share those 2 GB, and
one's crash would end both.

On Android each app is a process of its own. The way to have that here is a
package of its own for each Android app: Megler and the app's APK, with the app's
name and icon, its own process, memory limit and data, and its own entry in the
phone's task switcher. Windows 10 Mobile (15254) has no multi-instance apps
(desktop 1803 and later), and a packaged app cannot start processes of its own
with a window, so a package is the unit.

What such packages share is what Android apps share: external storage,
`/sdcard`, where a file manager such as Total Commander sees
`/sdcard/Android/data/<package>` of the others. A package's own folders are its
own; the folder they can share is the publisher cache folder
(`ApplicationData.GetPublisherCacheFolder`), the same for every package of one
publisher that declares it in its manifest:

    <Extensions>
      <Extension Category="windows.publisherCacheFolders">
        <PublisherCacheFolders>
          <Folder Name="shared" />
        </PublisherCacheFolders>
      </Extension>
    </Extensions>

## The probe

`app/PubCacheProbe`, built twice by `tools/build-pubcache-probe.sh` as
MeglerProbeA and MeglerProbeB (Megler's publisher, `CN=Computershik`) and run on the
phone by `tools/pubcache-probe.py`: A started, B started a quarter of a minute
later, which sends A to the background. On the HP Elite x3 (15254.603),
2026-10-20:

- The phone takes the manifest, and both packages get the same folder,
  `C:\Data\Users\DefApps\AppData\Local\Publishers\7p434m16tbgc6\shared` (the
  publisher id's folder).
- Win32 calls reach it, as Megler's file layer makes them: `CreateFile2`,
  `FindFirstFileExW`, `CreateDirectoryW`, `LockFileEx`.
- B reads what A wrote, lists it, and reads a file A made in
  `sdcard\Android\data\com.example.A`, a folder made level by level.
- Locks hold across the two processes while A lives: A's byte-range lock on
  `lock.dat` (as SQLite takes) leaves B error 33 (`ERROR_LOCK_VIOLATION`) for
  the lock and for reading those bytes, and A's `excl.dat`, opened with no
  sharing, leaves B error 32 (`ERROR_SHARING_VIOLATION`).
- Each process has a limit of 2048 MB, and both are alive at once (the portal's
  process list).
- A is suspended in the background: its heartbeat file, rewritten every half
  second, stood still at the same count over three seconds once B was in front.
  An app in the background holds its locks while it is suspended.
- A file a running app holds open (the probe's log, opened by the C runtime) is
  answered 404 by the portal; the probe script ends both apps before reading.
- The phone's own Device Portal, from an app (Winsock, five seconds to
  connect): port 443 connects on 127.0.0.1 and on the phone's Wi-Fi and USB
  addresses; port 80 connects on none of them (no answer, no refusal), though
  the PC reaches port 80 over Wi-Fi and USB. So an app on the phone reaches the
  portal by HTTPS only, the portal's certificate being one the phone made for
  itself. (One cellular address answered on neither port.)
- The portal has no install from a loose folder: `register` and `networkapp`
  under `/api/app/packagemanager/` answer 404, as an endpoint that does not
  exist. A package installed from the phone has to be a signed `.appx`.

## Making and signing a package on the phone

`core/src/appx/mg_appx.c` makes a package and signs it, with CNG (`bcrypt.dll`,
which a packaged app may use) for SHA-256 and RSA; on other hosts its calls
fail.

- The ZIP is laid out as makeappx lays one out (ZIP64, version 45 local headers
  with 24-byte data descriptors; `[Content_Types].xml` and the signature last,
  version 20, sizes in their local headers; an end record of all ones after
  the ZIP64 record and locator). Files are stored, not compressed: Megler without
  the Android root is some 9.5 MB, and a stored file's blocks need no sizes in
  the block map.
- `AppxBlockMap.xml`: each file's size, its local header's size and the SHA-256
  of each 64 KB block, in base 64. File names are kept to letters, digits,
  `.`, `-`, `_` and `/`, which need no percent-encoding as part names.
- The signature is Authenticode's for a package, as signtool makes it: a
  PKCS #7 SignedData (after the four bytes `PKCX`) over an
  SpcIndirectDataContent whose SpcSipInfo is the APPX subject interface
  package's (version 0x01010000 and its GUID) and whose digest is `APPX`
  followed by the tagged SHA-256 digests AXPC (every byte before the
  signature's local header), AXCD (the central directory without the
  signature's entry, then the ZIP64 end record, locator and end record
  written as if the signature were not there), AXCT (the content types) and
  AXBM (the block map). The signer's attributes are an empty SpcSpOpusInfo,
  the content type, the statement type (individual code signing) and the
  message digest, which is of the SpcIndirectDataContent's content without its
  own header. These were taken from osslsigncode's description of the format
  and checked against a package makeappx made and signtool signed: the five
  digests (that one has a code integrity catalog, AXCI) recomputed match.
- The key is an RSA key of 2048 bits made once and kept as CNG's full private
  blob; its certificate, self-signed and made with it, has the subject
  `CN=Computershik` (UTF8String), key usage digital signature, extended key
  usage code signing, basic constraints end entity and a subject key
  identifier, as signtool's certificates have, and is valid for twenty years.
  A package's publisher id is computed from the Publisher string, not the key,
  so packages signed with a key made on the phone are of the same publisher as
  Megler itself, and share its publisher cache folder. The phone has to trust the
  certificate. The phone's Device Portal installed it with the first package it was
  sent, which Megler did until 2026-11-07 where Windows's deployment engine refused
  a package; now, where Windows refuses one for its certificate (0x800b0109,
  `appPackage::untrustedCertificate`), the app's page says so and offers the
  certificate to Windows's own installer of certificates (**Open the
  Certificate**), and the user installs it once.
- No code integrity catalog (`AppxMetadata/CodeIntegrity.cat`) is made, and
  the phone does not want one: the probe packed and signed this way (MeglerProbeC,
  `tools/appx-phone-check.py` builds it) was installed on the HP Elite x3 by
  hand, through the Device Portal's page in the phone's own browser with its
  certificate, and started (2026-10-20).

On a PC, `tools/build-appx-tool.sh` builds `appx-tool` (the same code, x64),
which packs a folder; `tools/appx-verify.py` checks a package independently:
each file's blocks against the block map, the package digests against the
signed ones, the message digest, and the RSA signature with openssl. Both a
package of makeappx's and signtool's and one of `appx-tool`'s pass it;
`makeappx unpack /v` unpacks `appx-tool`'s, and `signtool verify /pa` stops at
the certificate's root not being trusted on the PC (as it should, and not at a
digest). On the phone such a package installs and runs (above).

## Installing: Windows's deployment engine

Since 0.1.0.614 an app's package, and since 0.1.0.612 Megler's own update from
`Pictures\Megler`, are installed by Windows's deployment engine
(`PackageManager.AddPackageAsync`, the restricted capability `packageManagement`,
`shell/AppPackage.cpp` installWithPackageManager), which reads the package where
it lies; ForceApplicationShutdown closes the app being updated -- Megler itself,
for its own update. Since 0.1.0.640 a package is installed as the Store installs
an app (ForceApplicationShutdown, else None), as WinArchive's developer, whose
way of installing it is, advised, and as a developer's package (Windows 10
Mobile's option 0x8000) only where that is refused: a phone takes only so many
developer's packages and refuses one more with 0x80073d11
(ERROR_DEV_SIDELOAD_LIMIT_EXCEEDED). A Megler 575 could not install Aurora Store
so, and a Lumia 650 had 20 developer's packages (the portal's PackageOrigin 5),
Megler's and its four apps' among them, installed so by 0.1.0.612 to 0.1.0.639.
The packages are listed by `PackageManager.FindPackagesForUser` (the restricted
capability `packageQuery`) and uninstalled by `RemovePackageAsync`. Every package
goes onto the package volume Megler itself is installed on, the phone's memory or
a memory card, not where Windows saves new apps (below, "A memory card").

A phone needs its full interop unlock for this, the capability part included:
`packageManagement` is a restricted capability, and Windows 10 Mobile grants it
to a sideloaded app only once Interop Tools' Unlock has **Interop/Cap Unlock** and
**New Capability Engine Unlock** turned on and the phone has been restarted. The
basic unlock alone grants `codeGeneration` (Android code runs, and Megler warns
where it does not) and lets the package manager list the packages, but every
install is refused at once: a user's phone with Megler 0.1.0.671 refused all four
tries with `0x80070005 Access is denied.`, which the app's page said as it was.
Since then a refusal for access at every try (`E_ACCESSDENIED`, thrown by the call
or in its result) is an outcome of its own (`InstallResult::accessDenied`), said
once a run in the log, and the shell says what happened and what to do instead of
Windows's words (`appPackage::accessDeniedText`): on the app's page, with Open the
Package and Open the Certificate as before (Windows's own installer installs the
package meanwhile); in the Library, once for all the apps, Update All's included;
for Google Play's installs at the first start, which then wait for Megler's next
start instead of being made and refused again every minute; for an uninstall; and
for Megler's own update, whose package is then left in (or, downloaded from
GitHub, moved to) `Pictures\Megler`, where the phone's file manager opens it and
Megler tries it again at each start, so that the start after the unlock installs
it. Desktop Windows grants a sideloaded package its restricted capabilities as a
rule; where it refuses this one, the shell says so, without Interop Tools. The
last refusal of any kind is kept in `LocalState\install-refusal.txt`, which Save
All Logs takes along and its summary says ([STATUS.md](STATUS.md), "Logs").

The engine is the only way since 2026-11-07 (the owner's wish): Megler asks the
phone's Device Portal nothing, and needs neither the portal nor its
authentication turned off. Before, the portal (`https://127.0.0.1`, the phone's
own) was what Megler installed through up to 0.1.0.611, and then what it fell
back on where the engine refused a package or the package manager would not list
or uninstall them. It gave nothing
the engine does not: it installs only a developer's package, which the engine's
last tries are too; its upload went through Megler's memory, and a phone of 1 GB
lets an app have 390 MB, so that a Lumia 650's Megler 574 sending a Megler of
356 MB was ended by Windows with nothing said; its list of the packages took 11 s
on an HP Elite x3, more than a minute on a Lumia 650, and at times never came on
a Lumia 950 XL, where the package manager answers at once. Through the engine the
Lumia 650 updated Megler in under a minute and installed Google Play services'
package (277 MB) in 16 s, where the portal had taken almost 4 minutes. The one
thing the portal did besides was to install the certificate the apps' packages
are signed with along with the first of them (above, "Making and signing a
package"); it did so only where all four of the engine's tries had been refused,
for a developer's package needs no certificate the phone trusts on a phone in
Developer mode (as Microsoft says of WinAppDeployCmd, which installs one and
installs no certificate), and the Store's way does. Where Windows refuses a
package for its certificate now, the user installs the certificate once from
the app's page. A package Windows refuses stays made, for Windows's installer
(below).

An Xbox is the exception: there the Xbox's build (`tools/build-appx.sh x64
--xbox`) installs, lists and
uninstalls the apps' packages and its own update through the console's own
Device Portal, `https://localhost:11443`, as DesktopMode_uwp's installer does
([XBOX.md](XBOX.md)). Only that build has the portal's client
(`shell/DevicePortal.cpp`).

## Megler as the place the apps are managed from

Since 0.1.0.359 Megler does not run the library's apps itself (the player is left
for an auto run, `auto/AutoRun.cpp`, which pushes it on its own): an app is
installed as an app of the phone's own, and Megler is where that is done
(`ui/SeparateApps.cpp`, `ui/LibraryScreen.cpp`, `ui/AppDetailScreen.cpp`):

- which apps are installed, and by which Megler, is asked of Windows's package
  manager (`PackageManager.FindPackagesForUser`, the restricted capability
  `packageQuery`, at once) when the library or an app's page is
  shown and after every action: a package of Megler's publisher whose name begins
  `MeglerApp.` is an app's, its version the version of the Megler that made it. Each
  row of the library says Installed, Installed and made by another Megler (to
  update), or Not installed; a list that cannot be had is said above the list;
- a package carries the Megler that made it, so an update of Megler updates none:
  above the list, **Update All** makes and installs again every app made by
  another Megler;
- an app's page has the section **On This Phone**: its state, **Install** (or
  **Update**, **Install Again**), which makes the package and installs it
  by Windows's deployment engine in one go, **App Settings** (below), and
  **Uninstall** (`RemovePackageAsync`: the package goes with its LocalState; its
  part of the shared storage stays). When Windows does not install a package
  made, the page offers it to Windows's installer, and its certificate;
- the actions go one at a time on a worker thread, whichever page is shown.

**The APK, kept once** (since 0.1.0.360). An app container may not open a file
the user picked by its path, which is why Megler used to copy every APK into
`LocalState\apks` before anything else. Windows' broker hands out real handles
to such a file (`IStorageItemHandleAccess`), and the core now takes a file by
handle under a path of the shell's (`megler_file_by_handle`; `mg_file_open` asks
the shell for a new handle at each open, so that each has a position of its
own): the report, the icon, a bundle's unpacking and the package's making read
the picked file where it is (`shell/PickedFile.cpp`). **Install APK** in the
library picks an APK and installs it at once: read where it is, made into a
package, installed by Windows's deployment engine. A bundle (`.apks`, `.xapk`) is unpacked
from where it is into its base and splits in `LocalState\apks`, then installed
from them.

After an install, the package made (`LocalState\packages\<identity>.appx`) is
removed; it stays only when Windows did not install it, for Windows's
installer. Megler keeps no copy of the app's APK: the one in the shared folder,
which the installed package's app reads (above), is the library's entry from
then on (`Library::adopt`), its report and icon cached as before, and an update
of the app (Update All) is made from it. Packages made before 0.1.0.645 had the
APK in them, and Megler read it there (`app\app.apk` in the package's install
folder, beside Megler's own: the folder above `Package::Current().InstalledLocation`)
while Windows let it. A package installed as the Store installs an app
(0.1.0.640 on) does not: it lets Megler open some of its app's files by their
paths, but shows it none of its folders (`app\app.apk.splits` has no attributes
and no list for Megler, though the app's own package starts with its splits),
its own files (`AppxBlockMap.xml`) give error 5, and so did the APK of every
package first installed so (Google Maps, AIDA64, Avito, Apktool M, Apple Music
on a Lumia 650, 644) and of Google Play services' from its third update so;
Megler then kept a copy of each such APK beside the package's. Hence the shared
folder. A package of 640 to 644 that let Megler's copy of an app in several
APKs go, its splits unseen (TikTok Lite on a Lumia 650), has them copied back
at the library's load, named by the report packed with the app
(`app\app.apk.json`, `appPackage::reportedSplits`). Before Uninstall, the APK
and splits are copied back into `LocalState\apks` (`Library::restore`), so that
the library keeps the app. A picked APK that cannot run here, or that was not
installed, is copied into the library, so that its page and Install stay.

**Settings** (`shell/Settings.h`, `ui/SettingsScreen.cpp`). Render size,
language and region, and location are kept in the shared folder, where every
package reads them when it starts: Megler's in `<shared>\settings.ini`, which
every app goes by, and an app's own in `<shared>\apps\<package>\settings.ini`,
over them, a key not there being Megler's. Megler's Settings tab changes Megler's;
**App Settings** on an app's page, and **Settings** in the player's menu of the
app's own package, change the app's, with **As in Megler** at the top of each
choice. They apply at the app's next start. Each file is written beside and
moved into place, so that a package reading it meanwhile finds a whole one. The
`settings.ini` of before, in LocalState, is copied to its new place the first
time.

**Megler's own storage** (`shareMeglersStorage` in `shell/AppPackage.cpp`). Megler's
`/sdcard` is the shared folder's too: once, in the background, Megler copies its
storage of before there (`LocalState\sdcard`, and each app's part
`LocalState\apps\<package>\sdcard` but those an app's own package has had
handed over), a file already there as large left, and writes
`LocalState\storage-in-shared.txt`; from its next start it gives the core the
shared folder as its storage, as an app's own package does. What it copied
stays in LocalState. From then on the handover of an app's data (below) takes
the private files alone: its part of the storage is the app's already.

### What has to be there first

Two things are asked for before anything else (the owner's wish, 2026-11-02):

- **The list of the installed apps.** Megler lists, installs and uninstalls
  every app's package, Update All's and its own update through Windows's
  package manager (above), and without the list an install went as far as
  making the package and then failed. Nothing is installed while the list has
  not come (`separateApps::requireList`): picking an APK, opening one, an
  app's request (Play Store's: it is told "Not installed: Windows's package
  manager did not tell Megler which apps are installed ..."), Install and
  Update All each wait for it, or say why it could not be had, and ask for it
  again for the next try. There is nothing for the user to turn on: the
  restricted capabilities `packageQuery` and `packageManagement` are in
  Megler's manifest. (Until 2026-11-07 the list came from the phone's Device
  Portal where Windows would not give it, and this said to turn the portal on
  in Settings › Update & security › For developers and its Authentication off;
  neither is needed any more.)
  An app's request waits for the list first when Megler has not had one
  (`separateApps::askList`): Megler started or woken by the request itself has
  not asked for it yet, and Play Store's installer session waits for the
  outcome meanwhile. Woken by Play Store's request for WhatsApp, after Play
  Store's request to uninstall the one installed before had started it, Megler
  said at once that the portal, which listed the apps then, "has not answered
  yet", and Play Store showed the install as failed (576). The app that asked
  is told the list could not be had only when it does not come then.
- **Google's own Google Play services before Google Play Store.** Play Store
  starts, in its own package or when an app asks for it, only once Google's own
  Google Play services, which Megler has built in, is installed as an app of its
  own (`AppShell::playStoreCanStart`, `appPackage::hasGoogleServices`); until
  then it says so, with the way to Google Play services' page in the Library
  (from Play Store's own package: Megler opened with `megler:library`). And it
  starts with Google's own whatever the setting of Google services says
  (`player/Session.cpp`): with microG's, or with none, users' Play Store had no
  Android ID and its front page said "Server error".

## A separate app, made on the phone

`shell/AppPackage.cpp` makes an app's package, on a worker thread:

  1. Android's libcore and framework (the package's `android` folder, some
     170 MB) are copied once for each build of Megler to
     `<shared>\android\<Megler's version>`, a file `complete` written last; every
     app package of that build reads them there, so a package carries none.
     Whenever Megler has the list of the packages (after its jobs, Update
     All's too, and when the list is asked for again), it removes
     the folders of older builds that no installed package is of
     (`removeUnusedAndroidRoots`), `complete` first: after Update All, all of
     them. Until 0.1.0.459 none was removed, and each build that made a
     package left its 170 MB there.
  2. The signing key and certificate are made on the first use, in
     `LocalState\signing` (`key.bin`, `MeglerApps.cer`), the certificate's
     subject Megler's own Publisher.
  3. The manifest is Megler's installed one with the identity name
     `MeglerApp.<Android package>` (`_` made `-`), always 50 characters: a
     shorter one filled with a dot and a hash of the package, a longer one
     cut and given one; a phone product id made from the Android package (the
     same each time, so that a package made again is an update); and the
     app's label for the shown names (`core/src/appx/mg_appx_manifest.c`).
     Its capabilities are those the app's permissions need and Megler's own
     (below, "What a package declares"). Dependencies, the entry point and the
     publisher cache folder stay Megler's. Where the app's icon is drawn on the logos (below),
     the tile's background is `transparent`, the phone's own colour; the
     splash screen keeps Megler's black.
  4. `resources.pri`, below.
  5. The package, `LocalState\packages\<identity name>.appx`, holds Megler's
     installed files but `android`, `selftest`, `demo`, `codec`,
     `appx-template`, `bitgapps` (Google's APKs, some 100 MB, which only
     Megler's library offers; the services an app hosts come from
     `<shared>\services`; the packages up to 0.1.0.458 carried them, some
     110 MB of Megler's in each instead of some 12 MB), its `resources.pri`
     and what the packaging makes itself; that manifest and `resources.pri`; and `MeglerApp.ini`.
     The app's APK is not in it since 0.1.0.645: it is in the shared folder,
     `<shared>\apks\<identity name>\<token>\app.apk`, with the library's report
     and icon of it (`app.apk.json`, `app.apk.icon`, `reports.version`), so
     that the app is not read again, and `MeglerApp.ini` says
     `apk=shared:apks\<identity name>\<token>`. An app of split APKs (an
     `.apks` or `.xapk` unpacked) has its splits in `app.apk.splits` beside the
     base, where the library looks for them. The first package of an app copies
     them there (byte for byte, `files::mend`; `<token>` is the time it began);
     the app's next packages (every Update All) take that folder as it is, so
     a package is Megler's own files alone, some 12 MB, whatever the app's
     size. After an install `<shared>\apks\<identity name>\installed.txt` says
     which folder the installed package reads, and the library's entry is that
     APK (`Library::adopt`: `<id>.installed` says the package's full name, then
     `shared=<folder>`): one copy of each APK on the phone. A folder that no
     installed package and no entry of the library names any more (a newer APK
     installed, the app uninstalled) goes at a later listing of the packages,
     when no package is being made and it is over an hour old
     (`appPackage::removeUnusedSharedApks`). Packages made before 0.1.0.645
     carried the APK as `app\app.apk` (`apk=app`), and are read as they are
     until Update All makes them again.
     The certificate is copied beside it.
  6. The logos: the app's icon (`megler_apk_icon`'s PNG, decoded by
     `Windows.Graphics.Imaging`, or WebP, which the phone's WIC cannot read,
     decoded by the core: `megler_webp_decode`; for an app that gives only an
     adaptive or a vector icon, a PNG the core draws from its drawables,
     `core/src/apk/mg_icon.c`) drawn in the middle of each of
     Megler's logos, with their names and sizes, which the `resources.pri`
     template lists: `StoreLogo` 50x50, `Square44x44Logo`, `Square150x150Logo`,
     `Wide310x150Logo` and `SplashScreen` 620x300, the icon taking as much of
     the shorter side as Megler's own does in them (`Assets/make-assets.ps1`),
     transparent round it. An APK with no icon that can be drawn keeps Megler's.
  7. The app's data in Megler goes along, once for each app: the first package
     made of it (the shared folder has no `apps\<package>\megler-data.txt`, which
     is written when the package has been made) copies the app's part of
     Megler's storage, `LocalState\apps\<package>\sdcard` (`Android/data`,
     `Android/obb`, whatever else it has there), to the same place in the shared
     folder, the separate apps' storage, a file already there as large left
     (the OBB of an earlier make); what the separate app had in its own
     `Android/data` is put aside first, as `apps\<package>\Android-data.before-megler`.
     The app's private files (`/data/data`, `LocalState\apps\<package>\data`),
     which no other package can write into, go to `<shared>\handover\<package>\data`
     with a file `complete` written last; the separate app, before its app
     starts (`takeHandover`), copies them into its own LocalState, its own put
     aside as `data.before-megler` (and put back if the copy fails), and removes the
     handover, `complete` first. Megler keeps its own copy of everything. An app
     running in Megler is not handed over (its files are being written). Later
     packages of the app leave the separate app's data alone; only the
     expansion files (`Android/obb/<package>`, an `.xapk`'s OBB) are copied
     again where they are missing.

**resources.pri.** A packaged XAML app reads its `resources.pri` at its start,
and the resource map in it is named after the package's identity name. Megler's
names `Megler`; the first app package (353), which carried Megler's, ended at
once when started (Total Commander). makepri is not on the phone, so Megler
carries one made for a placeholder name of 50 characters
(`appx-template\resources.pri`, `tools/build-appx.sh`), and the app's name,
as long, is put in its place (`core/src/appx/mg_appx_pri.c`). The name is in
the file twice, in UTF-16: the map's name and its unique name
`ms-appx://<name>/`. The hierarchical schema's checksum (in `[def_hnamesx]`, 24
bytes on) is a CRC-32 (zlib's) over data in which the name is twice too,
lower-cased and in UTF-16, followed by 520 and 628 bytes of the rest; found on
the PC from files makepri made for names a character apart (the checksums'
differences are linear, as a CRC's are, and the same for capitals and small
letters). A CRC being linear, the checksum of the new name is the old one
changed by the CRC of the two names' difference followed by as many zero
bytes as come after each place; the two tails are found at each build by
`tools/pri-tails.py`, from a second file made for a name one character apart,
and written to `appx-template\resources.txt`. The file so made is byte for
byte the one makepri makes for that name (checked on the PC for Total
Commander's and for names with capitals, digits, `.` and `-`).
- Until 2026-11-07 a package the deployment engine refused (and before
  0.1.0.612 every one) was sent with the certificate to the phone's own Device
  Portal, `https://127.0.0.1/api/app/packagemanager/package`, with Windows's own
  HTTP client, after a GET of `/api/os/info` for the portal's CSRF cookie, and
  `/api/app/packagemanager/state` was asked until it said how it went; the
  phone's build has none of that now (above, "Installing"). The Xbox's build
  installs so still, through the console's portal ([XBOX.md](XBOX.md)).
- **Open the Package (Windows Installer)** and **Open the Certificate**, when
  Windows's deployment engine did not install it, hand the file to Windows
  (`Launcher.LaunchFileAsync`), as a file manager would: the package to its
  installer, the certificate to Windows's installer of certificates. On the
  HP Elite x3 (353) Windows's installer installed a package so.

A package with `MeglerApp.ini` in its install folder is one app's
(`shell/AppPackage.cpp`, `single()`):

- the library reads the app's folder (`MeglerApp.ini`'s `apk`: in the shared
  folder since 0.1.0.645, the package's `app` folder before) instead of
  `LocalState\apks`, and leaves it as it is;
- the shell opens the app as soon as the library has it, into the player (or
  its page, when it cannot run here), and Back after the app has ended leaves
  (Back on the app's first screen, where the app goes to the back as on
  Android 12, is the system's, which takes the window away and leaves the app
  as it is; one that ends its last activity with Back closes Megler as it
  ends);
  until then it shows the splash screen's picture on black, as Windows showed
  it a moment before (`ui/StartScreen.cpp`), not Megler's library;
- the core is given the shared folder as `megler_config.storage_dir`
  (`core/include/megler.h`): `/sdcard` is `<shared>\sdcard`, and each app's own
  part of it (`Android/data` and `obb`) `<shared>\apps\<package>\sdcard`,
  mounted for the others as on Android 9, so a file manager in its own package
  sees every app's; the app's private files (`/data/data/<package>`) stay in
  its package's LocalState (taken from Megler's once, step 7 above);
- the Java driver is given `<shared>\android\<version>` from `MeglerApp.ini` as
  the Android root, and the fonts from there.

**What a package declares.** Windows lists an app under each capability its
package declares (Settings › Privacy) and asks the user for a device's the
first time the app opens it. Until this was chosen, every app's package
declared all of Megler's: a calculator was listed with the text messages and
the call history, and one whose Google Play services looked for the location
had Windows ask for it. Now `megler_appx_capabilities` (`core/src/appx/
mg_appx_manifest.c`) makes the list from the permissions the app's manifest asks
for (the report's `permissions`, `maxSdkVersion` notwithstanding: an app that
asks for `WRITE_EXTERNAL_STORAGE` up to Android 9 alone saves its pictures
through MediaStore on later ones), and the package declares those and Megler's
own, nothing else:

| Android | Windows |
|---|---|
| `INTERNET` | `internetClient`, `internetClientServer`, `privateNetworkClientServer` |
| `CAMERA` | `webcam` |
| `RECORD_AUDIO` | `microphone` |
| `ACCESS_FINE_LOCATION`, `ACCESS_COARSE_LOCATION`, `ACCESS_BACKGROUND_LOCATION` | `location` |
| `READ_EXTERNAL_STORAGE`, `WRITE_EXTERNAL_STORAGE`, `MANAGE_EXTERNAL_STORAGE`, `MANAGE_MEDIA` | `picturesLibrary`, `videosLibrary`, `musicLibrary` |
| `READ_MEDIA_IMAGES`; `READ_MEDIA_VIDEO`; `READ_MEDIA_VISUAL_USER_SELECTED`, `ACCESS_MEDIA_LOCATION`; `READ_MEDIA_AUDIO` | `picturesLibrary`; `videosLibrary`; both; `musicLibrary` |
| `RECEIVE_SMS`, `READ_SMS`, or Google Play services' SMS Retriever, a receiver of it in the manifest or its names in the app's code (the report's `smsRetriever`) | `chat` |
| `READ_PHONE_STATE`, `READ_CALL_LOG` | `phoneCallHistoryPublic` |
| `BLUETOOTH_SCAN`, `BLUETOOTH_CONNECT`, `BLUETOOTH_ADVERTISE`, `BLUETOOTH`, `BLUETOOTH_ADMIN` | the device capability `bluetooth` (Bluetooth LE, `player/BluetoothHost.h`) |
| every package | `picturesLibrary` (Megler's diagnosis folder `Pictures\Megler\diag`, `shell/PicturesDiag.h`; and an app's own pictures, which Android 10 and later let it save without asking), `codeGeneration`, `packageQuery` (the shell checks the Google Play services package is still installed), `backgroundMediaPlayback` (no permission on Android) |
| no package of an app's | `documentsLibrary` and `removableStorage`, reached only with file types declared, and an app's package declares none; `packageManagement`, Megler's alone |

A capability Megler's manifest has that this does not know is kept. A report an
older core made names no permissions, and the package then declares all of
Megler's, as before; Update All after an update of Megler reads every app again.
At run time the same list decides what the shell watches for the app: the text
messages and the call history only where its package declares them
(`player/PhoneEvents.h`). An app's permissions themselves are Android's runtime
ones (docs/JAVA.md, "Permissions"): the user is asked when the app requests one,
by Windows for the camera, the microphone and the location, by Megler for the
rest, and an app's App Settings list them.

**The phone's folders** (`ui/SharedFolders.cpp`). Music, Pictures, Movies and
DCIM come from the libraries' capabilities, which a package declares as above:
an app that asks for no storage has the shared storage's own folders there
(Pictures is the phone's in every package). The
Documents and Downloads folders and a memory card come in only through the
folder picker (`player/StorageHost.h`), and Windows remembers a picked folder
for the one package it was picked in (`FutureAccessList`): no other package of
the publisher, not even with the publisher cache folder, gets it. So each
separate app that asks for the storage has to be given each such folder by the
user once (an app that asks for none is not offered them, and one the user
refused the storage for good neither):

- Megler writes the folders added in its Settings (their names in `/sdcard` and
  their paths) to `<shared>\folders.txt`, once its search of the phone's folders
  is over and whenever Settings adds or takes one out;
- a separate app, before its app starts, offers each listed folder it has not
  got (by name or path): a dialog that says the folder, then the folder picker,
  started in Downloads or Documents; the folder chosen is added under Megler's
  name for it (`StorageHost::add`). "Not Now" is remembered
  (`megler.folders.notNow` in its `settings.ini`) and not asked at start again;
- the player's menu of a separate app has **Settings**, whose Android Storage
  has **Folders Megler Has**, which offers all of those it has not got, the "Not
  Now" ones too, and **Add a Folder** for any other; the app finds a folder
  added so from its next start (the links are given to the core when an app
  starts).

Flags (`LocalState\flags`) are each package's own: `tools/phone-flags.py --app
<Android package>` sets those of an app's own package.

**The Google APKs Megler brings** (`shell/BundledApks.cpp`). Megler's package
carries Google Play services, Google Play Store, Google Services Framework and
microG's Google Play services in its `bitgapps` folder, and Megler copies them:
into the library (`LocalState\apks\bitgapps-*.apk`), from there into an app's
folder of the shared folder (`apks\<identity>\<token>\app.apk`), and into
`<shared>\services` (`gsf`, `microg`, and Google Play services' copy for each
install of it, `registerServiceApk`). The apps run from those copies, and a byte
wrong in one stays wrong. On a Lumia 650, Google Play Store hashed its own APK
as `8xSmBxCuqpRxccCNeBfkQfAGRjU` (SHA-1, as it prints it) where the package's
`Phonesky.apk` is `DMRUOLPpLf0DK7_5xdCCavX_PrE`, so it refused every patch of
itself ("SU: Cannot patch com.android.vending:85372300, bad base file hash");
its APK in the shared folder had come byte for byte (0.1.0.645) from the
library's copy, which a plain copy had made before copies were compared with
anything. The same phone's copies of Google Play services had come out twice
with a bit wrong in a DEX file (0.1.0.625 and 632).

- Every copy of a bundled APK is made byte for byte: copied, put on the disk,
  and compared chunk by chunk with the package's as the disk holds it
  (`FILE_FLAG_NO_BUFFERING`, past Windows's cache, which just after a copy
  still has the pages it wrote), a chunk that differs written again
  (`files::copyWhole`, `files::mend`). An app's APK, and the services' copy of
  Google Play services, are copied from the package's own APK, not from the
  library's, where the library's is a copy of one (`bundledApks::originalOf`).
- Megler's own copies -- Google Services Framework and microG's Google Play
  services into `<shared>\services`, Google Play services and Google Play Store
  offered in the library once (`bitgapps-*.apk`, `apks\bitgapps.added`), a
  library copy that is not the package's made again, one whose app's package
  Windows took away put back -- are decided at the start by names, sizes and
  the last 64 KB of two files (`bundledApks::plan`), and made once the window
  is shown, on a thread of their own at background priority
  (`Library::afterStart`, `bundledApks::start`): each said in the log before it
  begins (`bundled apks: copying ...`, written out at once), every 10 s while it
  goes, and with its times after (to copy, to put on the disk, to compare);
  refused where the disk lacks the room; each library entry listed and read as
  its copy is made, the Library showing a card while they go, and Google Play's
  install at the first start waiting for them. A copy of 8 MB or more is made
  past Windows's cache, and compared with its source as the disk holds both,
  so that hundreds of megabytes do not push out of a phone's memory what
  everything else had in it. They were made in `Library::load` before, on the
  UI thread, before the window was shown: a Lumia 630 (512 MB, Windows 10
  Mobile 15063) with Megler 0.1.0.671 showed the splash screen for a few
  seconds and closed at every start -- Windows ends an app whose window it has
  not been shown in time, and tells it nothing -- each log ending a second in
  with the phone's folders, which another thread had looked up, and each start
  beginning the copies again. Google Play services' own package registers its
  APK for the other apps (a copy of it into `<shared>\services`) likewise after
  its window is shown, and outside the library's lock.
- Once for each build of Megler (`LocalState\bundled-apks-checked.txt`), a
  thread of its own at background priority compares every APK Megler keeps
  (the library's, each folder of `<shared>\apks`, `<shared>\services`) that is
  a copy of one of the package's -- of its size, and with its last 64 KB, the
  ZIP's central directory, differing in 64 bytes at most (another APK's differs
  in thousands) -- with that one, and mends it where it differs. The log says
  each copy mended and where, and at the end `bundled apks: N copies of the
  package's M Google APKs compared with them in S s`. A copy that differs and
  cannot be mended has it done again at the next start. It runs after the
  copies above, and leaves out those just made, which were compared as they
  were made (a first start's are all such).
- The flag `check-bundled-apks` (Megler's) has the check done at every start,
  and the log say each file's SHA-1 and SHA-256, each byte of a copy that
  differs (a copy's first 16 one by one; then how many bytes, bits and zeros,
  in how many pages, and whether each chunk that differed read alike three
  times), and the copy's differing pages are kept in
  `LocalState\bundled-apks\<n>.pages` (an 8-byte offset, then the page). In an
  app's own package (`phone-flags.py --app`), the same flag has the log say the
  size and digests of the app's own APK as that app reads it. The flag
  `test-copies` copies the package's largest Google APK three times as copies
  were made before (CopyFile2 and a flush), compares each with it so, and
  removes it: whether a phone gets copies wrong.

On the Lumia 650 (0.1.0.668, 2026-11-07) the first check found Google Play
Store's APK in the shared folder 2 bits off the package's: SHA-1
`8xSmBxCuqpRxccCNeBfkQfAGRjU` before, as Play Store had said, and
`DMRUOLPpLf0DK7_5xdCCavX_PrE` once mended. Google Play services' (made the same
way, by 645 from the library's copy of 2026-11-01) was 3 bits off, all in its
`resources.arsc`. Play Store's two were in stored libraries for x86 and x86-64
(`lib/x86/libcronet.156.0.8078.39.so`, `lib/x86_64/libtensorflowlite_jni.so`),
which nothing here loads, which is why only its patching noticed. All five were
the same bit at the same place of a 4 KB page: bit 3 (the value 8) of the byte
at 0x80D, a 1 become 0; the DEX file of 632's damaged copy summed 8 short as
well. That is this phone's memory -- one cell that loses a 1, in a page of
memory that whatever the phone copies passes through in turn -- not Megler's
copying, and nothing but comparing a copy with its source as the disk holds it
finds it. It does not lose it every time: the same start's `test-copies` made
three copies of the 279 MB Google Play services, each the same byte for byte
(7 to 12 s to copy and 10 to 11 s to compare each, at background priority).
The check took 279 s there, Update All's twelve packages going on beside it;
Play Store, started after, read its own APK as `DMRUOL...` (the flag in its
package).

**Google's APKs for each processor.** Megler runs the native code of its own
processor alone (32-bit ARM on a phone; x86_64, x86 or arm64-v8a on desktop
Windows, [DESKTOP.md](DESKTOP.md)), and Google builds Google Play services in a
variant for each processor -- APKMirror lists them by architecture and Android
version, the six digits in brackets after the version saying both: `100300`
armeabi-v7a for Android 9 and later, `100400` arm64-v8a + armeabi-v7a, `040700`
x86 and `040800` x86 + x86_64 for Android 7 and later (there is no x86 build
for Android 9). The variant for a 64-bit processor carries a few libraries of
the 32-bit one beside its own, for 32-bit apps that load its code: the
arm64-v8a variant 8 armeabi-v7a libraries of its 40, the x86_64 one 8 x86
libraries. The phones' packages carried the arm64-v8a variant (26.41.30,
264130013) until 2026-11-07, and the phones' logs said `libgmscore.so` and
`libleveldbjni.so` were not found. `tools/build-appx.sh` now checks each APK it
bundles (`tools/bitgapps-check.py`): it must be the app its name says, with
its native code wholly of the package's processor (as many libraries of its
ABI as of any other). Google Play Store (53.7.23, 11 libraries of each ABI),
microG (0.3.17, 6 of each) and Google Services Framework (no native code) are
the same APK for every package.

The bundles since then are Google Play services 26.41.31, the nearest release
APKMirror has for all four processors, of a variant that stores its DEX files
(mapped from the APK, not inflated into each app's data, `dvm/mg_dvm_apk.c`)
where one is built: `100300` (armeabi-v7a) and `100400` (arm64-v8a) for Android
9 and later, as before. For x86 and x86_64 the only ones are for Android 7 and
later (`040700`, `040800`), which deflate their DEX files (each app inflates
them once into its `dex` folder), and for Android 12 and later (`190700`,
`190800`), which store them but are built for API 31: Megler's libcore is
Android 9's, the R8 back-ports of the Java 9 to 11 methods Android added at
API 30 (`List.of` and its kin) are not in them, and APKMirror lists them as
signed with a second, newer key of Google's beside the old one. Read for what
they call, 26.41.31's Android 7 build calls 111 methods of the system's that
Megler's Android root does not have, its Android 9 build 214; of them 14 and 17
of `java.*`, the 14 among the 17 (`Stream.toList`, `takeWhile` and their like,
which Google's code calls on the Android versions that have them).

**A later APK of Megler's than the one installed.** Update All makes an app's
next package from the APK the app has (its folder in the shared folder), so a
later Megler's Google Play services never reached a phone that had installed
an earlier one's: the phones would have kept the arm64-v8a variant.
`bundledApks::replacementFor` compares an installed Google Play services
(Google's, not microG's) or Google Play Store with the one Megler's package
brings when the two files differ, by their reports, and the package's takes
the installed one's place when it is of a later version (264131009 after
264130013), or of the same release (version codes equal but for their last
three digits, the variant) with more libraries of this processor -- the x86
variant after the x86_64 one on a PC where x86 Megler followed x64 Megler. Its
package is then made from Megler's APK (a new folder of the shared folder,
copied from the package's own), its report is the library's once the package
is installed, and the old folder goes with the next listing. An older APK is
never put over a later one, as Android keeps an update of a system app over an
older system image; and an APK of the library's that is not installed yet is
installed as it is, the user's choice.

## A memory card

A user reported (2026-11-08, no log with it) that Megler crashes once it, or its
apps, are installed on the memory card: Windows 10 Mobile's Settings › System ›
Storage, "New apps will save to: SD card", or an app moved there. No phone with a
card was at hand; what follows is from the code and from how Windows keeps a
package on another volume than the system's. Such a package has its app data on
that volume too (`<drive>:\WpSystem\...\AppData\...`), the files an app writes
there encrypted for it; its publisher cache folder is that volume's, a folder
other than the phone's memory's, or Windows gives it none
(`GetPublisherCacheFolder` throws, `appPackage::sharedFolder` is empty). A card
is FAT32 or exFAT as a rule: no hard links, junctions, symbolic links, sparse
files or ACLs, a time of writing kept to two seconds, no file of 4 GB on FAT32.

What that did to Megler:

- **Its apps went to the card.** `installWithPackageManager` named no volume, so
  Windows put every package Megler made where it saves new apps. An app's own
  package on the card has the card's shared folder (or none), where Megler, in
  the phone's memory, had put neither Android's files nor the app's APK: its
  library found no APK and the app showed its splash picture for ever, which is
  a crash to whoever waits for it. An APK such an app handed Megler to install
  (`<shared>\incoming`) was in a folder Megler does not see.
- **No shared folder is no folder.** Many places made their paths as
  `sharedFolder() + L"\\..."`: with none, `\apks\...`, `\services\...`,
  `\incoming\...` -- the current drive's root. An app's own package listed
  `\apks\<identity>\<token>\*.apk` as its library and wrote its
  `reports.version` there, removing an app's registration took
  `\services\gms-package.txt` away, and `files::makeDirectories` would walk such
  a path up to the drive's root and make what it could there (an app container
  is refused most of it, which was all that stood in the way).

What Megler does since 2026-11-08:

- Every package it installs -- an app's, Update All's, Google's apps', its own
  update -- goes onto **the package volume Megler itself is on**
  (`installVolume`, `shell/AppPackage.cpp`): Windows's package manager lists its
  volumes (`FindPackageVolumes`, else `GetPackageVolumesAsync`), and the one that
  holds Megler's install folder (`Package::Current().InstalledLocation()`) is the
  one whose store path the folder is in, else whose volume name the folder's own
  handle gives (`GetFinalPathNameByHandle`, `\\?\Volume{...}`: a store path named
  by another drive letter, as a phone's data volume may be both `C:\Data` and
  `U:`), else whose mount point it is under (`placement::volumeHolding`,
  `shell/Placement.h`, tested on a PC with the volumes of a PC, of a phone and of
  a phone with a card). It is handed to `AddPackageAsync(uri, null, options,
  volume)` (`addPackage`); where none is found, Windows lists none, or the call
  with a volume is not there or not taken (E_NOINTERFACE, E_NOTIMPL or
  E_INVALIDARG thrown, said once), the call is as before. What Windows answers is
  taken as before, every set of options in its turn. The log says the volume
  once, and again when it is another: `app package: packages are installed on
  Megler's own volume, C:\Data\Programs\WindowsApps (mounted at ...,
  \\?\Volume{...}, the system's)`.
- **Paths that are not absolute are refused** by every function of
  `shell/Files.cpp` (ERROR_BAD_PATHNAME, the first said in the log once), and
  `files::removeTree` refuses a drive's or a volume's root: nothing is listed,
  made, written or removed at a drive's root whatever a caller forgot. And the
  callers know no shared folder from one: an app's own package has no APK folder
  and no Android root then (`single()`), its library reads nothing, Megler's
  library takes an installed app's APK for none, a link `megler:install` whose
  APK is not in Megler's shared folder says why (the app that asked is on other
  storage), and making a package says that Megler is on the card.
- **An app's own package says what it lacks.** At its start
  (`appPackage::singleAppProblem`, `placement::missingFor`) it reads the first
  bytes of its APK and Android's `complete` in its shared folder; where Windows
  gave it no shared folder, or either is not there or cannot be read, its app is
  not opened, and its screen (`ui/StartScreen.cpp`) says so: the app and Megler
  are on different storage; keep them on the same, the app (or Megler) moved to
  the phone's memory in Settings › System › Storage, or the app installed again
  from Megler's Library (**Open Megler**). The log says which was missing, where
  it was looked for, and each folder with its drive.
- **Megler on the card says so** once at each start, a dialog, and above the
  Library (`appPackage::meglerOnCard`, a phone's install folder not on the drive
  Windows is on, or no shared folder anywhere): its apps may not reach its files,
  the phone's memory is where it should be, and New apps will save to: This
  device. Megler still installs its apps beside itself on the card.
- **The log says where everything is.** At each start, `storage: the install
  folder on C: (\\?\Volume{...}), LocalState on C:, the shared folder on C:
  (...); Windows on C:`; Save All Logs' summary says it as `Folders:`, and the
  drive each of Megler's packages is installed on.
- **A memory card's file system:** a copy that CopyFile2 refuses for what it
  copies besides the bytes (ERROR_ENCRYPTION_FAILED, extended attributes or
  streams the card has not, ERROR_ACCESS_DENIED from an app container) is made by
  reading and writing, the time of writing kept; a file Windows encrypted (an
  app's on the card) is copied so at once, so that the copy in the shared folder
  is plain and the publisher's other packages read it. A file whose reads past
  Windows's cache are refused once opened so (`files::DiskFile`: sectors larger
  than a page, a driver without them) is compared through the cache. A folder
  of the phone's whose file system lists no files' ids (FileIdBothDirectoryInfo
  refused) is listed with FileFullDirectoryInfo, by the core's layer
  (`list_handle`, `core/src/host/mg_host_win32.c`) and by the shell's check of
  its links (`player/StorageHost.cpp`) -- not tried: a PC's NTFS answers the
  first, and FAT and exFAT are thought to. The logs' mirror takes a copy's time to
  two seconds. Each way round is said once in the log. Megler's own code made no
  hard links, junctions, symbolic links or sparse files, and asked for no file
  ids; the core's POSIX rename (FileRenameInfoEx) went round a file system
  without it before (a file put aside, then the plain rename), and its locks
  count a file system without them as granted.

*Proven* on a PC (`tools/build-tests.sh win`, `mg_shell_tests`): the volume
chosen for a PC's, a phone's and a phone-with-a-card's volumes; paths that are not
absolute refused and no drive's root removed; what an app's own package finds
missing and says; what Megler says of itself on the card; the copies made by
reading and writing where CopyFile2 is refused, and compared through the cache
where uncached reads are, each said once (`files::testing`); and
`tools/log-mirror-test.sh`, a copy's time kept to two seconds. *Not known* until a
phone with a card is tried: whether Windows 10 Mobile gives a package on the card a
publisher cache folder at all, and the same one to Megler and its apps there;
what its package volumes' store paths and mount points are (the log line above
says them); whether the deployment engine moves an app's package installed on the
other storage when its update is told Megler's volume, or refuses it (its refusal
is said on the app's page as any other: uninstall it and install it again);
whether the card's encryption of an app's files keeps the publisher's other
packages from reading what one of them put in the shared folder there; and
whether the deployment engine reads a package that Megler on the card made in its
LocalState there, which is encrypted for Megler.

What a user with a memory card does: keep Megler in the phone's memory (Settings
› System › Storage: the memory card, Apps & games, Megler, Move), set New apps will
save to: This device, and open Megler: it says nothing of the card any more. An app
of Megler's that says it cannot start is moved to the phone's memory the same way,
or installed again from its page in Megler's Library (Install Again, or Update All
after an update of Megler). Save All Logs then shows each package's drive (its
summary's `Megler's packages` and `Folders`), and the log the volume each install
went to.

## What follows (not done yet)

- What is left of Megler in each package, some 12 MB (`Megler.exe`, ANGLE, the
  C++ runtime's DLLs, the shell's fonts), is each package's own: a process
  loads code from its own package alone, another package's install folder
  cannot be read, and a framework package of one's own is not to be had.
- How apps reach one another (Android's intents to another app, "share", "open
  with", content providers): within one process that is calls inside Megler;
  between packages it needs a way of its own (protocol activation, app services).
