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
- No code integrity catalog (`AppxMetadata/CodeIntegrity.cat`) is made yet;
  whether the phone wants one for the executables is for the first install to
  say.

On a PC, `tools/build-appx-tool.sh` builds `appx-tool` (the same code, x64),
which packs a folder; `tools/appx-verify.py` checks a package independently:
each file's blocks against the block map, the package digests against the
signed ones, the message digest, and the RSA signature with openssl. Both a
package of makeappx's and signtool's and one of `appx-tool`'s pass it;
`makeappx unpack /v` unpacks `appx-tool`'s, and `signtool verify /pa` stops at
the certificate's root not being trusted on the PC (as it should, and not at a
digest). Installing one on the phone is still to be tried.

## What follows (not done yet)

- A tool that makes the package for an APK: Megler with the APK, the app's name
  and icon in the manifest, and the publisher cache folder as `/sdcard`. The
  Android root (libcore, the framework, `framework-res.apk`, the system
  libraries) is files Megler reads and loads itself, so it can live in the shared
  folder once instead of in each package; ANGLE and Megler's own DLLs stay in the
  package. The phone's Device Portal answers on 127.0.0.1 on the phone itself,
  and its install call takes the signing certificate, so such packages could be
  made and installed on the phone too.
- How apps reach one another (Android's intents to another app, "share", "open
  with", content providers): within one process that is calls inside Megler;
  between packages it needs a way of its own (protocol activation, app services).
