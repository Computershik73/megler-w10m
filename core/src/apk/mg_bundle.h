/*
 * Bundles of split APKs (.apks, .xapk): what megler_bundle_unpack (megler.h) writes, and the record it leaves for the APK
 * report to read again.
 */
#ifndef MG_BUNDLE_H
#define MG_BUNDLE_H

#include "../mg_base.h"

#ifdef __cplusplus
extern "C" {
#endif

/* In the folder of splits megler_bundle_unpack makes: what it did (the JSON it answers), and the icon the bundle came with. */
#define MG_BUNDLE_RECORD "bundle.json"
#define MG_BUNDLE_ICON "icon.png"

/* What the record says was left out of the bundle: the splits' names, and the ABIs of those that had native code. */
typedef struct mg_bundle_record {
    char left_out[32][96];
    int nleft_out;
    char abis[8][24];
    int nabis;
} mg_bundle_record;

/* False when the folder has no record that can be read. */
bool mg_bundle_record_read(const char *dir, mg_bundle_record *out);

#ifdef __cplusplus
}
#endif

#endif
