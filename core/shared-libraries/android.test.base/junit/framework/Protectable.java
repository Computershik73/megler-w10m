// Megler's android.test.base (tools/build-shared-libraries.sh): JUnit 3's API as Android documents it, written here.
package junit.framework;

/** Code that TestResult.runProtected runs, a failure or an error of it caught there and told to the result. */
public interface Protectable {
    void protect() throws Throwable;
}
