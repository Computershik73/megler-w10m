// Megler's android.test.base (tools/build-shared-libraries.sh): Android's android.test API as it documents it, written here.
package android.test;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/** A test method run `numIterations` times, each told to the instrumentation (InstrumentationTestCase.runTest). */
@Target(ElementType.METHOD)
@Retention(RetentionPolicy.RUNTIME)
public @interface RepetitiveTest {
    int numIterations() default 1;
}
