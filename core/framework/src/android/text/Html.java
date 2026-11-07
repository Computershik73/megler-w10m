package android.text;

import android.content.res.Resources;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.text.style.*;

import org.xml.sax.Attributes;
import org.xml.sax.ContentHandler;
import org.xml.sax.DTDHandler;
import org.xml.sax.EntityResolver;
import org.xml.sax.ErrorHandler;
import org.xml.sax.InputSource;
import org.xml.sax.Locator;
import org.xml.sax.SAXException;
import org.xml.sax.XMLReader;
import org.xml.sax.helpers.AttributesImpl;

import java.io.IOException;
import java.io.InputStreamReader;
import java.io.StringReader;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Locale;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/**
 * HTML into styled text as Android makes it. A SAX reader (HtmlReader, in TagSoup's place) gives the elements to a
 * ContentHandler (Converter, Android's HtmlToSpannedConverter), which puts a mark where an element starts and the element's
 * span from the mark where it ends. A TagHandler gets what Android gives it: each element the converter does not know --
 * html and body among them, which the reader puts around the text as TagSoup does -- as it opens and as it closes, with the
 * reader. Apps that want an element's attributes take the reader's ContentHandler and put their own in its place, which
 * hands everything on to it. VTB's (ru.vtb.a.bg11) asked the reader for its handler at its first element; the reader was
 * null here before, and the app's main thread ended with a NullPointerException (package 669).
 */
public class Html {
    public static final int FROM_HTML_MODE_LEGACY = 0, FROM_HTML_MODE_COMPACT = 63, TO_HTML_PARAGRAPH_LINES_CONSECUTIVE = 0, TO_HTML_PARAGRAPH_LINES_INDIVIDUAL = 1,
        FROM_HTML_SEPARATOR_LINE_BREAK_PARAGRAPH = 1, FROM_HTML_SEPARATOR_LINE_BREAK_HEADING = 2, FROM_HTML_SEPARATOR_LINE_BREAK_LIST_ITEM = 4, FROM_HTML_SEPARATOR_LINE_BREAK_LIST = 8,
        FROM_HTML_SEPARATOR_LINE_BREAK_DIV = 16, FROM_HTML_SEPARATOR_LINE_BREAK_BLOCKQUOTE = 32, FROM_HTML_OPTION_USE_CSS_COLORS = 256;
    public interface ImageGetter { Drawable getDrawable(String source); }
    public interface TagHandler { void handleTag(boolean opening, String tag, Editable output, XMLReader xmlReader); }
    private Html() {}
    @Deprecated public static Spanned fromHtml(String source) { return fromHtml(source, FROM_HTML_MODE_LEGACY, null, null); }
    public static Spanned fromHtml(String source, int flags) { return fromHtml(source, flags, null, null); }
    @Deprecated public static Spanned fromHtml(String source, ImageGetter g, TagHandler t) { return fromHtml(source, FROM_HTML_MODE_LEGACY, g, t); }
    public static Spanned fromHtml(String source, int flags, ImageGetter getter, TagHandler handler) {
        return new Converter(source, getter, handler, new HtmlReader(), flags).convert();
    }
    public static String toHtml(Spanned text) { return toHtml(text, TO_HTML_PARAGRAPH_LINES_CONSECUTIVE); }
    public static String toHtml(Spanned text, int option) { return "<p dir=\"ltr\">" + TextUtils.htmlEncode(text.toString()).replace("\n", "<br>\n") + "</p>\n"; }
    public static String escapeHtml(CharSequence text) { return TextUtils.htmlEncode(text.toString()); }

    /** Android's HtmlToSpannedConverter: the elements it knows made spans, the others the TagHandler's. */
    private static final class Converter implements ContentHandler {
        private static final float[] HEADING_SIZES = { 1.5f, 1.4f, 1.3f, 1.2f, 1.1f, 1f };
        private static final Pattern TEXT_ALIGN = Pattern.compile("(?:\\s+|\\A)text-align\\s*:\\s*(\\S*)\\b");
        private static final Pattern FOREGROUND = Pattern.compile("(?:\\s+|\\A)color\\s*:\\s*(\\S*)\\b");
        private static final Pattern BACKGROUND = Pattern.compile("(?:\\s+|\\A)background(?:-color)?\\s*:\\s*(\\S*)\\b");
        private static final Pattern DECORATION = Pattern.compile("(?:\\s+|\\A)text-decoration\\s*:\\s*(\\S*)\\b");
        /* Color.getHtmlColor's names, and where FROM_HTML_OPTION_USE_CSS_COLORS gives CSS's value instead (Android's sColorMap). */
        private static final HashMap<String, Integer> NAMED = new HashMap<>(), CSS = new HashMap<>();
        static {
            Object[] named = { "black", 0xFF000000, "darkgray", 0xFF444444, "gray", 0xFF888888, "lightgray", 0xFFCCCCCC, "white", 0xFFFFFFFF,
                "red", 0xFFFF0000, "green", 0xFF00FF00, "blue", 0xFF0000FF, "yellow", 0xFFFFFF00, "cyan", 0xFF00FFFF, "magenta", 0xFFFF00FF,
                "aqua", 0xFF00FFFF, "fuchsia", 0xFFFF00FF, "darkgrey", 0xFF444444, "grey", 0xFF888888, "lightgrey", 0xFFCCCCCC, "lime", 0xFF00FF00,
                "maroon", 0xFF800000, "navy", 0xFF000080, "olive", 0xFF808000, "purple", 0xFF800080, "silver", 0xFFC0C0C0, "teal", 0xFF008080 };
            Object[] css = { "darkgray", 0xFFA9A9A9, "gray", 0xFF808080, "lightgray", 0xFFD3D3D3, "darkgrey", 0xFFA9A9A9, "grey", 0xFF808080,
                "lightgrey", 0xFFD3D3D3, "green", 0xFF008000 };
            for (int i = 0; i < named.length; i += 2) NAMED.put((String) named[i], (Integer) named[i + 1]);
            for (int i = 0; i < css.length; i += 2) CSS.put((String) css[i], (Integer) css[i + 1]);
        }

        private final String mSource;
        private final XMLReader mReader;
        private final SpannableStringBuilder mOut = new SpannableStringBuilder();
        private final ImageGetter mImageGetter;
        private final TagHandler mTagHandler;
        private final int mFlags;

        Converter(String source, ImageGetter getter, TagHandler handler, XMLReader reader, int flags) {
            mSource = source;
            mImageGetter = getter;
            mTagHandler = handler;
            mReader = reader;
            mFlags = flags;
        }

        Spanned convert() {
            mReader.setContentHandler(this);
            try {
                mReader.parse(new InputSource(new StringReader(mSource)));
            } catch (IOException | SAXException e) {
                throw new RuntimeException(e);
            }
            // A paragraph's style ends with its line, not on a blank line after it, and is a paragraph's (SPAN_PARAGRAPH).
            for (Object o : mOut.getSpans(0, mOut.length(), ParagraphStyle.class)) {
                int start = mOut.getSpanStart(o), end = mOut.getSpanEnd(o);
                if (end - 2 >= 0 && mOut.charAt(end - 1) == '\n' && mOut.charAt(end - 2) == '\n') end--;
                if (end == start) mOut.removeSpan(o);
                else mOut.setSpan(o, start, end, Spannable.SPAN_PARAGRAPH);
            }
            return mOut;
        }

        private void handleStartTag(String tag, Attributes attributes) {
            if (tag.equalsIgnoreCase("br")) {
                // its line break comes at its end, which the reader gives at once
            } else if (tag.equalsIgnoreCase("p")) {
                startBlockElement(attributes, margin(FROM_HTML_SEPARATOR_LINE_BREAK_PARAGRAPH));
                startCssStyle(attributes);
            } else if (tag.equalsIgnoreCase("ul")) {
                startBlockElement(attributes, margin(FROM_HTML_SEPARATOR_LINE_BREAK_LIST));
            } else if (tag.equalsIgnoreCase("li")) {
                startBlockElement(attributes, margin(FROM_HTML_SEPARATOR_LINE_BREAK_LIST_ITEM));
                start(new Bullet());
                startCssStyle(attributes);
            } else if (tag.equalsIgnoreCase("div")) {
                startBlockElement(attributes, margin(FROM_HTML_SEPARATOR_LINE_BREAK_DIV));
            } else if (tag.equalsIgnoreCase("span")) {
                startCssStyle(attributes);
            } else if (tag.equalsIgnoreCase("strong") || tag.equalsIgnoreCase("b")) {
                start(new Bold());
            } else if (tag.equalsIgnoreCase("em") || tag.equalsIgnoreCase("cite") || tag.equalsIgnoreCase("dfn") || tag.equalsIgnoreCase("i")) {
                start(new Italic());
            } else if (tag.equalsIgnoreCase("big")) {
                start(new Big());
            } else if (tag.equalsIgnoreCase("small")) {
                start(new Small());
            } else if (tag.equalsIgnoreCase("font")) {
                startFont(attributes);
            } else if (tag.equalsIgnoreCase("blockquote")) {
                startBlockElement(attributes, margin(FROM_HTML_SEPARATOR_LINE_BREAK_BLOCKQUOTE));
                start(new Blockquote());
            } else if (tag.equalsIgnoreCase("tt")) {
                start(new Monospace());
            } else if (tag.equalsIgnoreCase("a")) {
                start(new Href(attributes.getValue("", "href")));
            } else if (tag.equalsIgnoreCase("u")) {
                start(new Underline());
            } else if (tag.equalsIgnoreCase("del") || tag.equalsIgnoreCase("s") || tag.equalsIgnoreCase("strike")) {
                start(new Strikethrough());
            } else if (tag.equalsIgnoreCase("sup")) {
                start(new Super());
            } else if (tag.equalsIgnoreCase("sub")) {
                start(new Sub());
            } else if (tag.length() == 2 && Character.toLowerCase(tag.charAt(0)) == 'h' && tag.charAt(1) >= '1' && tag.charAt(1) <= '6') {
                startBlockElement(attributes, margin(FROM_HTML_SEPARATOR_LINE_BREAK_HEADING));
                start(new Heading(tag.charAt(1) - '1'));
            } else if (tag.equalsIgnoreCase("img")) {
                startImg(attributes);
            } else if (mTagHandler != null) {
                mTagHandler.handleTag(true, tag, mOut, mReader);
            }
        }

        private void handleEndTag(String tag) {
            if (tag.equalsIgnoreCase("br")) {
                mOut.append('\n');
            } else if (tag.equalsIgnoreCase("p")) {
                endCssStyle();
                endBlockElement();
            } else if (tag.equalsIgnoreCase("ul") || tag.equalsIgnoreCase("div")) {
                endBlockElement();
            } else if (tag.equalsIgnoreCase("li")) {
                endCssStyle();
                endBlockElement();
                end(Bullet.class, new BulletSpan());
            } else if (tag.equalsIgnoreCase("span")) {
                endCssStyle();
            } else if (tag.equalsIgnoreCase("strong") || tag.equalsIgnoreCase("b")) {
                end(Bold.class, new StyleSpan(Typeface.BOLD));
            } else if (tag.equalsIgnoreCase("em") || tag.equalsIgnoreCase("cite") || tag.equalsIgnoreCase("dfn") || tag.equalsIgnoreCase("i")) {
                end(Italic.class, new StyleSpan(Typeface.ITALIC));
            } else if (tag.equalsIgnoreCase("big")) {
                end(Big.class, new RelativeSizeSpan(1.25f));
            } else if (tag.equalsIgnoreCase("small")) {
                end(Small.class, new RelativeSizeSpan(0.8f));
            } else if (tag.equalsIgnoreCase("font")) {
                Font font = getLast(Font.class);
                if (font != null) setSpanFromMark(font, new TypefaceSpan(font.mFace));
                Foreground fg = getLast(Foreground.class);
                if (fg != null) setSpanFromMark(fg, new ForegroundColorSpan(fg.mColor));
            } else if (tag.equalsIgnoreCase("blockquote")) {
                endBlockElement();
                end(Blockquote.class, new QuoteSpan());
            } else if (tag.equalsIgnoreCase("tt")) {
                end(Monospace.class, new TypefaceSpan("monospace"));
            } else if (tag.equalsIgnoreCase("a")) {
                Href h = getLast(Href.class);
                if (h != null && h.mHref != null) setSpanFromMark(h, new URLSpan(h.mHref));
            } else if (tag.equalsIgnoreCase("u")) {
                end(Underline.class, new UnderlineSpan());
            } else if (tag.equalsIgnoreCase("del") || tag.equalsIgnoreCase("s") || tag.equalsIgnoreCase("strike")) {
                end(Strikethrough.class, new StrikethroughSpan());
            } else if (tag.equalsIgnoreCase("sup")) {
                end(Super.class, new SuperscriptSpan());
            } else if (tag.equalsIgnoreCase("sub")) {
                end(Sub.class, new SubscriptSpan());
            } else if (tag.length() == 2 && Character.toLowerCase(tag.charAt(0)) == 'h' && tag.charAt(1) >= '1' && tag.charAt(1) <= '6') {
                // the size and the weight end with the heading's text, before its line breaks
                Heading h = getLast(Heading.class);
                if (h != null) setSpanFromMark(h, new RelativeSizeSpan(HEADING_SIZES[h.mLevel]), new StyleSpan(Typeface.BOLD));
                endBlockElement();
            } else if (mTagHandler != null) {
                mTagHandler.handleTag(false, tag, mOut, mReader);
            }
        }

        /** The line breaks around a block of that kind: one where the flags ask for it, else two. */
        private int margin(int flag) { return (mFlags & flag) != 0 ? 1 : 2; }

        private void appendNewlines(int min) {
            int len = mOut.length();
            if (len == 0) return;
            int existing = 0;
            for (int i = len - 1; i >= 0 && mOut.charAt(i) == '\n'; i--) existing++;
            for (int j = existing; j < min; j++) mOut.append("\n");
        }

        private void startBlockElement(Attributes attributes, int margin) {
            if (margin > 0) {
                appendNewlines(margin);
                start(new Newline(margin));
            }
            String style = attributes.getValue("", "style");
            if (style != null) {
                Matcher m = TEXT_ALIGN.matcher(style);
                if (m.find()) {
                    String a = m.group(1);
                    if (a.equalsIgnoreCase("start")) start(new Alignment(Layout.Alignment.ALIGN_NORMAL));
                    else if (a.equalsIgnoreCase("center")) start(new Alignment(Layout.Alignment.ALIGN_CENTER));
                    else if (a.equalsIgnoreCase("end")) start(new Alignment(Layout.Alignment.ALIGN_OPPOSITE));
                }
            }
        }

        private void endBlockElement() {
            Newline n = getLast(Newline.class);
            if (n != null) {
                appendNewlines(n.mCount);
                mOut.removeSpan(n);
            }
            Alignment a = getLast(Alignment.class);
            if (a != null) setSpanFromMark(a, new AlignmentSpan.Standard(a.mAlignment));
        }

        private void startCssStyle(Attributes attributes) {
            String style = attributes.getValue("", "style");
            if (style == null) return;
            Matcher m = FOREGROUND.matcher(style);
            if (m.find()) {
                int c = htmlColor(m.group(1));
                if (c != -1) start(new Foreground(c | 0xFF000000));
            }
            m = BACKGROUND.matcher(style);
            if (m.find()) {
                int c = htmlColor(m.group(1));
                if (c != -1) start(new Background(c | 0xFF000000));
            }
            m = DECORATION.matcher(style);
            if (m.find() && m.group(1).equalsIgnoreCase("line-through")) start(new Strikethrough());
        }

        private void endCssStyle() {
            Strikethrough s = getLast(Strikethrough.class);
            if (s != null) setSpanFromMark(s, new StrikethroughSpan());
            Background b = getLast(Background.class);
            if (b != null) setSpanFromMark(b, new BackgroundColorSpan(b.mColor));
            Foreground f = getLast(Foreground.class);
            if (f != null) setSpanFromMark(f, new ForegroundColorSpan(f.mColor));
        }

        private void startFont(Attributes attributes) {
            String color = attributes.getValue("", "color"), face = attributes.getValue("", "face");
            if (!TextUtils.isEmpty(color)) {
                int c = htmlColor(color);
                if (c != -1) start(new Foreground(c | 0xFF000000));
            }
            if (!TextUtils.isEmpty(face)) start(new Font(face));
        }

        /** An image the getter gives, else the platform's unknown_image, as one object character. */
        private void startImg(Attributes attributes) {
            String src = attributes.getValue("", "src");
            Drawable d = mImageGetter != null ? mImageGetter.getDrawable(src) : null;
            if (d == null) {
                try {
                    d = Resources.getSystem().getDrawable(megler.IR.drawable.unknown_image);
                } catch (RuntimeException e) {
                    d = null;
                }
                if (d == null) d = new ColorDrawable(0);
                d.setBounds(0, 0, Math.max(0, d.getIntrinsicWidth()), Math.max(0, d.getIntrinsicHeight()));
            }
            int len = mOut.length();
            mOut.append("\uFFFC");
            mOut.setSpan(new ImageSpan(d, src), len, mOut.length(), Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        }

        /** Color.getHtmlColor, and CSS's colors with FROM_HTML_OPTION_USE_CSS_COLORS; -1 for one it cannot read. */
        private int htmlColor(String color) {
            String key = color.toLowerCase(Locale.ROOT);
            Integer i = (mFlags & FROM_HTML_OPTION_USE_CSS_COLORS) == FROM_HTML_OPTION_USE_CSS_COLORS ? CSS.get(key) : null;
            if (i == null) i = NAMED.get(key);
            if (i != null) return i;
            try {
                return decodeInt(color);
            } catch (NumberFormatException e) {
                return -1;
            }
        }

        /** XmlUtils.convertValueToInt's reading: #hex, 0x hex, 0 octal, or decimal, with a sign. */
        private static int decodeInt(String s) {
            if (s.isEmpty()) return -1;
            int index = 0, base = 10, sign = 1;
            if (s.charAt(0) == '-') { sign = -1; index++; }
            if (index < s.length() && s.charAt(index) == '0') {
                if (index == s.length() - 1) return 0;
                char c = s.charAt(index + 1);
                if (c == 'x' || c == 'X') { index += 2; base = 16; } else { index++; base = 8; }
            } else if (index < s.length() && s.charAt(index) == '#') {
                index++;
                base = 16;
            }
            return Integer.parseInt(s.substring(index), base) * sign;
        }

        private void start(Object mark) {
            int len = mOut.length();
            mOut.setSpan(mark, len, len, Spannable.SPAN_INCLUSIVE_EXCLUSIVE);
        }

        private void end(Class<?> kind, Object repl) {
            Object mark = getLast(kind);
            if (mark != null) setSpanFromMark(mark, repl);
        }

        /** The mark of that kind put last: getSpans gives them in the order they were put. */
        private <T> T getLast(Class<T> kind) {
            T[] marks = mOut.getSpans(0, mOut.length(), kind);
            return marks.length == 0 ? null : marks[marks.length - 1];
        }

        private void setSpanFromMark(Object mark, Object... spans) {
            int where = mOut.getSpanStart(mark);
            mOut.removeSpan(mark);
            int len = mOut.length();
            if (where != len) for (Object span : spans) mOut.setSpan(span, where, len, Spannable.SPAN_EXCLUSIVE_EXCLUSIVE);
        }

        public void setDocumentLocator(Locator locator) {}
        public void startDocument() {}
        public void endDocument() {}
        public void startPrefixMapping(String prefix, String uri) {}
        public void endPrefixMapping(String prefix) {}
        public void startElement(String uri, String localName, String qName, Attributes attributes) { handleStartTag(localName, attributes); }
        public void endElement(String uri, String localName, String qName) { handleEndTag(localName); }

        /** Text, white space run together into one space, and none at the start of a line. */
        public void characters(char[] ch, int start, int length) {
            StringBuilder sb = new StringBuilder();
            for (int i = 0; i < length; i++) {
                char c = ch[i + start];
                if (c == ' ' || c == '\n') {
                    char pred;
                    int len = sb.length();
                    if (len == 0) {
                        len = mOut.length();
                        pred = len == 0 ? '\n' : mOut.charAt(len - 1);
                    } else {
                        pred = sb.charAt(len - 1);
                    }
                    if (pred != ' ' && pred != '\n') sb.append(' ');
                } else {
                    sb.append(c);
                }
            }
            mOut.append(sb);
        }

        public void ignorableWhitespace(char[] ch, int start, int length) {}
        public void processingInstruction(String target, String data) {}
        public void skippedEntity(String name) {}

        private static final class Bold {}
        private static final class Italic {}
        private static final class Underline {}
        private static final class Strikethrough {}
        private static final class Big {}
        private static final class Small {}
        private static final class Monospace {}
        private static final class Blockquote {}
        private static final class Super {}
        private static final class Sub {}
        private static final class Bullet {}
        private static final class Font { final String mFace; Font(String face) { mFace = face; } }
        private static final class Href { final String mHref; Href(String href) { mHref = href; } }
        private static final class Foreground { final int mColor; Foreground(int color) { mColor = color; } }
        private static final class Background { final int mColor; Background(int color) { mColor = color; } }
        private static final class Heading { final int mLevel; Heading(int level) { mLevel = level; } }
        private static final class Newline { final int mCount; Newline(int count) { mCount = count; } }
        private static final class Alignment { final Layout.Alignment mAlignment; Alignment(Layout.Alignment a) { mAlignment = a; } }
    }

    /**
     * The SAX reader Html parses with, in place of Android's TagSoup: elements and text in the order they come, with what
     * TagSoup does to HTML that is not well formed -- html and body (and head, for what belongs there) put around the text,
     * an element left open ended by the end of one around it or of the text, a p ended by a block that starts in it, an li
     * by the next li of its list (dt and dd, option, table rows and cells alike), br and img and the other empty elements
     * ended at once, an end tag of nothing open left out, script and style read as plain text -- names lowercased,
     * character references read. Each event goes to the ContentHandler set at that moment: one put in place by a handler of
     * the events gets the rest.
     */
    private static final class HtmlReader implements XMLReader {
        private static final String XHTML = "http://www.w3.org/1999/xhtml";
        private static final Attributes NONE = new AttributesImpl();
        private static final HashSet<String> VOID = set("area base basefont br col embed frame hr img input isindex keygen link meta param source track wbr");
        private static final HashSet<String> HEAD = set("title meta link style script base noscript");
        private static final HashSet<String> ENDS_P = set("address article aside blockquote center dir div dl fieldset figure footer form h1 h2 h3 h4 h5 h6 header hr listing menu nav ol p pre section table ul xmp");
        private static final HashMap<String, Integer> ENTITIES = new HashMap<>();
        static {
            String latin1 = "nbsp iexcl cent pound curren yen brvbar sect uml copy ordf laquo not shy reg macr deg plusmn sup2 sup3 acute micro para middot cedil "
                + "sup1 ordm raquo frac14 frac12 frac34 iquest Agrave Aacute Acirc Atilde Auml Aring AElig Ccedil Egrave Eacute Ecirc Euml Igrave Iacute Icirc "
                + "Iuml ETH Ntilde Ograve Oacute Ocirc Otilde Ouml times Oslash Ugrave Uacute Ucirc Uuml Yacute THORN szlig agrave aacute acirc atilde auml "
                + "aring aelig ccedil egrave eacute ecirc euml igrave iacute icirc iuml eth ntilde ograve oacute ocirc otilde ouml divide oslash ugrave "
                + "uacute ucirc uuml yacute thorn yuml";
            String[] names = latin1.split(" ");
            for (int i = 0; i < names.length; i++) ENTITIES.put(names[i], 160 + i);
            String greek = "Alpha Beta Gamma Delta Epsilon Zeta Eta Theta Iota Kappa Lambda Mu Nu Xi Omicron Pi Rho - Sigma Tau Upsilon Phi Chi Psi Omega";
            names = greek.split(" ");
            for (int i = 0; i < names.length; i++) {
                if (names[i].equals("-")) continue;
                ENTITIES.put(names[i], 913 + i);
                ENTITIES.put(names[i].toLowerCase(Locale.ROOT), 945 + i);
            }
            String other = "quot 34 amp 38 apos 39 lt 60 gt 62 sigmaf 962 thetasym 977 upsih 978 piv 982 OElig 338 oelig 339 Scaron 352 scaron 353 Yuml 376 "
                + "fnof 402 circ 710 tilde 732 ensp 8194 emsp 8195 thinsp 8201 zwnj 8204 zwj 8205 lrm 8206 rlm 8207 ndash 8211 mdash 8212 lsquo 8216 "
                + "rsquo 8217 sbquo 8218 ldquo 8220 rdquo 8221 bdquo 8222 dagger 8224 Dagger 8225 bull 8226 hellip 8230 permil 8240 prime 8242 Prime 8243 "
                + "lsaquo 8249 rsaquo 8250 oline 8254 frasl 8260 euro 8364 image 8465 weierp 8472 real 8476 trade 8482 alefsym 8501 larr 8592 uarr 8593 "
                + "rarr 8594 darr 8595 harr 8596 crarr 8629 lArr 8656 uArr 8657 rArr 8658 dArr 8659 hArr 8660 forall 8704 part 8706 exist 8707 empty 8709 "
                + "nabla 8711 isin 8712 notin 8713 ni 8715 prod 8719 sum 8721 minus 8722 lowast 8727 radic 8730 prop 8733 infin 8734 ang 8736 and 8743 "
                + "or 8744 cap 8745 cup 8746 int 8747 there4 8756 sim 8764 cong 8773 asymp 8776 ne 8800 equiv 8801 le 8804 ge 8805 sub 8834 sup 8835 "
                + "nsub 8836 sube 8838 supe 8839 oplus 8853 otimes 8855 perp 8869 sdot 8901 lceil 8968 rceil 8969 lfloor 8970 rfloor 8971 lang 9001 "
                + "rang 9002 loz 9674 spades 9824 clubs 9827 hearts 9829 diams 9830";
            names = other.split(" ");
            for (int i = 0; i < names.length; i += 2) ENTITIES.put(names[i], Integer.parseInt(names[i + 1]));
        }
        private static HashSet<String> set(String names) { return new HashSet<>(Arrays.asList(names.split(" "))); }

        private ContentHandler mContent;
        private DTDHandler mDtd;
        private EntityResolver mEntities;
        private ErrorHandler mErrors;
        private final HashMap<String, Boolean> mFeatures = new HashMap<>();
        private final HashMap<String, Object> mProperties = new HashMap<>();
        /** The elements open, the outermost first. */
        private final ArrayList<String> mOpen = new ArrayList<>();
        private final StringBuilder mText = new StringBuilder();
        private boolean mHtml, mBody;

        public boolean getFeature(String name) { Boolean b = mFeatures.get(name); return b != null && b; }
        public void setFeature(String name, boolean value) { mFeatures.put(name, value); }
        public Object getProperty(String name) { return mProperties.get(name); }
        public void setProperty(String name, Object value) { mProperties.put(name, value); }
        public void setEntityResolver(EntityResolver resolver) { mEntities = resolver; }
        public EntityResolver getEntityResolver() { return mEntities; }
        public void setDTDHandler(DTDHandler handler) { mDtd = handler; }
        public DTDHandler getDTDHandler() { return mDtd; }
        public void setContentHandler(ContentHandler handler) { mContent = handler; }
        public ContentHandler getContentHandler() { return mContent; }
        public void setErrorHandler(ErrorHandler handler) { mErrors = handler; }
        public ErrorHandler getErrorHandler() { return mErrors; }
        public void parse(String systemId) throws IOException, SAXException { parse(new InputSource(systemId)); }

        public void parse(InputSource input) throws IOException, SAXException {
            java.io.Reader r = input.getCharacterStream();
            if (r == null && input.getByteStream() != null) r = new InputStreamReader(input.getByteStream(), input.getEncoding() != null ? input.getEncoding() : "UTF-8");
            StringBuilder sb = new StringBuilder();
            if (r != null) {
                char[] buf = new char[4096];
                for (int n; (n = r.read(buf)) > 0; ) sb.append(buf, 0, n);
            }
            read(sb.toString());
        }

        private void read(String s) throws SAXException {
            mOpen.clear();
            mText.setLength(0);
            mHtml = mBody = false;
            ContentHandler h = mContent;
            if (h != null) h.startDocument();
            int i = 0, n = s.length();
            while (i < n) {
                char c = s.charAt(i);
                if (c == '<' && i + 1 < n) {
                    if (s.startsWith("<!--", i)) {
                        int e = s.indexOf("-->", i + 4);
                        i = e < 0 ? n : e + 3;
                        continue;
                    }
                    if (s.startsWith("<![CDATA[", i)) {
                        int e = s.indexOf("]]>", i + 9);
                        mText.append(s, i + 9, e < 0 ? n : e);
                        i = e < 0 ? n : e + 3;
                        continue;
                    }
                    char d = s.charAt(i + 1);
                    if (d == '!' || d == '?') {
                        int e = s.indexOf('>', i);
                        i = e < 0 ? n : e + 1;
                        continue;
                    }
                    boolean end = d == '/';
                    int at = end ? i + 2 : i + 1;
                    int e = at < n && Character.isLetter(s.charAt(at)) ? tagEnd(s, at) : -1;
                    if (e >= 0) {
                        i = tag(s, at, e, end);
                        continue;
                    }
                } else if (c == '&') {
                    i = reference(s, i);
                    continue;
                } else if (c == '\r') {
                    // a line's end is one '\n', as TagSoup reads CR LF and CR
                    if (i + 1 < n && s.charAt(i + 1) == '\n') i++;
                    c = '\n';
                }
                mText.append(c);
                i++;
            }
            text();
            // what is still open ends with the text, html and body last
            while (!mOpen.isEmpty()) endLast();
            h = mContent;
            if (h != null) h.endDocument();
        }

        /** The '>' that ends a tag whose name begins at i, past quoted attribute values; -1 when there is none. */
        private static int tagEnd(String s, int i) {
            for (int n = s.length(); i < n; i++) {
                char c = s.charAt(i);
                if (c == '>') return i;
                if (c != '=') continue;
                int k = i + 1;
                while (k < n && Character.isWhitespace(s.charAt(k))) k++;
                if (k < n && (s.charAt(k) == '"' || s.charAt(k) == '\'')) {
                    int q = s.indexOf(s.charAt(k), k + 1);
                    if (q > 0) i = q;
                }
            }
            return -1;
        }

        /** The tag whose name begins at `at` and which ends at the '>' at e: what follows it. */
        private int tag(String s, int at, int e, boolean end) throws SAXException {
            int k = at;
            while (k < e && !Character.isWhitespace(s.charAt(k)) && s.charAt(k) != '/') k++;
            String name = s.substring(at, k).toLowerCase(Locale.ROOT);
            text();
            if (end) {
                end(name);
                return e + 1;
            }
            int last = e;
            boolean empty = false;
            while (last > k && Character.isWhitespace(s.charAt(last - 1))) last--;
            if (last > k && s.charAt(last - 1) == '/') {
                empty = true;
                last--;
            }
            AttributesImpl attributes = new AttributesImpl();
            while (k < last) {
                char c = s.charAt(k);
                if (Character.isWhitespace(c) || c == '/') {
                    k++;
                    continue;
                }
                int a = k;
                while (k < last && !Character.isWhitespace(s.charAt(k)) && s.charAt(k) != '=' && s.charAt(k) != '/') k++;
                String attr = s.substring(a, k).toLowerCase(Locale.ROOT), value;
                while (k < last && Character.isWhitespace(s.charAt(k))) k++;
                if (k < last && s.charAt(k) == '=') {
                    k++;
                    while (k < last && Character.isWhitespace(s.charAt(k))) k++;
                    int v = k;
                    if (k < last && (s.charAt(k) == '"' || s.charAt(k) == '\'')) {
                        int q = s.indexOf(s.charAt(k), k + 1);
                        if (q < 0 || q > last) q = last;
                        v = k + 1;
                        k = Math.min(q + 1, last);
                        value = decode(s.substring(v, q));
                    } else {
                        while (k < last && !Character.isWhitespace(s.charAt(k))) k++;
                        value = decode(s.substring(v, k));
                    }
                } else {
                    value = attr;
                }
                if (!attr.isEmpty() && attributes.getIndex("", attr) < 0) attributes.addAttribute("", attr, attr, "CDATA", value);
            }
            start(name, attributes, empty);
            if (empty || VOID.contains(name) || !(name.equals("script") || name.equals("style"))) return e + 1;
            // script and style: their text as it is, up to their end tag
            int close = indexOfIgnoreCase(s, "</" + name, e + 1);
            mText.append(s, e + 1, close < 0 ? s.length() : close);
            text();
            end(name);
            if (close < 0) return s.length();
            int gt = s.indexOf('>', close);
            return gt < 0 ? s.length() : gt + 1;
        }

        private void start(String name, Attributes attributes, boolean empty) throws SAXException {
            if (name.equals("html")) {
                html();
                return;
            }
            if (name.equals("body")) {
                body();
                return;
            }
            if (name.equals("head")) {
                if (!mBody) head();
                return;
            }
            if (!mBody && HEAD.contains(name)) head();
            else body();
            if (ENDS_P.contains(name)) close("p", "body div li td th blockquote button table dd dt");
            if (name.equals("li")) close("li", "body ul ol menu dir");
            else if (name.equals("dt") || name.equals("dd")) close("dt dd", "body dl");
            else if (name.equals("option")) close("option", "body select datalist");
            else if (name.equals("tr")) close("tr td th", "body table thead tbody tfoot");
            else if (name.equals("td") || name.equals("th")) close("td th", "body tr table");
            open(name, attributes);
            if (empty || VOID.contains(name)) endLast();
        }

        /** An end tag: what is open above its element ends with it; one of nothing open is left out. html and body end with the text. */
        private void end(String name) throws SAXException {
            if (name.equals("html") || name.equals("body")) return;
            for (int k = mOpen.size() - 1; k >= 0; k--) {
                if (!mOpen.get(k).equals(name)) continue;
                while (mOpen.size() > k) endLast();
                return;
            }
        }

        /** The innermost element named in `names`, if one is open within the innermost of `within`, ended with what is above it. */
        private void close(String names, String within) throws SAXException {
            HashSet<String> these = set(names), stops = set(within);
            for (int k = mOpen.size() - 1; k >= 0; k--) {
                String o = mOpen.get(k);
                if (these.contains(o)) {
                    while (mOpen.size() > k) endLast();
                    return;
                }
                if (stops.contains(o)) return;
            }
        }

        private void html() throws SAXException {
            if (mHtml) return;
            mHtml = true;
            open("html", NONE);
        }

        private void head() throws SAXException {
            html();
            if (!mOpen.contains("head")) open("head", NONE);
        }

        private void body() throws SAXException {
            html();
            if (mBody) return;
            if (mOpen.contains("head")) end("head");
            mBody = true;
            open("body", NONE);
        }

        private void open(String name, Attributes attributes) throws SAXException {
            mOpen.add(name);
            ContentHandler h = mContent;
            if (h != null) h.startElement(XHTML, name, name, attributes);
        }

        private void endLast() throws SAXException {
            String name = mOpen.remove(mOpen.size() - 1);
            ContentHandler h = mContent;
            if (h != null) h.endElement(XHTML, name, name);
        }

        /** The text read since the last tag, given out: in body, or in an element of head; white space before body is left out. */
        private void text() throws SAXException {
            if (mText.length() == 0) return;
            String inner = mOpen.isEmpty() ? null : mOpen.get(mOpen.size() - 1);
            if (!mBody && (inner == null || inner.equals("html") || inner.equals("head"))) {
                boolean blank = true;
                for (int k = 0; k < mText.length() && blank; k++) blank = Character.isWhitespace(mText.charAt(k));
                if (blank) {
                    mText.setLength(0);
                    return;
                }
                body();
            }
            char[] chars = new char[mText.length()];
            mText.getChars(0, chars.length, chars, 0);
            mText.setLength(0);
            ContentHandler h = mContent;
            if (h != null) h.characters(chars, 0, chars.length);
        }

        /** A character reference at i (&name; &#n; &#xh;) read into the text: what follows it. One it does not know stays as it is. */
        private int reference(String s, int i) {
            int n = s.length(), k = i + 1;
            if (k < n && s.charAt(k) == '#') {
                boolean hex = k + 1 < n && (s.charAt(k + 1) == 'x' || s.charAt(k + 1) == 'X');
                int d = hex ? k + 2 : k + 1, j = d;
                while (j < n && Character.digit(s.charAt(j), hex ? 16 : 10) >= 0) j++;
                if (j > d && j - d <= 8) {
                    int cp = (int) Long.parseLong(s.substring(d, j), hex ? 16 : 10);
                    mText.appendCodePoint(Character.isValidCodePoint(cp) && cp != 0 ? cp : 0xFFFD);
                    return j < n && s.charAt(j) == ';' ? j + 1 : j;
                }
            } else {
                int j = k;
                while (j < n && j - k < 10 && Character.isLetterOrDigit(s.charAt(j))) j++;
                Integer cp = ENTITIES.get(s.substring(k, j));
                if (cp != null) {
                    mText.appendCodePoint(cp);
                    return j < n && s.charAt(j) == ';' ? j + 1 : j;
                }
            }
            mText.append('&');
            return i + 1;
        }

        /** An attribute's value with its character references read. */
        private String decode(String v) {
            if (v.indexOf('&') < 0) return v;
            StringBuilder saved = new StringBuilder(mText);
            mText.setLength(0);
            for (int i = 0; i < v.length(); ) {
                if (v.charAt(i) == '&') i = reference(v, i);
                else mText.append(v.charAt(i++));
            }
            String out = mText.toString();
            mText.setLength(0);
            mText.append(saved);
            return out;
        }

        private static int indexOfIgnoreCase(String s, String what, int from) {
            for (int i = from, last = s.length() - what.length(); i <= last; i++)
                if (s.regionMatches(true, i, what, 0, what.length())) return i;
            return -1;
        }
    }
}
