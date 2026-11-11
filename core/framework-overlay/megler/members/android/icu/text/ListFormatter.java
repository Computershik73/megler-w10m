/*
 * Megler: the two getInstance methods Android 13 added to android.icu.text.ListFormatter (ICU 67), which Android 9's
 * core-libart (ICU 60) has without them; the runtime takes them from this class (mg_dvm_class.c, "methods a boot class
 * lacks"). A type and a width are a list pattern's name in ICU's data, as ICU 67 names them -- "standard", "or", "unit",
 * each alone or with "-short" or "-narrow" -- and ICU 60's own formatters are made for a name by ListFormatter's cache, which
 * is private: asked by reflection. ICU 60's data (CLDR 32) has no narrow "and", and no short or narrow "or"; the next wider
 * one that is there stands for a missing one, which is what CLDR's own data falls back to.
 */
package megler.members.android.icu.text;

import android.icu.text.ListFormatter$Type;
import android.icu.text.ListFormatter$Width;
import android.icu.util.ULocale;
import java.lang.reflect.Field;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Method;
import java.util.Locale;

public final class ListFormatter {
    private static Object sCache;
    private static Method sGet;

    private ListFormatter() {}

    public static android.icu.text.ListFormatter getInstance(Locale locale, ListFormatter$Type type, ListFormatter$Width width) {
        return getInstance(ULocale.forLocale(locale), type, width);
    }

    public static android.icu.text.ListFormatter getInstance(ULocale locale, ListFormatter$Type type, ListFormatter$Width width) {
        if (type == null || width == null) throw new NullPointerException();
        String base = type == ListFormatter$Type.AND ? "standard" : type == ListFormatter$Type.OR ? "or" : "unit";
        android.icu.text.ListFormatter made = null;
        if (width == ListFormatter$Width.NARROW) made = styled(locale, base + "-narrow");
        if (made == null && width != ListFormatter$Width.WIDE) made = styled(locale, base + "-short");
        if (made == null) made = styled(locale, base);
        return made != null ? made : android.icu.text.ListFormatter.getInstance(locale);
    }

    /* ICU 60's formatter of the locale's list pattern of this name; null when its data has none. */
    private static android.icu.text.ListFormatter styled(ULocale locale, String style) {
        try {
            Object cache;
            Method get;
            synchronized (ListFormatter.class) {
                if (sGet == null) {
                    Field field = android.icu.text.ListFormatter.class.getDeclaredField("cache");
                    field.setAccessible(true);
                    sCache = field.get(null);
                    Method method = sCache.getClass().getDeclaredMethod("get", ULocale.class, String.class);
                    method.setAccessible(true);
                    sGet = method;
                }
                cache = sCache;
                get = sGet;
            }
            return (android.icu.text.ListFormatter) get.invoke(cache, locale, style);
        } catch (InvocationTargetException e) {
            if (e.getCause() instanceof java.util.MissingResourceException) return null;
            throw new IllegalStateException(e.getCause());
        } catch (ReflectiveOperationException e) {
            throw new IllegalStateException(e);
        }
    }
}
