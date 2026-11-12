# A reference Android device

`droidguard-reference.js` takes a metadata snapshot inside an existing Google
Play services process through Frida. Phase one identifies the installed version
and ABI before choosing version-specific interception points. The GMS APK used
in the qemu investigation requires Android 12; an Android 11 reference uses its
installed GMS version.

Select the Mi 4c explicitly, using its Frida device ID and a running GMS process:

```powershell
frida -D <Mi-4c-device-id> -n com.google.android.gms.unstable -l tools/droidguard-reference.js
```

If that process is absent, identify the running GMS process first. The agent
checks that its application's package is `com.google.android.gms`. It does not
spawn a process or trigger an attestation, check-in, device-key request or
location request. The Frida client and server must be compatible.

The output lines start with `DGReference` and contain JSON:

- GMS version, process ABI, Android SDK and requested-permission count.
- Public signing-certificate SHA-256 hashes, separating the legacy certificate,
  current signer and rotation history.
- System feature availability, display type/state and total memory.
- Existence and size of `device_key` in the current and device-protected storage
  contexts. It does not read the key. A missing file is inconclusive if a module
  uses a different context; an existing file may predate this run.
- Loaded VM library names/sizes and already-loaded DroidGuard class names,
  loader types and method signatures. It does not load the VM deliberately.

It collects no accounts, device IDs, locations, Wi-Fi data, HTTP traffic, tokens
or key contents, and replaces no methods. Frida's attachment and metadata
inspection can still affect what an integrity checker observes. A rooted
reference device is not assumed to pass DroidGuard; an observed successful
exchange is needed before treating it as a successful control.

This snapshot supplies the information for a separate, narrowly scoped VM and
program-exchange trace. The Megler diagnostic APK's `XposedBridge.hook0` is a
Megler API and is not used on the reference Android device.

The agent uses Frida's documented [Java bridge and loaded-class enumeration](https://frida.re/docs/javascript-api/#java).
It is intended for the Frida CLI, which supplies the Java bridge, including with
Frida 17. Initial local validation is `node --check tools/droidguard-reference.js`;
Execution was verified with Frida 16.2.1 on the selected Android 11 reference:
GMS 262634022, ARM64. The legacy signer and rotation history match the APK
signing evidence in `docs/MICROG.md`. An existing device-key file was observed
only by size; no key contents were read. A later, separately authorized program
exchange succeeded; see that document for the distinction between the snapshot
and the exchange, and for the attestation-error comparison.
