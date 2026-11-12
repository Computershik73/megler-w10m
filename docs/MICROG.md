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

The client library asks which module to load of Google Play services'
provider (`content://com.google.android.gms.chimera`), under the class lock
of its `DynamiteModule`. A thread that asks the provider before their
Application is attached waits for the app's main thread to attach it
(`MicroG.attachOnMain`, ten seconds at most), and ends the wait when their
Application is attached by anything the main thread runs. It does not wait
for a main thread that waits for it: one that stands waiting to enter a
monitor the asking thread holds, or one whose holder waits for one of the
asking thread's (`Native.mainWaitsForMe`, the runtime's `mg_dvm_waits_for`,
asked every 50 ms of the wait), attaches nothing until the thread goes on,
and the provider then answers before their Application is attached; the log
says so once ("I/MicroG: Google Play services' provider is asked by a thread
(...) holding a monitor that the main thread waits to enter"). Citymapper's
`Application.onCreate` initializes Google Maps, which loads a module under
that class lock, while a thread of the measurement library's, the lock held,
asks the provider for its own: the app stood ten seconds at every start
(0.1.0.690). On Android the provider is their process's and answers whatever
the app's main thread does.

## Google's own Play services: their process, and their module scans

Google's own Play services (not microG) run in the app that hosts them, and
their application is told that this process is its own: the process name of
their `ApplicationInfo` is the app's (`MicroG`'s constructor, said in the log:
"Google Play services' application runs in this process, ..., as its main
one"), which is what `Application.getProcessName` gives their code here. Their
components keep the process names of their manifest.

Their code takes the process whose name is its application's for its main one
(`bjej.i` in 26.41.31: `getApplicationInfo().processName.equals(
Application.getProcessName())`), and only there does `GmsModuleFinder`
make their module configuration again at once when a scan finds it changed
(`aeac.z`, its reason "Features changed", "Device properties changed",
"Verify failed" or "Unmigrated"). Anywhere else the scan asks for another
(`startService(MODULE_SCAN)`, which `ConfigUpdateIntentOperation` takes) and
leaves it to that one. With "com.google.android.gms" there and the app's
name here, no scan was ever the main process's, and each asked for the next:
five a second, without end, in every app that hosted Google's own Play
services. The log showed it as "Beginning GMS chimera module scan (logged 1000
times)" every minute or so, the same three files opened 200 times in 8 to 9 s
(`current_fileapks.pb`, `stored_modulesets.pb`, `ChimeraConfigService.xml`),
and a collection every three seconds, 30 to 40 MB of each made by the thread
"[com.google.android.gms.chimera.container.intentoperation..." (Aliucord,
Facebook, VTB, Ozon and Play Store on 0.1.0.671). The other readers of that
comparison in 26.41.31 are the measurement's tag of its process's kind and
the feedback screens; and `PhenotypeCommittedOperation` scans again after a
change of flags in the main process only.

`MicroG.start` counts the starts of their services by what they were started
for, and one asked for a hundred times in ten seconds is said, with what asked
(three times at most each): "Google Play services asked for <action> ->
<service> 100 times in N ms". The test app `gms-host` hosts a stand-in for
their package with one service (`gms-own`, no microG in it) and starts it:
"main process true application=megler.test.gmshost current=megler.test.gmshost"
(`tools/build-tests.sh arm`).

## Google's own Play services: their network location

Google's Play services find the phone from the Wi-Fi networks around it (their
network location module, in the app's process here, started by their fused
provider). Three things it needs were missing, and are there now (docs/JAVA.md,
"Wi-Fi, and Google's network location", which says where each was found in
26.41.31): scans through `WifiManager` (`startScan`, the broadcast,
`getScanResults`; it uses them where `getSystemService("wifiscanner")` is null,
as it is here); Google Location Accuracy, `network_location_opt_in` in Google's
settings (`content://com.google.settings/partner`, Google Services Framework's
provider), which `megler.LocationAccuracy` asks of the user once an app that
holds the location binds their location API, and writes; and `PendingIntent`'s
sends with an `OnFinished` and options, by which the module hands each fix to
the fused provider, and which sent nothing. Their fixes are seen on that way
(`megler.LocationService.sent`) and said in the log. The module also runs
on Google Play services' own alarms (`GmsAlarm`: `AlarmManager`'s forms with
an `Executor` and a `WorkSource`, which did nothing up to 0.1.0.684, so
that a phone scanned once and never again), asks for the cells
at each scan (`requestCellInfoUpdate`, answered now with none), and asks
its server with an `X-Goog-Spatula` header of Google Play services' AppCert
service, signed with the device key where the phone has one (JAVA.md says
what a phone without one sends). With microG nothing of this is asked:
microG's own location backends are its own.

## Google's own Play services: DroidGuard

DroidGuard is Google Play services' check of the device it runs on (their
module `com.google.android.gms.droidguard`). For a flow -- the check-in, the
device key, a Play Integrity token, a sign-in -- it asks Google's server for
a program (`androidantiabuse/v1/x/create`), runs it in a virtual machine the
server names (an APK of 24 classes with a native library `libd<12 hex
digits>.so`, kept in `app_dg_cache/<the APK's SHA-1>/`, from
`www.gstatic.com/droidguard/`), and hands the machine's bytes to whoever
asked, who sends them to Google with its own request. Here the module runs
in the app's process with the rest of Google Play services: the machine's APK
gets a class scope of its own under theirs ("a dalvik/system/DexClassLoader
over .../app_dg_cache/.../the.apk: scope N of its own" in the log), and its
library loads in their ELF scope ("ld: libdB21738DAE92C.so at ..."), after
an `UnsatisfiedLinkError` for the library's path inside the APK, which the
machine's own Java answers by copying the library out of it, as it does
where a linker cannot load from an APK.

Since 26.4x Google Play services first asks for a "full" program, runs it,
and sends its result back asking for a "fast" one (DroidGuard's "fast
refresh": the log's tag `DGFRR`, `bvre` in 26.41.31, `byts` in 26.43.30),
which then serves every flow that may use it; without one each flow asks
for a program of its own, as before.

**What fails, and where.** On the Lumia 650 (0.1.0.685) and the Elite x3
(0.1.0.681), with Google Play services 26.43.30, every flow logs "E/DGFRR: FR
error ... bywg: byteCode" and "W/DG: [flow devicekey] DroidGuardRunner.
initSignalCollection() exception" (checkin, cryptauth_synckeys_request,
po-token-fast, constellation_verify and recaptchabase-android likewise), and
the device key is refused ("W/GLSUser: [AppCertManager] Exception while
requesting key: ... dmyc: Parsing Exception ... Protocol message end-group
tag did not match expected tag", then "[DeviceKeyStore] Cannot load key:
Device key file not found"). Under qemu, with 26.41.31's armeabi-v7a APK as
the app and the same machine (AA6305A5..., `libdB21738DAE92C.so`), it is the
same, and there it was followed through (`trace-calls-within-com.google.ccc.
abuse.droidguard`, `trace-syscalls`, `trace-files-libdB`, `trace-calls-bvts`,
`trace-calls-java.io.PipedOutputStream`):

- no class-loading or native execution failure was observed on this path:
  the machine's classes load, its library loads and runs, `initNative` and
  `ssNative` return, and it produces results of 1 to 2 KB ("DroidGuard.ss
  returns [B of 1898: 0a06..."), the "full" one going to Google in the "fast"
  request;
- Google's server answers that request with a signed answer that holds no
  program -- no `byteCode` (field 1, the first of the fields 26.41.31's
  `bvrp.a` looks for, and the one it names) and no extra bytes to carry back
  (field 9: "bvts.<init>(Ljava/lang/String;[B)V "byteCode" [B of 0");
- the device-key request (`android.googleapis.com/auth/devicekey`, with the
  result of the "devicekey" flow's own program) is answered with Google's
  HTML page "Error 400 (Bad Request)" (1664 bytes) where its protocol buffer
  was expected. Google's other answers come whole: the check-in succeeds,
  the programs' answers verify their signatures, and on the Lumia the
  account tokens' answers decrypt.

**What the machine is told here that no Android device tells it.** Through
JNI it reads the displays (`Display.getType()`, a stub here, says
`TYPE_UNKNOWN` for the phone's own screen), the memory, Google Play services'
package and signatures, and the features (`android.hardware.strongbox_keystore`,
`android.software.device_id_attestation`, `android.software.verified_boot`:
none here); and it makes two EC keys in `AndroidKeyStore` with an attestation
challenge (`unstable.<hash>.A-...` and `.B-...`), which Megler's key store
refuses ("ProviderException: Failed to generate attestation certificate
chain"), as a device whose keymaster cannot attest -- Google Play services'
check-in asks for such a key too ("KeyAttestationHandler: Failed to generate
key attestation"). By number it looks for `/sys/fs/selinux/enforce`, which
every Android device since 4.3 has, and finds nothing there. And the device
it is told of is a Pixel of 2016 (`sailfish`) on Android 14, with a
fingerprint Google never built, and a 32-bit ARM one alone.

**What was tried, under qemu**, with code and flags of the experiments'
own, not kept in Megler: the attested pairs made as Android's software
keymaster makes them where there is no trusted environment (AOSP's
`system/keymaster`: its software attestation keys and certificates, the
key's description at the security level Software, checked with openssl),
first with AOSP's EC chain, whose batch certificate expired in January 2026,
then with its RSA one, unexpired, together with a `/sys/fs/selinux/enforce`
that says 1. The machine took both: it read the file, its pairs were made,
and its "full" result grew from 1898 to 3750 bytes with their chains in it.
Google's answers were the same: no "fast" program, an empty extra, and the
device key's "Error 400". These experiments did not fix either failure.

**What this establishes, and what it does not.** The immediate failure is
the program-creation response's missing `byteCode`, after native execution
of the "full" program. In 26.41.31, `bvre.a` obtains and runs "full", puts
its result in the "fast" request, and calls `bvrp.a` again. That parser
verifies the response signature through `bvro.a` before checking the
`byteCode` presence bit; a failed signature has a different exception,
"Creation response signature verification failed." The `byteCode` error
therefore does not mean a missing native entry point or a bytecode
interpreter exception. The device-key protobuf error is a separate failure
to parse an HTML response; it does not by itself identify why that request
was rejected.

Successful native returns and the size of the results do not establish
that Megler answered every probe with Android's semantics. Neither response
explains Google's decision. The missing attestation and other environment
differences above are candidates, not an established root cause; the two
software-attestation experiments do not prove that hardware attestation is
the only remedy. No change has been demonstrated to obtain a device key or
a Google Wi-Fi location fix. Play Integrity and SafetyNet were not validated
by these runs.

**Rechecked with 26.43.30, 2026-11-11.** A fresh qemu run on the current
integration base, with versionCode 264330027 (190300-1006944126,
armeabi-v7a), reproduces the same path. The APK was obtained from
[APK Cafe](https://apk.cafe/download?file_id=3543596/google-play-services),
SHA-256 `5e4c6fc21493dfa5113bbb7b5194a972649dfdc7e3e08e772393702d2033da8c`;
Android's `apksigner verify --print-certs` verifies its signature and reports
certificate SHA-256 `7ce83c1b71f3d572fed04c8d40c5cb10ff75e6d87d9df6fbd53f0468c2905053`.
This is the same version and variant as the phone log, not an APK extracted
from the phone. Its decompiled `byts.a` has the full-to-fast sequence above,
and `byud.a` has the same signature and field-presence checks as `bvrp.a`.
The fresh run downloads the same AA6305A5... VM, loads its native library,
and returns 1960 bytes from `DroidGuard.ss`; `byuc.a` then returns 1 (the
fast response's signature verifies), followed on that thread by
`bywg.<init>(Ljava/lang/String;[B)V "byteCode" [B of 0`. Check-in succeeds.
The result is also reproduced with 26.41.31 on fresh data (1939 bytes from
the full VM, followed by `bvts` with the same missing field and empty extra).

The same 26.43.30 APK was then hosted by a separate diagnostic app under
qemu, through `MG_JAVA_GMS_APK`, with Google Services Framework beside it.
The client binds the real DroidGuard service and invokes GMS's own
`arcf` (`AppCertManager`) on a worker thread. The service connects, the full
VM returns 1921 bytes, the fast response's signature verifies, and its
missing `byteCode` throws as above. Check-in succeeds; the Android ID read
by `brtb.d(context)`, which `arcf.b` uses, is nonzero. The device-key
response is 1664 bytes starting with HTML, followed by `dmyc: Parsing
Exception` and the missing device-key file, as on the phone. This reproduces
the failure with GMS inside a client's process, not just as the main app.

One further candidate was tested in that client on separate fresh data.
Megler reports its fixed Google certificate (`f0fd6c5b...`) for GMS, whereas
this APK's certificate is `7ce83c1b...`. Before binding, the probe replaced
the hosted GMS `PackageInfo.signatures` and `signingInfo` with the APK's
actual signer, read by `ApkSigner.fromSigningBlock`; the probe logs that the
replacement was applied. The full VM then returned 1956 bytes, but the
verified fast response still lacked `byteCode` and the device-key response
was still 1664 bytes of HTML. Replacing those two fields alone did not fix
the failure. The experiment is confined to the diagnostic client; no
certificate substitution change is kept in the runtime.

**The complete full-to-fast exchange.** The opt-in diagnostic app
`core/tests/app/droidguard` captures GmsCore 264330027's request protobufs
(`byvy.a`), the exact response bodies and signatures given to its verifier
(`byuc.a`), and the full VM's result (`byun.c`). Its hooks call the original
methods and return their results unchanged. The hook bridge is loaded in a
child DEX scope, outside the parent class path the VM searches. The app
captures at most 128 calls, with at most 2 MiB per byte array, in its own
`files/capture` folder. It is built with the other Java test apps and is run
explicitly, not by the normal suites; it needs internet access and Google's
26.43.30 APK mounted through `MG_JAVA_GMS_APK`.

On fresh test data, an independent protobuf reader finds the full VM's 1882
bytes unchanged in field 10 of the fast request. The fast response is
exactly 30 bytes: field 2 is an empty URL, field 3 the 20-byte VM checksum,
field 4 is 604800 seconds, and field 6 is true. Field 1 (`byteCode`) is absent.
OpenSSL independently verifies its SHA-256/RSA signature with the public key
from `byuc`, so this is not just Megler's signature verifier accepting it.
GMS carries field 6 to `bytx.d`; after parsing, `byts.a` would throw `bytt`,
whose message is "Server requested the module to retry the full collection."
Its parser first throws for the missing bytecode, however. This retry flag
does not identify the signal the server objected to.

The full reply's program has a ten-second lifetime. In a timed run the reply
arrives at elapsed time 88545 ms and the fast request starts at 89335 ms:
790 ms later, including 428 ms running the VM. Expiry of that window does
not explain this failure. Another fresh run uses
`no-plt-rewrite-libdB21738DAE92C.so`; the linker confirms zero PLT entries
rewritten, but the same verified 30-byte response without bytecode returns.
Leaving this library's PLT unchanged alone does not fix it. The main payload
of the VM result is opaque in these captures; its bytes and the retry flag
do not by themselves establish which environment check failed.

To repeat the capture under qemu after building the test app, with the same
heavy-job lock and `MEGLER_JOBS=4` as any test run:

```sh
data=$(mktemp -d /tmp/megler-droidguard.XXXXXX)
MG_JAVA_GMS_APK=/path/to/GmsCore.apk \
MG_JAVA_GSF_APK=/path/to/GoogleServicesFramework.apk \
qemu-arm -L /usr/arm-linux-gnueabihf /path/to/mg_tests --java app \
  /path/to/droidguard.apk "$data" /path/to/android-root - \
  0 720x1280@320 wait:120000
python3 -I tools/droidguard-inspect.py \
  "$data/apps/megler.test.droidguard/data/files/capture"
```

`tools/droidguard-inspect.py` checks field 10 against the preceding VM result
on the same thread and describes the response's fields without printing
device identifiers or the token. It reports mismatches and malformed
captures as errors. A successful inspection means the capture is consistent,
not that DroidGuard obtained a fast program. Instrumentation may affect the
VM's observations; a proposed fix must also pass an uninstrumented run.

**The device-key token and an independent HTTP submission.** A separate
fresh 26.43.30 client run captures `alkl.b("devicekey", map)` and the `jofz`
message passed to `atvj.b`, without the native-memory inspection used in
other experiments. The flow downloads its own signed program, initializes
and runs it, and returns a non-null 2996-byte base64 token. The independent
protobuf reader finds that token unchanged in field 1 of the 3047-byte
request. The request also contains a nonzero Android ID, a session nonce,
`missing_token`, and SDK/GMS version metadata. A nonempty token is therefore
being sent; the fast-program failure alone does not establish that the
device-key request has no DroidGuard token.

In another fresh run, the diagnostic hook retains the device-key request
before calling the original network method and throws deliberately, so GMS
does not submit it. Its first submission is instead by WSL's `curl`, over
HTTP/1.1, with the captured protobuf and GMS headers (`app`, `gmscoreFlow`,
`gmsversion`, `device`, and `Content-Type: application/x-protobuf`). That
3022-byte request, carrying a new 2971-byte token, gets HTTP 400 and 1664
bytes of Google's HTML error page too. This is not a replay of an already
submitted token. The alternate client changes HTTP/TLS defaults and has no
successful Android control; this does not certify every detail of Megler's
transport. It does reproduce the rejection outside that transport.

Both experiments clear only the diagnostic client's `arcf` retry timestamp
to make it attempt the request during the short test. They neither replace
the token nor change the keystore, package certificate or VM's answers. No
runtime change from these experiments is retained. The server's reason for
rejecting the request remains unknown; no device-key or Wi-Fi-positioning
fix has been demonstrated.

**Signing-certificate rotation changes the interpretation of that experiment.**
The same APK's v3 signing block has proof-of-rotation attribute `0x3ba06f8c`:
its first certificate hashes to
`f0fd6c5b410f25cb25c3b53346c8972fae30f8ee7411df910480ad6b2d60db83`,
and its next to the `7ce83c1b...` signer above. This is the
[AOSP v3 rotation structure](https://source.android.com/docs/security/features/apksigning/v3).
Android's legacy [`PackageInfo.signatures`](https://developer.android.com/reference/android/content/pm/PackageInfo#signatures)
returns the oldest certificate after rotation. The native trace asks for
`GET_SIGNATURES` (64), so Megler's old `f0fd6c5b...` value matches that API's
expected certificate for this APK. Replacing it with the current signer was
an experiment, not a demonstrated correction of the legacy field. It must
not be used as evidence that the old certificate caused DroidGuard's failure.
This does not validate the separate `SigningInfo` API's rotation semantics.

**Native field reads and the hosted package's permissions.** A temporary
JNI field trace on 26.43.30 shows that the full VM asks for GmsCore's
`PackageInfo` with `GET_PERMISSIONS`, then reads `requestedPermissions` as
null. The service host parsed components but never those manifest tags;
`GET_PERMISSIONS` could also take its basic version/signature-only path.

The host now reads `uses-permission`, `uses-permission-sdk-23` and
`uses-permission-sdk-m` outside the application, including declarations after
it, deduplicates names, and respects `maxSdkVersion` as the client's manifest
reader does. `GET_PERMISSIONS` asks for the complete manifest. Its granted
bits agree with the hosted package context's existing permission policy.
The gms-host/gms-own fixture checks this on a cold query: the hosted
package's own three permissions, one duplicate, an expired declaration,
and one declaration after the application. The new check fails against the
old framework (59 checks, 1 failed) and passes with the change (59 checks,
0 failed). A fresh, uninstrumented hosted GMS run with the changed framework
still throws `bywg("byteCode", empty)` after the full VM returns 1888 bytes;
its AppCertManager still fails to obtain a key.

This corrects an observed answer to the VM, but is not a demonstrated
DroidGuard fix. In separate client experiments, supplying all 401 applicable
permissions from the exact GMS APK still gets the verified 30-byte fast
response without bytecode; supplying both those permissions and the APK's
actual signer gets the same response. Neither experiment obtains a device
key. The native field instrumentation and certificate substitution are not
retained in the runtime.

A further fresh run changes `Display.getType()` from its stub's
`TYPE_UNKNOWN` (0) to `TYPE_INTERNAL` (1), using the populated permissions
and keeping the correct legacy signing certificate. The hook logs each
replacement, but the fast response is still verified, 30 bytes long and
without bytecode, and no device key is obtained. That answer alone does not
fix either failure; the display override is confined to the diagnostic app.

**What a Spatula header proves.** In 26.43.30, `arcd.a(package)` reads
the key through `arcg.b()` (`device_key` on disk), attempts `arcf.b()` if it is missing,
and can still return a nonempty header if no key was obtained. Its base64
protobuf (`hyfk`) then carries only the app/certificate message in field 1
and a device identifier in field 3. With a device key it additionally sets field 2
to the HMAC-SHA256 from `arcf.c`, and fields 4 and 5 to the key metadata.
Thus the presence of `X-Goog-Spatula` is not a signing check: look for its
nonempty field 2. That establishes local MAC generation, not server
acceptance or a location fix. Do not copy a captured header or its device
identifier into the repository.

**Reference Android and attestation errors (2026-11-12).** On the explicitly
selected rooted Android 11 reference device, GMS 26.26.34 (262634022) reports
both signing certificates in its rotation history and the oldest certificate
through the legacy API, as above. A scoped EC key-generation probe with a public
test challenge fails with `ProviderException` caused by
`android.security.KeyStoreException`, internal error `-100` (unimplemented).
The probe removes only its own temporary alias and checks that it is absent.
No existing device key is read or removed.

That device nevertheless completes a fresh full/fast exchange. Its ordinary
Cronet request fails with `ERR_CERT_INVALID` (internal code -207); standard
Android HTTPS to the same host validates TLS. With the user's explicit approval,
a temporary Frida agent sends the original GMS-built program requests through
that verified HTTPS client, retaining Google's protobuf and RSA verification.
Neither TLS verification nor the VM result is replaced. The full response has
79,460 bytes of bytecode and a 10-second TTL. The native VM returns 24,987 bytes;
those exact bytes become the fast request's field 10. Google returns a verified
fast response with 82,668 bytes of bytecode, TTL 604800, and no retry-full request.
The VM observes the `-100` attestation failure during this successful exchange.
This rules out unsupported attestation alone as an explanation of Megler's
failure; the Android ABI and GMS release differ from the phone's ARM32 26.43.30.
Only class names, packet structure, lengths and verification results are
exported from this reference exchange, not its requests, tokens or identifiers.

Megler's attestation failure originally had no cause, and the generated
`KeyStoreException` discarded the code and message. The provider now attaches
`KeyStoreException(-100, "Not implemented")`; the exception preserves its code
and message and classifies that failure as permanent, system-wide and
`ERROR_UNIMPLEMENTED`, following the
[Android exception mapping](https://android.googlesource.com/platform/frameworks/base/+/refs/heads/main/keystore/java/android/security/KeyStoreException.java).
Other codes currently use the public API's fallback classification, rather than a complete KeyMint error table. The data app's
`keystore-certificates` test fails before the change (`error=0`, 276 steps
passed, one failed) and passes afterward (`error=-100`, 277 steps passed,
445 runner checks, zero failed). A fresh uninstrumented 26.43.30 qemu run still
fails the fast-response bytecode check after the full VM returns 2057 bytes
(2064 with Android 14 error wording),
and device-key acquisition still fails. This is an Android API compatibility
fix, not a demonstrated DroidGuard fix.

**Matching the reference release and ARM32 program.** The opt-in capture app
also supports GMS 26.26.34 (262634022), using `bohf.a`, `bofj.a`, and
`bofu.c`/`bofu.b` for the same pass-through captures. Under qemu this release
also selects full refresh: the VM returns 2134 bytes, those exact bytes become
fast field 10, and Google returns the same verified 30-byte response without
bytecode. Its full/fast timings are 245 ms for the VM and 72 ms for the fast
request. Changing GMS releases therefore does not remove the failure.

A separate offline diagnostic APK, with no network permission and its own
storage, runs the exact ARM32 VM APK and full bytecode from that qemu exchange
on the reference device. The first run returns 22,957 bytes on Android and
2205 bytes in Megler. A bounded JNI-name tracer in that APK observes additional
collection on Android; the exact result length varies with state and tracing
(about 33.9 KB in later Android runs). It exports only class/method/field names,
signatures, lookup success, and result lengths. It never exports the result.
The comparison uses no GMS private storage. Matching SDK 30 and the two observed
unavailable API lookups (`getDeviceId` and `isTransientFailure`) inside the
isolated Megler diagnostic still returns only 2129 bytes. Those experimental
API overrides are not runtime fixes and are not retained.

A separate offline experiment supplies a simulated SELinux status file containing
`1`: the VM's `fstatat64` and `openat` then succeed, but it still returns just
2124 bytes. The simulated file is not retained, since it would not establish
SELinux enforcement on Windows. The native property trace finds requests for
`ro.boot.verifiedbootstate` and `ro.boot.vbmeta.digest`; both are absent in
Megler and their values are empty on the reference device. This observation
alone does not establish equal native property-presence semantics.
The smaller result
alone does not identify the failed signal or establish a server verdict for the
offline run; it supplies a reproducible native-collection comparison independent
of GMS version, VM architecture and network transport.

**Validation after the attestation-cause fix (2026-11-12).** With all temporary
native tracing and simulated answers removed, `tools/build-tests.sh arm` and
`tools/build-tests.sh win` both finish with exit code 0 and zero failed checks.
ARM's main runs report 8751 and 8910 checks; Windows reports 8210 and 8332.
The data app passes all 277 steps on both runs, including the strengthened
keystore assertion (445 and 460 runner checks). Both suites run serially under
the shared heavy-build lock with `MEGLER_JOBS=4`. Local logs are
`build/droidguard-keystore-arm.log`, `build/droidguard-keystore-win.log` and
`build/droidguard-keystore-exit.txt`. These are compatibility-suite results;
the real GMS reproductions above still fail DroidGuard and device-key issuance.

A subsequent offline qemu `-strace` run with `trace-hooks`, on the same retained
runtime, returns 2131 bytes and exits normally. It emits no guest hook-code
trace and shows no direct host-kernel opens of the three inspected paths
(`/dev/binder`, `/sys/fs/selinux/enforce`, `/proc/cpuinfo`). This limited check
does not demonstrate a generated-code or syscall-interception failure. It is
not a full audit of all kernel calls or of the VM's environment checks.

**Comparing the same signed APK on Android and Megler.** Later offline runs
use the same signed diagnostic APK on both systems. The app verifies its
73,066-byte program at runtime and both copies contain the same 293,088-byte
ARM32 native library. Android returns about 33.9 KB and Megler about 2.2 KB.
Turning off Megler's JIT or matching the reference's reported CPU capabilities
and core count leaves Megler's result near 2.2 KB. Compressing the native entry
in the VM APK forces the Android run to extract the library and still gives
about 34 KB. These controls cover the tested JIT setting, CPU metadata and
loading paths.

The JNI trace also records whether a lookup succeeded and the caller's
offset in the VM library. The trace does not attribute the first
`ClassLoader.getParent` on Android to the VM. The common `getPackageInfo` and
`generateKeyPair` lookups come from the same VM instruction, `0x3cff0`, on both
systems. Both runs make
two attested key-generation attempts and get a `ProviderException` each
time. After the second error, Android looks up `ServiceManager.getService`
from `0x3cd28`, gets a binder, calls `dump` and continues with more Java
queries. Megler proceeds to package checks without that lookup. Android
generates a non-attested key much later in the trace. The order does not
establish that this is an immediate fallback from either attestation error.

The early feature-query results agree in call order. The primary display's
type and flags differ. In the isolated APK, setting the test screen to the
reference's dimensions and supplying its type and flags still gives 2,229
bytes in Megler. With the Megler type and flags supplied to the Android run
the VM returns 33,899 bytes. The changes to those answers were confined to
the diagnostic APK, which has no network permission.

Android CPU samples and qemu instruction coverage also show that Android
enters native routines at `0xd966` and `0xdaf8` that are absent from the
Megler run. The sampling and coverage methods measure different things.
Together with the JNI sequence they give points to investigate in the
interpreter, but neither gives the reason for the branch or decodes the VM's
result. The offline comparison uses a stand-in for GMS's RuntimeApi and does
not test which result Google will accept.

**The VM responds to changes in its executable code.** Before running the
same ARM32 program, the offline APK compares the VM's executable segment
with the file. All 289,648 bytes agree on Android. The Megler run differs
at 556 bytes with ordinary patching and at four with
`no-plt-rewrite-libdB21738DAE92C.so`. Those four bytes are at `0x2e18`,
where Megler turns the VM's ARM `svc #0` (`0xef000000`) into a `udf`
(`0xe7f00af6`) so that Megler, rather than the host kernel, does the
Android call. Megler returns 2,228 bytes in both runs.

The same APK on the Mi 4c returns 34,018 bytes with the original code. It
returns 2,104 bytes with that exact replacement in place, while the
diagnostic's signal handler routes all eleven calls at that instruction
through Android's own syscall entry and returns their results to the VM.
When it installs the replacement and then restores the instruction before
running, the result is 33,920 bytes. An equivalent Thumb instruction at a
different VM address also reduces the result to 2,103 bytes. These controls
show that changing executable bytes is sufficient to shorten the VM's
collection, independently of Megler's syscall answers.

On ARM Windows Megler now treats `libdB21738DAE92C.so` like its other
libraries that inspect their code. It leaves the PLT alone and puts the
syscall at a hardware breakpoint. The same Megler syscall handler does
the call and resumes after it. The qemu host has no hardware breakpoints
for guest code and still uses the patched instruction. The Self-test loads
its own ARM library under the VM's name in its scratch directory. It checks
the original instruction and zero PLT rewrites on a host with breakpoints,
the trap instruction on qemu, and the shim's result for both the direct
call and a call through the PLT.

The offline controls establish one compatibility fault. Fast-refresh
acceptance and device-key issuance still need a run on ARM Windows with the
new loader behavior. The Android diagnostic package and its transfer files
were removed after the comparison.

**Following it on a phone** (flags in `Pictures\Megler\diag\<package>\flags`):
`trace-syscalls` (a library's calls by number with the path each names:
"/sys/fs/selinux/enforce" = -2 from `libdB21738DAE92C.so+0x2e18`, its own
`syscall()`), `trace-files-libdB` (the files that library opens or looks at
through the C library), `trace-calls-bywg` with 26.43.30 (`bvts` with
26.41.31: the refusal, and the answer's extra bytes), and
`trace-calls-com.google.ccc.abuse.droidguard.DroidGuard` (the machine's Java:
the flow it was made for, and `run`'s and `ss`'s results with their length).
On an older package with hardware breakpoint support, also set
`pristine-libdB21738DAE92C.so`; the fix selects that mode by default.
Expect the loader's "left as the file has it" line with one system call
at a hardware breakpoint, zero PLT rewrites, and no warning about falling
back to a patched instruction. With `trace-syscalls`, calls from
`libdB21738DAE92C.so+0x2e18` should still reach Megler.
For 26.43.30 also use `trace-calls-byud` to locate the creation-response
parser's failure relative to those returns. Keep the sequence for the same
flow and thread: the "full" machine's return, the fast-refresh request,
then `bywg`'s construction with "byteCode" and the extra's length. A
class-loader or native failure before the return is a different result
from the qemu trace above. After a prospective fix, check that a device key
is stored and loaded on the next start, and that Google's network location
actually supplies a fix; the absence of `DGFRR` alone proves neither.

## Google's own Play services from Play Store: their splits

Play Store updates Google's own Play services as it installs any app from a
bundle: a base and splits of the phone's density and languages (26.43.30 on a
Lumia 650: "its base and 10 split(s) kept", for 320 dpi in English and
Russian). The library keeps them as an app's (`<apk>.splits`), and Google
Play services' own package runs with them. An app that hosts them had the base
alone: the shell copied `GmsCore.apk` into `<shared>\services\<token>` and
nothing else, and their resources were the base's table. A resource whose
every value is a density's -- the Google logo `googlelogo_standard_color_74x24`
of `ProductLockupView`, its default when the layout names none (in 26.41.31's
universal APK PNGs for mdpi to xxhdpi and a vector for anydpi, nothing of no
density; `0x7f0804bc` in 26.43.30) -- is in the density's split alone, and
"Manage your Google Account" (their account settings' Compose screen) and
"Help and feedback" (`HelpChimeraActivity`'s layout) ended Play Store on
`Resources$NotFoundException: Resource ID #0x7f0804bc` (0.1.0.685 and 686).
Play Store's own table has an entry under that number: the lookup was in
theirs.

Now `registerServiceApk` copies the splits beside the base
(`<token>\GmsCore.apk.splits`, `files::copyFilesWhole`: each whole, a copy
registered before them gets them at the next registration), the player passes
the folder (`megler_run_config.gms_splits_dir`) and holds its files open as it
holds the base, and the core mounts each split beside `GmsCore.apk`
(`/system/priv-app/GmsCore/split_<name>.apk`, `Native.gmsSplitPaths`; the log
says "Google Play services are in N APKs"). `megler.MicroG` makes their
`AssetManager` of the base and the splits: the splits' tables are merged into
the base's (`Resources.meglerPackageTable`, each cut to the languages as the
base's is), a file the base has not is read from the split that has it, and
their `ApplicationInfo` names the splits as Android does. A split with code
or native libraries is said in the log; nothing of either is taken from one.

And a context of theirs made for another configuration
(`createConfigurationContext`: AppCompat's `attachBaseContext2` wraps an
activity's base in one where the app's night mode or locales differ, and their
own code makes such contexts) is their resources under that configuration. It
was the app's, through the app's `ContextImpl`, which `ContextWrapper` asks.
So was their package context's theme, from which each activity of theirs
begins (`ContextThemeWrapper` copies its base's): the app's own, whose
attributes stood under their numbers in theirs wherever their theme set none.
It is a theme of their resources now, the platform's default for their target
(`Theme.DeviceDefault.Light.DarkActionBar`), and an activity of theirs that
names no theme (their application names none) has that default too, not the
number of the app's theme.

An activity of theirs whose `onCreate` throws is taken off the stack at once
and never destroyed (`AppRunner.launch`), the one below resumed; the app goes
on, as it did. Their help screen failed so and finished itself, and its
destruction at the next idle threw in their lifecycle ("State must be at
least 'CREATED' to be moved to 'DESTROYED'").

The test app `gms-host` hosts a stand-in of theirs built as Play Store's
bundle (`gms-own`: a base and the splits `config.mdpi`, `config.xxhdpi` and
`config.en`), unpacked as the shell unpacks one for 160 dpi: an activity of
theirs reads a picture of their density's split alone and a word of their
English split, and both through a context of another configuration, and finds
no attribute of the app's theme in its own (`gms-host` has a theme with one);
and one whose `onCreate` throws.

## Android's Geocoder

An app's `android.location.Geocoder` asks Google Play services' geocode
provider, as Android's location manager does: their service for
`com.android.location.service.GeocodeProvider` -- Google's own
`com.google.android.gms.location.geocode.GeocodeService` (not exported; in
26.41.31 a Chimera proxy whose `GeocodeChimeraService` gives the binder of a
`com.android.location.provider.GeocodeProvider`, its legacy provider below
Android 15, `GeocodeProviderBase` from 15 on), or microG's
`GeocodeProviderService` (OpenStreetMap's Nominatim). It is bound as the system
binds it and kept (`MicroG.bindAsSystemNow`, on the main thread), and its
binder, local, is called in place on a thread of the Geocoder's own with the
app's package, uid, attribution tag and locale (`GeocoderParams`). The
framework has the library's `GeocodeProvider`, which neither carries, the
provider's interfaces (`IGeocodeProvider`, `IGeocodeListener`) and Android's
`Address`. Where no provider is there, `isPresent()` is false and a request
has no addresses and no error, as on Android. A synchronous request asked on
the main thread binds the provider there before it waits; what the provider
wants of the main thread while it answers waits with it (a phone's provider
has a process of its own). Whether Google's server answers their geocoder
here is for a phone to show ("I/Geocoder: a place asked ... address(es)").

Windows' own `MapLocationFinder` is no way round: without a Bing Maps key
(`MapService.ServiceToken`) it answers `InvalidCredentials` to a reverse and
a forward geocode alike (tried on this PC, Windows 11 26300).

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

The package context's own attribution tag (where none was asked for) is
`megler.app` only where the user lets Google Play services' own features use
the location, and null otherwise: Google's GmsCore refuses a location request
of its own whose identity (`ClientIdentity.c(Context)`, made of that tag) has a
null tag or its location module's ("internal location usage has a forbidden
attribution tag"), and those requests are its own features' in every app's
process -- personal safety, semantic location, place suggestions --, not the
app's. One answer for every app, asked over the app the first time such an
identity is made, kept in Megler's settings (docs/JAVA.md, "Permissions").
microG makes no such identity, and is never asked about.

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
