/*
 * A resource table (resources.arsc) cut down to some languages: the resources of no language and of the languages asked
 * for are kept, every other language's go, and the table's pool of strings keeps only the strings those that stay use.
 *
 * Why. Google Play services 26 runs in each app here (megler.MicroG), and its table is 84 MB: 58 MB of it the strings of 75
 * languages, 26 MB the resources, of which 6 MB have no language. Read whole into the app's Java heap it took 84 MB of the
 * app's memory and of its 2 GB of addresses. Yandex Maps ran out of addresses half a minute after it started (package 489),
 * and Facebook's and WhatsApp's large objects were mostly this table. An app sees one language at a time (the phone's),
 * and English is what a resource falls back to.
 */
#ifndef MG_ARSC_H
#define MG_ARSC_H

#include "../mg_base.h"

/*
 * The table cut down to the resources of no language and those whose language is one of `languages` (two or three lower
 * case letters each; Hebrew, Indonesian, Yiddish and Filipino under either of their codes), malloc'd, its size in
 * *out_size. NULL when nothing would go, or the table is not one this understands (the caller keeps the whole table).
 */
u8 *mg_arsc_keep_languages(const u8 *table, size_t size, const char *const *languages, int n, size_t *out_size);

/*
 * Each string value (Res_value TYPE_STRING) of the table's resources, in the order the table has them, as its entry in the
 * pool (length prefix, bytes, terminator): those of every language when `n` < 0, else only of the resources
 * mg_arsc_keep_languages keeps. False when the table is not one this understands. For the tests: a table cut down has the
 * same strings, in the same order, as the whole one has for what is kept.
 */
bool mg_arsc_each_string(const u8 *table, size_t size, const char *const *languages, int n, void (*fn)(const u8 *s, size_t len, void *user),
                         void *user);

#endif
