// Megler's android.test.base (tools/build-shared-libraries.sh): JUnit 3's API as Android documents it, written here.
package junit.framework;

/** Something that can be run into a TestResult and counted: a TestCase, or a TestSuite of tests. */
public interface Test {
    /** How many test cases running this runs. */
    int countTestCases();

    /** Runs it, its outcome told to `result`. */
    void run(TestResult result);
}
