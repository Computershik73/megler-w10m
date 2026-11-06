package com.android.internal.util;

import android.util.Xml;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.HashSet;
import java.util.List;
import java.util.Map;
import java.util.Set;
import org.xmlpull.v1.XmlPullParser;
import org.xmlpull.v1.XmlPullParserException;
import org.xmlpull.v1.XmlSerializer;

/**
 * Android's hidden XML helpers, which apps built on old platform code call although the SDK never had them: Launcher2 and
 * the launchers made from it read their default workspace with beginDocument and nextElement (Samsung's TouchWiz launcher
 * stopped at its start with NoClassDefFoundError before). Here: walking a document, the attribute and value conversions,
 * and the map, list and value XML that SharedPreferences files are written in, for the types they hold.
 */
public class XmlUtils {
    public static void skipCurrentTag(XmlPullParser parser) throws XmlPullParserException, IOException {
        int outer = parser.getDepth();
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT && (type != XmlPullParser.END_TAG || parser.getDepth() > outer)) { }
    }

    public static final int convertValueToList(CharSequence value, String[] options, int defaultValue) {
        if (value != null) {
            for (int i = 0; i < options.length; i++) if (value.equals(options[i])) return i;
        }
        return defaultValue;
    }

    public static final boolean convertValueToBoolean(CharSequence value, boolean defaultValue) {
        if (value == null) return defaultValue;
        return value.equals("1") || value.equals("true") || value.equals("TRUE");
    }

    /** A decimal, 0x or # hexadecimal, or 0 octal number, with a sign: as Android reads integer attributes. */
    public static final int convertValueToInt(CharSequence charSeq, int defaultValue) {
        if (charSeq == null) return defaultValue;
        String s = charSeq.toString();
        int len = s.length();
        int sign = 1, index = 0, base = 10;
        if (len > 0 && s.charAt(0) == '-') { sign = -1; index++; }
        if (index < len && s.charAt(index) == '0') {
            if (index == len - 1) return 0;
            char c = s.charAt(index + 1);
            if (c == 'x' || c == 'X') { index += 2; base = 16; }
            else { index++; base = 8; }
        } else if (index < len && s.charAt(index) == '#') {
            index++;
            base = 16;
        }
        return (int) (Long.parseLong(s.substring(index), base) * sign);
    }

    public static int convertValueToUnsignedInt(String value, int defaultValue) {
        return value == null ? defaultValue : parseUnsignedIntAttribute(value);
    }

    public static int parseUnsignedIntAttribute(CharSequence charSeq) {
        String s = String.valueOf(charSeq);
        int len = s.length(), index = 0, base = 10;
        if (len > 0 && s.charAt(0) == '0') {
            if (len == 1) return 0;
            char c = s.charAt(1);
            if (c == 'x' || c == 'X') { index = 2; base = 16; }
            else { index = 1; base = 8; }
        } else if (len > 0 && s.charAt(0) == '#') {
            index = 1;
            base = 16;
        }
        return (int) Long.parseLong(s.substring(index), base);
    }

    public static final void beginDocument(XmlPullParser parser, String firstElementName) throws XmlPullParserException, IOException {
        int type;
        while ((type = parser.next()) != XmlPullParser.START_TAG && type != XmlPullParser.END_DOCUMENT) { }
        if (type != XmlPullParser.START_TAG) throw new XmlPullParserException("No start tag found");
        if (!parser.getName().equals(firstElementName)) {
            throw new XmlPullParserException("Unexpected start tag: found " + parser.getName() + ", expected " + firstElementName);
        }
    }

    public static final void nextElement(XmlPullParser parser) throws XmlPullParserException, IOException {
        int type;
        while ((type = parser.next()) != XmlPullParser.START_TAG && type != XmlPullParser.END_DOCUMENT) { }
    }

    public static boolean nextElementWithin(XmlPullParser parser, int outerDepth) throws IOException, XmlPullParserException {
        for (;;) {
            int type = parser.next();
            if (type == XmlPullParser.END_DOCUMENT || (type == XmlPullParser.END_TAG && parser.getDepth() == outerDepth)) return false;
            if (type == XmlPullParser.START_TAG && parser.getDepth() == outerDepth + 1) return true;
        }
    }

    public static int readIntAttribute(XmlPullParser in, String name, int defaultValue) {
        try { return Integer.parseInt(in.getAttributeValue(null, name)); } catch (NumberFormatException e) { return defaultValue; }
    }
    public static int readIntAttribute(XmlPullParser in, String name) throws IOException {
        String value = in.getAttributeValue(null, name);
        try { return Integer.parseInt(value); } catch (NumberFormatException e) { throw new java.net.ProtocolException("problem parsing " + name + "=" + value + " as int"); }
    }
    public static void writeIntAttribute(XmlSerializer out, String name, int value) throws IOException {
        out.attribute(null, name, Integer.toString(value));
    }
    public static long readLongAttribute(XmlPullParser in, String name, long defaultValue) {
        try { return Long.parseLong(in.getAttributeValue(null, name)); } catch (NumberFormatException e) { return defaultValue; }
    }
    public static long readLongAttribute(XmlPullParser in, String name) throws IOException {
        String value = in.getAttributeValue(null, name);
        try { return Long.parseLong(value); } catch (NumberFormatException e) { throw new java.net.ProtocolException("problem parsing " + name + "=" + value + " as long"); }
    }
    public static void writeLongAttribute(XmlSerializer out, String name, long value) throws IOException {
        out.attribute(null, name, Long.toString(value));
    }
    public static float readFloatAttribute(XmlPullParser in, String name) throws IOException {
        String value = in.getAttributeValue(null, name);
        try { return Float.parseFloat(value); } catch (NumberFormatException e) { throw new java.net.ProtocolException("problem parsing " + name + "=" + value + " as float"); }
    }
    public static void writeFloatAttribute(XmlSerializer out, String name, float value) throws IOException {
        out.attribute(null, name, Float.toString(value));
    }
    public static boolean readBooleanAttribute(XmlPullParser in, String name) {
        return Boolean.parseBoolean(in.getAttributeValue(null, name));
    }
    public static boolean readBooleanAttribute(XmlPullParser in, String name, boolean defaultValue) {
        String value = in.getAttributeValue(null, name);
        return value == null ? defaultValue : Boolean.parseBoolean(value);
    }
    public static void writeBooleanAttribute(XmlSerializer out, String name, boolean value) throws IOException {
        out.attribute(null, name, Boolean.toString(value));
    }
    public static String readStringAttribute(XmlPullParser in, String name) {
        return in.getAttributeValue(null, name);
    }
    public static void writeStringAttribute(XmlSerializer out, String name, CharSequence value) throws IOException {
        if (value != null) out.attribute(null, name, value.toString());
    }

    // ---- maps, lists and values: the XML of SharedPreferences files

    @SuppressWarnings("unchecked")
    public static final HashMap<String, ?> readMapXml(InputStream in) throws XmlPullParserException, IOException {
        XmlPullParser parser = Xml.newPullParser();
        parser.setInput(in, StandardCharsets.UTF_8.name());
        return (HashMap<String, ?>) readValueXml(parser, new String[1]);
    }

    public static final ArrayList readListXml(InputStream in) throws XmlPullParserException, IOException {
        XmlPullParser parser = Xml.newPullParser();
        parser.setInput(in, StandardCharsets.UTF_8.name());
        return (ArrayList) readValueXml(parser, new String[1]);
    }

    public static final HashSet readSetXml(InputStream in) throws XmlPullParserException, IOException {
        XmlPullParser parser = Xml.newPullParser();
        parser.setInput(in, null);
        return (HashSet) readValueXml(parser, new String[1]);
    }

    /** The value at the parser's next start tag; name[0] is its name attribute. */
    public static final Object readValueXml(XmlPullParser parser, String[] name) throws XmlPullParserException, IOException {
        int type = parser.getEventType();
        do {
            if (type == XmlPullParser.START_TAG) return readThisValueXml(parser, name);
            if (type == XmlPullParser.END_TAG) throw new XmlPullParserException("Unexpected end tag at: " + parser.getName());
            if (type == XmlPullParser.TEXT) throw new XmlPullParserException("Unexpected text: " + parser.getText());
            type = parser.next();
        } while (type != XmlPullParser.END_DOCUMENT);
        throw new XmlPullParserException("Unexpected end of document");
    }

    public static final HashMap<String, ?> readThisMapXml(XmlPullParser parser, String endTag, String[] name) throws XmlPullParserException, IOException {
        HashMap<String, Object> map = new HashMap<>();
        int type = parser.getEventType();
        do {
            if (type == XmlPullParser.START_TAG) {
                Object value = readThisValueXml(parser, name);
                map.put(name[0], value);
            } else if (type == XmlPullParser.END_TAG) {
                if (parser.getName().equals(endTag)) return map;
                throw new XmlPullParserException("Expected " + endTag + " end tag at: " + parser.getName());
            }
            type = parser.next();
        } while (type != XmlPullParser.END_DOCUMENT);
        throw new XmlPullParserException("Document ended before " + endTag + " end tag");
    }

    public static final ArrayList readThisListXml(XmlPullParser parser, String endTag, String[] name) throws XmlPullParserException, IOException {
        ArrayList<Object> list = new ArrayList<>();
        int type = parser.getEventType();
        do {
            if (type == XmlPullParser.START_TAG) {
                list.add(readThisValueXml(parser, name));
            } else if (type == XmlPullParser.END_TAG) {
                if (parser.getName().equals(endTag)) return list;
                throw new XmlPullParserException("Expected " + endTag + " end tag at: " + parser.getName());
            }
            type = parser.next();
        } while (type != XmlPullParser.END_DOCUMENT);
        throw new XmlPullParserException("Document ended before " + endTag + " end tag");
    }

    public static final HashSet readThisSetXml(XmlPullParser parser, String endTag, String[] name) throws XmlPullParserException, IOException {
        HashSet<Object> set = new HashSet<>();
        int type = parser.getEventType();
        do {
            if (type == XmlPullParser.START_TAG) {
                set.add(readThisValueXml(parser, name));
            } else if (type == XmlPullParser.END_TAG) {
                if (parser.getName().equals(endTag)) return set;
                throw new XmlPullParserException("Expected " + endTag + " end tag at: " + parser.getName());
            }
            type = parser.next();
        } while (type != XmlPullParser.END_DOCUMENT);
        throw new XmlPullParserException("Document ended before " + endTag + " end tag");
    }

    private static Object readThisValueXml(XmlPullParser parser, String[] name) throws XmlPullParserException, IOException {
        String valueName = parser.getAttributeValue(null, "name");
        String tag = parser.getName();
        Object value;
        if (tag.equals("null")) {
            value = null;
        } else if (tag.equals("string")) {
            StringBuilder b = new StringBuilder();
            int type;
            while ((type = parser.next()) != XmlPullParser.END_DOCUMENT) {
                if (type == XmlPullParser.END_TAG) {
                    if (parser.getName().equals("string")) {
                        name[0] = valueName;
                        return b.toString();
                    }
                    throw new XmlPullParserException("Unexpected end tag in <string>: " + parser.getName());
                } else if (type == XmlPullParser.TEXT) {
                    b.append(parser.getText());
                } else if (type == XmlPullParser.START_TAG) {
                    throw new XmlPullParserException("Unexpected start tag in <string>: " + parser.getName());
                }
            }
            throw new XmlPullParserException("Unexpected end of document in <string>");
        } else if (tag.equals("map")) {
            parser.next();
            value = readThisMapXml(parser, "map", name);
            name[0] = valueName;
            return value;
        } else if (tag.equals("list")) {
            parser.next();
            value = readThisListXml(parser, "list", name);
            name[0] = valueName;
            return value;
        } else if (tag.equals("set")) {
            parser.next();
            value = readThisSetXml(parser, "set", name);
            name[0] = valueName;
            return value;
        } else {
            String v = parser.getAttributeValue(null, "value");
            try {
                switch (tag) {
                    case "int": value = Integer.parseInt(v); break;
                    case "long": value = Long.valueOf(v); break;
                    case "float": value = Float.valueOf(v); break;
                    case "double": value = Double.valueOf(v); break;
                    case "boolean": value = Boolean.valueOf(v); break;
                    default: throw new XmlPullParserException("Unknown tag: " + tag);
                }
            } catch (NumberFormatException e) {
                throw new XmlPullParserException("Not a number in value attribute in <" + tag + ">");
            }
        }
        // (The value's own end tag.)
        int type;
        while ((type = parser.next()) != XmlPullParser.END_DOCUMENT) {
            if (type == XmlPullParser.END_TAG) {
                if (parser.getName().equals(tag)) {
                    name[0] = valueName;
                    return value;
                }
                throw new XmlPullParserException("Unexpected end tag in <" + tag + ">: " + parser.getName());
            } else if (type == XmlPullParser.TEXT) {
                throw new XmlPullParserException("Unexpected text in <" + tag + ">: " + parser.getName());
            } else if (type == XmlPullParser.START_TAG) {
                throw new XmlPullParserException("Unexpected start tag in <" + tag + ">: " + parser.getName());
            }
        }
        throw new XmlPullParserException("Unexpected end of document in <" + tag + ">");
    }

    public static final void writeMapXml(Map val, OutputStream out) throws XmlPullParserException, IOException {
        XmlSerializer serializer = Xml.newSerializer();
        serializer.setOutput(out, StandardCharsets.UTF_8.name());
        serializer.startDocument(null, true);
        serializer.setFeature("http://xmlpull.org/v1/doc/features.html#indent-output", true);
        writeMapXml(val, null, serializer);
        serializer.endDocument();
    }

    public static final void writeListXml(List val, OutputStream out) throws XmlPullParserException, IOException {
        XmlSerializer serializer = Xml.newSerializer();
        serializer.setOutput(out, StandardCharsets.UTF_8.name());
        serializer.startDocument(null, true);
        serializer.setFeature("http://xmlpull.org/v1/doc/features.html#indent-output", true);
        writeListXml(val, null, serializer);
        serializer.endDocument();
    }

    public static final void writeMapXml(Map val, String name, XmlSerializer out) throws XmlPullParserException, IOException {
        if (val == null) {
            out.startTag(null, "null");
            out.endTag(null, "null");
            return;
        }
        out.startTag(null, "map");
        if (name != null) out.attribute(null, "name", name);
        for (Object o : val.entrySet()) {
            Map.Entry e = (Map.Entry) o;
            writeValueXml(e.getValue(), (String) e.getKey(), out);
        }
        out.endTag(null, "map");
    }

    public static final void writeListXml(List val, String name, XmlSerializer out) throws XmlPullParserException, IOException {
        if (val == null) {
            out.startTag(null, "null");
            out.endTag(null, "null");
            return;
        }
        out.startTag(null, "list");
        if (name != null) out.attribute(null, "name", name);
        for (Object o : val) writeValueXml(o, null, out);
        out.endTag(null, "list");
    }

    public static final void writeSetXml(Set val, String name, XmlSerializer out) throws XmlPullParserException, IOException {
        if (val == null) {
            out.startTag(null, "null");
            out.endTag(null, "null");
            return;
        }
        out.startTag(null, "set");
        if (name != null) out.attribute(null, "name", name);
        for (Object o : val) writeValueXml(o, null, out);
        out.endTag(null, "set");
    }

    public static final void writeValueXml(Object v, String name, XmlSerializer out) throws XmlPullParserException, IOException {
        if (v == null) {
            out.startTag(null, "null");
            if (name != null) out.attribute(null, "name", name);
            out.endTag(null, "null");
            return;
        }
        if (v instanceof String) {
            out.startTag(null, "string");
            if (name != null) out.attribute(null, "name", name);
            out.text(v.toString());
            out.endTag(null, "string");
            return;
        }
        if (v instanceof Map) { writeMapXml((Map) v, name, out); return; }
        if (v instanceof List) { writeListXml((List) v, name, out); return; }
        if (v instanceof Set) { writeSetXml((Set) v, name, out); return; }
        if (v instanceof CharSequence) {
            out.startTag(null, "string");
            if (name != null) out.attribute(null, "name", name);
            out.text(v.toString());
            out.endTag(null, "string");
            return;
        }
        String tag;
        if (v instanceof Integer) tag = "int";
        else if (v instanceof Long) tag = "long";
        else if (v instanceof Float) tag = "float";
        else if (v instanceof Double) tag = "double";
        else if (v instanceof Boolean) tag = "boolean";
        else throw new RuntimeException("writeValueXml: unable to write value " + v);
        out.startTag(null, tag);
        if (name != null) out.attribute(null, "name", name);
        out.attribute(null, "value", v.toString());
        out.endTag(null, tag);
    }
}
