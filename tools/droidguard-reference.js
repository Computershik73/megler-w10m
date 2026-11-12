/*
 * Phase one on a reference Android device, through the Frida CLI:
 *   frida -D <Mi-4c-device-id> -n com.google.android.gms.unstable -l tools/droidguard-reference.js
 * Attach to a running GMS process. No method replacements, requests, key generation,
 * file contents, identifiers, accounts, locations, Wi-Fi data or raw tokens.
 * A stored key is evidence of an earlier successful request; absence is inconclusive
 * because a module may use a different storage context.
 */
'use strict';

function report(kind, data) {
    console.log('DGReference ' + JSON.stringify({ kind: kind, data: data }));
}

function errorType(error) {
    // Exception messages can contain paths or data. Only report the type.
    try { if (error.$className) return String(error.$className); } catch (_) {}
    return error instanceof TypeError ? 'TypeError' : 'probe failed';
}

function probe(name, action) {
    try { report(name, action()); }
    catch (error) { report(name, { error: errorType(error) }); }
}

if (typeof Java === 'undefined' || !Java.available) {
    report('error', 'Java bridge unavailable; load with the Frida CLI in a running Android GMS process');
} else {
    Java.perform(function () {
        const ActivityThread = Java.use('android.app.ActivityThread');
        const app = ActivityThread.currentApplication();
        if (app === null || String(app.getPackageName()) !== 'com.google.android.gms') {
            report('error', 'Attach to a running com.google.android.gms process');
            return;
        }
        report('runtime', { frida: Frida.version, arch: Process.arch, pointerBytes: Process.pointerSize });
        const pm = app.getPackageManager();
        const Build = Java.use('android.os.Build');
        const Version = Java.use('android.os.Build$VERSION');
        const Digest = Java.use('java.security.MessageDigest');
        function certificateHashes(signatures) {
            const result = [];
            for (let i = 0; signatures !== null && i < Math.min(signatures.length, 8); i++) {
                const bytes = Digest.getInstance('SHA-256').digest(signatures[i].toByteArray());
                let hex = '';
                for (let j = 0; j < bytes.length; j++) hex += ('0' + (bytes[j] & 255).toString(16)).slice(-2);
                result.push(hex);
            }
            return result;
        }
        probe('gms', function () {
            const info = pm.getPackageInfo.overload('java.lang.String', 'int')
                .call(pm, 'com.google.android.gms', 64 | 4096 | 0x08000000);
            const signing = info.signingInfo.value;
            const permissions = info.requestedPermissions.value;
            return {
                versionCode: info.versionCode.value,
                versionName: String(info.versionName.value),
                sdk: Version.SDK_INT.value,
                supportedAbis: Array.prototype.map.call(Build.SUPPORTED_ABIS.value, String),
                permissionCount: permissions === null ? null : permissions.length,
                legacyCertificateSha256: certificateHashes(info.signatures.value),
                currentCertificateSha256: signing === null ? [] : certificateHashes(signing.getApkContentsSigners()),
                certificateHistorySha256: signing === null ? [] : certificateHashes(signing.getSigningCertificateHistory())
            };
        });
        probe('features', function () {
            const result = {};
            ['android.hardware.strongbox_keystore', 'android.software.device_id_attestation',
             'android.software.verified_boot', 'android.hardware.ram.normal'].forEach(function (name) {
                result[name] = pm.hasSystemFeature.overload('java.lang.String').call(pm, name);
            });
            return result;
        });
        probe('display', function () {
            const manager = Java.cast(app.getSystemService('display'), Java.use('android.hardware.display.DisplayManager'));
            const display = manager.getDisplay(0);
            if (display === null) return { present: false };
            return { present: true, type: display.getType(), state: display.getState() };
        });
        probe('memory', function () {
            const manager = Java.cast(app.getSystemService('activity'), Java.use('android.app.ActivityManager'));
            const info = Java.use('android.app.ActivityManager$MemoryInfo').$new();
            manager.getMemoryInfo(info);
            return { totalBytes: String(info.totalMem.value) };
        });
        function keyStat(context) {
            const file = context.getFileStreamPath('device_key');
            const exists = file.isFile();
            return { exists: exists, bytes: exists ? String(file.length()) : '0' };
        }
        probe('deviceKeyCurrentStorage', function () { return keyStat(app); });
        probe('deviceKeyProtectedStorage', function () { return keyStat(app.createDeviceProtectedStorageContext()); });
        probe('nativeVmModules', function () {
            return Process.enumerateModules().filter(function (module) {
                return /^libd[0-9a-f]+\.so$/i.test(module.name) || /droidguard/i.test(module.name);
            }).map(function (module) { return { name: module.name, bytes: module.size }; });
        });
        const classes = [];
        const Class = Java.use('java.lang.Class');
        Java.enumerateLoadedClasses({
            onMatch: function (name, handle) {
                if (name.indexOf('com.google.ccc.abuse.droidguard.') !== 0 || classes.length >= 64) return;
                // Inspect after enumeration has released ART's class-list locks.
                classes.push({ name: name, type: Java.retain(Java.cast(handle, Class)) });
            },
            onComplete: function () {
                const result = [];
                classes.forEach(function (entry) {
                    const type = entry.type;
                    try {
                        const loader = type.getClassLoader();
                        const declared = type.getDeclaredMethods();
                        const methods = [];
                        for (let i = 0; i < Math.min(declared.length, 32); i++) {
                            const method = declared[i];
                            const parameters = method.getParameterTypes();
                            const arguments_ = [];
                            for (let j = 0; j < parameters.length; j++) arguments_.push(String(parameters[j].getName()));
                            methods.push({ name: String(method.getName()), arguments: arguments_,
                                returns: String(method.getReturnType().getName()), native: (method.getModifiers() & 256) !== 0 });
                        }
                        result.push({ name: entry.name, loaderType: loader === null ? 'boot' : String(loader.getClass().getName()), methods: methods });
                    } catch (error) { result.push({ name: entry.name, error: errorType(error) }); }
                    finally { type.$dispose(); }
                });
                report('loadedVmClasses', result);
                report('complete', 'Metadata only; no DroidGuard request was triggered');
            }
        });
    });
}
