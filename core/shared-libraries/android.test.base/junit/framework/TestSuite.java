// Megler's android.test.base (tools/build-shared-libraries.sh): JUnit 3's API as Android documents it, written here.
package junit.framework;

import java.io.PrintWriter;
import java.io.StringWriter;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.util.Enumeration;
import java.util.HashSet;
import java.util.Set;
import java.util.Vector;

/**
 * Tests run one after another. Made of a TestCase's class, it has a test of that class for each public method of no
 * arguments named test... that returns nothing -- the class's and its superclasses' up to TestCase, a name once -- made by
 * the class's constructor of a String, or of nothing and then named. What cannot be made so is a test that fails with
 * why (warning): a class with neither constructor, or not public, a test method that is not public, none found.
 */
public class TestSuite implements Test {
    private String fName;
    private final Vector<Test> fTests = new Vector<>();

    public TestSuite() {
    }

    public TestSuite(Class<?> theClass) {
        collect(theClass);
    }

    public TestSuite(Class<? extends TestCase> theClass, String name) {
        this(theClass);
        setName(name);
    }

    public TestSuite(String name) {
        setName(name);
    }

    /** A suite of each class's suite; a class that is no TestCase a warning. */
    public TestSuite(Class<?>... classes) {
        for (Class<?> c : classes) addTest(suiteOf(c));
    }

    public TestSuite(Class<? extends TestCase>[] classes, String name) {
        this(classes);
        setName(name);
    }

    private void collect(Class<?> theClass) {
        fName = theClass.getName();
        try {
            getTestConstructor(theClass);
        } catch (NoSuchMethodException e) {
            addTest(warning("Class " + theClass.getName() + " has no public constructor TestCase(String name) or TestCase()"));
            return;
        }
        if (!Modifier.isPublic(theClass.getModifiers())) {
            addTest(warning("Class " + theClass.getName() + " is not public"));
            return;
        }
        // A subclass's method first: one of the same name further up is the same test.
        Set<String> taken = new HashSet<>();
        for (Class<?> c = theClass; c != null && Test.class.isAssignableFrom(c); c = c.getSuperclass()) {
            for (Method m : c.getDeclaredMethods()) {
                if (!isTestMethod(m) || taken.contains(m.getName())) continue;
                if (!Modifier.isPublic(m.getModifiers())) {
                    addTest(warning("Test method isn't public: " + m.getName() + "(" + theClass.getCanonicalName() + ")"));
                    continue;
                }
                taken.add(m.getName());
                addTest(createTest(theClass, m.getName()));
            }
        }
        if (fTests.isEmpty()) addTest(warning("No tests found in " + theClass.getName()));
    }

    private static boolean isTestMethod(Method m) {
        return m.getParameterTypes().length == 0 && m.getName().startsWith("test") && m.getReturnType() == Void.TYPE;
    }

    private static Test suiteOf(Class<?> c) {
        if (TestCase.class.isAssignableFrom(c)) return new TestSuite(c.asSubclass(TestCase.class));
        return warning(c.getCanonicalName() + " does not extend TestCase");
    }

    /** A test of `theClass` named `name`, made by its constructor of a String or of nothing; a warning where it cannot be. */
    public static Test createTest(Class<?> theClass, String name) {
        Constructor<?> constructor;
        try {
            constructor = getTestConstructor(theClass);
        } catch (NoSuchMethodException e) {
            return warning("Class " + theClass.getName() + " has no public constructor TestCase(String name) or TestCase()");
        }
        try {
            if (constructor.getParameterTypes().length == 1) return (Test) constructor.newInstance(name);
            Object test = constructor.newInstance();
            if (test instanceof TestCase) ((TestCase) test).setName(name);
            return (Test) test;
        } catch (InstantiationException e) {
            return warning("Cannot instantiate test case: " + name + " (" + stackOf(e) + ")");
        } catch (InvocationTargetException e) {
            return warning("Exception in constructor: " + name + " (" + stackOf(e.getTargetException()) + ")");
        } catch (IllegalAccessException e) {
            return warning("Cannot access test case: " + name + " (" + stackOf(e) + ")");
        }
    }

    /** The public constructor of a String, else the public one of nothing. */
    public static Constructor<?> getTestConstructor(Class<?> theClass) throws NoSuchMethodException {
        try {
            return theClass.getConstructor(String.class);
        } catch (NoSuchMethodException e) {
            return theClass.getConstructor();
        }
    }

    /** A test named "warning" that fails with `message`. */
    public static Test warning(final String message) {
        return new TestCase("warning") {
            @Override
            protected void runTest() {
                fail(message);
            }
        };
    }

    private static String stackOf(Throwable t) {
        StringWriter out = new StringWriter();
        t.printStackTrace(new PrintWriter(out));
        return out.toString();
    }

    public void addTest(Test test) {
        fTests.add(test);
    }

    public void addTestSuite(Class<? extends TestCase> testClass) {
        addTest(new TestSuite(testClass));
    }

    @Override
    public int countTestCases() {
        int n = 0;
        for (Test t : fTests) n += t.countTestCases();
        return n;
    }

    /** Each test in turn, until the result is asked to stop. */
    @Override
    public void run(TestResult result) {
        for (Test t : fTests) {
            if (result.shouldStop()) break;
            runTest(t, result);
        }
    }

    public void runTest(Test test, TestResult result) {
        test.run(result);
    }

    public Test testAt(int index) {
        return fTests.get(index);
    }

    public int testCount() {
        return fTests.size();
    }

    public Enumeration<Test> tests() {
        return fTests.elements();
    }

    public String getName() {
        return fName;
    }

    public void setName(String name) {
        fName = name;
    }

    @Override
    public String toString() {
        return fName != null ? fName : super.toString();
    }
}
