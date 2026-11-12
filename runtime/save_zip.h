// The zip file of a shared save (save_share.c; the launcher writes and reads the same file, launcher/saves.py
// export_save): a zip in memory with stored or deflated members, its members read back with their CRC-32 checked, raw
// deflate both ways, and the top-level keys of its manifest.json. Plain C on the C library alone, so
// tests/save_zip_check.py builds it by itself and checks it against Python's zipfile and zlib.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

uint32_t sz_crc32(uint32_t crc, const void *data, size_t n);   // the zip CRC-32; crc 0 to start, the last result to go on

// Raw deflate (RFC 1951) of src[0..n): one block of the fixed Huffman codes over LZ77 matches. A malloc'd buffer of
// *out_n bytes, NULL when out of memory.
uint8_t *sz_deflate(const uint8_t *src, size_t n, size_t *out_n);
// Raw deflate data src[0..n) into exactly dst_n bytes at dst: stored, fixed and dynamic blocks. 0 on success, -1 when
// the data is damaged, ends early or holds another number of bytes.
int sz_inflate(const uint8_t *src, size_t n, uint8_t *dst, size_t dst_n);

// ---- writing: the members go into one buffer as they come, then the central directory after them
typedef struct {
    char *name;
    uint32_t crc, csize, usize, offset;
    uint16_t method;
} SzMember;
typedef struct {
    uint8_t *buf;
    size_t n, cap;
    SzMember *m;
    int count, cap_m;
    int failed;                  // out of memory, or past the 4 GiB a zip without its 64-bit extension holds
    uint16_t dos_time, dos_date;
} SzWriter;
// Every member gets this local time (the zip's DOS time stamp, two-second steps).
void sz_writer_init(SzWriter *w, int year, int month, int day, int hour, int minute, int second);
// A member: name relative with '/' separators, its bytes deflated when that makes them smaller, else stored.
void sz_writer_add(SzWriter *w, const char *name, const void *data, size_t n);
// The whole archive (*n bytes; the caller frees it), NULL when any add failed. Frees the writer's own memory either way.
uint8_t *sz_writer_finish(SzWriter *w, size_t *n);

// ---- reading
enum { SZ_OK, SZ_E_NOT_ZIP, SZ_E_DAMAGED, SZ_E_UNSUPPORTED, SZ_E_MEMORY };
typedef struct {
    char name[256];              // as stored: '/' separators
    uint32_t crc;
    uint64_t csize, usize, data;   // data: the offset of its bytes in the zip
    uint16_t method;
    int is_dir, is_link;         // a folder entry (name ends in '/'); a symbolic link (a Unix mode in its attributes)
} SzEntry;
// The members of the zip zip[0..n) from its central directory: a malloc'd array in *out (the caller frees it) and its
// length in *count. SZ_OK or why not: not a zip, damaged, or what this reader leaves out (encryption, a 64-bit zip, a
// method but stored and deflate, a name of 256 bytes or more).
int sz_read_dir(const uint8_t *zip, size_t n, SzEntry **out, int *count);
// The bytes of member e, checked against its CRC-32: malloc'd with a NUL after them (the caller frees them), NULL with
// *err set to the SZ_E_* reason.
uint8_t *sz_extract(const uint8_t *zip, size_t n, const SzEntry *e, int *err);

// ---- the manifest: a JSON object whose values the reader needs are strings and integers at its top level
// The string value of key into out (cut to cap - 1 bytes; \u escapes outside ASCII become '?'). 1 when found.
int sz_json_str(const char *json, const char *key, char *out, size_t cap);
// The integer value of key. 1 when found and an integer.
int sz_json_int(const char *json, const char *key, long long *out);
// s as the inside of a JSON string ('"', '\' and control characters escaped) into out, cut to fit cap. Returns out.
char *sz_json_escape(const char *s, char *out, size_t cap);

#ifdef __cplusplus
}
#endif
