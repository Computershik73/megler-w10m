// Megler's android.test.base (tools/build-shared-libraries.sh): Android's android.test API as it documents it, written here.
package android.test.suitebuilder.annotation;

import java.lang.annotation.ElementType;
import java.lang.annotation.Retention;
import java.lang.annotation.RetentionPolicy;
import java.lang.annotation.Target;

/** A mark on a test method or class, for a runner that chooses tests by it. */
@Deprecated
@Target({ ElementType.METHOD, ElementType.TYPE })
@Retention(RetentionPolicy.RUNTIME)
public @interface SmallTest {
}
