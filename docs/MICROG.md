# microG services

Megler can load a separately installed `com.google.android.gms` microG APK as
an optional service provider. Add/install the official microG Services APK in
Megler as an ordinary application. The library records its readable APK path
in the publisher cache (`shared/services/gms-apk.txt`). Clients built with this
version discover it at startup; existing client APPX packages must be updated.
The APK is not copied into every client. Updating microG replaces the recorded
path; an uninstalled or unreadable APK is not used. Restart clients after an
update. Installing Google’s proprietary GmsCore is not supported by this host.

This is in-process hosting with an isolated loader, not Android cross-process
Binder. Each client still has its own microG Application, service instances
and private microG state under its data directory. Installing and launching
microG does not create a shared background daemon, a common signed-in account,
or push delivery while clients are stopped. Account, push, provider and activity
integration require further work. Play Integrity is not implemented by this change.

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

## Verification

`tools/microg-fetch.sh` downloads upstream microG `v0.3.17.252432` and verifies
its pinned SHA-256. No build downloads it automatically.

With the normal Android-root dependencies present, `tools/build-tests.sh arm`
also runs the `microg` test when `build/microg/GmsCore.apk` exists. The client
fixture intentionally defines an incompatible `GetServiceRequest.CREATOR`.
It checks independent class identities, reflection and arrays, shared Binder
identity, resources/storage isolation, Advertising ID transactions,
unbind/rebind, and the actual `IGmsServiceBroker.getService` transaction 46
with a callback returning `ICommonService`.

A focused run of the built ARM runner is:

```sh
MG_JAVA_GMS_APK=/path/to/GmsCore.apk qemu-arm -L /usr/arm-linux-gnueabihf   /path/to/mg_tests --java microg /path/to/microg.apk /tmp/microg-data   /path/to/android-root -
```

The `microg` runner requires the PASS marker and fails on missing callbacks
or a reported fixture exception; an ordinary `--java app` exit code alone
is not evidence that a real application started successfully.
