/*
 * resources.pri for an app's own package (hk_appx.h, docs/PER-APP-PACKAGES.md).
 *
 * A packaged XAML app has a resources.pri, and its resource map is named after the package's identity name. One made by
 * makepri for Megler names "Megler": an app's package, named otherwise, needs its own. makepri is not on the phone; so
 * Megler carries one made for a placeholder name of 50 characters (the most an identity name may have; every app package's
 * name has that length, hk_appx_manifest.c) and the name is put in its place here.
 *
 * The name is in the file twice, as the resource map's name and in its unique name "ms-appx://<name>/" (UTF-16). The
 * hierarchical schema's checksum (in "[def_hnamesx]", 24 bytes on) is the CRC-32 of data in which the name is twice too,
 * lower-cased and in UTF-16, followed by `tail1` and `tail2` bytes of the rest. A CRC is linear: replacing the name by
 * another as long changes the checksum by the CRC of the difference followed by as many zero bytes as come after it, at
 * each of the two places. The tails are found when Megler is built (tools/build-appx.sh, from a second file made for a name
 * one character apart). A file so made is byte for byte the one makepri makes for that name (checked on the PC for names
 * with capitals, digits, '.' and '-').
 */
#include "hk_appx.h"

#include <stdio.h>

#include "zlib.h"

/* The CRC-32 of `delta` followed by `after` zero bytes, as a CRC of a difference: with the initial and final values taken
   out (the CRC of as many zero bytes is XOR'd away). */
static u32 crc_of_difference(const u8 *delta, size_t n, u32 after)
{
    static const u8 zeros[4096] = { 0 };
    uLong a = crc32(0L, Z_NULL, 0), z = crc32(0L, Z_NULL, 0);
    size_t left;
    a = crc32(a, delta, (uInt)n);
    for (left = after; left; ) {
        size_t k = left > sizeof(zeros) ? sizeof(zeros) : left;
        a = crc32(a, zeros, (uInt)k);
        left -= k;
    }
    for (left = n + after; left; ) {
        size_t k = left > sizeof(zeros) ? sizeof(zeros) : left;
        z = crc32(z, zeros, (uInt)k);
        left -= k;
    }
    return (u32)(a ^ z);
}

static u8 lower(u8 c) { return c >= 'A' && c <= 'Z' ? (u8)(c + 32) : c; }

bool hk_appx_pri_rename(const u8 *pri, size_t size, const char *placeholder, u32 tail1, u32 tail2, const char *name, u8 *out,
                        char *err, size_t errlen)
{
    size_t n = strlen(placeholder), i, found = 0, at = 0;
    u8 old16[128], new16[128], delta[128];
    u32 sum;
    const u8 *mark = NULL;
    if (strlen(name) != n || n == 0 || n > 60) {
        if (err && errlen) snprintf(err, errlen, "the name \"%s\" is not as long as the placeholder (%u)", name, (unsigned)n);
        return false;
    }
    for (i = 0; i < n; i++) {
        old16[2 * i] = (u8)placeholder[i];
        old16[2 * i + 1] = 0;
        new16[2 * i] = (u8)name[i];
        new16[2 * i + 1] = 0;
        delta[2 * i] = (u8)(lower((u8)placeholder[i]) ^ lower((u8)name[i]));
        delta[2 * i + 1] = 0;
    }
    memcpy(out, pri, size);
    for (i = 0; i + 2 * n <= size; i++) {
        if (memcmp(out + i, old16, 2 * n)) continue;
        memcpy(out + i, new16, 2 * n);
        found++;
        i += 2 * n - 1;
    }
    for (i = 0; i + 13 <= size; i++)
        if (!memcmp(pri + i, "[def_hnamesx]", 13)) {
            mark = pri + i;
            at = i + 24;
            break;
        }
    if (found != 2 || !mark || at + 4 > size) {
        if (err && errlen) snprintf(err, errlen, "the template has the placeholder %u times (2 wanted)%s", (unsigned)found, mark ? "" : " and no schema");
        return false;
    }
    sum = (u32)out[at] | (u32)out[at + 1] << 8 | (u32)out[at + 2] << 16 | (u32)out[at + 3] << 24;
    sum ^= crc_of_difference(delta, 2 * n, tail1) ^ crc_of_difference(delta, 2 * n, tail2);
    out[at] = (u8)sum;
    out[at + 1] = (u8)(sum >> 8);
    out[at + 2] = (u8)(sum >> 16);
    out[at + 3] = (u8)(sum >> 24);
    return true;
}
