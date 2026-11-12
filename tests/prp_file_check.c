// runtime/prp_file.h: the layout of shader-prepared.bin. Checks the parse of a version 1 list, of a version 2 list and
// of damaged ones, then writes a version 2 list into the folder given as argv[1] for tests/prp_file_check.py, which
// reads its head with tools/make_game.py prepared_from. Prints each failed check and exits 1 on any.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prp_file.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(int argc, char **argv) {
    if (argc < 2) return printf("usage: prp_file_check <folder>\n"), 2;
    uint8_t file[PRP_HEAD + PRP_FROM_SIZE + 3 * PRP_REC + 8];
    PrpFrom from, got;
    for (int i = 0; i < 32; i++) from.programs[i] = (uint8_t)i, from.answers[i] = (uint8_t)(0x40 + i), from.translator[i] = (uint8_t)(0x80 + i);
    size_t n = 99;
    int has = 9;

    // version 2: the head, then three records
    size_t head = prp_head_write(file, &from);
    CHECK(head == PRP_HEAD + PRP_FROM_SIZE && head == 112, "the head is %zu bytes", head);
    for (int r = 0; r < 3; r++) {
        uint8_t *rec = file + head + r * PRP_REC;
        memset(rec, 0, PRP_REC);
        rec[0] = (uint8_t)(r + 1), rec[1] = (uint8_t)r, memset(rec + 4, 0xa0 + r, 32);
    }
    const uint8_t *recs = prp_parse(file, head + 3 * PRP_REC, &n, &got, &has);
    CHECK(recs == file + head && n == 3 && has == 1, "version 2: %zu records, has_from %d", n, has);
    CHECK(!memcmp(&got, &from, sizeof from), "version 2: what it was prepared from comes back");
    CHECK(recs && recs[PRP_REC * 2] == 3 && recs[PRP_REC * 2 + 4] == 0xa2, "version 2: the third record");
    // a cut last record is not counted
    recs = prp_parse(file, head + 2 * PRP_REC + 35, &n, &got, &has);
    CHECK(recs && n == 2, "a cut record: %zu", n);
    // the head alone, or less: no list
    CHECK(!prp_parse(file, head, &n, &got, &has) && n == 0, "no records");
    CHECK(!prp_parse(file, head - 1, &n, &got, &has) && n == 0, "a cut head");
    CHECK(!prp_parse(NULL, 0, &n, &got, &has), "no file");

    // version 1: records right after the 16 bytes, and nothing about where they came from
    uint8_t v1[PRP_HEAD + 2 * PRP_REC];
    uint32_t one = 1, rec = PRP_REC;
    memcpy(v1, PRP_MAGIC, 8), memcpy(v1 + 8, &one, 4), memcpy(v1 + 12, &rec, 4);
    memset(v1 + PRP_HEAD, 0x11, 2 * PRP_REC);
    recs = prp_parse(v1, sizeof v1, &n, &got, &has);
    CHECK(recs == v1 + PRP_HEAD && n == 2 && has == 0, "version 1: %zu records, has_from %d", n, has);

    // another version, record size or magic: not read
    uint32_t three = 3, other = 40;
    memcpy(v1 + 8, &three, 4);
    CHECK(!prp_parse(v1, sizeof v1, &n, &got, &has), "version 3");
    memcpy(v1 + 8, &one, 4), memcpy(v1 + 12, &other, 4);
    CHECK(!prp_parse(v1, sizeof v1, &n, &got, &has), "another record size");
    memcpy(v1 + 12, &rec, 4), v1[0] = 'X';
    CHECK(!prp_parse(v1, sizeof v1, &n, &got, &has), "another magic");

    char path[1024];
    snprintf(path, sizeof path, "%s/shader-prepared.bin", argv[1]);
    FILE *f = fopen(path, "wb");
    CHECK(f && fwrite(file, 1, head + 3 * PRP_REC, f) == head + 3 * PRP_REC, "cannot write %s", path);
    if (f) fclose(f);

    if (!fails) printf("prepared list file: all C checks passed\n");
    return fails != 0;
}
