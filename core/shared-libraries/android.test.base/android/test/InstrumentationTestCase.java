// Megler's android.test.base (tools/build-shared-libraries.sh): Android's android.test API as it documents it, written here.
package android.test;

import android.app.Activity;
import android.app.Instrumentation;
import android.content.Intent;
import android.os.Bundle;
import android.util.Log;
import android.view.KeyEvent;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import junit.framework.TestCase;

/**
 * A test case with an Instrumentation, which whoever runs it gives it (injectInstrumentation): activities launched through
 * it, code and test methods run on the main thread (@UiThreadTest), keys sent, a method run again while it fails
 * (@FlakyTest) or a number of times (@RepetitiveTest).
 */
@Deprecated
public class InstrumentationTestCase extends TestCase {
    private static final String TAG = "InstrumentationTestCase";
    private Instrumentation mInstrumentation;

    public InstrumentationTestCase() {
    }

    public void injectInstrumentation(Instrumentation instrumentation) {
        mInstrumentation = instrumentation;
    }

    /** (The old name, misspelt.) */
    @Deprecated
    public void injectInsrumentation(Instrumentation instrumentation) {
        injectInstrumentation(instrumentation);
    }

    public Instrumentation getInstrumentation() {
        return mInstrumentation;
    }

    /** That activity of `pkg`, started by ACTION_MAIN with `extras`, once the main thread is idle. */
    public final <T extends Activity> T launchActivity(String pkg, Class<T> activityCls, Bundle extras) {
        Intent intent = new Intent(Intent.ACTION_MAIN);
        if (extras != null) intent.putExtras(extras);
        return launchActivityWithIntent(pkg, activityCls, intent);
    }

    @SuppressWarnings("unchecked")
    public final <T extends Activity> T launchActivityWithIntent(String pkg, Class<T> activityCls, Intent intent) {
        intent.setClassName(pkg, activityCls.getName());
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        T activity = (T) getInstrumentation().startActivitySync(intent);
        getInstrumentation().waitForIdleSync();
        return activity;
    }

    /** Runs `r` on the main thread and waits for it; what it threw is thrown here. */
    public void runTestOnUiThread(final Runnable r) throws Throwable {
        final Throwable[] thrown = new Throwable[1];
        getInstrumentation().runOnMainSync(new Runnable() {
            @Override
            public void run() {
                try {
                    r.run();
                } catch (Throwable t) {
                    thrown[0] = t;
                }
            }
        });
        if (thrown[0] != null) throw thrown[0];
    }

    /** The test method of its name, as its annotations ask: on the main thread, again while it fails, or a number of times. */
    @Override
    protected void runTest() throws Throwable {
        String name = getName();
        assertNotNull(name);
        Method found = null;
        try {
            found = getClass().getMethod(name);
        } catch (NoSuchMethodException e) {
            fail("Method \"" + name + "\" not found");
        }
        final Method method = found;
        if (!Modifier.isPublic(method.getModifiers())) fail("Method \"" + name + "\" should be public");
        int runs = 1;
        boolean repetitive = false;
        if (method.isAnnotationPresent(FlakyTest.class)) {
            runs = method.getAnnotation(FlakyTest.class).tolerance();
        } else if (method.isAnnotationPresent(RepetitiveTest.class)) {
            runs = method.getAnnotation(RepetitiveTest.class).numIterations();
            repetitive = true;
        }
        if (!method.isAnnotationPresent(UiThreadTest.class)) {
            invoke(method, runs, repetitive);
            return;
        }
        final int times = runs;
        final boolean each = repetitive;
        final Throwable[] thrown = new Throwable[1];
        getInstrumentation().runOnMainSync(new Runnable() {
            @Override
            public void run() {
                try {
                    invoke(method, times, each);
                } catch (Throwable t) {
                    thrown[0] = t;
                }
            }
        });
        if (thrown[0] != null) throw thrown[0];
    }

    /**
     * Calls `method` up to `runs` times: while it throws, or every time where it is repetitive -- each run then told to the
     * instrumentation (its "currentiterations"). What the last run threw is thrown.
     */
    private void invoke(Method method, int runs, boolean repetitive) throws Throwable {
        Throwable last;
        int done = 0;
        do {
            last = null;
            try {
                method.invoke(this);
            } catch (InvocationTargetException e) {
                e.fillInStackTrace();
                last = e.getTargetException();
            } catch (IllegalAccessException e) {
                e.fillInStackTrace();
                last = e;
            } finally {
                done++;
                if (repetitive) {
                    Bundle iterations = new Bundle();
                    iterations.putInt("currentiterations", done);
                    getInstrumentation().sendStatus(2, iterations);
                }
            }
        } while (done < runs && (repetitive || last != null));
        if (last != null) throw last;
    }

    /**
     * Keys by their names without KEYCODE_, separated by spaces, each with a count before a '*' where it is to be sent more
     * than once: "3*DPAD_DOWN ENTER". A key that has no such name, or a count that is no number, is said and skipped.
     */
    public void sendKeys(String keysSequence) {
        Instrumentation instrumentation = getInstrumentation();
        for (String token : keysSequence.split(" ")) {
            if (token.isEmpty()) continue;
            int times = 1;
            String key = token;
            int star = token.indexOf('*');
            if (star >= 0) {
                try {
                    times = Integer.parseInt(token.substring(0, star));
                } catch (NumberFormatException e) {
                    Log.w(TAG, "Invalid repeat count: " + token);
                    continue;
                }
                key = token.substring(star + 1);
            }
            int code;
            try {
                Field field = KeyEvent.class.getField("KEYCODE_" + key);
                code = field.getInt(null);
            } catch (NoSuchFieldException | IllegalAccessException e) {
                Log.w(TAG, "Unknown keycode: KEYCODE_" + key);
                continue;
            }
            for (int i = 0; i < times; i++) sendKey(instrumentation, code);
        }
        instrumentation.waitForIdleSync();
    }

    public void sendKeys(int... keys) {
        Instrumentation instrumentation = getInstrumentation();
        for (int key : keys) sendKey(instrumentation, key);
        instrumentation.waitForIdleSync();
    }

    /** Pairs of a count and a key code. */
    public void sendRepeatedKeys(int... keys) {
        if (keys.length % 2 != 0) throw new IllegalArgumentException("The size of the keys array must be a multiple of 2");
        Instrumentation instrumentation = getInstrumentation();
        for (int i = 0; i < keys.length; i += 2)
            for (int n = 0; n < keys[i]; n++) sendKey(instrumentation, keys[i + 1]);
        instrumentation.waitForIdleSync();
    }

    /** A key down and up; one refused for want of a focused window of the app's is passed over, as Android's are. */
    private static void sendKey(Instrumentation instrumentation, int code) {
        try {
            instrumentation.sendKeyDownUpSync(code);
        } catch (SecurityException e) {
            Log.w(TAG, "a key not sent: " + e);
        }
    }

    /** What the test held is let go before the next: the collector run, and the finalizers. */
    @Override
    protected void tearDown() throws Exception {
        Runtime.getRuntime().gc();
        Runtime.getRuntime().runFinalization();
        Runtime.getRuntime().gc();
        super.tearDown();
    }
}
