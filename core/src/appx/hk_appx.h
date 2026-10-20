/*
 * Packages (.appx) made and signed here, on the phone: a package of Megler for one Android app, its own process and memory
 * (docs/PER-APP-PACKAGES.md). Written as makeappx writes one, its files stored (not compressed), and signed as signtool
 * signs one, with a key and a self-signed certificate made here once.
 *
 * Windows only (CNG for SHA-256 and RSA); elsewhere every call fails.
 */
#ifndef HK_APPX_H
#define HK_APPX_H

#include "../hk_base.h"

/* The signing key and its certificate: an RSA key of 2048 bits and a self-signed certificate for code signing whose
   subject is `publisher` (a package's Publisher, "CN=..."), kept in two files, the key as CNG's full private blob and the
   certificate as DER (what the phone is to trust: the Device Portal's install takes it with the package). Loaded when
   both are there, made and written otherwise. Paths are UTF-8. */
typedef struct hk_appx_key hk_appx_key;
hk_appx_key *hk_appx_key_open(const char *key_path, const char *cert_path, const char *publisher, char *err, size_t errlen);
void hk_appx_key_close(hk_appx_key *k);
const u8 *hk_appx_key_cert(const hk_appx_key *k, size_t *len);

/* A package being written to `path`: files added in order (names with '/', AppxManifest.xml among them), then finished
   with the block map, the content types and the signature. On any failure the package is not to be used; finish says
   why, and removes the file. */
typedef struct hk_appx hk_appx;
hk_appx *hk_appx_begin(const char *path, char *err, size_t errlen);
bool hk_appx_add_file(hk_appx *a, const char *name, const char *src_path);
bool hk_appx_add_data(hk_appx *a, const char *name, const void *data, size_t size);
/* Finishes and closes the package (and frees `a`, whatever the outcome). */
bool hk_appx_finish(hk_appx *a, const hk_appx_key *k, char *err, size_t errlen);

#endif
