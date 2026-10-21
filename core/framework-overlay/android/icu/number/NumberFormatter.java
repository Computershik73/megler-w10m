package android.icu.number;

public final class NumberFormatter {
    private NumberFormatter() {}

    public static UnlocalizedNumberFormatter with() { return new UnlocalizedNumberFormatter(); }

    public enum UnitWidth { NARROW, SHORT, FULL_NAME, ISO_CODE, HIDDEN, FORMAL, VARIANT }
}
