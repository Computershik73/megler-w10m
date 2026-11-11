// Megler's android.test.base (tools/build-shared-libraries.sh): Android's android.test API as it documents it, written here.
package android.test;

import android.content.ContentValues;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.util.Log;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import junit.framework.TestCase;

/** A test case with a Context, which whoever runs it gives it (setContext), and checks of what a permission guards. */
@Deprecated
public class AndroidTestCase extends TestCase {
    protected Context mContext;
    private Context mTestContext;

    public AndroidTestCase() {
    }

    @Override
    protected void setUp() throws Exception {
        super.setUp();
    }

    @Override
    protected void tearDown() throws Exception {
        super.tearDown();
    }

    public void testAndroidTestCaseSetupProperly() {
        assertNotNull("Context is null. setContext should be called before tests are run", mContext);
    }

    public void setContext(Context context) {
        mContext = context;
    }

    public Context getContext() {
        return mContext;
    }

    /** The test package's own context, where the runner gives one. */
    public void setTestContext(Context context) {
        mTestContext = context;
    }

    public Context getTestContext() {
        return mTestContext;
    }

    /** Starting that activity is refused with a SecurityException that names `permission`. */
    public void assertActivityRequiresPermission(String packageName, String className, String permission) {
        Intent intent = new Intent().setClassName(packageName, className).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        try {
            getContext().startActivity(intent);
            fail("expected security exception for " + permission);
        } catch (SecurityException expected) {
            assertNotNull("security exception's error message.", expected.getMessage());
            assertTrue("error message should contain " + permission + ".", expected.getMessage().contains(permission));
        }
    }

    /** A query of `uri` is refused with a SecurityException that names `permission`. */
    public void assertReadingContentUriRequiresPermission(Uri uri, String permission) {
        try {
            getContext().getContentResolver().query(uri, null, null, null, null);
            fail("expected SecurityException requiring " + permission);
        } catch (SecurityException expected) {
            refusedFor(expected, permission);
        }
    }

    /** An insert into `uri` is refused with a SecurityException that names `permission`. */
    public void assertWritingContentUriRequiresPermission(Uri uri, String permission) {
        try {
            getContext().getContentResolver().insert(uri, new ContentValues());
            fail("expected SecurityException requiring " + permission);
        } catch (SecurityException expected) {
            refusedFor(expected, permission);
        }
    }

    private static void refusedFor(SecurityException e, String permission) {
        String message = e.getMessage();
        assertNotNull("security exception's error message.", message);
        assertTrue("error message should contain \"" + permission + "\". Got: \"" + message + "\".", message.contains(permission));
    }

    /** Sets this test's fields of objects that `testCaseClass` declares, and that are not static, to null, for the collector. */
    protected void scrubClass(final Class<?> testCaseClass) throws IllegalAccessException {
        for (Field field : testCaseClass.getDeclaredFields()) {
            if (field.getType().isPrimitive() || Modifier.isStatic(field.getModifiers())) continue;
            try {
                field.setAccessible(true);
                field.set(this, null);
            } catch (Exception e) {
                Log.d("TestCase", "Error: Could not nullify field!");
            }
            if (field.get(this) != null) Log.d("TestCase", "Error: Could not nullify field!");
        }
    }
}
