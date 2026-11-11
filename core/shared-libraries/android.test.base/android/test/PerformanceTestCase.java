// Megler's android.test.base (tools/build-shared-libraries.sh): Android's android.test API as it documents it, written here.
package android.test;

/** A test that measures itself, for a runner that times tests. */
public interface PerformanceTestCase {
    /** What a performance test tells its runner as it goes. */
    interface Intermediates {
        void setInternalIterations(int count);

        void startTiming(boolean realTime);

        void addIntermediate(String name);

        void addIntermediate(String name, long timeInNS);

        void finishTiming(boolean realTime);
    }

    /** Begins it; the number of iterations it runs internally, or 0 where the runner is to choose. */
    int startPerformance(Intermediates intermediates);

    /** Whether it is to be run only when performance is measured. */
    boolean isPerformanceOnly();
}
