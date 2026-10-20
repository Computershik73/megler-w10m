/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Overlay: AOSP's android/webkit/WebViewLibraryLoader.java at android-15.0.0_r36 (frameworks/base/core/java), new
// to the original's framework (tools/build-framework.sh; docs/JAVA.md, "The WebView").
//
// AOSP's reserves address space in the zygote, has an isolated process load the library once to write its relocated RELRO
// to /data/misc/shared_relro, and loads it into each app's reservation with that RELRO (libwebviewchromium_loader.so's
// natives, over android_dlopen_ext). Here there is one process and no zygote: loadNativeLibrary reserves the address space
// itself, as the zygote would (130 MB for 32-bit ARM, AOSP's figure), and loads the library into it; no RELRO is written or
// used, since there is no other process to share it with. The two natives are the core's
// (core/src/drivers/hk_java_webview.c), which loads the library the way android_dlopen_ext with
// ANDROID_DLEXT_RESERVED_ADDRESS_HINT does (core/src/bionic/hk_libdl.c), from the APK named by the class loader's native
// path, without running its JNI_OnLoad: as on Android, that is run when Chromium's own LibraryLoader calls
// System.loadLibrary("webviewchromium").
package android.webkit;

import android.util.Log;

/**
 * @hide
 */
public class WebViewLibraryLoader {
    private static final String LOGTAG = WebViewLibraryLoader.class.getSimpleName();

    private static final String CHROMIUM_WEBVIEW_NATIVE_RELRO_32 =
            "/data/misc/shared_relro/libwebviewchromium32.relro";

    private static final boolean DEBUG = false;

    private static boolean sAddressSpaceReserved = false;

    /**
     * Reserve space for the native library to be loaded into.
     */
    static void reserveAddressSpaceInZygote() {
        // On 32-bit the address space is fairly scarce, hence we should keep it to a realistic
        // number that permits some future growth but doesn't hog space. For ARM we use 130MB
        // which is roughly what was calculated on older OS versions. The size has been
        // growing gradually, but a few efforts have offset it back to the size close to the
        // original.
        long addressSpaceToReserve = 130 * 1024 * 1024;

        sAddressSpaceReserved = nativeReserveAddressSpace(addressSpaceToReserve);

        if (sAddressSpaceReserved) {
            if (DEBUG) {
                Log.v(LOGTAG, "address space reserved: " + addressSpaceToReserve + " bytes");
            }
        } else {
            Log.e(LOGTAG, "reserving " + addressSpaceToReserve + " bytes of address space failed");
        }
    }

    /**
     * Load WebView's native library into the current process.
     *
     * @param clazzLoader class loader used to find the linker namespace to load the library into.
     * @param libraryFileName the filename of the library to load.
     */
    public static int loadNativeLibrary(ClassLoader clazzLoader, String libraryFileName) {
        synchronized (WebViewLibraryLoader.class) {
            // Meglet: no zygote has reserved the space; it is reserved now, once (see the top).
            if (!sAddressSpaceReserved) reserveAddressSpaceInZygote();
        }
        if (!sAddressSpaceReserved) {
            Log.e(LOGTAG, "can't load with relro file; address space not reserved");
            return WebViewFactory.LIBLOAD_ADDRESS_SPACE_NOT_RESERVED;
        }

        int result = nativeLoadWithRelroFile(libraryFileName, CHROMIUM_WEBVIEW_NATIVE_RELRO_32, clazzLoader);
        if (result != WebViewFactory.LIBLOAD_SUCCESS) {
            Log.w(LOGTAG, "failed to load " + libraryFileName + " (error " + result + ")");
        } else if (DEBUG) {
            Log.v(LOGTAG, "loaded with relro file");
        }
        return result;
    }

    static native boolean nativeReserveAddressSpace(long addressSpaceToReserve);
    static native int nativeLoadWithRelroFile(String lib, String relro, ClassLoader clazzLoader);
}
