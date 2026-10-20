/*
 * A guest library that uses what tools/android9-natives.sh built, for
 * tools/android9-natives/loadtest.sh: Meglet's linker loads it, and its
 * constructor calls into libicuuc, libicui18n, libexpat and libopenjdk and
 * says through the log what came back. `mg_tests --load` runs constructors and
 * nothing else, and gives a guest no file to open, so:
 *
 *   - everything happens in the constructor;
 *   - ICU's data file is not opened but carried: the build puts
 *     icudt60l.dat into this library's read-only data (probe_icudata.S) and
 *     the constructor hands it to ICU with udata_setCommonData(), which is the
 *     call libjavacore makes on Android after mapping the same file.
 *
 * What is asked of each library is something with one right answer that the
 * library cannot give without its real code and, for ICU, its real data: a
 * Turkish upper-casing, a Swedish collation, a German number, a Japanese
 * converter, a regular expression, a word count, a date in a named time zone,
 * a parsed document, a sine. Each line of output ends in "ok" or "WRONG".
 *
 * Not a test of the layer's own tests' kind (it is not in core/tests): it is
 * here to show that the libraries built for it run on it.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <android/log.h>

#include "unicode/utypes.h"
#include "unicode/uclean.h"
#include "unicode/udata.h"
#include "unicode/uchar.h"
#include "unicode/ustring.h"
#include "unicode/ucol.h"
#include "unicode/unum.h"
#include "unicode/ucnv.h"
#include "unicode/uregex.h"
#include "unicode/ubrk.h"
#include "unicode/udat.h"
#include "unicode/ucal.h"
#include "unicode/uversion.h"

#include "expat.h"

/* libopenjdk.so exports the fdlibm it links (java.lang.StrictMath's functions). */
double ieee_sin(double);
double ieee_pow(double, double);

extern const char probe_icudata[];

#define SAY(...) __android_log_print(ANDROID_LOG_INFO, "probe", __VA_ARGS__)

static int g_bad;

static void check(const char *what, int ok, const char *got)
{
    if (!ok) g_bad++;
    SAY("%s: %s -- %s", what, got, ok ? "ok" : "WRONG");
}

/* UTF-16 to UTF-8 for the log; ASCII or not, the bytes are compared by the caller. */
static const char *u8(const UChar *s, int32_t n)
{
    static char buf[4][256];
    static int at;
    UErrorCode e = U_ZERO_ERROR;
    char *out = buf[at++ & 3];
    u_strToUTF8(out, 255, NULL, s, n, &e);
    out[255] = 0;
    return U_SUCCESS(e) ? out : "(not convertible)";
}

static int g_elements, g_chars;
static void XMLCALL on_start(void *user, const XML_Char *name, const XML_Char **atts)
{
    (void)user;
    g_elements++;
    if (!strcmp(name, "b") && atts[0] && !strcmp(atts[0], "k") && !strcmp(atts[1], "v")) g_elements += 100;
}
static void XMLCALL on_chars(void *user, const XML_Char *s, int len)
{
    (void)user; (void)s;
    g_chars += len;
}

__attribute__((constructor)) static void probe(void)
{
    UErrorCode e = U_ZERO_ERROR;
    UChar a[64], b[64], out[128];
    char text[160];
    int32_t n;

    /* Without data: the character properties are compiled into libicuuc. */
    {
        UVersionInfo v;
        u_getVersion(v);
        snprintf(text, sizeof(text), "ICU %d.%d", v[0], v[1]);
        check("u_getVersion", v[0] == 60, text);
        snprintf(text, sizeof(text), "U+%04X, digit value %d", (unsigned)u_toupper(0x0444), (int)u_charDigitValue(0x0667));
        check("u_toupper(U+0444), u_charDigitValue(U+0667)", u_toupper(0x0444) == 0x0424 && u_charDigitValue(0x0667) == 7, text);
    }

    /* The data, as libjavacore's ICURegistration gives it. */
    udata_setCommonData(probe_icudata, &e);
    check("udata_setCommonData", U_SUCCESS(e), u_errorName(e));
    e = U_ZERO_ERROR;
    u_init(&e);
    check("u_init", U_SUCCESS(e), u_errorName(e));

    /* Upper-casing by locale (common, with the data's break rules untouched). */
    e = U_ZERO_ERROR;
    u_uastrcpy(a, "istanbul");
    n = u_strToUpper(out, 128, a, -1, "tr", &e);
    check("u_strToUpper(\"istanbul\", tr)", U_SUCCESS(e) && n == 8 && out[0] == 0x0130 && out[1] == 'S', u8(out, n));

    /* Collation: in Swedish o-umlaut sorts after z, in German before it. */
    {
        UCollator *sv, *de;
        UCollationResult rs, rd;
        e = U_ZERO_ERROR;
        sv = ucol_open("sv", &e);
        de = ucol_open("de", &e);
        a[0] = 0x00F6; a[1] = 0;
        u_uastrcpy(b, "z");
        rs = sv ? ucol_strcoll(sv, a, -1, b, -1) : UCOL_EQUAL;
        rd = de ? ucol_strcoll(de, a, -1, b, -1) : UCOL_EQUAL;
        snprintf(text, sizeof(text), "o-umlaut against z: sv %d, de %d (%s)", (int)rs, (int)rd, u_errorName(e));
        check("ucol_strcoll", U_SUCCESS(e) && rs == UCOL_GREATER && rd == UCOL_LESS, text);
        if (sv) ucol_close(sv);
        if (de) ucol_close(de);
    }

    /* A number, the German way: a double goes in, softfp, and decNumber and the layer's printf do the rest. */
    {
        UNumberFormat *f;
        e = U_ZERO_ERROR;
        f = unum_open(UNUM_DECIMAL, NULL, 0, "de_DE", NULL, &e);
        n = f ? unum_formatDouble(f, 1234567.891, out, 128, NULL, &e) : 0;
        check("unum_formatDouble(1234567.891, de_DE)", U_SUCCESS(e) && !strcmp(u8(out, n), "1.234.567,891"), U_SUCCESS(e) ? u8(out, n) : u_errorName(e));
        if (f) unum_close(f);
    }

    /* A converter that is nothing but a table in the data. */
    {
        static const char sjis[] = { (char)0x93, (char)0xFA, (char)0x96, (char)0x7B, 0 };     /* "Nihon" in Shift_JIS */
        UConverter *c;
        e = U_ZERO_ERROR;
        c = ucnv_open("Shift_JIS", &e);
        n = c ? ucnv_toUChars(c, out, 128, sjis, 4, &e) : 0;
        snprintf(text, sizeof(text), "%d units: U+%04X U+%04X (%s)", (int)n, n > 0 ? out[0] : 0, n > 1 ? out[1] : 0, u_errorName(e));
        check("ucnv_toUChars(Shift_JIS)", U_SUCCESS(e) && n == 2 && out[0] == 0x65E5 && out[1] == 0x672C, text);
        if (c) ucnv_close(c);
    }

    /* A regular expression (java.util.regex is this). */
    {
        URegularExpression *re;
        UBool found = 0;
        int32_t start = -1, end = -1;
        e = U_ZERO_ERROR;
        re = uregex_openC("(\\d+)-(\\p{L}+)", 0, NULL, &e);
        u_uastrcpy(a, "see 2026-meglet now");
        if (re) {
            uregex_setText(re, a, -1, &e);
            found = uregex_find(re, 0, &e);
            if (found) { start = uregex_start(re, 2, &e); end = uregex_end(re, 2, &e); }
        }
        snprintf(text, sizeof(text), "found %d, group 2 at %d..%d (%s)", (int)found, (int)start, (int)end, u_errorName(e));
        check("uregex_find", U_SUCCESS(e) && found && start == 9 && end == 13, text);
        if (re) uregex_close(re);
    }

    /* Words, by the break rules in the data. */
    {
        UBreakIterator *it;
        int words = 0;
        e = U_ZERO_ERROR;
        u_uastrcpy(a, "one two, three.");
        it = ubrk_open(UBRK_WORD, "en", a, -1, &e);
        if (it) {
            while (ubrk_next(it) != UBRK_DONE)
                if (ubrk_getRuleStatus(it) != UBRK_WORD_NONE) words++;
            ubrk_close(it);
        }
        snprintf(text, sizeof(text), "%d words (%s)", words, u_errorName(e));
        check("ubrk_next", U_SUCCESS(e) && words == 3, text);
    }

    /* A date in a named zone: the zone table, the calendar and a pattern. */
    {
        UDateFormat *f;
        e = U_ZERO_ERROR;
        u_uastrcpy(a, "Asia/Tokyo");
        u_uastrcpy(b, "yyyy-MM-dd HH:mm zzzz");
        f = udat_open(UDAT_PATTERN, UDAT_PATTERN, "en_US", a, -1, b, -1, &e);
        n = f ? udat_format(f, 1500000000000.0, out, 128, NULL, &e) : 0;      /* 2017-07-14 02:40 UTC */
        check("udat_format(1500000000000, Asia/Tokyo)", U_SUCCESS(e) && !strcmp(u8(out, n), "2017-07-14 11:40 Japan Standard Time"),
              U_SUCCESS(e) ? u8(out, n) : u_errorName(e));
        if (f) udat_close(f);
    }

    /* libexpat, with handlers in this library called back from it. */
    {
        static const char doc[] = "<a><b k=\"v\">text</b><c/>&amp;</a>";
        XML_Parser p = XML_ParserCreate(NULL);
        enum XML_Status st;
        XML_SetElementHandler(p, on_start, NULL);
        XML_SetCharacterDataHandler(p, on_chars);
        st = XML_Parse(p, doc, (int)strlen(doc), 1);
        snprintf(text, sizeof(text), "status %d, elements %d, characters %d, expat %s", (int)st, g_elements, g_chars, XML_ExpatVersion());
        check("XML_Parse", st == XML_STATUS_OK && g_elements == 103 && g_chars == 5, text);
        XML_ParserFree(p);
    }

    /* fdlibm inside libopenjdk: doubles in and out between two guest libraries. */
    {
        double s = ieee_sin(0.5), p = ieee_pow(2.0, 0.5);
        snprintf(text, sizeof(text), "sin(0.5) = %.17g, pow(2, 0.5) = %.17g", s, p);
        check("ieee_sin, ieee_pow", s == 0.47942553860420301 && p == 1.4142135623730951, text);
    }

    SAY("probe: %d wrong", g_bad);
}
