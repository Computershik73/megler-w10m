package android.icu.number;

/** Compile-only (tools/build-test-app.sh, "stubs"): what the step icu-number calls of Android 13's classes. */
public abstract class NumberFormatterSettings<T extends NumberFormatterSettings<?>> {
    public T notation(Notation notation) { return null; }
    public T unit(android.icu.util.MeasureUnit unit) { return null; }
    public T precision(Precision precision) { return null; }
    public T roundingMode(java.math.RoundingMode mode) { return null; }
    public T grouping(NumberFormatter.GroupingStrategy strategy) { return null; }
    public T unitWidth(NumberFormatter.UnitWidth width) { return null; }
    public T sign(NumberFormatter.SignDisplay sign) { return null; }
    public T usage(String usage) { return null; }
}
