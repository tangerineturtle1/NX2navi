// runtime/save_zip.c on its own, driven by tests/save_zip_check.py:
//   save_zip_check selftest                       the manifest's JSON keys, round trips in memory, damaged input
//   save_zip_check inflate IN SIZE OUT            raw deflate IN (zlib's) into SIZE bytes at OUT; exit 3 when refused
//   save_zip_check deflate IN OUT                 raw deflate of IN into OUT, for zlib to read back
//   save_zip_check zip OUT NAME FILE [NAME FILE]  a zip of the files under those names
//   save_zip_check unzip ZIP DIR                  every member into DIR/<index>, a line "name|crc|size|dir|link" each;
//                                                 exit 3 with the SZ_E_* code when a member or the zip is refused
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "save_zip.h"

static int failures;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            failures++;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                                                      \
            fprintf(stderr, __VA_ARGS__);                                                                              \
            fputc('\n', stderr);                                                                                       \
        }                                                                                                              \
    } while (0)

static uint8_t *read_all(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = malloc(len > 0 ? (size_t)len : 1);
    *n = buf && fread(buf, 1, (size_t)len, f) == (size_t)len ? (size_t)len : 0;
    fclose(f);
    return buf;
}

static int write_all(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fwrite(data, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

static void selftest_json(void) {
    const char *j = "{\n \"format\": \"nx2navi-save\", \"nested\": {\"format\": \"inner\", \"list\": [1, \"]\", {}]},\n"
                    " \"format_version\": 1, \"neg\": -12, \"float\": 1.5, \"esc\": \"a\\\"b\\\\c\\u0041\\u00e9\",\n"
                    " \"t\": true, \"title_id\": \"0400486024DEC000\"}";
    char s[64];
    long long v = 0;
    CHECK(sz_json_str(j, "format", s, sizeof s) && !strcmp(s, "nx2navi-save"), "format: %s", s);
    CHECK(sz_json_int(j, "format_version", &v) && v == 1, "format_version: %lld", v);
    CHECK(sz_json_int(j, "neg", &v) && v == -12, "neg: %lld", v);
    CHECK(!sz_json_int(j, "float", &v), "a float is no integer");
    CHECK(!sz_json_int(j, "format", &v), "a string is no integer");
    CHECK(!sz_json_str(j, "format_version", s, sizeof s), "an integer is no string");
    CHECK(sz_json_str(j, "esc", s, sizeof s) && !strcmp(s, "a\"b\\cA?"), "escapes: %s", s);
    CHECK(sz_json_str(j, "title_id", s, sizeof s) && !strcmp(s, "0400486024DEC000"), "after a literal: %s", s);
    CHECK(!sz_json_str(j, "list", s, sizeof s) && !sz_json_str(j, "missing", s, sizeof s), "not at the top level");
    CHECK(sz_json_str(j, "format", s, 5) && !strcmp(s, "nx2n"), "cut: %s", s);
    CHECK(!sz_json_str("[\"format\"]", "format", s, sizeof s) && !sz_json_str("{\"format\" \"x\"}", "format", s, sizeof s) &&
              !sz_json_str("", "format", s, sizeof s) && !sz_json_str("{\"format\": \"open", "format", s, sizeof s),
          "malformed JSON");
    char e[32];
    CHECK(!strcmp(sz_json_escape("a\"b\\\n", e, sizeof e), "a\\\"b\\\\\\u000a"), "escape: %s", e);
    CHECK(!strcmp(sz_json_escape("0123456789", e, 5), "0123"), "escape cut: %s", e);
}

static void selftest_codec(void) {
    // text with repeats, then bytes of every value: literals of both fixed code lengths, long and far matches
    size_t n = 200000;
    uint8_t *src = malloc(n), *back = malloc(n);
    for (size_t i = 0; i < n; i++) src[i] = i < 120000 ? "fs commit save: 14 files, 2 changed\n"[i % 36] : (uint8_t)(i * 2654435761u >> 13);
    size_t dn = 0;
    uint8_t *d = sz_deflate(src, n, &dn);
    CHECK(d && dn < n, "deflate: %zu bytes of %zu", dn, n);
    CHECK(d && !sz_inflate(d, dn, back, n) && !memcmp(back, src, n), "inflate of deflate");
    CHECK(d && sz_inflate(d, dn, back, n - 1), "one byte short of room");
    CHECK(d && sz_inflate(d, dn - 1, back, n), "the data cut short");
    uint8_t *empty = sz_deflate(src, 0, &dn);
    CHECK(empty && !sz_inflate(empty, dn, back, 0), "empty input");
    free(empty), free(d);
    CHECK(sz_crc32(0, "123456789", 9) == 0xcbf43926u, "crc32 check value");
    CHECK(sz_crc32(sz_crc32(0, "1234", 4), "56789", 5) == 0xcbf43926u, "crc32 in parts");

    SzWriter w;
    sz_writer_init(&w, 2026, 11, 9, 17, 30, 5);
    sz_writer_add(&w, "manifest.json", "{}", 2);
    sz_writer_add(&w, "save/slot_00/progress.sav", src, n);
    sz_writer_add(&w, "save/empty.sav", "", 0);
    size_t zn = 0;
    uint8_t *zip = sz_writer_finish(&w, &zn);
    CHECK(zip != NULL, "writer");
    SzEntry *e = NULL;
    int count = 0;
    CHECK(zip && sz_read_dir(zip, zn, &e, &count) == SZ_OK && count == 3, "read_dir: %d members", count);
    if (count == 3) {
        int err;
        CHECK(!strcmp(e[1].name, "save/slot_00/progress.sav") && e[1].method == 8 && e[0].method == 0, "methods");
        uint8_t *got = sz_extract(zip, zn, &e[1], &err);
        CHECK(got && !memcmp(got, src, n) && err == SZ_OK, "extract");
        free(got);
        got = sz_extract(zip, zn, &e[2], &err);
        CHECK(got && got[0] == 0 && e[2].usize == 0, "empty member");
        free(got);
        zip[e[1].data + 100] ^= 0x55;   // damaged data: refused, by inflate or by the CRC
        got = sz_extract(zip, zn, &e[1], &err);
        CHECK(!got && err == SZ_E_DAMAGED, "damaged member read");
        free(got);
        zip[e[1].data + 100] ^= 0x55;
        zip[e[0].data] ^= 1;   // a stored member's byte: only the CRC tells
        got = sz_extract(zip, zn, &e[0], &err);
        CHECK(!got && err == SZ_E_DAMAGED, "damaged stored member read");
        free(got);
    }
    free(e);
    for (size_t cut = 0; zip && cut < zn; cut += 997) {   // every prefix is refused, never read past its end
        uint8_t *part = malloc(cut ? cut : 1);
        memcpy(part, zip, cut);
        CHECK(sz_read_dir(part, cut, &e, &count) != SZ_OK, "a zip cut at %zu read", cut);
        free(part);
    }
    CHECK(sz_read_dir((const uint8_t *)"PK\5\6", 4, &e, &count) == SZ_E_NOT_ZIP, "too short");
    free(zip), free(src), free(back);
}

int main(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "selftest")) {
        selftest_json();
        selftest_codec();
        printf("save_zip selftest: %d failures\n", failures);
        return failures ? 1 : 0;
    }
    if (argc == 5 && !strcmp(argv[1], "inflate")) {
        size_t n, want = (size_t)strtoull(argv[3], NULL, 10);
        uint8_t *in = read_all(argv[2], &n), *out = malloc(want ? want : 1);
        if (!in || sz_inflate(in, n, out, want)) return 3;
        return write_all(argv[4], out, want) ? 0 : 2;
    }
    if (argc == 4 && !strcmp(argv[1], "deflate")) {
        size_t n, dn;
        uint8_t *in = read_all(argv[2], &n), *out = in ? sz_deflate(in, n, &dn) : NULL;
        return out && write_all(argv[3], out, dn) ? 0 : 2;
    }
    if (argc >= 3 && argc % 2 == 1 && !strcmp(argv[1], "zip")) {
        SzWriter w;
        sz_writer_init(&w, 2026, 11, 9, 17, 30, 5);
        for (int i = 3; i + 1 < argc; i += 2) {
            size_t n;
            uint8_t *data = read_all(argv[i + 1], &n);
            if (!data) return 2;
            sz_writer_add(&w, argv[i], data, n);
            free(data);
        }
        size_t zn;
        uint8_t *zip = sz_writer_finish(&w, &zn);
        return zip && write_all(argv[2], zip, zn) ? 0 : 2;
    }
    if (argc == 4 && !strcmp(argv[1], "unzip")) {
        size_t n;
        uint8_t *zip = read_all(argv[2], &n);
        SzEntry *e;
        int count, r = zip ? sz_read_dir(zip, n, &e, &count) : SZ_E_NOT_ZIP;
        if (r) {
            printf("refused %d\n", r);
            return 3;
        }
        for (int i = 0; i < count; i++) {
            int err;
            uint8_t *data = sz_extract(zip, n, &e[i], &err);
            if (!data) {
                printf("refused %d %s\n", err, e[i].name);
                return 3;
            }
            char path[1024];
            snprintf(path, sizeof path, "%s/%d", argv[3], i);
            if (!write_all(path, data, (size_t)e[i].usize)) return 2;
            printf("%s|%08x|%llu|%d|%d\n", e[i].name, e[i].crc, (unsigned long long)e[i].usize, e[i].is_dir, e[i].is_link);
            free(data);
        }
        return 0;
    }
    fprintf(stderr, "usage: see the top of tests/save_zip_check.c\n");
    return 2;
}
