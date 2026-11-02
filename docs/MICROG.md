# microG services

Megler can use a separately installed `com.google.android.gms` microG APK as an
optional service provider. Install the official microG Services APK in Megler
as its own application. After installation, Megler copies its APK once into the
shared Publisher Cache and records the installed Windows package identity and
cache token. Client APPX packages do not contain GmsCore; at launch, the runtime
mounts the registered APK from the shared cache. Reinstalling or updating
microG publishes a new cache entry, so already running clients can finish with
the version they loaded. Uninstalling microG clears its registration, and
clients stop using it after restart. If automatic publication fails, launching
the standalone microG app retries registration. Installing Google’s proprietary
GmsCore is not supported by this host.

GmsCore services currently execute inside each client process, with a separate
Java class namespace and ELF library scope. The APK is installed and stored once,
but Megler does not yet run a centralized microG daemon or provide Android
cross-process Binder. Each client has its own service instances, but microG's
state is one for all apps (see "Shared state"). Push delivery while clients are
stopped and live cross-client provider state remain unfinished. Play Integrity
is not implemented, and nothing here works around a check.

## Shared state

The core mounts `/data/megler-services` on `<storage>/services/data`, beside the
shared storage (the publisher's shared folder when storage is shared, else the
data folder). A client keeps microG's state in
`/data/megler-services/com.google.android.gms`, and microG's own package, run as
an app (its settings), has that same folder for its private files
(`core/src/run/mg_run.c`). What the user sets in microG's settings (device
registration, cloud messaging), the check-in's Android ID and microG's
databases are therefore every client's. Preferences are read when a process
first opens them: a change made while another app runs reaches it at its next
start.

## Check-in and broadcasts

`megler.MicroG` reads microG's receivers and routes a broadcast that names
microG's package or one of its receivers to them (`megler.Broadcasts.send`).
When microG's code first runs in an app (its Application is created), its
receivers that list `BOOT_COMPLETED` hear it once in that process, as after
Android's boot. microG's check-in trigger then checks in when device
registration is on and the last check-in is older than twelve hours. microG
keeps registration off until it is turned on in its settings; Megler turns it
on (`checkin_enable_service`, `MicroG.registrationOn`) unless the user has set
it there, since Play Store has no Android ID without a check-in and its front
page refused every request ("Server error", package 570). Under qemu, with
registration on, Play Store's process checked in ("Checked in as ..."),
microG stored the Android ID and filled its GServices database, and Play Store
read the Android ID from GServices on its next start. GServices
(`com.google.android.gsf.gservices`) is microG's own provider when microG is the
Google Play services (`megler.GmsProviders`, `PackageManager.resolveContentProvider`):
Google Services Framework, mounted beside it for Google's own Play services,
answered it before, and its database never had microG's Android ID.

## Accounts

`android.accounts.AccountManager` keeps accounts in
`/data/megler-services/accounts/accounts.json` (`megler.Accounts`), one file for
all apps, read again when another process wrote it. Requests that need an
account type's authenticator bind its service -- microG's `GoogleLoginService`
for `com.google`, which is not exported, as the system binds it -- and call it
off the main thread with the caller's package, uid and pid, as
AccountManagerService does. An authenticator's screen (microG's sign-in) is
started for a caller that gave an activity; when it opens in microG's own app,
`addAccount` ends when the new account appears in the shared file. A caller
that starts the screen itself for a result (Play Store calls `addAccount` with
no activity and starts the intent it gets with `startActivityForResult`) gets
the result the sign-in sets as it finishes (`RESULT_OK`): `megler.AppResults`
passes it through `/data/megler-services/results/`, and microG's app brings the
caller to the front again. There is no account chooser, no visibility, and no
session API.

A content provider's caller (`ContentProvider.getCallingPackage`) is the app:
every caller is in the app's one process, whose uid and pid are the app's, and
microG's providers hold the package they are told against the one the pid
names. It was the provider's own package, and microG's account provider, which
an app's `GoogleAuthUtil` asks for the Google accounts, refused every app:
"UID [10100] is not related to packageName [com.google.android.gms] (seems to
be com.google.android.apps.classroom)". The test app `microg` asks it
(`get_accounts`): past that check it reads the caller's certificate, which the
unsigned test app has none of ("CertData to small"); an app from a store is
signed.

## Parcels between a client and microG

Megler's `Parcel` keeps a Parcelable as the object itself. A client and microG
have different classes of the same names (the client library's
`GetServiceRequest`, `Feature`, `Scope`), so a reader that names its creator or
class loader gets an object of its own class: one of another class of that name
is written out and read in through the reader's creator, as between processes
(`core/framework-overlay/android/os/Parcel.java`). The typed readers also read
Android's own layout (the count, then each item written out in place), which the
client library's SafeParcelWriter writes itself. Without both, microG's broker
could not read Play Store's `GetServiceRequest`. Bundles are still passed as
they are; a Parcelable of the client's class inside a Bundle reaches microG as
the client's object.

## Dynamite modules

microG loads a client's dynamite module (measurement, provider installer)
through a `PathClassLoader` it makes over its own APK. The runtime makes such a
loader another name of microG's class scope (`mg_dvm_art.c`): the module's
classes are microG's, and its native libraries load from microG's APK in
microG's ELF scope. The provider installer thus puts microG's Conscrypt
(`GmsCore_OpenSSL`) first for the client, as on Android. Classes a module takes
from its client (`MERGED_CLASSES`, Cronet's API) are not followed yet; the log
says so when a module names any.
## Class isolation

The failed implementation reverted in `1adae04` appended GmsCore's DEX files
to the client's global class path. Client Google SDK classes then displaced
microG classes with the same names and different fields, notably
`GetServiceRequest.CREATOR`.

`megler.ServiceClassLoader` now assigns all DEX files in a service APK a
separate namespace. Definitions, linked classes, DEX type/member caches,
superclasses, interfaces, reflection field/method types and reference arrays
resolve in that namespace. Framework/libcore classes remain shared. Class
mirrors identify the defining loader, and GC retains the loader. JNI class
wrappers do not overwrite the client's wrappers; FindClass in a scoped native
method uses its defining namespace. This is an explicit service-loader path;
ordinary arbitrary Android ClassLoader delegation is still not fully implemented.

## A module's own scope (child scopes)

Google Play services loads each of its Chimera and Dynamite modules (Cronet,
the ads module, the Dynamite loader itself) through a class loader of its own
over the module's APK: a `DelegateLastClassLoader` once SDK_INT is 29 or more
(Android 10's four-argument constructor, which `core/libcore-overlay` adds to
Android 9's class), or a `PathClassLoader` of Google's that looks in its own
files first; its parent is the service's loader, another module's, or a filter
over them or the boot class path. Such a loader's classes, and those of any
`DelegateLastClassLoader`, get a scope of their own, from 16 on
(`mg_dvm_art.c`, child scopes): a name is looked for among the platform's
classes first, then in the loader's own files and its parent's scope, in the
order the loader asks them (its own first for a delegate-last loader, its
parent's first otherwise; a parent of the boot class path alone adds nothing).
The parent's scope is found by walking the loader's parents, and through a
filter loader of no files of its own, its fields. A child's native libraries
come from its APK in a linker scope of the same number, so the Cronet module
loads its own `libcronet.<version>.so` (stored in the APK) and not the one Play
Store embeds. A loader whose parents reach the app's own loader and that asks
its parent first (an app's plug-in) still puts its classes on the app's path;
so does every loader before the app's own is made, and a file with no DEX in it
gets no scope. Which service a child's code is of (a thread's owner, the
uncaught exceptions of a service's thread, guarded native calls) is its root
scope's (`mg_dvm_scope_root`).

Before, a module's classes went on the app's one class path, where the app's
classes of the same short obfuscated names won: Google Play services 17's ads
module called Play Store's `kk.a` (NoSuchMethodError), and 26's modules ended
Play Store 53 with "Cannot cast ruh to algy" and a NoSuchMethodError of
`j$.util.Objects`. The data app's step `delegate-last-scope` checks the order
with a DEX of its own (`module-src/`, carried as `assets/module.dex`): a
`DelegateLastClassLoader` gets the module's `Twin`, a `PathClassLoader` whose
parent is the app's gets the app's.

An attribution context of Google Play services' (`createAttributionContext`,
which version 26 makes for much of its work) is its package context itself:
`ContextWrapper` now asks its base for one, as Android's does, and the base of
the service's context is the app's, which bound the service's own services as
the app. A binding meant for Google Play services that none of its services
takes is said in the log ("no service of Google Play services binds ...").

A service of Google Play services is given one `onBind` for each intent that is
different as Android's `Intent.filterEquals` has it -- its action, data, type,
identifier and categories, not its extras (`MicroG.bindKey`) -- and the clients
of the same intent share that binder, as on Android; each new one is a line in
the log ("bound ... for ..."). They were told apart by the action alone. GmsCore
26's container services (`GmsApiService`, the bound brokers) serve every API of
the modules they hold, and the intent a client is given for an API by the
chimera provider's dynamic lookup (`BoundService.getStartIntent`) is the same
action for all of them, `com.google.android.chimera.BoundService.START`, with
the API in its data (`chimera-action:<the API's action>`): the broker made for
the first API a container was asked for was handed to the clients of all the
others, which it refused ("Unexpected service id: 343" to `GoogleAuth.API`, and
40, 41, 270... about a hundred a minute). Google Play services' own sign-in did
not connect, and WhatsApp's search for its Google Drive backup failed until it
said it took too long (package 486). The microG test checks the keys.

## Verification

`tools/microg-fetch.sh` downloads upstream microG `v0.3.17.252432` and verifies
its pinned SHA-256. No build downloads it automatically.

With the normal Android-root dependencies present, `tools/build-tests.sh arm`
also runs the `microg` test when `build/microg/GmsCore.apk` exists. The client
fixture intentionally defines an incompatible `GetServiceRequest.CREATOR`.
It checks independent class identities, reflection and arrays, shared Binder
identity, resources/storage isolation, Advertising ID transactions,
unbind/rebind, native `libconscrypt_gmscore_jni.so` loading in the service ELF scope, and the actual `IGmsServiceBroker.getService` transaction 46
with a callback returning `ICommonService`.

A focused run of the built ARM runner is:

```sh
MG_JAVA_GMS_APK=/path/to/GmsCore.apk qemu-arm -L /usr/arm-linux-gnueabihf   /path/to/mg_tests --java microg /path/to/microg.apk /tmp/microg-data   /path/to/android-root -
```

The `microg` runner requires the PASS marker and fails on missing callbacks
or a reported fixture exception; an ordinary `--java app` exit code alone
is not evidence that a real application started successfully.
