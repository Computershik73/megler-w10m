// Overlay of core/framework/gen-src/android/util/TimeFormatException.java (tools/build-framework.sh).
// It differs in keeping its message, as Android's does: the generated stub dropped it, and android.text.format.Time's parse
// and parse3339 say in it where a date could not be read.
package android.util;

public class TimeFormatException extends RuntimeException {
    public TimeFormatException(String s) { super(s); }
}
