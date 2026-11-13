// Names for a sampling profile's exe addresses from the exe's own COFF symbol table (runtime/prof_win.c). A MinGW link
// keeps the table in the file (PointerToSymbolTable), so a profile names its functions without the link map: for each
// address the profile lists, `sym <address> <function>+<offset>`. Plain file reads, no Windows headers, so the parser
// builds and is tested anywhere.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "prof_syms.h"

typedef struct { uint32_t rva; uint32_t str; char sname[8]; } CSym;   // str: string-table offset, 0 short name

static int cmp_sym(const void *a, const void *b) {
    uint32_t x = ((const CSym *)a)->rva, y = ((const CSym *)b)->rva;
    return x < y ? -1 : x > y;
}

static int cmp_u64(const void *a, const void *b) {
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

int prof_syms_each(const char *exe_path, uint64_t base, const uint64_t *addr, size_t n,
                   void (*fn)(void *ctx, uint64_t addr, const char *name, uint32_t offset), void *ctx) {
    FILE *f = fopen(exe_path, "rb");
    if (!f) return -1;
    uint8_t h[0x400];
    int named = -1;
    CSym *syms = NULL;
    char *strtab = NULL;
    uint8_t *raw = NULL;
    uint64_t *a = NULL;
    if (fread(h, 1, 0x40, f) != 0x40 || h[0] != 'M' || h[1] != 'Z') goto done;
    uint32_t pe = rd32(h + 0x3c);
    if (fseek(f, pe, SEEK_SET) || fread(h, 1, 24, f) != 24 || memcmp(h, "PE\0\0", 4)) goto done;
    uint16_t nsec = rd16(h + 6), opt = rd16(h + 20);
    uint32_t symptr = rd32(h + 12), nsym = rd32(h + 16);
    if (!symptr || !nsym || nsec > 96) goto done;
    uint32_t sec_va[96], sec_exec[96];
    if (fseek(f, pe + 24 + opt, SEEK_SET)) goto done;
    for (int i = 0; i < nsec; i++) {
        if (fread(h, 1, 40, f) != 40) goto done;
        sec_va[i] = rd32(h + 12);
        sec_exec[i] = (rd32(h + 36) & 0x20000020) != 0;   // CNT_CODE or MEM_EXECUTE
    }
    // the string table follows the symbols: its first 4 bytes are its size
    uint8_t szb[4];
    if (fseek(f, (long)symptr + (long)nsym * 18, SEEK_SET) || fread(szb, 1, 4, f) != 4) goto done;
    uint32_t strsz = rd32(szb);
    strtab = malloc(strsz + 1u);
    if (!strtab) goto done;
    memcpy(strtab, szb, 4);
    if (strsz > 4 && fread(strtab + 4, 1, strsz - 4, f) != strsz - 4) goto done;
    strtab[strsz] = 0;
    // the symbols in chunks; functions in executable sections
    size_t ns = 0, cs = 1 << 16;
    syms = malloc(cs * sizeof *syms);
    enum { CH = 1 << 16 };
    raw = malloc(CH * 18);
    if (!syms || !raw || fseek(f, symptr, SEEK_SET)) goto done;
    uint32_t skip = 0;
    for (uint32_t done_n = 0; done_n < nsym;) {
        uint32_t k = nsym - done_n < CH ? nsym - done_n : CH;
        if (fread(raw, 18, k, f) != k) goto done;
        for (uint32_t i = 0; i < k; i++) {
            const uint8_t *s = raw + i * 18;
            if (skip) { skip--; continue; }   // auxiliary records
            skip = s[17];
            int16_t sec = (int16_t)rd16(s + 12);
            uint8_t cls = s[16];
            if (sec <= 0 || sec > nsec || !sec_exec[sec - 1] || (cls != 2 && cls != 3)) continue;
            CSym c = {sec_va[sec - 1] + rd32(s + 8), 0, {0}};
            if (rd32(s)) memcpy(c.sname, s, 8);
            else c.str = rd32(s + 4);
            const char *nm = c.str ? (c.str < strsz ? strtab + c.str : "") : c.sname;
            if (nm[0] == '.' || !nm[0]) continue;   // section symbols
            if (ns == cs) {
                CSym *g = realloc(syms, (cs *= 2) * sizeof *syms);
                if (!g) goto done;
                syms = g;
            }
            syms[ns++] = c;
        }
        done_n += k;
    }
    qsort(syms, ns, sizeof *syms, cmp_sym);
    a = malloc((n ? n : 1) * sizeof *a);
    if (!a) goto done;
    memcpy(a, addr, n * sizeof *a);
    qsort(a, n, sizeof *a, cmp_u64);
    named = 0;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (i && a[i] == a[i - 1]) continue;
        if (a[i] < base || a[i] - base >= 0xffffffffull) continue;
        uint32_t rva = (uint32_t)(a[i] - base);
        while (j < ns && syms[j].rva <= rva) j++;   // a is sorted, so j only grows
        if (!j) continue;
        const CSym *c = &syms[j - 1];
        char sn[9] = {0};
        const char *nm = c->str ? (c->str < strsz ? strtab + c->str : "?") : (memcpy(sn, c->sname, 8), sn);
        fn(ctx, a[i], nm, rva - c->rva);
        named++;
    }
done:
    free(a), free(raw), free(syms), free(strtab);
    fclose(f);
    return named;
}

static void write_line(void *ctx, uint64_t addr, const char *name, uint32_t offset) {
    fprintf((FILE *)ctx, "sym %llx %s+0x%x\n", (unsigned long long)addr, name, offset);
}

int prof_syms_write(FILE *out, const char *exe_path, uint64_t base, const uint64_t *addr, size_t n) {
    return prof_syms_each(exe_path, base, addr, n, write_line, out);
}
