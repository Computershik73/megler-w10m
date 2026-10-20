/*
 * JSON, as far as the APK reports and bundles of APKs need it: written into a growing buffer, and read where it is, without
 * a tree. What is read is untrusted (an .xapk's manifest.json is whoever made it): every reader stops at the end it is
 * given, and a value nested deeper than 32 levels is not read.
 */
#ifndef MG_JSON_H
#define MG_JSON_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------- writing */

typedef struct mg_jbuf {
    char *s;               /* malloc'd, always terminated once anything is written; the caller frees it */
    size_t n, cap;
} mg_jbuf;

void mg_jraw(mg_jbuf *j, const char *text);
/* A string, quoted, with what JSON escapes escaped (the text is UTF-8 and stays so). */
void mg_jstr(mg_jbuf *j, const char *text);
/* A member's name and its colon, after a comma unless it is the object's first. */
void mg_jkey(mg_jbuf *j, const char *key, bool first);
void mg_jint(mg_jbuf *j, long long v);

/* ------------------------------------------------------------- reading */

/*
 * The value of member `key` of the object that starts at `obj` (at its "{", spaces before it allowed), looked for among that
 * object's own members only: where the value starts, or NULL when there is no such member or the object is not whole.
 */
const char *mg_json_member(const char *obj, const char *end, const char *key);
/* A string value at `v` into out, UTF-8 (escapes undone, \u included), cut to fit; false when `v` is not a string. */
bool mg_json_string(const char *v, const char *end, char *out, size_t cap);
/* A number value at `v` (a string of digits is taken too: APKPure writes version codes so); false when it is neither. */
bool mg_json_number(const char *v, const char *end, long long *out);
/*
 * The elements of the array that starts at `v`: call with *at NULL for the first; each call puts where the next element
 * starts in *at and answers true, or false when there are no more (or the array is not whole).
 */
bool mg_json_next(const char *v, const char *end, const char **at);

#ifdef __cplusplus
}
#endif

#endif
