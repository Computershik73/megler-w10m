/*
 * Packages (.appx) made and signed here, on the phone: a package of Megler for one Android app, its own process and memory
 * (docs/PER-APP-PACKAGES.md). Written as makeappx writes one, its files stored (not compressed), and signed as signtool
 * signs one, with a key and a self-signed certificate made here once.
 *
 * Windows only (CNG for SHA-256 and RSA); elsewhere every call fails. On desktop Windows this is part of Megler.exe, not of
 * the core's ELF (it calls Windows): what the core calls of it is MG_HOSTAPI, as the host layer is (docs/DESKTOP.md).
 */
#ifndef MG_APPX_H
#define MG_APPX_H

#include "../mg_base.h"

/* The signing key and its certificate: an RSA key of 2048 bits and a self-signed certificate for code signing whose
   subject is `publisher` (a package's Publisher, "CN=..."), kept in two files, the key as CNG's full private blob and the
   certificate as DER (what the phone is to trust: the Device Portal's install takes it with the package). Loaded when
   both are there, made and written otherwise. Paths are UTF-8. */
typedef struct mg_appx_key mg_appx_key;
MG_HOSTAPI mg_appx_key *mg_appx_key_open(const char *key_path, const char *cert_path, const char *publisher, char *err, size_t errlen);
MG_HOSTAPI void mg_appx_key_close(mg_appx_key *k);
MG_HOSTAPI const u8 *mg_appx_key_cert(const mg_appx_key *k, size_t *len);

/* A package being written to `path`: files added in order (names with '/', AppxManifest.xml among them), then finished
   with the block map, the content types and the signature. On any failure the package is not to be used; finish says
   why, and removes the file. */
typedef struct mg_appx mg_appx;
MG_HOSTAPI mg_appx *mg_appx_begin(const char *path, char *err, size_t errlen);
MG_HOSTAPI bool mg_appx_add_file(mg_appx *a, const char *name, const char *src_path);
MG_HOSTAPI bool mg_appx_add_data(mg_appx *a, const char *name, const void *data, size_t size);
/* Finishes and closes the package (and frees `a`, whatever the outcome). */
MG_HOSTAPI bool mg_appx_finish(mg_appx *a, const mg_appx_key *k, char *err, size_t errlen);

/* The identity name of the package of the Android app `android_package` ("MeglerApp." and the package; mg_appx_manifest.c),
   into out (at least 51 bytes); false for a name that is not an Android package's. */
MG_HOSTAPI bool mg_appx_app_identity(const char *android_package, char *out, size_t outlen);
/* What such a package declares to Windows, as megler.h's MEGLER_APPX_CAP_* (the same values), made from the permissions the
   Android app's manifest asks for and whether it has a receiver of Google Play services' SMS Retriever (mg_appx_manifest.c). */
enum {
    MG_APPX_CAP_NETWORK = 1,
    MG_APPX_CAP_PICTURES = 2,
    MG_APPX_CAP_VIDEOS = 4,
    MG_APPX_CAP_MUSIC = 8,
    MG_APPX_CAP_WEBCAM = 16,
    MG_APPX_CAP_MICROPHONE = 32,
    MG_APPX_CAP_LOCATION = 64,
    MG_APPX_CAP_CHAT = 128,
    MG_APPX_CAP_CALL_HISTORY = 256,
};
MG_HOSTAPI unsigned mg_appx_capabilities(const char *const *permissions, int count, bool sms_retriever);
/* The manifest of that package, made from Megler's own (`megler`, the text of its AppxManifest.xml): its identity name,
   phone product id and shown names changed, and the tile's and the splash screen's background colours when given
   ("#rrggbb" or "transparent"; NULL keeps Megler's), its capabilities those of `capabilities` (MG_APPX_CAP_*) and Megler's
   own (-1 keeps all of Megler's), all else Megler's. A new string, or NULL (err says why). Any host. */
MG_HOSTAPI char *mg_appx_app_manifest(const char *megler, const char *android_package, const char *label, const char *tile_color,
                           const char *splash_color, int capabilities, char *err, size_t errlen);
/* The resources.pri of a package named `name`, made from one made for `placeholder` (as long) whose schema checksum
   covers the name, lower-cased, followed by `tail1` and `tail2` bytes (mg_appx_pri.c): into `out`, as large as `pri`.
   False (err says why) unless the placeholder is there twice. Any host. */
MG_HOSTAPI bool mg_appx_pri_rename(const u8 *pri, size_t size, const char *placeholder, u32 tail1, u32 tail2, const char *name, u8 *out,
                        char *err, size_t errlen);

#endif
