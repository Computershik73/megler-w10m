/*
 * An APK reader: ZIP and ZIP64, stored and deflated entries. The archive is
 * read where it is needed, never mapped: a game's APK is hundreds of
 * megabytes, and a 32-bit process has 2 GB of addresses for everything (a
 * 462 MB one mapped whole left Unity without room for its textures). It is
 * untrusted input -- every offset and length taken from it is checked against
 * the file before use.
 */
#ifndef MG_ZIP_H
#define MG_ZIP_H

#include "../host/mg_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mg_zip_entry {
    char *name;
    u16 method;            /* 0 stored, 8 deflated */
    u16 flags;
    u32 crc;
    u64 csize, usize;
    u64 local_offset;
} mg_zip_entry;

typedef struct mg_zip {
    mg_file *file;
    u64 size;
    mg_lock lock;          /* one read of the file at a time */
    char *path;
    mg_zip_entry *entries; /* sorted by name */
    u32 count;
} mg_zip;

bool mg_zip_open(mg_zip *z, const char *path, char *err, size_t errlen);
void mg_zip_close(mg_zip *z);
const mg_zip_entry *mg_zip_find(const mg_zip *z, const char *name);
/* The entry's bytes, malloc'd and followed by a NUL; NULL when it is damaged or larger than `limit`. */
u8 *mg_zip_read(const mg_zip *z, const mg_zip_entry *e, size_t limit, size_t *size);
/* Part of an entry into a caller's buffer; false when the range is outside it or it does not inflate. */
bool mg_zip_read_into(const mg_zip *z, const mg_zip_entry *e, u8 *out, size_t size);
/* Its first `want` bytes (all of it when it is smaller), malloc'd: inflated no further than they reach. NULL when damaged. */
u8 *mg_zip_read_head(const mg_zip *z, const mg_zip_entry *e, size_t want, size_t *size);
/* Where a stored entry's bytes are in the file; false for a deflated one (or a damaged one). */
bool mg_zip_stored_at(const mg_zip *z, const mg_zip_entry *e, u64 *offset);
/* Where an entry's bytes are in the file, as they are there (deflated, for a deflated one: e->csize of them); false when it is damaged. */
bool mg_zip_data_at(const mg_zip *z, const mg_zip_entry *e, u64 *offset);
/* `n` bytes of the file itself from `offset`; false when they are not all there. */
bool mg_zip_pread(const mg_zip *z, u64 offset, void *buf, size_t n);
/* Every entry whose name starts with `prefix`. */
void mg_zip_each(const mg_zip *z, const char *prefix, void (*cb)(const mg_zip_entry *e, void *user), void *user);
/*
 * An entry's bytes handed to `piece` as they are inflated, a megabyte or less at a time, never all in memory; `piece` returns
 * false to stop there. True when the entry was read to its end and its size and CRC-32 are the archive's, or `piece` stopped
 * it; false, with the reason in `err`, when it is damaged or cannot be read. For an app's code read for a name in it (the
 * report's SMS Retriever, core/src/apk/mg_apk.c): a DEX file of an app is tens of megabytes.
 */
bool mg_zip_stream(const mg_zip *z, const mg_zip_entry *e, bool (*piece)(void *user, const u8 *data, size_t n), void *user, char *err,
                   size_t errlen);
/*
 * An entry written out to a file of the host's (made, or emptied first), a piece at a time: neither its compressed nor its
 * inflated bytes are ever all in memory, for an APK inside a bundle of APKs is hundreds of megabytes. Its CRC-32 is checked.
 * `progress`, when given, is called after each piece with the bytes written so far. False, with the file removed and the
 * reason in `err`, when the entry is damaged or the file cannot be written.
 */
bool mg_zip_extract(const mg_zip *z, const mg_zip_entry *e, const char *path, void (*progress)(void *user, u64 written), void *user,
                    char *err, size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
