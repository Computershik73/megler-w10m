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

`core/src/appx/hk_appx.c` makes a package and signs it, with CNG (`bcrypt.dll`,
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
  certificate: the Device Portal's install takes it with the package.
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

## Megler as the place the apps are managed from

Since 0.1.0.359 Megler does not run the library's apps itself (the player is left
for an auto run, `auto/AutoRun.cpp`, which pushes it on its own): an app is
installed as an app of the phone's own, and Megler is where that is done
(`ui/SeparateApps.cpp`, `ui/LibraryScreen.cpp`, `ui/AppDetailScreen.cpp`):

- which apps are installed, and by which Megler, is asked of the Device Portal
  (`/api/app/packagemanager/packages`) when the library or an app's page is
  shown and after every action: a package of Megler's publisher whose name begins
  `MeglerApp.` is an app's, its version the version of the Megler that made it. Each
  row of the library says Installed, Installed and made by another Megler (to
  update), or Not installed; a portal that cannot be asked is said above the
  list;
- a package carries the Megler that made it, so an update of Megler updates none:
  above the list, **Update All** makes and installs again every app made by
  another Megler;
- an app's page has the section **On This Phone**: its state, **Install** (or
  **Update**, **Install Again**), which makes the package and installs it
  through the portal in one go, **App Settings** (below), and **Uninstall**
  (`DELETE /api/app/packagemanager/package`: the package goes with its
  LocalState; its part of the shared storage stays). When the portal does not
  install a package made, the page offers it to Windows's installer, and its
  certificate;
- the actions go one at a time on a worker thread, whichever page is shown.

**The APK, kept once** (since 0.1.0.360). An app container may not open a file
the user picked by its path, which is why Megler used to copy every APK into
`LocalState\apks` before anything else. Windows' broker hands out real handles
to such a file (`IStorageItemHandleAccess`), and the core now takes a file by
handle under a path of the shell's (`megler_file_by_handle`; `hk_file_open` asks
the shell for a new handle at each open, so that each has a position of its
own): the report, the icon, a bundle's unpacking and the package's making read
the picked file where it is (`shell/PickedFile.cpp`). **Install APK** in the
library picks an APK and installs it at once: read where it is, made into a
package, installed through the portal. A bundle (`.apks`, `.xapk`) is unpacked
from where it is into its base and splits in `LocalState\apks`, then installed
from them.

After an install, the package made (`LocalState\packages\<identity>.appx`) is
removed; it stays only when the portal did not install it, for Windows's
installer. And when Megler can read the installed package's APK
(`app\app.apk` in its install folder, beside Megler's own: the folder above
`Package::Current().InstalledLocation`) and it is as large as the one packed,
Megler keeps no copy: its own copy and splits go, if there were any, and
`LocalState\apks\<id>.installed` says the package's full name; the library's
entry is that APK, its report and icon cached as before (`Library::adopt`). An
update of the app (Update All) is made from it. Whether an app container may
read another package's install folder is not known for Windows 10 Mobile: the
check is made after each install, and where it fails Megler keeps (or makes) a
copy, as before, and the log says so. Before Uninstall, the APK and splits are
copied back into `LocalState\apks` (`Library::restore`), so that the library
keeps the app. A picked APK that cannot run here, or that was not installed,
is copied into the library, so that its page and Install stay.

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

## A separate app, made on the phone

`shell/AppPackage.cpp` makes an app's package, on a worker thread:

  1. Android's libcore and framework (the package's `android` folder, some
     150 MB) are copied once for each build of Megler to
     `<shared>\android\<Megler's version>`, a file `complete` written last; every
     app package of that build reads them there, so a package carries none.
  2. The signing key and certificate are made on the first use, in
     `LocalState\signing` (`key.bin`, `MeglerApps.cer`), the certificate's
     subject Megler's own Publisher.
  3. The manifest is Megler's installed one with the identity name
     `MeglerApp.<Android package>` (`_` made `-`), always 50 characters: a
     shorter one filled with a dot and a hash of the package, a longer one
     cut and given one; a phone product id made from the Android package (the
     same each time, so that a package made again is an update); and the
     app's label for the shown names (`core/src/appx/hk_appx_manifest.c`).
     Capabilities, dependencies, the entry point and the publisher cache
     folder stay Megler's. Where the app's icon is drawn on the logos (below),
     the tile's background is `transparent`, the phone's own colour; the
     splash screen keeps Megler's black.
  4. `resources.pri`, below.
  5. The package, `LocalState\packages\<identity name>.appx`, holds Megler's
     installed files but `android`, `selftest`, `demo`, `codec`,
     `appx-template`, its `resources.pri` and what the packaging makes
     itself; that manifest and `resources.pri`; `MeglerApp.ini`; and
     `app\app.apk` with the library's report and icon of it (`app.apk.json`,
     `app.apk.icon`, `reports.version`), so that the app is not read again.
     An app of split APKs (an `.apks` or `.xapk` unpacked) has its splits in
     `app\app.apk.splits`, beside the base, where the library looks for them.
     The certificate is copied beside it.
  6. The logos: the app's icon (`megler_apk_icon`'s PNG, decoded by
     `Windows.Graphics.Imaging`, or WebP, which the phone's WIC cannot read,
     decoded by the core: `megler_webp_decode`; for an app that gives only an
     adaptive or a vector icon, a PNG the core draws from its drawables,
     `core/src/apk/hk_icon.c`) drawn in the middle of each of
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
as long, is put in its place (`core/src/appx/hk_appx_pri.c`). The name is in
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
- Installing it through the Device Portal sends the package and the certificate
  to `https://127.0.0.1/api/app/packagemanager/package` with Windows's own HTTP
  client (`Windows.Web.Http`), after a GET of `/api/os/info` for the portal's
  CSRF cookie, and asks `/api/app/packagemanager/state` every two seconds
  until it says how it went. The cookies are handled by hand, as
  `tools/phone.py` sends them: those of the GET's `Set-Cookie` go back in the
  `Cookie` header, and the `CSRF-Token` one in `X-CSRF-Token` too (left to the
  HTTP client's own cookies, 353's install was refused 403); what the portal
  answered, cookies and refusals, is in the log. The portal's certificate, which
  the phone made for itself, is taken as it is (untrusted, wrong name, expired,
  incomplete chain), there only. A portal that asks for a user name and
  password is said to; none is sent yet.
- **Open the Package (Windows Installer)** and **Open the Certificate**, when the
  portal did not install it, hand the file to Windows (`Launcher.LaunchFileAsync`), as a file manager would. On the
  HP Elite x3 (353) Windows's installer installed a package so.

A package with `MeglerApp.ini` in its install folder is one app's
(`shell/AppPackage.cpp`, `single()`):

- the library reads the package's `app` folder instead of `LocalState\apks`,
  and leaves it as it is;
- the shell opens the app as soon as the library has it, into the player (or
  its page, when it cannot run here), and Back after the app has ended leaves;
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

**The phone's folders** (`ui/SharedFolders.cpp`). Music, Pictures, Movies and
DCIM come from capabilities, which every package of Megler's declares alike. The
Documents and Downloads folders and a memory card come in only through the
folder picker (`player/StorageHost.h`), and Windows remembers a picked folder
for the one package it was picked in (`FutureAccessList`): no other package of
the publisher, not even with the publisher cache folder, gets it. So each
separate app has to be given each such folder by the user once:

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

## What follows (not done yet)

- Old builds' Android files in `<shared>\android`: each build of Megler that
  makes a package copies its own, and none is removed yet.
- How apps reach one another (Android's intents to another app, "share", "open
  with", content providers): within one process that is calls inside Megler;
  between packages it needs a way of its own (protocol activation, app services).
