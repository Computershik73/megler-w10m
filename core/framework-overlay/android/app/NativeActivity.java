// Overlay of core/framework/gen-src/android/app/NativeActivity.java (tools/build-framework.sh), which had the platform's
// signatures and nothing behind them.
//
// Why. An activity of the app's own on top of NativeActivity is the app's Java code with its native code under it, and
// the native code asks the Java code: KOReader's launcher (org.koreader.launcher.MainActivity, LuaJIT's android.lua) calls
// its activity's extractAssets(), getScreenWidth() and some eighty others through JNI on ANativeActivity's clazz. Such an
// app was run by the core's NativeActivity driver (core/src/drivers/mg_native_activity.c), whose activity is a stand-in
// that only has a name: every one of those calls returned zero, and KOReader ended at its start with "error extracting
// assets" (issue #23). Now the Java driver runs such an app, its activity the app's own class, and this is Android's
// NativeActivity under it, as Android 9's frameworks/base has it: onCreate loads the library its manifest names
// (android.app.lib_name, android.app.func_name) through the app's class loader and calls its entry with an ANativeActivity
// whose clazz is this activity; the activity's life, its window's surface (Window.takeSurface), its input
// (Window.takeInputQueue), focus, the content's place, the configuration and low memory go to the native callbacks as
// Android's do. The natives are the core's (core/src/drivers/mg_java_ndk.c, "NativeActivity").
//
// What native code asks back (ANativeActivity_finish, _setWindowFlags, _setWindowFormat, _showSoftInput, _hideSoftInput)
// comes from whichever thread asked, and is done on the main thread, as Android's pipe to the main thread's looper has it
// done (meglerFromNative).
package android.app;

import android.content.Context;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.res.AssetManager;
import android.content.res.Configuration;
import android.graphics.PixelFormat;
import android.os.Build;
import android.os.Bundle;
import android.os.Looper;
import android.os.MessageQueue;
import android.util.AttributeSet;
import android.view.InputQueue;
import android.view.Surface;
import android.view.SurfaceHolder;
import android.view.View;
import android.view.ViewTreeObserver.OnGlobalLayoutListener;
import android.view.WindowManager;
import android.view.inputmethod.InputMethodManager;
import dalvik.system.BaseDexClassLoader;
import java.io.File;

public class NativeActivity extends Activity implements SurfaceHolder.Callback2, InputQueue.Callback, OnGlobalLayoutListener {
    public static final String META_DATA_LIB_NAME = "android.app.lib_name";
    public static final String META_DATA_FUNC_NAME = "android.app.func_name";
    private static final String KEY_NATIVE_SAVED_STATE = "android:native_state";

    private NativeContentView mNativeContentView;
    private InputMethodManager mIMM;
    private long mNativeHandle;
    private InputQueue mCurInputQueue;
    private SurfaceHolder mCurSurfaceHolder;
    final int[] mLocation = new int[2];
    int mLastContentX, mLastContentY, mLastContentWidth, mLastContentHeight;
    private boolean mDestroyed;

    private native long loadNativeCode(String path, String funcname, MessageQueue queue, String internalDataPath, String obbPath, String externalDataPath,
                                       int sdkVersion, AssetManager assetMgr, byte[] savedState, ClassLoader classLoader, String libraryPath);
    private native String getDlError();
    private native void unloadNativeCode(long handle);
    private native void onStartNative(long handle);
    private native void onResumeNative(long handle);
    private native byte[] onSaveInstanceStateNative(long handle);
    private native void onPauseNative(long handle);
    private native void onStopNative(long handle);
    private native void onConfigurationChangedNative(long handle);
    private native void onLowMemoryNative(long handle);
    private native void onWindowFocusChangedNative(long handle, boolean focused);
    private native void onSurfaceCreatedNative(long handle, Surface surface);
    private native void onSurfaceChangedNative(long handle, Surface surface, int format, int width, int height);
    private native void onSurfaceRedrawNeededNative(long handle, Surface surface);
    private native void onSurfaceDestroyedNative(long handle);
    private native void onInputQueueCreatedNative(long handle, long queuePtr);
    private native void onInputQueueDestroyedNative(long handle, long queuePtr);
    private native void onContentRectChangedNative(long handle, int x, int y, int w, int h);

    static class NativeContentView extends View {
        NativeActivity mActivity;
        public NativeContentView(Context context) { super(context); }
        public NativeContentView(Context context, AttributeSet attrs) { super(context, attrs); }
    }

    public NativeActivity() { super(); }

    @Override protected void onCreate(Bundle savedInstanceState) {
        String libname = "main", funcname = "ANativeActivity_onCreate";
        mIMM = getSystemService(InputMethodManager.class);
        getWindow().takeSurface(this);
        getWindow().takeInputQueue(this);
        getWindow().setFormat(PixelFormat.RGB_565);
        getWindow().setSoftInputMode(WindowManager.LayoutParams.SOFT_INPUT_STATE_UNSPECIFIED | WindowManager.LayoutParams.SOFT_INPUT_ADJUST_RESIZE);
        mNativeContentView = new NativeContentView(this);
        mNativeContentView.mActivity = this;
        setContentView(mNativeContentView);
        mNativeContentView.requestFocus();
        mNativeContentView.getViewTreeObserver().addOnGlobalLayoutListener(this);
        try {
            ActivityInfo ai = getPackageManager().getActivityInfo(getIntent().getComponent(), PackageManager.GET_META_DATA);
            if (ai.metaData != null) {
                String ln = ai.metaData.getString(META_DATA_LIB_NAME);
                if (ln != null) libname = ln;
                ln = ai.metaData.getString(META_DATA_FUNC_NAME);
                if (ln != null) funcname = ln;
            }
        } catch (PackageManager.NameNotFoundException e) {
            throw new RuntimeException("Error getting activity info", e);
        }
        ClassLoader loader = getClassLoader();
        String path = loader instanceof BaseDexClassLoader ? ((BaseDexClassLoader) loader).findLibrary(libname) : null;
        if (path == null) throw new IllegalArgumentException("Unable to find native library " + libname + " using classloader: " + loader);
        byte[] nativeSavedState = savedInstanceState != null ? savedInstanceState.getByteArray(KEY_NATIVE_SAVED_STATE) : null;
        mNativeHandle = loadNativeCode(path, funcname, Looper.myQueue(), getAbsolutePath(getFilesDir()), getAbsolutePath(getObbDir()),
                                       getAbsolutePath(getExternalFilesDir(null)), Build.VERSION.SDK_INT, getAssets(), nativeSavedState, loader, null);
        if (mNativeHandle == 0) throw new UnsatisfiedLinkError("Unable to load native library \"" + path + "\": " + getDlError());
        super.onCreate(savedInstanceState);
    }

    private static String getAbsolutePath(File file) { return file != null ? file.getAbsolutePath() : null; }

    @Override protected void onDestroy() {
        mDestroyed = true;
        if (mCurSurfaceHolder != null) {
            onSurfaceDestroyedNative(mNativeHandle);
            mCurSurfaceHolder = null;
        }
        if (mCurInputQueue != null) {
            onInputQueueDestroyedNative(mNativeHandle, mCurInputQueue.getNativePtr());
            mCurInputQueue = null;
        }
        unloadNativeCode(mNativeHandle);
        super.onDestroy();
    }
    @Override protected void onPause() { super.onPause(); onPauseNative(mNativeHandle); }
    @Override protected void onResume() { super.onResume(); onResumeNative(mNativeHandle); }
    @Override protected void onSaveInstanceState(Bundle outState) {
        super.onSaveInstanceState(outState);
        byte[] state = onSaveInstanceStateNative(mNativeHandle);
        if (state != null) outState.putByteArray(KEY_NATIVE_SAVED_STATE, state);
    }
    @Override protected void onStart() { super.onStart(); onStartNative(mNativeHandle); }
    @Override protected void onStop() { super.onStop(); onStopNative(mNativeHandle); }
    @Override public void onConfigurationChanged(Configuration newConfig) {
        super.onConfigurationChanged(newConfig);
        if (!mDestroyed) onConfigurationChangedNative(mNativeHandle);
    }
    @Override public void onLowMemory() {
        super.onLowMemory();
        if (!mDestroyed) onLowMemoryNative(mNativeHandle);
    }
    @Override public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (!mDestroyed) onWindowFocusChangedNative(mNativeHandle, hasFocus);
    }

    public void surfaceCreated(SurfaceHolder holder) {
        if (!mDestroyed) {
            mCurSurfaceHolder = holder;
            onSurfaceCreatedNative(mNativeHandle, holder.getSurface());
        }
    }
    public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
        if (!mDestroyed) {
            mCurSurfaceHolder = holder;
            onSurfaceChangedNative(mNativeHandle, holder.getSurface(), format, width, height);
        }
    }
    public void surfaceRedrawNeeded(SurfaceHolder holder) {
        if (!mDestroyed) {
            mCurSurfaceHolder = holder;
            onSurfaceRedrawNeededNative(mNativeHandle, holder.getSurface());
        }
    }
    public void surfaceDestroyed(SurfaceHolder holder) {
        mCurSurfaceHolder = null;
        if (!mDestroyed) onSurfaceDestroyedNative(mNativeHandle);
    }
    public void onInputQueueCreated(InputQueue queue) {
        if (!mDestroyed) {
            mCurInputQueue = queue;
            onInputQueueCreatedNative(mNativeHandle, queue.getNativePtr());
        }
    }
    public void onInputQueueDestroyed(InputQueue queue) {
        if (!mDestroyed) {
            onInputQueueDestroyedNative(mNativeHandle, queue.getNativePtr());
            mCurInputQueue = null;
        }
    }
    public void onGlobalLayout() {
        mNativeContentView.getLocationInWindow(mLocation);
        int w = mNativeContentView.getWidth(), h = mNativeContentView.getHeight();
        if (mLocation[0] != mLastContentX || mLocation[1] != mLastContentY || w != mLastContentWidth || h != mLastContentHeight) {
            mLastContentX = mLocation[0];
            mLastContentY = mLocation[1];
            mLastContentWidth = w;
            mLastContentHeight = h;
            if (!mDestroyed) onContentRectChangedNative(mNativeHandle, mLastContentX, mLastContentY, mLastContentWidth, mLastContentHeight);
        }
    }

    void setWindowFlags(int flags, int mask) { getWindow().setFlags(flags, mask); }
    void setWindowFormat(int format) { getWindow().setFormat(format); }
    void showIme(int mode) { mIMM.showSoftInput(mNativeContentView, mode); }
    void hideIme(int mode) { mIMM.hideSoftInputFromWindow(mNativeContentView.getWindowToken(), mode); }

    /*
     * Megler: what native code asked of the activity (core/src/drivers/mg_java_ndk.c), from any thread: done on the main thread,
     * as Android's NativeActivity does what comes down its pipe (android_app_NativeActivity.cpp, mainWorkCallback). 1 finish,
     * 2 window flags (a added, b removed), 3 window format (a), 4 show the soft input (a its flags), 5 hide it.
     */
    void meglerFromNative(final int what, final int a, final int b) {
        runOnUiThread(() -> {
            if (mDestroyed && what != 1) return;
            switch (what) {
                case 1: if (!isFinishing()) finish(); break;
                case 2: setWindowFlags(a, a | b); break;
                case 3: setWindowFormat(a); break;
                case 4: showIme(a); break;
                case 5: hideIme(a); break;
                default: break;
            }
        });
    }
}
