// Megler's android.test.base (tools/build-shared-libraries.sh): Android's android.test API as it documents it, written here.
package android.test;

import android.app.Instrumentation;
import junit.framework.Test;
import junit.framework.TestResult;
import junit.framework.TestSuite;

/** A suite that gives its Instrumentation to each InstrumentationTestCase it runs, its own suites' too. */
@Deprecated
public class InstrumentationTestSuite extends TestSuite {
    private final Instrumentation mInstrumentation;

    public InstrumentationTestSuite(Instrumentation instr) {
        mInstrumentation = instr;
    }

    public InstrumentationTestSuite(String name, Instrumentation instr) {
        super(name);
        mInstrumentation = instr;
    }

    public InstrumentationTestSuite(final Class<?> theClass, Instrumentation instr) {
        super(theClass);
        mInstrumentation = instr;
    }

    @Override
    public void addTestSuite(Class testClass) {
        addTest(new InstrumentationTestSuite(testClass, mInstrumentation));
    }

    @Override
    public void runTest(Test test, TestResult result) {
        if (test instanceof InstrumentationTestCase) ((InstrumentationTestCase) test).injectInstrumentation(mInstrumentation);
        super.runTest(test, result);
    }
}
