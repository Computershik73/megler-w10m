/*
 * An APK reader: ZIP and ZIP64, stored and deflated entries. The archive is
 * read where it is needed, never mapped: a game's APK is hundreds of
 * megabytes, and a 32-bit process has 2 GB of addresses for everything (a
 * 462 MB one mapped whole left Unity without room for its textures). It is
 * untrusted input -- every offset and length taken from it is checked against
 * the file before use.
 */
#ifndef HK_ZIP_H
#define HK_ZIP_H

#include "../host/hk_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hk_zip_entry {
    char *name;
    u16 method;            /* 0 stored, 8 deflated */
    u16 flags;
    u32 crc;
    u64 csize, usize;
    u64 local_offset;
} hk_zip_entry;

typedef struct hk_zip {
    hk_file *file;
    u64 size;
    hk_lock lock;          /* one read of the file at a time */
    char *path;
    hk_zip_entry *entries; /* sorted by name */
    u32 count;
} hk_zip;

bool hk_zip_open(hk_zip *z, const char *path, char *err, size_t errlen);
void hk_zip_close(hk_zip *z);
const hk_zip_entry *hk_zip_find(const hk_zip *z, const char *name);
/* The entry's bytes, malloc'd and followed by a NUL; NULL when it is damaged or larger than `limit`. */
u8 *hk_zip_read(const hk_zip *z, const hk_zip_entry *e, size_t limit, size_t *size);
/* Part of an entry into a caller's buffer; false when the range is outside it or it does not inflate. */
bool hk_zip_read_into(const hk_zip *z, const hk_zip_entry *e, u8 *out, size_t size);
/* Where a stored entry's bytes are in the file; false for a deflated one (or a damaged one). */
bool hk_zip_stored_at(const hk_zip *z, const hk_zip_entry *e, u64 *offset);
/* `n` bytes of the file itself from `offset`; false when they are not all there. */
bool hk_zip_pread(const hk_zip *z, u64 offset, void *buf, size_t n);
/* Every entry whose name starts with `prefix`. */
void hk_zip_each(const hk_zip *z, const char *prefix, void (*cb)(const hk_zip_entry *e, void *user), void *user);
/*
 * An entry written out to a file of the host's (made, or emptied first), a piece at a time: neither its compressed nor its
 * inflated bytes are ever all in memory, for an APK inside a bundle of APKs is hundreds of megabytes. Its CRC-32 is checked.
 * `progress`, when given, is called after each piece with the bytes written so far. False, with the file removed and the
 * reason in `err`, when the entry is damaged or the file cannot be written.
 */
bool hk_zip_extract(const hk_zip *z, const hk_zip_entry *e, const char *path, void (*progress)(void *user, u64 written), void *user,
                    char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
