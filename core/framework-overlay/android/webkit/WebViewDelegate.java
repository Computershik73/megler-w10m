/*
 * Copyright (C) 2014 The Android Open Source Project
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

// Overlay: AOSP's android/webkit/WebViewDelegate.java at android-15.0.0_r36 (frameworks/base/core/java), new to the
// original's framework (tools/build-framework.sh; docs/JAVA.md, "The WebView").
//
// It differs from AOSP's in what is this framework's to give:
//  - isMultiProcessEnabled() is false. Android 15's answers true, and the WebView then runs its renderer in a sandboxed
//    process of its own; Windows 10 Mobile lets an app start no process. With false the WebView adds no
//    --webview-sandboxed-renderer switch and its library runs the renderer on threads of the app's process
//    (AwMainDelegate's --single-process), as WebView did before Android 8. Its GPU work is in-process in any case.
//  - drawWebViewFunctor draws the functor at once, on the WebView's render thread, and records the shell's picture of it
//    (WebView.meglerDrawFunctor, drivers/mg_java_webview.c): a WebView's canvas says it is hardware-accelerated only where
//    the platform's draw functor is there (megler.WebFunctors.on). On any other canvas it refuses, as AOSP's does for one
//    that is not a RecordingCanvas. The GL functor calls (Android 9's and before) refuse: Chromium makes no GL functor
//    where it has the draw functor's table.
//  - addWebViewAssetPath adds the WebView APK to the app's AssetManager as a shared library (AssetManager's overlay), where
//    AOSP's goes through LoadedApk and ResourcesManager, which this framework does not have.
//  - tracing is never on (no atrace here).
package android.webkit;

import android.app.ActivityThread;
import android.app.Application;
import android.content.Context;
import android.content.res.Resources;
import android.graphics.Canvas;
import android.util.SparseArray;
import android.view.View;

/**
 * Delegate used by the WebView provider implementation to access
 * the required framework functionality needed to implement a {@link WebView}.
 *
 * @hide
 */
public final class WebViewDelegate {

    /* package */ WebViewDelegate() { }

    /**
     * Listener that gets notified whenever tracing has been enabled/disabled.
     */
    public interface OnTraceEnabledChangeListener {
        void onTraceEnabledChange(boolean enabled);
    }

    /**
     * Register a callback to be invoked when tracing for the WebView component has been
     * enabled/disabled.
     */
    public void setOnTraceEnabledChangeListener(final OnTraceEnabledChangeListener listener) {
        // Megler: tracing is never turned on here, so the listener is never called.
    }

    /**
     * Returns {@code true} if the WebView trace tag is enabled and {@code false} otherwise.
     */
    public boolean isTraceTagEnabled() {
        return false;
    }

    /**
     * Throws {@link UnsupportedOperationException}
     * @deprecated Use {@link #drawWebViewFunctor(Canvas, int)}
     */
    @Deprecated
    public boolean canInvokeDrawGlFunctor(View containerView) {
        throw new UnsupportedOperationException();
    }

    /**
     * Throws {@link UnsupportedOperationException}
     * @deprecated Use {@link #drawWebViewFunctor(Canvas, int)}
     */
    @Deprecated
    public void invokeDrawGlFunctor(View containerView, long nativeDrawGLFunctor,
            boolean waitForCompletion) {
        throw new UnsupportedOperationException();
    }

    /**
     * Throws {@link UnsupportedOperationException}
     * @deprecated Use {@link #drawWebViewFunctor(Canvas, int)}
     */
    @Deprecated
    public void callDrawGlFunction(Canvas canvas, long nativeDrawGLFunctor) {
        throw new UnsupportedOperationException();
    }

    /**
     * Throws {@link UnsupportedOperationException}
     * @deprecated Use {@link #drawWebViewFunctor(Canvas, int)}
     */
    @Deprecated
    public void callDrawGlFunction(Canvas canvas, long nativeDrawGLFunctor,
            Runnable releasedRunnable) {
        throw new UnsupportedOperationException();
    }

    /**
     * Call webview draw functor. See API in draw_fn.h.
     * @param canvas a {@link RecordingCanvas}.
     * @param functor created by AwDrawFn_CreateFunctor in draw_fn.h.
     */
    public void drawWebViewFunctor(Canvas canvas, int functor) {
        // Megler: drawn at once, the shell's picture of it recorded (see the top).
        if (WebView.meglerDrawFunctor(canvas, functor)) return;
        throw new IllegalArgumentException(canvas.getClass().getName()
                + " is not a RecordingCanvas canvas");
    }

    /**
     * Detaches the draw GL functor.
     *
     * @param nativeDrawGLFunctor the pointer to the native functor that implements
     *        system/core/include/utils/Functor.h
     * @deprecated Use {@link #drawWebViewFunctor(Canvas, int)}
     */
    @Deprecated
    public void detachDrawGlFunctor(View containerView, long nativeDrawGLFunctor) {
        throw new UnsupportedOperationException();
    }

    /**
     * Returns the package id of the given {@code packageName}.
     */
    public int getPackageId(Resources resources, String packageName) {
        SparseArray<String> packageIdentifiers =
                resources.getAssets().getAssignedPackageIdentifiers();
        for (int i = 0; i < packageIdentifiers.size(); i++) {
            final String name = packageIdentifiers.valueAt(i);

            if (packageName.equals(name)) {
                return packageIdentifiers.keyAt(i);
            }
        }
        throw new RuntimeException("Package not found: " + packageName);
    }

    /**
     * Returns the application which is embedding the WebView.
     */
    public Application getApplication() {
        return ActivityThread.currentApplication();
    }

    /**
     * Returns the error string for the given {@code errorCode}.
     */
    public String getErrorString(Context context, int errorCode) {
        return LegacyErrorStrings.getString(errorCode, context);
    }

    /**
     * Adds the WebView asset path to {@link android.content.res.AssetManager}.
     */
    public void addWebViewAssetPath(Context context) {
        // Megler: straight to the app's AssetManager, which every Resources of the app shares (see the top).
        context.getAssets().addAssetPathAsSharedLibrary(
                WebViewFactory.getLoadedPackageInfo().applicationInfo.sourceDir);
    }

    /**
     * Returns whether WebView should run in multiprocess mode.
     */
    public boolean isMultiProcessEnabled() {
        // Megler: no process can be started here; the renderer runs in the app's process (see the top).
        return false;
    }

    /**
     * Returns the data directory suffix to use, or null for none.
     */
    public String getDataDirectorySuffix() {
        return WebViewFactory.getDataDirectorySuffix();
    }

    /**
     * Get the timestamps at which various WebView startup events occurred in this process.
     * This method must be called on the same thread where the
     * WebViewChromiumFactoryProvider#create method was invoked.
     */
    public WebViewFactory.StartupTimestamps getStartupTimestamps() {
        return WebViewFactory.getStartupTimestamps();
    }
}
