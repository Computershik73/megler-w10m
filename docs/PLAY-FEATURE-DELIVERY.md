# Play Feature Delivery

An app published as an Android App Bundle can leave parts of itself out of
what the Play Store installs: *on-demand feature modules*, which the app asks
the Play Store for while it runs (Play Feature Delivery, the app's side of it
the Play Core library). This document is what such an app does, what the Play
Store does with the request, and how Megler passes the one to the other.

The app that showed the need is **TikTok Lite from Google Play** (issue #6,
Megler 0.1.0.699): it does not get past its first screen.

Status in one paragraph: the app's side and the Play Store's side of the relay
are written, and **proven under the tests** (the ARM build under qemu, six
runs, none failing) against a stand-in Play Store (`core/tests/app/playstore`)
and an app that speaks Play Core's interface by hand
(`core/tests/app/playclient`); **nothing of it has run on a phone**, and
nothing has run against the real Play Store or the real Play Core. Whether the
real Play Store answers a passed-on request the way its code reads is the open
question; the section [What is not known](#what-is-not-known) lists what a
phone has to show.

## The app: a small base and the rest on demand

TikTok Lite 47.7.4 (`com.zhiliaoapp.musically.go`, version code 470704) as
Google Play delivers it:

- the base's application class is `com.zhiliaoapp.musically.go.mini.
  MiniApplication`, a small "mini" app;
- installed with it (the manifest's `requiredSplitTypes`): the base's ABI and
  density splits and the modules `df_aa_base_1`, `df_aa_base_2`,
  `df_aa_base_3`, `df_edit_effects`, `df_edit_sticker`, `df_fusing`,
  `df_record_prop` and `post_video` -- ten split APKs with the user's phone;
- on demand (the base's `res/xml/splits0.xml`): `df_ecommerce`,
  `df_edit_filter`, `df_livesdk`, `df_music`, `df_vk`.

The real app is in `df_edit_filter`. The base's `MiniDynamicFeatureManager`
(in the app's own copy of `com.google.android.play.core.splitcompat`) runs
`SplitCompat.install`, looks for `df_edit_filter` among the modules Play Core
says are installed, the fused modules, `PackageInfo.splitNames` and its
`files/splitcompat/<version code>/verified-splits` folder, and with it there
loads `com.ss.android.ugc.aweme.host.shell.LiteApplicationProxy`, the full
app's application. Without it the mini app asks the Play Store for the module
and waits.

So this app cannot start without Play Feature Delivery working, whatever else
is right. (The log of the report also has `NoSuchFieldException:
mInstrumentation`, a field of `ActivityThread` the mini app swaps when it
hands over to the full app; that is a separate fault with its own fix.)

## What Play Core does

Read from the app's Play Core (obfuscated; version code 11004, which it sends
with each request) and the Play Store's side of the same interface.

1. It asks `PackageManager` for `com.android.vending`: installed, enabled and
   signed by Google, or every request fails with `PLAY_STORE_NOT_FOUND` (-14)
   and nothing is bound.
2. It binds the service of `com.android.vending` with the action
   `com.google.android.play.core.splitinstall.BIND_SPLIT_INSTALL_SERVICE`.
   A bind that is refused fails every waiting request (not with a
   `SplitInstallException`; the app sees a failed task).
3. It calls `ISplitInstallService` (descriptor `com.google.android.play.core.
   splitinstall.protocol.ISplitInstallService`), every call one-way:

   | Code | Call | Arguments |
   |---|---|---|
   | 2 | startInstall | package, list of Bundle, Bundle (options), callback |
   | 4 | cancelInstall | package, session id, [Bundle], callback |
   | 5 | getSessionState | package, session id, callback |
   | 6 | getSessionStates | package, callback |
   | 7, 8 | deferredUninstall, deferredInstall | as 2 |
   | 13, 14 | deferredLanguageInstall, -Uninstall | as 2 |

   A module is the Bundle `{"module_name": name}`, a language `{"language":
   tag}`; the options hold `playcore_version_code` and `event_timestamps`.
4. The answer comes to `ISplitInstallServiceCallback` (also one-way): 2
   onStartInstall (session id, Bundle), 3 onCompleteInstall, 4
   onCancelInstall, 5 onGetSession (each an int and a Bundle), 6 onError
   (Bundle with `error_code`), 7 onGetSessionStates (list of Bundle), 8 to 13
   the deferred calls' (Bundle).
5. The session's progress does not come through the callback. The Play Store
   **broadcasts** it to the app's package: action `com.google.android.play.
   core.splitinstall.receiver.SplitInstallUpdateIntentService`, the extra
   `session_state` a Bundle of `session_id`, `status`, `error_code`,
   `bytes_downloaded`, `total_bytes_to_download`, `module_names`, `languages`,
   and where they apply `user_confirmation_intent` and `split_file_intents`.
   Play Core registers a receiver for the action when the app adds a listener.

Statuses: 1 PENDING, 2 DOWNLOADING, 3 DOWNLOADED, 4 INSTALLING, 5 INSTALLED,
6 FAILED, 7 CANCELED, 8 REQUIRES_USER_CONFIRMATION. Error codes: -2
MODULE_UNAVAILABLE, -3 INVALID_REQUEST, -4 SESSION_NOT_FOUND, -5
API_NOT_AVAILABLE, -6 NETWORK_ERROR, -7 ACCESS_DENIED, -8
INCOMPATIBLE_WITH_EXISTING_SESSION, -9 SERVICE_DIED, -14
PLAY_STORE_NOT_FOUND, -15 APP_NOT_OWNED, -100 INTERNAL_ERROR.

### Two ways a module arrives

**Installed by the system.** The Play Store commits a `PackageInstaller`
session of mode `MODE_INHERIT_EXISTING` for the app's package, holding the new
split APKs alone; Android adds them to the installed app without ending it,
and the session's states run 1, 2, 3, 4, 5. The app sees the module in
`ApplicationInfo.splitSourceDirs` and `splitNames`.

**SplitCompat.** The Play Store keeps the downloaded split in its own files
and stops at status 3 (DOWNLOADED) with `split_file_intents`: one Intent for
each split, its data a `content://com.google.android.finsky.FileProvider/...`
URI with read permission granted to the app, its extras `module_name`,
`split_id`, `version_code` and `derived_id`. Play Core in the app, under a
file lock, opens each with `ContentResolver.openAssetFileDescriptor(uri,
"r")`, copies it to `files/splitcompat/<version code>/unverified-splits/
<split_id>.apk`, checks the copy's signing certificates against the app's
own, moves it to `verified-splits`, adds it to the app's class loader and
resources, and reports INSTALLED to the app's listeners itself.

The Play Store 53.7 chooses SplitCompat on Android 7 to 9 for an app whose
Play Core is 10300 or later (its flag `DynamicSplits.
min_playcore_client_version_for_splitcompat_on_nop`). Megler is Android 9 and
this app's Play Core is 11004, so **SplitCompat is the way this app's module
is expected to come**; and this app's Play Core sets its SplitCompat receiver
up on every Android version.

## What the Play Store checks

`com.google.android.finsky.splitinstallservice.SplitInstallService` of Play
Store 53.7.23, read from its decompiled code (obfuscated: the class names in
brackets are that build's). In the order a `startInstall` meets them:

| Check | Failing it |
|---|---|
| The package named is one of `getPackagesForUid(Binder.getCallingUid())` (`avyr.i`, on the thread the call comes in on) | no answer at all; its log has "Package name %s is not owned by caller." and "Split install access not permitted: %s" |
| The flag `DynamicSplits.dynamic_split_package_controller` lets the package (its default, `-`, reads as a list of packages kept out with none in it) | no answer; "Package name %s is not permitted by global flag." |
| The device is not of a class without the feature | -5; "Split install requested but this class of devices is not supported." |
| The request names modules or languages, of the right types | -3 |
| The Play Store knows the app as installed (its own record of the phone's packages, from `PackageManager`) | an error; "Split install requested but app not found, package: %s" |
| The app is owned: Play Core 10900 or later, or the account has it; and no other store owns its updates (`avyr.b`, `c`, `d`) | -15 (-5 to an old Play Core); "Split install requested but the app is not owned, package: %s", "Play cannot unlock package: %s" |
| The installed APK is Google Play's: the source stamp of the base APK (`avyr.a`; the file itself is read) | -15 or -5 |
| Not every module asked is installed already | "Start install for package: %s. Nothing to do (all modules already installed)", and the answer of a finished install |
| No session of the app with the same modules is running | that session's state is sent again |
| No session of the app with other modules is running | -8 |
| No request of the app in the last second | -7; "Split install start download throttled: %s" |
| The app has a process in front: one of `getRunningAppProcesses()` with an importance of at most 200 (the flag `persistent_importance_for_start_install_relaxed_foreground_check`) and the package as its `processName` or in its `pkgList` (`afvn.ah`) | -7; "Split install start download but in background: %s" |
| The Play Store has an account | -5; "Split install requested but no Play Store accounts on device." |

The first three are made before the call returns, the rest on a thread of the
Play Store's own. After them the log has "Start install for package: %s", the
callback's onStartInstall is called, and the download is the Play Store's
own: asked of Google under its account, for the app's version code and the
splits it has.

## Megler: the request passed on

On Android the app and the Play Store are two processes with Binder between
them. In Megler each Android app is a Windows package of its own
([PER-APP-PACKAGES.md](PER-APP-PACKAGES.md)), the Play Store too, and Windows
10 Mobile runs the one in front. What every app shares is the shared folder.
So a bind of another app's service has nothing to reach, and
`megler/Services.java` refuses it.

For this one service Megler stands in (`megler/PlaySplits.java`): the app's
process has a Binder that speaks `ISplitInstallService`, the Play Store's
process is started with the request and has it put to the Play Store's real
service, and the two talk through files.

    the app's process                     the Play Store's process
    -----------------                     ------------------------
    Play Core binds com.android.vending
      -> PlayStore.bind -> PlaySplits     (not running, or suspended)
    startInstall(modules, callback)
      -> <n>.request written
      -> Native.launchApp(Play Store,
           "megler.PlaySplits.<app>")  ->  AppRunner.startRequested
    (suspended: the Play Store is            -> PlaySplits.requested
     in front)                             <n>.events: {"taken":true}
                                           binds its own service, calls
                                           startInstall as the app's call
                                           the callback's calls, the states
                                           broadcast to the app's package
                                             -> lines of <n>.events
                                           split files copied to files/
                                           the request over:
    (in front again)                   <-  Native.launchApp(app)
    <n>.events read: the callback is
    called, the states are broadcast
    to Play Core's receiver

No change of the shell is needed. `Native.launchApp(package, activity)` is
what an app's `startActivity` for another app already uses; the shell starts
the other app's package with `megler:run?package=..&activity=..`, and there
`megler.AppRunner.startRequested(cls)` is called once the app's first activity
is up -- its first line now gives `megler.PlaySplits.<package>` to
`PlaySplits.requested`. Showing the app again is `Native.launchApp(app,
null)`.

### The folder and the files

`mg_run_files` (`core/src/run/mg_run.c`) mounts one folder for every app:
`/data/megler-play-splits`, which is `<shared folder>\services\play-splits`.
In it each asking app has a folder under its package's name:

| File | Written by | Holds |
|---|---|---|
| `<n>.request` | the app's side | one request, JSON; `n` counts from 1 |
| `<n>.events` | the Play Store's side | what came of it, a JSON object a line |
| `files/<session>/<split_id>.apk` | the Play Store's side | the split files of a SplitCompat session |

A request file is written whole (to another name, then renamed). A
`startInstall` is

    {"code":2, "package":"<app>", "asked":"[module df_edit_filter]",
     "list":[...], "options":{...}}

and a `cancelInstall` is `{"code":4, "package", "session", "withOptions",
"options"}`. `asked` is the modules and languages in the order asked, and is
what tells one request from another. A Bundle is a JSON object of its keys,
each value a pair `[type, value]`: `int`, `long`, `boolean`, `string`,
`bundle`, `intent` (an object of `action`, `data`, `type`, `package`,
`component`, `flags`, `extras`), `longs`, `ints`, `strings`, and `list` (of
such pairs). Nothing else is in what Play Core and the Play Store send; a
value of another type is left out and said in the log.

The lines of an events file, in the order they happened:

| Line | Means |
|---|---|
| `{"taken":true}` | the Play Store's process has the request |
| `{"callback":c,"session":s,"bundle":{..}}` | the callback's call `c` (`session` with 2 to 5, `list` in place of `bundle` with 7) |
| `{"state":{..}}` | a session state the Play Store broadcast to the app |
| `{"lost":code,"why":".."}` | the Play Store's side gave up, with the error for the app |

A request is **over** with `lost`, the callback's onError, a state FAILED,
CANCELED, INSTALLED or REQUIRES_USER_CONFIRMATION, or a state DOWNLOADED that
has `split_file_intents`. Then the Play Store's side asks the shell to show
the app again.

### The app's side

`megler.PlayStore.bind`, which already answers an app's bind of the Play
Store's licensing and integrity services, gives `PlaySplits.bind` the action
`BIND_SPLIT_INSTALL_SERVICE`; the app's `onServiceConnected` gets the
stand-in. (In the Play Store's own process `PlaySplits.bind` answers nothing,
and the bind finds the Play Store's own service.)

`startInstall`:

- no Play Store among the phone's apps (`megler.PhoneApps`): the callback's
  onError with **-14** at once. Megler tells every app that
  `com.android.vending` is installed (`megler.GooglePackages`, for the apps
  that will not start without it), so Play Core's own check does not see
  this, and the stand-in says it;
- the latest request asked for the same and ended well (INSTALLED, or
  DOWNLOADED with files that are still there): it is **answered again from
  its events**, with no word to the Play Store. This is what makes the relay
  work when Windows ends the app while the Play Store is in front: the app
  starts anew, asks again as it did, and gets the answer that was waiting.
  The Play Store itself tells an app again of a session it asks for twice;
- else request `n+1` is written and the Play Store is started. If the shell
  cannot start it: **-9**.

The stand-in then reads the events file every quarter of a second of the
app's own running time (the app is suspended while the Play Store is in
front) and gives the app each new line: a callback's call as that call, a
state as the broadcast, to the app's own receivers (`sendBroadcast` with the
app's package set, as the Play Store sends it). The errors it makes up
itself, all through the callback's onError as the Play Store's are:

| Error | When |
|---|---|
| -14 PLAY_STORE_NOT_FOUND | the phone has no Play Store app |
| -9 SERVICE_DIED | the shared folder is not there; the Play Store could not be started; it did not take the request in 20 s of the app's running time; it took it and said nothing more for two minutes; (from the Play Store's side) it has no split install service, or the service did not answer a call in 40 s |
| -5 API_NOT_AVAILABLE | a deferred install or uninstall (calls 7, 8, 13, 14), which are not passed on |
| -4 SESSION_NOT_FOUND | a cancel of, or a question about, a session that is none of a request this run of the app is waiting for |
| -100 INTERNAL_ERROR | (from the Play Store's side) a split file could not be copied out of the Play Store, or a state could not be written |

-9 is the truthful one of Play Core's codes for a Play Store that is there and
does not answer: it is what Play Core itself reports when its connection to
the service is lost. -5 would say the phone can never do it, and an app may
stop asking for good.

`getSessionStates` is answered from the latest request's events: its last
state, if it is not over; `getSessionState` the same for one session.

### The Play Store's side

`PlaySplits.requested("megler.PlaySplits.<app>")`, in the Play Store's process
once its first activity is up:

1. The app must be one of the phone's apps (`PhoneApps.find`), and its latest
   request not over. `{"taken":true}` is appended.
2. The Play Store's own service is bound, in this process, by
   `Services.bind` with the action and the package `com.android.vending` --
   the service of its manifest, made and bound as for any app's own service.
3. The request is written into a Parcel as Play Core's proxy writes it and
   given to the service's Binder with `transact`. For the time of that call
   `Binder.getCallingUid()` is the asking app's uid (`Binder.
   meglerCallingUid`, on that thread alone); `transact` here calls
   `onTransact` on the calling thread, which is where the Play Store asks.
   The uid is the one `megler.PhoneApps` gives the app everywhere, and
   `PackageManager.getPackagesForUid` of it names the app.
4. While a request is open, `ActivityManager.getRunningAppProcesses()` in the
   Play Store's process lists the asking app as a process in front. That is
   what Android would say of it: the app did ask from the front, and is
   behind only because the phone shows one app at a time.
5. The callback given is a recorder: each call becomes a line of the events.
   The session states are taken where the Play Store sends them:
   `megler.Broadcasts.send` gives `PlaySplits.broadcast` every broadcast, and
   one with the session-state action and an asking app's package becomes a
   line too (and goes nowhere else: its receiver is in another process).
6. A state with `split_file_intents`: each Intent's URI is opened here, in
   the process whose provider it names (`ContentResolver.openInputStream`),
   and copied to `files/<session_id>/<split_id>.apk`; the line written has
   `file:` URIs of the copies in place of the `content:` ones, and no
   `user_confirmation_intent`. Play Core in the app opens a `file:` URI the
   same way, and copies and checks the file as it would the Play Store's.
7. When a line ends the request, the shell is asked to show the app.

If the service says nothing to a call for 40 s the request is lost with -9:
the Play Store's stub drops a call it refuses without a word (the first two
rows of the table above), and the app must not wait for ever.

Nothing the Play Store verifies with Google is made up. It sees the app's
package, uid, version and signing certificates as Megler reports every app
of the phone to it -- the same answers it gets when it updates that app --
and its account, its ownership check and the download are its own.

### A module the system installs

For the first of the two ways, Megler's `PackageInstaller` now knows a session
that adds to an installed app. `SessionParams(MODE_INHERIT_EXISTING)` with
`setAppPackageName`: on commit, when each APK written is a split of that
package (its manifest names the package and a `split`) and the app is one of
the phone's, the files are put beside the app's other splits --
`<shared folder>\apks\<identity>\<token>\app.apk.splits\split_<name>.apk`
(`megler.Native.addSplit`, `core/src/drivers/mg_java.c`) -- and the session's
status is `STATUS_SUCCESS`. The app has the split from its next start: the
shell hands the splits folder to the core at each start, and
`ApplicationInfo.splitNames`, `splitSourceDirs` and the class loader have it.
Before this such a session was given to Megler's installer as if it were a
whole app named `<package>.apk`.

## The tests

Two test apps and six runs (`tools/build-tests.sh`; `core/tests/
mg_java_tests.c`, `run_playclient` and `run_playstore`):

- `core/tests/app/playclient`, package `megler.test.playclient`: asks for
  modules as Play Core does -- the bind, the transactions written by hand,
  the callback, the receiver;
- `core/tests/app/playstore`, package `com.android.vending`: a split install
  service that checks what the Play Store's checks on its calling thread and
  after (the caller's uid owns the package; the app is in front), then calls
  the callback and broadcasts the states. The module `extra` it installs with
  an inherit-existing `PackageInstaller` session; the module `files` it hands
  over as SplitCompat's, by a URI of its own provider. The split it carries
  is built from `carries/` in its folder (`tools/build-test-app.sh`).

The tests host runs one app in a process, as the phone does, so the runs
follow one another over one data folder. Each passes (26 to 36 checks a run):

| Run | What is checked |
|---|---|
| 1. client, no Play Store among the phone's apps | error -14 in under 10 s, no request written, nothing asked of the shell |
| 2. client, a Play Store that is not run | request 1 written, the shell asked to start the Play Store with `megler.PlaySplits.megler.test.playclient`, error -9 after 20 s |
| 3. the store, started with that | request 1 taken; the service sees the app's package owned by the caller and the app in front; the callback's onStartInstall, states 1 to 5 in order in the events; the split added to the app's APKs by the session; the shell asked to show the app |
| 4. client | the split in `splitNames` and `splitSourceDirs`, its class loaded; `extra` asked again is answered from the events (onStartInstall, then INSTALLED) with no new request; `getSessionStates` empty; a deferred call -5; a cancel of an unknown session -4; `files` asked: request 2, the shell asked |
| 5. the store | request 2 taken; states 1, 2, then 3 with the file; the provider opened in the store's process, the file copied to `files/42/files.apk`; the shell asked to show the app |
| 6. client | `files` asked again is answered from the events with a `file:` URI; the app copies it through `openAssetFileDescriptor` as Play Core does and loads its class |

What the runs do not cover: two processes alive at once (the app reading
events while the Play Store writes them; the files are written whole and the
lines appended whole, by reading); the shell's `launchApp` and `megler:run`
(the host records what was asked and starts nothing); Windows suspending and
ending the app; and the real Play Store and Play Core.

## What is not known

All of this needs a phone with Megler, the Play Store signed in, and an app
with on-demand modules installed from it. In the order they would show:

1. **Whether the Play Store's checks pass.** By reading they do. The log
   lines to look for in the Play Store's log are the ones in the table above:
   "Start install for package" is success; "is not owned by caller" or
   "access not permitted" would mean the calling uid or the package flag;
   "in background" the process list; "throttled" two requests within a
   second. Megler's own lines are tagged `MeglerPlaySplits` in both apps'
   logs.
2. **The Play Store's server-side flags.** `dynamic_split_package_controller`
   and the choice of SplitCompat are flags the Play Store gets from Google;
   the defaults are what was read.
3. **Whether Google gives this install the module.** The Play Store asks for
   the splits of the app's version under its account. An app installed by
   that Play Store is what it expects; an app installed from an APK file may
   be refused (-15 APP_NOT_OWNED, or -2), and that is Google's answer to
   give. Nothing in Megler changes it.
4. **Play Core's check of the copy.** It compares the split's signing
   certificates with the app's own, read through `PackageManager`; Megler
   reports the app's real certificates and the split is Google's file, so by
   reading it passes.
5. **SplitCompat on Megler's VM.** Adding the split to the class loader is
   reflection on `BaseDexClassLoader.pathList` and its `dexElements` and
   `nativeLibraryPathElements`, and on `AssetManager.addAssetPath`; Megler has
   Android 9's libcore, and none of it was run with this app.
6. **How long the Play Store's cold start takes.** The app waits 20 s of its
   own running time for the request to be taken; it does not run while the
   Play Store is in front, so only the time before Windows switches counts.
   If the Play Store was already running, `megler:run` reaches it in place.
7. **Whether the app's process survives the Play Store in front.** If it does
   not, the app starts anew when shown and asks again; the answer is kept
   (the tests' runs 4 and 6 are this case).
8. **Whether the Play Store's package may write where the app's splits are.**
   A module installed by a session is copied by the Play Store's process into
   the app's `app.apk.splits` folder of the shared folder, and SplitCompat's
   files into `services\play-splits`; both are the shared folder every
   Megler package has, but no package has written into another app's APK
   folder on a phone before. The core's line `java: a split is not added to
   <package>: ...` says when it cannot.

### Known gaps

- **Ten splits.** The core takes ten split APKs of a Java app (`MAX_SPLITS`
  in `mg_java.c`), and TikTok Lite from Google Play has ten already. A module
  that comes SplitCompat's way is not counted (the app loads it from its own
  files); one that the Play Store installs by a session would be the
  eleventh and left out at the app's next start, with a line in the log.
- **A module installed by a session reaches the app at its next start**, not
  while it runs, where Android adds it to the running app. The INSTALLED
  state is passed on as the Play Store sends it; an app that uses the module
  at once without SplitCompat would not find it.
- **REQUIRES_USER_CONFIRMATION** (a large download on a metered network) ends
  the request, and its `user_confirmation_intent`, a PendingIntent of the
  Play Store's, is not passed on: the app can show nothing with it.
- **cancelInstall**: this Play Core writes a Bundle before the callback and
  the Play Store 53.7 reads none there. The stand-in keeps what the app
  wrote and passes it on as written, so the mismatch, if it is one, is
  theirs as on Android.
- **Deferred installs and uninstalls** are answered -5.
- **A request the app gave up on** (-9 after 20 s) may still be taken by the
  Play Store later; its answer is then the one the app gets when it asks
  again.
- **Other apps' services in general** are still refused; this is the one
  interface passed on, and it is passed by files because the two processes
  are never both running on the phone.
