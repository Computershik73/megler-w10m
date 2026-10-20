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
