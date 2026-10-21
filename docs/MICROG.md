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
cross-process Binder. Each client has its own service instances and private
microG state. Shared sign-in, push delivery while clients are stopped, and
cross-client provider state therefore remain unfinished. Play Integrity is not
implemented by this change.
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
unbind/rebind, native `libconscrypt_gmscore_jni.so` loading in the service ELF scope, and the actual `IGmsServiceBroker.getService` transaction 46
with a callback returning `ICommonService`.

A focused run of the built ARM runner is:

```sh
MG_JAVA_GMS_APK=/path/to/GmsCore.apk qemu-arm -L /usr/arm-linux-gnueabihf   /path/to/mg_tests --java microg /path/to/microg.apk /tmp/microg-data   /path/to/android-root -
```

The `microg` runner requires the PASS marker and fails on missing callbacks
or a reported fixture exception; an ordinary `--java app` exit code alone
is not evidence that a real application started successfully.
