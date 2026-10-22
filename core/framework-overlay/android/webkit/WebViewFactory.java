/*
 * Copyright (C) 2012 The Android Open Source Project
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

// Overlay: AOSP's android/webkit/WebViewFactory.java at android-15.0.0_r36 (frameworks/base/core/java), in the
// place of the original's generated signature (tools/build-framework.sh; docs/JAVA.md, "The WebView").
//
// It differs from AOSP's in where the WebView package comes from and how it is loaded. On Android the system's
// WebViewUpdateService names the package, the package manager describes it, the zygote has reserved address space for its
// library and a separate process has written the library's relocated RELRO, and the app's LoadedApk makes a Context and a
// class loader for it. None of those exist here. The WebView package is the APK the Android root carries where Android's
// own image has it, /system/app/webview/webview.apk (tools/dvm-android-root.sh puts it there; tools/webview-fetch.sh brings
// it from AOSP's prebuilts). Its PackageInfo is read from its manifest (readPackageInfo); its resources and files are added
// to the app's AssetManager as a shared library; the class loader is a PathClassLoader over the APK, whose native path is
// the APK's lib/armeabi-v7a, as Android's is for a package whose libraries are not extracted; and the native library is
// loaded by WebViewLibraryLoader into address space reserved for it, as Android's loader does. AOSP's flags
// (updateServiceIpcWrapper, registerResourcePaths) are off here, as they are on an Android 15 phone.
//
// The class loader is this runtime's loader of a name space of its own (megler.ServiceClassLoader), as Android's is a loader
// of the WebView's own beside the app's: the WebView's classes are its own, beside the app's classes of the same names. They
// had joined the app's one class path after the app's, and an app with Chromium classes of its own lost the WebView's of
// those names: microG (Cronet) as an app had 193 of the WebView's 3478 classes shadowed, and its sign-in screen ended in
// NoSuchMethodError: org/chromium/base/task/PostTask.a(I) when it made its WebView. Its native library is loaded for that
// loader too (core/src/drivers/mg_java_webview.c), where Chromium's own System.loadLibrary("webviewchromium") finds it.
package android.webkit;

import android.app.AppGlobals;
import android.app.Application;
import android.content.pm.ApplicationInfo;
import android.content.pm.PackageInfo;
import android.content.pm.Signature;
import android.content.res.AssetManager;
import android.content.res.XmlBlock;
import android.os.Bundle;
import android.os.SystemClock;
import android.os.Trace;
import android.os.UserHandle;
import android.util.AndroidRuntimeException;
import android.util.Log;

import org.xmlpull.v1.XmlPullParser;

import java.io.File;
import java.io.InputStream;
import java.lang.reflect.Method;
import java.util.Collection;
import java.util.Collections;

/**
 * Top level factory, used creating all the main WebView implementation classes.
 *
 * @hide
 */
public final class WebViewFactory {
    private static final String CHROMIUM_WEBVIEW_FACTORY_METHOD = "create";

    private static final String LOGTAG = "WebViewFactory";

    private static final boolean DEBUG = false;

    /** Megler: the WebView package, where Android's own image has it: the Android root is mounted at /system. */
    static final String WEBVIEW_APK = "/system/app/webview/webview.apk";
    /** Megler: the processor's library folder in an APK whose native libraries are used from it, not extracted. */
    static final String WEBVIEW_ABI = "armeabi-v7a";

    // Cache the factory both for efficiency, and ensure any one process gets all webviews from the
    // same provider.
    private static WebViewFactoryProvider sProviderInstance;
    private static final Object sProviderLock = new Object();
    private static PackageInfo sPackageInfo;
    private static Boolean sWebViewSupported;
    private static boolean sWebViewDisabled;
    private static String sDataDirectorySuffix; // stored here so it can be set without loading WV

    // Error codes for loadWebViewNativeLibraryFromPackage
    public static final int LIBLOAD_SUCCESS = 0;
    public static final int LIBLOAD_WRONG_PACKAGE_NAME = 1;
    public static final int LIBLOAD_ADDRESS_SPACE_NOT_RESERVED = 2;

    // error codes for waiting for WebView preparation
    public static final int LIBLOAD_FAILED_WAITING_FOR_RELRO = 3;
    public static final int LIBLOAD_FAILED_LISTING_WEBVIEW_PACKAGES = 4;

    // native relro loading error codes
    public static final int LIBLOAD_FAILED_TO_OPEN_RELRO_FILE = 5;
    public static final int LIBLOAD_FAILED_TO_LOAD_LIBRARY = 6;
    public static final int LIBLOAD_FAILED_JNI_CALL = 7;

    // more error codes for waiting for WebView preparation
    public static final int LIBLOAD_FAILED_WAITING_FOR_WEBVIEW_REASON_UNKNOWN = 8;

    // error for namespace lookup
    public static final int LIBLOAD_FAILED_TO_FIND_NAMESPACE = 10;

    // generic error for future use
    static final int LIBLOAD_FAILED_OTHER = 11;

    /**
     * Stores the timestamps at which various WebView startup events occurred in this process.
     */
    public static class StartupTimestamps {
        long mWebViewLoadStart;
        long mCreateContextStart;
        long mCreateContextEnd;
        long mAddAssetsStart;
        long mAddAssetsEnd;
        long mGetClassLoaderStart;
        long mGetClassLoaderEnd;
        long mNativeLoadStart;
        long mNativeLoadEnd;
        long mProviderClassForNameStart;
        long mProviderClassForNameEnd;

        StartupTimestamps() {}

        /** When the overall WebView provider load began. */
        public long getWebViewLoadStart() {
            return mWebViewLoadStart;
        }

        /** Before creating the WebView APK Context. */
        public long getCreateContextStart() {
            return mCreateContextStart;
        }

        /** After creating the WebView APK Context. */
        public long getCreateContextEnd() {
            return mCreateContextEnd;
        }

        /** Before adding WebView assets to AssetManager. */
        public long getAddAssetsStart() {
            return mAddAssetsStart;
        }

        /** After adding WebView assets to AssetManager. */
        public long getAddAssetsEnd() {
            return mAddAssetsEnd;
        }

        /** Before creating the WebView ClassLoader. */
        public long getGetClassLoaderStart() {
            return mGetClassLoaderStart;
        }

        /** After creating the WebView ClassLoader. */
        public long getGetClassLoaderEnd() {
            return mGetClassLoaderEnd;
        }

        /** Before preloading the WebView native library. */
        public long getNativeLoadStart() {
            return mNativeLoadStart;
        }

        /** After preloading the WebView native library. */
        public long getNativeLoadEnd() {
            return mNativeLoadEnd;
        }

        /** Before looking up the WebView provider class. */
        public long getProviderClassForNameStart() {
            return mProviderClassForNameStart;
        }

        /** After looking up the WebView provider class. */
        public long getProviderClassForNameEnd() {
            return mProviderClassForNameEnd;
        }
    }

    static final StartupTimestamps sTimestamps = new StartupTimestamps();

    static StartupTimestamps getStartupTimestamps() {
        return sTimestamps;
    }

    static class MissingWebViewPackageException extends Exception {
        public MissingWebViewPackageException(String message) { super(message); }
        public MissingWebViewPackageException(Exception e) { super(e); }
    }

    /** Megler: a WebView is supported when the Android root has the WebView package. */
    static boolean isWebViewSupported() {
        // No lock; this is a benign race as Boolean's state is final.
        if (sWebViewSupported == null) {
            sWebViewSupported = new File(WEBVIEW_APK).isFile();
        }
        return sWebViewSupported;
    }

    /**
     * @hide
     */
    static void disableWebView() {
        synchronized (sProviderLock) {
            if (sProviderInstance != null) {
                throw new IllegalStateException(
                        "Can't disable WebView: WebView already initialized");
            }
            sWebViewDisabled = true;
        }
    }

    /**
     * @hide
     */
    static void setDataDirectorySuffix(String suffix) {
        synchronized (sProviderLock) {
            if (sProviderInstance != null) {
                throw new IllegalStateException(
                        "Can't set data directory suffix: WebView already initialized");
            }
            if (suffix.indexOf(File.separatorChar) >= 0) {
                throw new IllegalArgumentException("Suffix " + suffix
                                                   + " contains a path separator");
            }
            sDataDirectorySuffix = suffix;
        }
    }

    /**
     * @hide
     */
    static String getDataDirectorySuffix() {
        synchronized (sProviderLock) {
            return sDataDirectorySuffix;
        }
    }

    /**
     * @hide
     */
    public static String getWebViewLibrary(ApplicationInfo ai) {
        if (ai.metaData != null)
            return ai.metaData.getString("com.android.webview.WebViewLibrary");
        return null;
    }

    public static PackageInfo getLoadedPackageInfo() {
        synchronized (sProviderLock) {
            return sPackageInfo;
        }
    }

    /**
     * Megler: the WebView package of the Android root, whether or not it is loaded (WebView.getCurrentWebViewPackage asks
     * the update service for this on Android); null when there is none.
     */
    static PackageInfo getCurrentWebViewPackage() {
        synchronized (sProviderLock) {
            if (sPackageInfo != null) return sPackageInfo;
            if (!isWebViewSupported()) return null;
            try {
                return readPackageInfo(AppGlobals.getInitialApplication().getAssets());
            } catch (MissingWebViewPackageException e) {
                Log.e(LOGTAG, "the WebView package cannot be read", e);
                return null;
            }
        }
    }

    /**
     * @hide
     */
    public static Class<WebViewFactoryProvider> getWebViewProviderClass(ClassLoader clazzLoader)
            throws ClassNotFoundException {
        return (Class<WebViewFactoryProvider>) Class.forName(
                WebViewFactoryProvider.getWebViewFactoryClassName(), true, clazzLoader);
    }

    /**
     * Load the native library for the given package name if that package
     * name is the same as the one providing the webview.
     */
    public static int loadWebViewNativeLibraryFromPackage(String packageName,
                                                          ClassLoader clazzLoader) {
        if (!isWebViewSupported()) {
            return LIBLOAD_WRONG_PACKAGE_NAME;
        }
        PackageInfo packageInfo = getCurrentWebViewPackage();
        if (packageInfo == null) {
            return LIBLOAD_FAILED_LISTING_WEBVIEW_PACKAGES;
        }
        if (!packageInfo.packageName.equals(packageName)) {
            return LIBLOAD_WRONG_PACKAGE_NAME;
        }
        return WebViewLibraryLoader.loadNativeLibrary(clazzLoader,
                getWebViewLibrary(packageInfo.applicationInfo));
    }

    static WebViewFactoryProvider getProvider() {
        synchronized (sProviderLock) {
            // For now the main purpose of this function (and the factory abstraction) is to keep
            // us honest and minimize usage of WebView internals when binding the proxy.
            if (sProviderInstance != null) return sProviderInstance;

            sTimestamps.mWebViewLoadStart = SystemClock.uptimeMillis();
            final int appId = UserHandle.getAppId(android.os.Process.myUid());
            if (appId == android.os.Process.ROOT_UID || appId == android.os.Process.SYSTEM_UID
                    || appId == android.os.Process.PHONE_UID || appId == android.os.Process.NFC_UID
                    || appId == android.os.Process.BLUETOOTH_UID) {
                throw new UnsupportedOperationException(
                        "For security reasons, WebView is not allowed in privileged processes");
            }

            if (!isWebViewSupported()) {
                // Device doesn't support WebView; don't try to load it, just throw.
                throw new UnsupportedOperationException(
                        "this phone's Android root has no WebView package (" + WEBVIEW_APK + ")");
            }

            if (sWebViewDisabled) {
                throw new IllegalStateException(
                        "WebView.disableWebView() was called: WebView is disabled");
            }

            Trace.traceBegin(Trace.TRACE_TAG_WEBVIEW, "WebViewFactory.getProvider()");
            try {
                Class<WebViewFactoryProvider> providerClass = getProviderClass();
                Method staticFactory = providerClass.getMethod(
                        CHROMIUM_WEBVIEW_FACTORY_METHOD, WebViewDelegate.class);

                Trace.traceBegin(Trace.TRACE_TAG_WEBVIEW, "WebViewFactoryProvider invocation");
                try {
                    sProviderInstance = (WebViewFactoryProvider)
                            staticFactory.invoke(null, new WebViewDelegate());
                    if (DEBUG) Log.v(LOGTAG, "Loaded provider: " + sProviderInstance);
                    return sProviderInstance;
                } finally {
                    Trace.traceEnd(Trace.TRACE_TAG_WEBVIEW);
                }
            } catch (Exception e) {
                Log.e(LOGTAG, "error instantiating provider", e);
                throw new AndroidRuntimeException(e);
            } finally {
                Trace.traceEnd(Trace.TRACE_TAG_WEBVIEW);
            }
        }
    }

    /**
     * Megler: the WebView package's PackageInfo, read from its manifest, as the package manager would describe an installed
     * system package: what WebViewFactory and the WebView's own code read of it (its name, version, target SDK, the meta-data
     * that names its library, where its APK and libraries are). The APK is added to the app's AssetManager first, as
     * getProviderClass adds it on Android, so that its manifest is read through it.
     */
    private static PackageInfo readPackageInfo(AssetManager assets) throws MissingWebViewPackageException {
        int cookie = assets.addAssetPathAsSharedLibrary(WEBVIEW_APK);
        if (cookie == 0) {
            throw new MissingWebViewPackageException("the WebView package cannot be read: " + WEBVIEW_APK);
        }
        PackageInfo pi = new PackageInfo();
        ApplicationInfo ai = new ApplicationInfo();
        ai.metaData = new Bundle();
        boolean inApplication = false;
        try (InputStream in = assets.openNonAsset(cookie, "AndroidManifest.xml")) {
            byte[] data = readAll(in);
            XmlBlock.Parser p = new XmlBlock(data, WEBVIEW_APK + "!/AndroidManifest.xml").newParser();
            final String ns = "http://schemas.android.com/apk/res/android";
            int t;
            while ((t = p.next()) != XmlPullParser.END_DOCUMENT) {
                if (t == XmlPullParser.END_TAG && "application".equals(p.getName())) inApplication = false;
                if (t != XmlPullParser.START_TAG) continue;
                switch (p.getName()) {
                    case "manifest":
                        pi.packageName = p.getAttributeValue(null, "package");
                        pi.versionCode = p.getAttributeIntValue(ns, "versionCode", 0);
                        pi.versionName = p.getAttributeValue(ns, "versionName");
                        break;
                    case "uses-sdk":
                        ai.minSdkVersion = p.getAttributeIntValue(ns, "minSdkVersion", 1);
                        ai.targetSdkVersion = p.getAttributeIntValue(ns, "targetSdkVersion", ai.minSdkVersion);
                        break;
                    case "application":
                        inApplication = true;
                        ai.className = p.getAttributeValue(ns, "name");
                        break;
                    case "meta-data":
                        // the application's own meta-data, not its components'
                        if (inApplication && p.getDepth() == 3) {
                            String name = p.getAttributeValue(ns, "name"), value = p.getAttributeValue(ns, "value");
                            if (name != null && value != null) ai.metaData.putString(name, value);
                        }
                        break;
                    default:
                        break;
                }
            }
        } catch (Exception e) {
            throw new MissingWebViewPackageException(e);
        }
        if (pi.packageName == null) {
            throw new MissingWebViewPackageException("the WebView package's manifest names no package");
        }
        ai.packageName = pi.packageName;
        ai.processName = AppGlobals.getInitialPackage();
        ai.sourceDir = ai.publicSourceDir = WEBVIEW_APK;
        ai.nativeLibraryDir = new File(WEBVIEW_APK).getParent() + "/lib/arm";
        ai.flags = ApplicationInfo.FLAG_SYSTEM | ApplicationInfo.FLAG_INSTALLED | ApplicationInfo.FLAG_HAS_CODE;
        ai.enabled = true;
        pi.applicationInfo = ai;
        pi.signatures = new Signature[0];
        return pi;
    }

    private static byte[] readAll(InputStream in) throws java.io.IOException {
        java.io.ByteArrayOutputStream out = new java.io.ByteArrayOutputStream();
        byte[] buf = new byte[8192];
        int n;
        while ((n = in.read(buf)) > 0) out.write(buf, 0, n);
        return out.toByteArray();
    }

    /** Megler: "apk!/lib/<abi>", where a library of an APK whose libraries are not extracted is looked for. */
    static String getWebViewNativeLibraryPath() {
        return WEBVIEW_APK + "!/lib/" + WEBVIEW_ABI;
    }

    private static Class<WebViewFactoryProvider> getProviderClass() {
        Application initialApplication = AppGlobals.getInitialApplication();
        try {
            sTimestamps.mCreateContextStart = sTimestamps.mAddAssetsStart = SystemClock.uptimeMillis();
            PackageInfo packageInfo = readPackageInfo(initialApplication.getAssets());
            synchronized (sProviderLock) {
                sPackageInfo = packageInfo;
            }
            sTimestamps.mCreateContextEnd = sTimestamps.mAddAssetsEnd = SystemClock.uptimeMillis();
            Log.i(LOGTAG, "Loading " + sPackageInfo.packageName + " version " +
                    sPackageInfo.versionName + " (code " + sPackageInfo.getLongVersionCode() + ")");
            if (!WebViewFactoryProvider.isCompatibleImplementationPackage(sPackageInfo)) {
                throw new MissingWebViewPackageException("the WebView package is too old for this framework: "
                        + WebViewFactoryProvider.describeCompatibleImplementationPackage());
            }
            if (getWebViewLibrary(sPackageInfo.applicationInfo) == null) {
                throw new MissingWebViewPackageException("Tried to load an invalid WebView provider: "
                        + sPackageInfo.packageName);
            }

            Trace.traceBegin(Trace.TRACE_TAG_WEBVIEW, "WebViewFactory.getChromiumProviderClass()");
            try {
                sTimestamps.mGetClassLoaderStart = SystemClock.uptimeMillis();
                ClassLoader clazzLoader = makeClassLoader(initialApplication);
                Trace.traceBegin(Trace.TRACE_TAG_WEBVIEW, "WebViewFactory.loadNativeLibrary()");
                sTimestamps.mGetClassLoaderEnd = sTimestamps.mNativeLoadStart =
                        SystemClock.uptimeMillis();
                int loaded = WebViewLibraryLoader.loadNativeLibrary(clazzLoader,
                        getWebViewLibrary(sPackageInfo.applicationInfo));
                Trace.traceEnd(Trace.TRACE_TAG_WEBVIEW);
                if (loaded != LIBLOAD_SUCCESS) {
                    throw new MissingWebViewPackageException("the WebView's native library did not load (error "
                            + loaded + "; the layer's log says why)");
                }
                Trace.traceBegin(Trace.TRACE_TAG_WEBVIEW, "Class.forName()");
                sTimestamps.mNativeLoadEnd = sTimestamps.mProviderClassForNameStart =
                        SystemClock.uptimeMillis();
                try {
                    return getWebViewProviderClass(clazzLoader);
                } finally {
                    sTimestamps.mProviderClassForNameEnd = SystemClock.uptimeMillis();
                    Trace.traceEnd(Trace.TRACE_TAG_WEBVIEW);
                }
            } catch (ClassNotFoundException e) {
                Log.e(LOGTAG, "error loading provider", e);
                throw new AndroidRuntimeException(e);
            } finally {
                Trace.traceEnd(Trace.TRACE_TAG_WEBVIEW);
            }
        } catch (MissingWebViewPackageException e) {
            Log.e(LOGTAG, "Chromium WebView package does not exist", e);
            throw new AndroidRuntimeException(e);
        }
    }

    /**
     * Megler: the WebView's class loader, which Android's LoadedApk makes for the package: one of a name space of its own over
     * its APK, sharing the platform's classes and not the app's (see the top), whose libraries are the APK's lib/armeabi-v7a.
     */
    private static ClassLoader makeClassLoader(Application app) throws MissingWebViewPackageException {
        try {
            return new megler.ServiceClassLoader(WEBVIEW_APK);
        } catch (java.io.IOException e) {
            throw new MissingWebViewPackageException(e);
        }
    }

    /**
     * Perform any WebView loading preparations that must happen in the zygote.
     * Currently, this means allocating address space to load the real JNI library later.
     */
    public static void prepareWebViewInZygote() {
        // Megler: there is no zygote; WebViewLibraryLoader reserves the address space when it loads the library.
    }

    /**
     * @hide
     */
    public static int onWebViewProviderChanged(PackageInfo packageInfo) {
        // Megler: there is no system server to call this; the WebView package is the Android root's.
        return 0;
    }
}
