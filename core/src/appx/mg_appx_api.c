/* meglet.h's calls for an app's own package, as mg_appx.h has them. */
#include "../../include/meglet.h"
#include "mg_appx.h"

meglet_appx_key *meglet_appx_key_open(const char *key_path, const char *cert_path, const char *publisher, char *err, size_t errlen)
{
    return mg_appx_key_open(key_path, cert_path, publisher, err, errlen);
}
void meglet_appx_key_close(meglet_appx_key *key) { mg_appx_key_close(key); }
meglet_appx *meglet_appx_begin(const char *path, char *err, size_t errlen) { return mg_appx_begin(path, err, errlen); }
bool meglet_appx_add_file(meglet_appx *a, const char *name, const char *src_path) { return mg_appx_add_file(a, name, src_path); }
bool meglet_appx_add_data(meglet_appx *a, const char *name, const void *data, size_t size) { return mg_appx_add_data(a, name, data, size); }
bool meglet_appx_finish(meglet_appx *a, const meglet_appx_key *key, char *err, size_t errlen) { return mg_appx_finish(a, key, err, errlen); }
bool meglet_appx_app_identity(const char *android_package, char *out, size_t outlen) { return mg_appx_app_identity(android_package, out, outlen); }
char *meglet_appx_app_manifest(const char *meglet_manifest, const char *android_package, const char *label, const char *tile_color,
                             const char *splash_color, char *err, size_t errlen)
{
    return mg_appx_app_manifest(meglet_manifest, android_package, label, tile_color, splash_color, err, errlen);
}
bool meglet_appx_pri_rename(const uint8_t *pri, size_t size, const char *placeholder, uint32_t tail1, uint32_t tail2, const char *name,
                          uint8_t *out, char *err, size_t errlen)
{
    return mg_appx_pri_rename(pri, size, placeholder, tail1, tail2, name, out, err, errlen);
}
