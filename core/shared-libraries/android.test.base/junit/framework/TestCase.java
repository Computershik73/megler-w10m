// Megler's android.test.base (tools/build-shared-libraries.sh): JUnit 3's API as Android documents it, written here.
package junit.framework;

import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;

/**
 * A test case: its name is the public method without arguments that runTest calls, between setUp and tearDown. A TestSuite
 * of a class makes one of the class for each of its methods named test....
 */
public abstract class TestCase extends Assert implements Test {
    private String fName;

    public TestCase() {
    }

    public TestCase(String name) {
        fName = name;
    }

    @Override
    public int countTestCases() {
        return 1;
    }

    protected TestResult createResult() {
        return new TestResult();
    }

    /** Runs it into a result of its own (createResult), which it gives back. */
    public TestResult run() {
        TestResult result = createResult();
        run(result);
        return result;
    }

    @Override
    public void run(TestResult result) {
        result.run(this);
    }

    /**
     * setUp, runTest and tearDown, which runs whatever runTest did; what is thrown is the test's own throwable if it threw one,
     * else tearDown's.
     */
    public void runBare() throws Throwable {
        Throwable thrown = null;
        setUp();
        try {
            runTest();
        } catch (Throwable t) {
            thrown = t;
        } finally {
            try {
                tearDown();
            } catch (Throwable t) {
                if (thrown == null) thrown = t;
            }
        }
        if (thrown != null) throw thrown;
    }

    /** Calls the public method of its name, which takes no arguments; what that throws is thrown on. */
    protected void runTest() throws Throwable {
        assertNotNull("TestCase.fName cannot be null", fName);
        Method method = null;
        try {
            method = getClass().getMethod(fName);
        } catch (NoSuchMethodException e) {
            fail("Method \"" + fName + "\" not found");
        }
        if (!Modifier.isPublic(method.getModifiers())) fail("Method \"" + fName + "\" should be public");
        try {
            method.invoke(this);
        } catch (InvocationTargetException e) {
            e.fillInStackTrace();
            throw e.getTargetException();
        } catch (IllegalAccessException e) {
            e.fillInStackTrace();
            throw e;
        }
    }

    protected void setUp() throws Exception {
    }

    protected void tearDown() throws Exception {
    }

    /** "name(the class)". */
    @Override
    public String toString() {
        return getName() + "(" + getClass().getName() + ")";
    }

    public String getName() {
        return fName;
    }

    public void setName(String name) {
        fName = name;
    }
}
