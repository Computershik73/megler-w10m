# Megler on an Xbox

An Xbox One or Xbox Series X|S in Developer Mode runs UWP packages that it is
given, and its processor is x64: Megler for desktop Windows on x64
([DESKTOP.md](DESKTOP.md)) runs Android's `x86_64` code there as it does on a
PC. What the console's package needs besides is a manifest for the Xbox, and a
way to install packages: the console's Device Portal, through which
DesktopMode_uwp's App Installer installs them on a console (Windows's deployment
engine, which a phone's and a PC's Megler install with, is not known to be an
app's to use there, and is not tried). A retail console in Retail Mode installs
no package that is not the Store's, and Megler is not there.

Nothing of this has run on a console yet (there is none here): what is proven
is the package's build and manifest, and the portal's client against a portal
of the tests' own (below, "Proven").

## The package

    tools/build-appx.sh x64 --xbox --java

makes `app/Megler/AppPackages/Megler_<version>_Xbox_x64.appx` (the name
[RELEASES.md](RELEASES.md) gives a package of another Windows), with its
objects in `build/appx-xbox-x64`. It is the x64 package, built from x64's
inputs and with every option x64's takes -- the core's ELF of `x86_64`
(`tools/build-desktop.sh x64 app`), x64's ANGLE, the Android root of `x86_64`
with Android's System WebView of `x86_64` (`build/webview/x86_64/webview.apk`,
`tools/webview-fetch.sh x86_64`: the WebView Google's sign-in pages open in),
and Google's apps of x64's processor where `--bitgapps`, `--gms`,
`--play-store` and `--microg` give them -- with two differences:

- **The shell is compiled with `MEGLER_XBOX`**, and only this build has
  `shell/DevicePortal.cpp`, the client of the console's Device Portal, in it:
  the apps' packages, Megler's own update, the list of the apps' packages and
  their uninstalls go through the portal (`shell/AppPackage.cpp`), and the
  Library asks for the portal's user name and password where it wants them
  (`ui/SeparateApps.cpp`). A phone's and a PC's Megler have no word of the
  portal: they install through Windows's deployment engine
  ([PER-APP-PACKAGES.md](PER-APP-PACKAGES.md), "Installing").
- **Its manifest is the Xbox's**, made from `Package.appxmanifest` as it is
  staged:

  | | phone, PC | Xbox |
  |---|---|---|
  | `TargetDeviceFamily` | `Windows.Mobile` and `Windows.Universal`, 10.0.15063.0 | `Windows.Xbox` alone (no PC takes the package), 10.0.19041.0 (the console's Windows of 2020; Developer Mode keeps a console up to date), tested 10.0.22621.0 (the SDK it is built with) |
  | `chat`, `phoneCallHistoryPublic` | declared (a phone's text messages and calls) | left out: an Xbox has neither |
  | `rescap:expandedResources` | no | declared: an app runs with a game's processor and graphics resources instead of an app's on an Xbox |
  | `Description` | Android games on Windows 10 Mobile | Android apps on Xbox |

  Everything else stays: `codeGeneration` (Android code made executable, which
  the emulators that run in Developer Mode need too), `packageQuery` and
  `packageManagement` (Megler asks whether a package the portal installed is
  its user's, below), the libraries, the camera and microphone, the
  publisher cache folder. The apps' own packages are made on the console from
  this manifest, and keep it (`core/src/appx/mg_appx_manifest.c` keeps a
  capability it does not know, `expandedResources` among them).

On an Xbox the OTA ([RELEASES.md](RELEASES.md), `ui/Ota.cpp`) takes
`Megler_<version>_Xbox_x64.appx`: a Megler whose device family is
`Windows.Xbox` asks for that name and no other.

## On the console

1. **Developer Mode**, with the Xbox Dev Mode app from the Store.
2. **The Device Portal on**: Dev Home, Remote access settings, Enable Xbox
   Device Portal. With its authentication on, Megler asks for its user name
   and password the first time it needs the portal (one field, then the
   other, the password's not shown) and keeps them in Windows's credential
   locker; refused ones are forgotten, and asked for again.
3. **Megler let reach the console's own address.** Windows keeps an app from
   connecting to the machine it runs on unless it is exempted, and only an
   administrator exempts one: from a PC on the same network, open an SSH
   session to the console (user `DevToolsUser`, the password the Visual
   Studio PIN Dev Home shows) and run

       CheckNetIsolation.exe LoopbackExempt -a -n=Megler_7p434m16tbgc6

   (Megler's package family name, which its message says too) -- again after
   every start of the console, which forgets it. This is how DesktopMode_uwp
   (<https://github.com/Pedro1234-code/DesktopMode_uwp>, its LOOPBACK.md)
   reaches the same portal from its own App Installer. Where the portal cannot
   be reached, the Library says all of this.
4. **Megler itself**, the first time: from a PC's browser, the console's
   Device Portal (`https://<the console's address>:11443`), Add, the
   `.appx`. It needs no package beside it: its C runtime is linked in, and its
   ANGLE carries its own.

Then Megler installs the apps' packages and its own updates through the portal
from the console itself.

## How Megler talks to the portal

`shell/DevicePortal.cpp`, from what DesktopMode_uwp's App Installer
(`factoryos-10x-shell/Services/Files/AppPackageService.cs`) and Microsoft's
WindowsDevicePortalWrapper do:

- the portal is `https://localhost:11443`; its certificate is one the console
  made for itself, which nothing vouches for, for another name: taken as it is
  (untrusted, wrong name, expired, incomplete chain, no revocation answer),
  and only from an address of the machine's own;
- each call begins with a GET of `/api/os/info`, with `CSRF-Token: Fetch`
  (as the wrapper asks), for the portal's cookies: they go back in the
  `Cookie` header, the `CSRF-Token` one in `X-CSRF-Token` too, by hand, as
  `tools/phone.py` sends them (left to the HTTP client's own cookies, a
  phone's portal refused an install 403);
- with the user name and password, Basic authentication on every request, as
  both send it; a 401 asks for them, or says the portal did not take them;
- an install: the portal's state asked first, and an install of another
  client's (204) waited for, a minute at most; then a POST of
  `/api/app/packagemanager/package?package=<file name>` with a multipart form,
  each file a part named by its file name, read where it lies, the share sent
  told as it goes; its answer may be the result itself (a refusal); then
  `/api/app/packagemanager/state` every second until it says, 204 meanwhile,
  20 minutes at most: `Success`, else its `Reason` (or `CodeText`) and `Code`;
- no certificate goes with a package: DesktopMode's installer sends none, a
  console in Developer Mode taking a package whatever signed it. Should a
  console refuse one for its certificate (0x800b0109), it is sent again with
  the certificate beside it, which the portal installs, as a phone's did;
- after an install, whether Windows has the package for the user Megler runs
  as (`FindPackageForUser`), and where not, `RegisterPackageByFullNameAsync`
  for them: DesktopMode found the portal, which deploys in a service's
  context, to register a package for the console's DefaultAccount alone. Its
  notes call that unproven on a console, and so it is here: where Windows
  will not answer, the log says so and nothing else is done;
- the list is `/api/app/packagemanager/packages` (every package of the
  console, whichever user it was registered for: Windows's own list would be
  of Megler's user alone), an uninstall a DELETE of
  `/api/app/packagemanager/package?package=<full name>`.

What the portal answered is in Megler's log (`device portal: ...`): its
cookies' names (never their values), each refusal, each install.

## What else an Xbox has otherwise

- **Input.** An app on an Xbox is in mouse mode unless it asks otherwise: the
  gamepad's stick moves a pointer and A presses. Megler asks nothing, so the
  library is used with the pointer, and the player takes a pointer's presses
  as touches, as it takes a mouse's on a PC: an Android app is tapped with A,
  and B is Back. A game's own gamepad input (Android's `InputDevice`, key and
  motion events of a controller) is not there yet. (DesktopMode's notes say an
  Xbox gives its mouse APIs only to an app signed as Microsoft's.)
- **The screen.** A XAML app on an Xbox is drawn at 200 percent on 1920 by
  1080: 960 by 540 of its pixels, within the television's safe area (the
  library: `UseVisible`), the player on the whole of it. The phone-shaped
  window Megler opens on a PC is not asked for there, and the window is never
  resized or turned: an app that asks for portrait gets a surface of that
  shape in the middle of the television, letterboxed (docs/DESKTOP.md, "The
  window").
- **Memory and graphics.** Microsoft's table of an Xbox's resources: an app
  in front has 1 GB of memory, a share of two to four processor cores and of
  45 percent of the graphics processor, Direct3D 11 at feature level 10.1 on
  an Xbox One (11.0 on a Series X|S), and 128 MB in the background; a game 5 GB,
  four cores of its own and two shared, the whole graphics processor, and it is
  suspended and ended in the background. A Megler of an Android app with
  Google Play services and a WebView open (Play Store's sign-in) needs more than
  an app's gigabyte -- Play Store reached 877 MB on a 32-bit phone, and x64's
  pointers are twice as large -- so the manifest declares `expandedResources`,
  which is what makes a UWP title a game on an Xbox (Microsoft grants it a
  title at a time in the Store; whether a console in Developer Mode honours it
  for a package of its own is to be seen, and Dev Home's App type, Game, is
  the other way to it). As a game, an app of Megler's is ended when it goes to
  the background: its music does not play on there as on a PC or a phone.
- **What an Xbox has not got**: text messages and calls (left out of the
  manifest), a sensor of the location, the folder picker
  (an Xbox has none for an app: the folders added in Megler's Settings and an
  app's Android Storage are not to be had there), the credential APIs of
  Windows Hello (an app's BiometricPrompt is told there is no hardware; and
  Windows's credential locker may keep the portal's password for a run alone),
  a phone's MTP: an update copied to `Pictures\Megler` is the phone's way, and
  the OTA is the console's.
- **Google's apps and the WebView** are x64's (above): nothing of the console's
  stands in their way that is not a PC's but the memory (above) and the
  graphics processor's feature level, at which ANGLE gives OpenGL ES 3.0 all
  the same. Their packages, Play Store's and Google Play services', are
  installed through the portal like any app's, Megler's files alone (their
  APKs are in the shared folder since 0.1.0.645): some 60 MB each sent.

## The app's first picture, and "Starting <app>…"

The first report from a console (0.1.0.671, F-Droid): the app started, the
player stayed on its busy overlay -- "Starting F-Droid…" and the ring -- and
the app's picture showed for a moment as Megler was closed. No log came with
it. What the player had then could keep the overlay up while the app drew,
each of which is gone now:

- **The overlay went by the Presents that had succeeded** (WindowHost's
  count of them): a picture that came and whose Present failed, or that the UI
  thread put on the panel again at another panel size (a Present of its own,
  which the count never saw), kept it up. It goes now by the pictures that
  came, whichever way and whatever their Present said
  (`WindowHost::pictures`): a game's frame in ANGLE's texture, the views drawn
  in memory, drawn on the graphics processor into the window's texture or
  straight into the swap chain's buffer. Every frame of every app reaches the
  panel through `player/WindowHost.cpp` -- ANGLE never has a swap chain of its
  own on it -- so there is no other way to count.
- **It was set only at the player's poll**, every 250 ms on the UI thread, and
  after the system's media controls had been asked: a call of theirs that
  Windows refused threw out of the poll (AppShell catches it, "ui: refresh
  failed") before the overlay was set, at every poll for as long as the call
  failed. The window now tells the player at once, from the thread the first
  picture came on (`onFirstPicture`, posted to the UI thread at high
  priority); the poll sets the overlay before anything else; the media
  controls' refusals are caught where they are made
  (`player/MediaControls.cpp`).
- **The surface changed as the app starts**: Windows gives the player the
  whole television (UseCoreWindow) a moment after it opened in the safe area,
  and the surface follows -- told the moment `megler_run` has returned, before
  the app's first picture. That is fine: the first picture comes at the new
  size, and the overlay goes with it (`mg_tests --java first ... xbox`,
  below).

What the console's log says now, in this order (the numbers an example's):
the swap chain, with the panel it went to (`window: swap chain 1280x720 x3 on the drawing's (GfxHost)
device (feature level 10_1); on a panel of 960x540 DIPs at 2.00x2.00 of the
display's pixels for each`); the first picture (`window: the app's first
picture, 1280x720, came 2410 ms after Play: the views drawn on the graphics
processor into the window's texture, copied to the swap chain; Present put it
on the screen` -- or `Present failed, 0x...: it is not on the screen`, or `it
was not presented: ...`); and the overlay (`player: the busy overlay is hidden
2412 ms after Play (at once, on the window's word): the app's first picture
came at 2410 ms, ...`). With no picture of the app's on the screen 10 s after
Play, and again at 60 s, the player says where the window stands (`player: 10 s
after Play no picture of the app's is on the screen (the app runs): ...`):
whether a swap chain was made and why not, how many pictures came, how many
Presents went and failed, the last failure, the panel's size. A UI thread held
up says so itself (`ui: the UI thread has not answered for N ms: ...`, with
where it stands). If the overlay goes and the screen stays black, the first
picture's line says whether Present put it there, and if it did, what is left
is the console's composition of the panel.

The flag `as-xbox` (a file of that name in Megler's `LocalState\flags`) has a
PC's Megler take the Xbox's ways where the shell has its own
(`platform::isXbox`, `platform::asXbox`): its window opened at 960 by 540,
never turned for an app's wish, a portrait app's surface the strip in the
middle. What Windows alone does on a console -- 200 percent, the safe area,
Developer Mode's App or Game -- a PC does not; Megler's OTA stays the PC's. The
flag `fail-present` takes every Present of the window as refused without
calling it, for the player's way with pictures that cannot be shown.

## Proven

On this PC, 2026-11-08, of the first picture (no console):

- `tools/build-tests.sh win` runs `mg_tests --window-windows`: the shell's
  `WindowHost` itself, with no panel, on this PC's graphics processor -- a Java
  app's views drawn by the shell's drawing (into the window's texture, then
  straight into the swap chain's buffer), drawn in memory (`present_pixels`,
  `present_pixels_part`), a game's frame in a texture of a device standing for
  ANGLE's, and a picture whose Present is refused (`fail-present`): each
  counted as it came, the first kept and said in the log with Present's
  answer, the player told once, on the thread it came on, and told at once
  when it asks after; the window's state says the Presents that failed.
- `tools/build-desktop.sh x64 tests`'s `mg_tests --java first <hello.apk> ...
  pc` and `... xbox`: hello started as the player starts an app on a PC's
  window (500x950 at 200 dpi) and on an Xbox's television (1280x720 at 237
  dpi in the safe area, then the whole of it, 1422x800, and the safe area's
  insets told as the app starts), the overlay as the player now decides it,
  polled every 10 ms: gone at the first poll after the first picture (shown
  from the canvas, as the shell's graphics processor shows it) in both, the
  pictures after the change at the new size, the insets kept clear.

On this PC, 2026-11-07:

- `tools/build-appx.sh x64 --xbox --java` builds and signs
  `Megler_<version>_Xbox_x64.appx`, 170 MB with the WebView of `x86_64` in its
  Android root (261 MB of it); its `AppxManifest.xml` has the device family
  `Windows.Xbox` alone, no `chat` and no `phoneCallHistoryPublic`, and
  `expandedResources`; its `Megler.exe` has the portal's client, and the
  phone's `Megler.exe` of the same tree has none of its strings.
- `tools/build-tests.sh win` runs the client against
  `tools/device-portal-mock.py`, a portal of the tests' own that answers as an
  Xbox's (made from the wrapper and DesktopMode, not from a console), over TLS
  with a certificate made for another name: a portal that is not there, no
  user name and password and a wrong one (401), the token (the mock refuses a
  POST without it, 403), an install another client's holds up, a package of
  the core's own packer received byte for byte and listed under the full name
  Windows would give it, a refused install's reason and code, a file sent
  beside a package, an uninstall and one of what is not there.

To be checked on a console: that the package installs and starts (the
capabilities granted, `codeGeneration` among them, the core's ELF loaded,
ANGLE on the console's Direct3D 11); that the portal answers Megler on
`https://localhost:11443` with the loopback exemption, and how it refuses
without it; its authentication and CSRF as the mock has them; that an app's
package installs, is listed and starts from Dev Home or the console's
collection, and whether it is registered for the signed-in user; Megler's own
update through the portal (some 170 MB sent through Megler's memory); the
player's first picture on the television and its busy overlay gone with it (the
log's lines above); mouse mode in the player; whether `expandedResources` gives Megler a game's memory
in Developer Mode, and Google's sign-in in Play Store's WebView within it.
