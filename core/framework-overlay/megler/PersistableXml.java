package megler;

import android.os.PersistableBundle;
import java.io.IOException;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.xmlpull.v1.XmlSerializer;

/**
 * A PersistableBundle as XML, the way Android writes one (PersistableBundle.saveToXml, XmlUtils.writeMapXml) and reads it
 * back (restoreFromXml): an element a value, named by its type -- int, long, double, boolean, string, their arrays (int-array
 * of item elements), and a bundle inside as pbundle_as_map -- each with its key as the attribute name. A file Android wrote
 * reads here, and one written here reads on Android.
 *
 * Android's own Settings (AOSP 9, which Megler's package carries) keeps whether each of its main screen's notices was put
 * away so, and ended at its start on the null restoreFromXml said.
 */
public final class PersistableXml {
    private PersistableXml() {}

    /** The values of `b` as the children of the element the serializer is in. */
    public static void save(PersistableBundle b, XmlSerializer out) throws IOException {
        for (String k : b.keySet()) write(out, k, b.get(k));
    }

    private static void write(XmlSerializer out, String name, Object v) throws IOException {
        if (v == null) {
            out.startTag(null, "null"); out.attribute(null, "name", name); out.endTag(null, "null");
        } else if (v instanceof PersistableBundle) {
            out.startTag(null, "pbundle_as_map"); out.attribute(null, "name", name);
            save((PersistableBundle) v, out);
            out.endTag(null, "pbundle_as_map");
        } else if (v instanceof String) {
            out.startTag(null, "string"); out.attribute(null, "name", name); out.text((String) v); out.endTag(null, "string");
        } else if (v instanceof Integer || v instanceof Long || v instanceof Double || v instanceof Boolean) {
            String tag = v instanceof Integer ? "int" : v instanceof Long ? "long" : v instanceof Double ? "double" : "boolean";
            out.startTag(null, tag); out.attribute(null, "name", name); out.attribute(null, "value", v.toString()); out.endTag(null, tag);
        } else if (v instanceof int[] || v instanceof long[] || v instanceof double[] || v instanceof boolean[]) {
            String tag = v instanceof int[] ? "int-array" : v instanceof long[] ? "long-array" : v instanceof double[] ? "double-array" : "boolean-array";
            int n = java.lang.reflect.Array.getLength(v);
            out.startTag(null, tag); out.attribute(null, "name", name); out.attribute(null, "num", Integer.toString(n));
            for (int i = 0; i < n; i++) {
                out.startTag(null, "item"); out.attribute(null, "value", String.valueOf(java.lang.reflect.Array.get(v, i))); out.endTag(null, "item");
            }
            out.endTag(null, tag);
        } else if (v instanceof String[]) {
            String[] a = (String[]) v;
            out.startTag(null, "string-array"); out.attribute(null, "name", name); out.attribute(null, "num", Integer.toString(a.length));
            for (String s : a) {
                out.startTag(null, "item"); if (s != null) out.attribute(null, "value", s); out.endTag(null, "item");
            }
            out.endTag(null, "string-array");
        }
    }

    /** The bundle whose values are the children of the element the parser is at the start of; it stops at that element's end. */
    public static PersistableBundle restore(XmlPullParser in) throws IOException, XmlPullParserException {
        PersistableBundle b = new PersistableBundle();
        int depth = in.getDepth();
        int t;
        while ((t = in.next()) != XmlPullParser.END_DOCUMENT && (t != XmlPullParser.END_TAG || in.getDepth() > depth)) {
            if (t != XmlPullParser.START_TAG) continue;
            String tag = in.getName(), name = in.getAttributeValue(null, "name"), value = in.getAttributeValue(null, "value");
            if (name == null) { skip(in); continue; }
            switch (tag) {
                case "int": b.putInt(name, Integer.parseInt(value)); skip(in); break;
                case "long": b.putLong(name, Long.parseLong(value)); skip(in); break;
                case "double": b.putDouble(name, Double.parseDouble(value)); skip(in); break;
                case "boolean": b.putBoolean(name, Boolean.parseBoolean(value)); skip(in); break;
                case "string": b.putString(name, in.nextText()); break;
                case "pbundle_as_map": b.putPersistableBundle(name, restore(in)); break;
                case "int-array": case "long-array": case "double-array": case "boolean-array": case "string-array": {
                    java.util.ArrayList<String> items = items(in);
                    int n = items.size();
                    if (tag.equals("string-array")) b.putStringArray(name, items.toArray(new String[0]));
                    else if (tag.equals("int-array")) { int[] a = new int[n]; for (int i = 0; i < n; i++) a[i] = Integer.parseInt(items.get(i)); b.putIntArray(name, a); }
                    else if (tag.equals("long-array")) { long[] a = new long[n]; for (int i = 0; i < n; i++) a[i] = Long.parseLong(items.get(i)); b.putLongArray(name, a); }
                    else if (tag.equals("double-array")) { double[] a = new double[n]; for (int i = 0; i < n; i++) a[i] = Double.parseDouble(items.get(i)); b.putDoubleArray(name, a); }
                    else { boolean[] a = new boolean[n]; for (int i = 0; i < n; i++) a[i] = Boolean.parseBoolean(items.get(i)); b.putBooleanArray(name, a); }
                    break;
                }
                default: skip(in);
            }
        }
        return b;
    }

    /** The values of an array element's items, the parser left at its end. */
    private static java.util.ArrayList<String> items(XmlPullParser in) throws IOException, XmlPullParserException {
        java.util.ArrayList<String> out = new java.util.ArrayList<>();
        int depth = in.getDepth();
        int t;
        while ((t = in.next()) != XmlPullParser.END_DOCUMENT && (t != XmlPullParser.END_TAG || in.getDepth() > depth)) {
            if (t == XmlPullParser.START_TAG && "item".equals(in.getName())) {
                out.add(in.getAttributeValue(null, "value"));
                skip(in);
            }
        }
        return out;
    }

    /** The parser moved to the end of the element it is at the start of. */
    private static void skip(XmlPullParser in) throws IOException, XmlPullParserException {
        int depth = in.getDepth();
        int t;
        while ((t = in.next()) != XmlPullParser.END_DOCUMENT && (t != XmlPullParser.END_TAG || in.getDepth() > depth)) {}
    }

    /** Whether Android keeps a value of this type in a PersistableBundle. */
    public static boolean validType(Object v) {
        return v == null || v instanceof Integer || v instanceof Long || v instanceof Double || v instanceof String || v instanceof Boolean
                || v instanceof int[] || v instanceof long[] || v instanceof double[] || v instanceof String[] || v instanceof boolean[]
                || v instanceof PersistableBundle;
    }
}
