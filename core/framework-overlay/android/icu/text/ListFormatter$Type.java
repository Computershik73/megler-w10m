package android.icu.text;

/**
 * Megler: ListFormatter.Type, which Android 13 added (ICU 67) and Android 9's core-libart (ICU 60) has not. ListFormatter is
 * core-libart's and has no sources here to nest it in, so it is a class of its own by the name a nested one has; the
 * getInstance that takes it is megler.members.android.icu.text.ListFormatter's. Citymapper ended with NoClassDefFoundError as
 * it listed a route's lines (0.1.0.690).
 */
public enum ListFormatter$Type {
    AND, OR, UNITS
}
