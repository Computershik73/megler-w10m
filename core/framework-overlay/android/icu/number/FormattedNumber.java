package android.icu.number;

public final class FormattedNumber {
    private final String text;

    FormattedNumber(String text) { this.text = text; }

    @Override public String toString() { return text; }
}
