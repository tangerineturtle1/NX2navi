// The zip file of a shared save (save_zip.h). Little-endian fields as the zip format (PKWARE APPNOTE) lays them out;
// deflate as RFC 1951 defines it.
#include <stdlib.h>
#include <string.h>

#include "save_zip.h"

// ---- CRC-32 (the zip polynomial, reflected), one table built on first use
static uint32_t crc_table[256];
static volatile int crc_ready;

uint32_t sz_crc32(uint32_t crc, const void *data, size_t n) {
    if (!crc_ready) {   // two threads may build it at once: they write the same values
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            crc_table[i] = c;
        }
        __atomic_store_n(&crc_ready, 1, __ATOMIC_RELEASE);
    }
    const uint8_t *p = data;
    crc = ~crc;
    while (n--) crc = crc_table[(crc ^ *p++) & 0xff] ^ (crc >> 8);
    return ~crc;
}

// ---- deflate tables shared by both directions: length codes 257..285 and distance codes 0..29
static const uint16_t len_base[29] = {3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                      31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const uint8_t len_extra[29] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const uint16_t dist_base[30] = {1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                       193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const uint8_t dist_extra[30] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// ---- inflate
typedef struct {
    const uint8_t *in;
    size_t n, pos;
    uint64_t bitbuf;
    int bitcnt, bad;
    uint8_t *out;
    size_t cap, at;
} Inflate;

// need (0..16) bits, the first read in the lowest bit; past the end of the input: 0 and bad set.
static unsigned get_bits(Inflate *s, int need) {
    while (s->bitcnt < need) {
        if (s->pos >= s->n) {
            s->bad = 1;
            return 0;
        }
        s->bitbuf |= (uint64_t)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    unsigned v = (unsigned)(s->bitbuf & ((1u << need) - 1));
    s->bitbuf >>= need;
    s->bitcnt -= need;
    return v;
}

// A canonical Huffman code: how many codes of each length, and the symbols in code order.
typedef struct {
    uint16_t count[16];
    uint16_t symbol[288];
} Huff;

// The code of lengths[0..n) (0: the symbol is unused). 0 when usable: no length is over-subscribed (an incomplete
// code is allowed, as a stream with a single distance code has one; a code it lacks then reads as damaged).
static int huff_build(Huff *h, const uint8_t *lengths, int n) {
    uint16_t offs[16];
    memset(h->count, 0, sizeof h->count);
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    int left = 1;
    for (int len = 1; len < 16; len++) {
        left = (left << 1) - h->count[len];
        if (left < 0) return -1;
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++)
        if (lengths[i]) h->symbol[offs[lengths[i]]++] = (uint16_t)i;
    return 0;
}

// The next symbol of code h, the bits read one at a time (Huffman codes are packed from their first bit); -1 damaged.
static int huff_decode(Inflate *s, const Huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= (int)get_bits(s, 1);
        if (s->bad) return -1;
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
    }
    return -1;
}

// The symbols of one block until its end code.
static int inflate_codes(Inflate *s, const Huff *lit, const Huff *dist) {
    for (;;) {
        int sym = huff_decode(s, lit);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->at >= s->cap) return -1;
            s->out[s->at++] = (uint8_t)sym;
            continue;
        }
        if (sym == 256) return 0;
        sym -= 257;
        if (sym >= 29) return -1;
        size_t len = len_base[sym] + get_bits(s, len_extra[sym]);
        int d = huff_decode(s, dist);
        if (d < 0 || d >= 30) return -1;
        size_t back = dist_base[d] + get_bits(s, dist_extra[d]);
        if (s->bad || back > s->at || len > s->cap - s->at) return -1;
        for (size_t i = 0; i < len; i++, s->at++) s->out[s->at] = s->out[s->at - back];   // overlapping on purpose
    }
}

static int inflate_stored(Inflate *s) {
    s->bitbuf = 0, s->bitcnt = 0;   // the rest of the current byte
    if (s->n - s->pos < 4) return -1;
    unsigned len = s->in[s->pos] | s->in[s->pos + 1] << 8, nlen = s->in[s->pos + 2] | s->in[s->pos + 3] << 8;
    s->pos += 4;
    if (len != (~nlen & 0xffffu) || len > s->n - s->pos || len > s->cap - s->at) return -1;
    memcpy(s->out + s->at, s->in + s->pos, len);
    s->pos += len, s->at += len;
    return 0;
}

static int inflate_fixed(Inflate *s) {
    static Huff lit, dist;
    static volatile int built;
    if (!built) {   // two threads may build them at once: they write the same values
        uint8_t l[288];
        int i = 0;
        for (; i < 144; i++) l[i] = 8;
        for (; i < 256; i++) l[i] = 9;
        for (; i < 280; i++) l[i] = 7;
        for (; i < 288; i++) l[i] = 8;
        huff_build(&lit, l, 288);
        for (i = 0; i < 30; i++) l[i] = 5;
        huff_build(&dist, l, 30);
        __atomic_store_n(&built, 1, __ATOMIC_RELEASE);
    }
    return inflate_codes(s, &lit, &dist);
}

static int inflate_dynamic(Inflate *s) {
    static const uint8_t order[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    uint8_t lengths[320];
    Huff lit, dist;
    int nlen = (int)get_bits(s, 5) + 257, ndist = (int)get_bits(s, 5) + 1, ncode = (int)get_bits(s, 4) + 4;
    if (s->bad || nlen > 286 || ndist > 30) return -1;
    memset(lengths, 0, sizeof lengths);
    for (int i = 0; i < ncode; i++) lengths[order[i]] = (uint8_t)get_bits(s, 3);
    if (s->bad || huff_build(&lit, lengths, 19)) return -1;   // the code lengths' own code, in lit for now
    for (int i = 0; i < nlen + ndist;) {
        int sym = huff_decode(s, &lit);
        if (sym < 0) return -1;
        if (sym < 16) {
            lengths[i++] = (uint8_t)sym;
            continue;
        }
        int repeat, value = 0;
        if (sym == 16) {
            if (!i) return -1;
            value = lengths[i - 1];
            repeat = 3 + (int)get_bits(s, 2);
        } else if (sym == 17) {
            repeat = 3 + (int)get_bits(s, 3);
        } else {
            repeat = 11 + (int)get_bits(s, 7);
        }
        if (s->bad || i + repeat > nlen + ndist) return -1;
        while (repeat--) lengths[i++] = (uint8_t)value;
    }
    if (!lengths[256]) return -1;   // a block without its end code
    if (huff_build(&lit, lengths, nlen) || huff_build(&dist, lengths + nlen, ndist)) return -1;
    return inflate_codes(s, &lit, &dist);
}

int sz_inflate(const uint8_t *src, size_t n, uint8_t *dst, size_t dst_n) {
    Inflate s = {.in = src, .n = n, .out = dst, .cap = dst_n};
    for (int last = 0; !last;) {
        last = (int)get_bits(&s, 1);
        int type = (int)get_bits(&s, 2), r;
        if (s.bad) return -1;
        if (type == 0) r = inflate_stored(&s);
        else if (type == 1) r = inflate_fixed(&s);
        else if (type == 2) r = inflate_dynamic(&s);
        else r = -1;
        if (r || s.bad) return -1;
    }
    return s.at == dst_n ? 0 : -1;
}

// ---- deflate: greedy LZ77 over a 32 KiB window (hash chains of 3-byte keys), the fixed Huffman codes
typedef struct {
    uint8_t *out;
    size_t at, cap;
    uint64_t bitbuf;
    int bitcnt;
} BitOut;

static void put_bits(BitOut *b, uint32_t v, int n) {
    b->bitbuf |= (uint64_t)v << b->bitcnt;
    b->bitcnt += n;
    while (b->bitcnt >= 8) {
        if (b->at < b->cap) b->out[b->at] = (uint8_t)b->bitbuf;
        b->at++;
        b->bitbuf >>= 8;
        b->bitcnt -= 8;
    }
}

// A Huffman code goes out from its first bit: reversed into the LSB-first stream.
static void put_code(BitOut *b, uint32_t code, int len) {
    uint32_t r = 0;
    for (int i = 0; i < len; i++) r |= ((code >> i) & 1u) << (len - 1 - i);
    put_bits(b, r, len);
}

static void put_literal(BitOut *b, int sym) {   // a literal/length symbol in the fixed code
    if (sym < 144) put_code(b, 0x30u + (uint32_t)sym, 8);
    else if (sym < 256) put_code(b, 0x190u + (uint32_t)(sym - 144), 9);
    else if (sym < 280) put_code(b, (uint32_t)(sym - 256), 7);
    else put_code(b, 0xc0u + (uint32_t)(sym - 280), 8);
}

static void put_match(BitOut *b, size_t len, size_t back) {
    int i = 28;
    while (len_base[i] > len) i--;
    put_literal(b, 257 + i);
    put_bits(b, (uint32_t)(len - len_base[i]), len_extra[i]);
    int d = 29;
    while (dist_base[d] > back) d--;
    put_code(b, (uint32_t)d, 5);
    put_bits(b, (uint32_t)(back - dist_base[d]), dist_extra[d]);
}

#define WINDOW 32768u
#define HASH_BITS 15
#define CHAIN 48          // candidates tried per position: logs repeat a lot, saves little
#define MAX_MATCH 258

uint8_t *sz_deflate(const uint8_t *src, size_t n, size_t *out_n) {
    BitOut b = {.cap = n + n / 8 + 64};   // a literal takes at most 9 bits
    b.out = malloc(b.cap);
    int32_t *head = malloc(sizeof(int32_t) << HASH_BITS), *prev = malloc(sizeof(int32_t) * WINDOW);
    if (!b.out || !head || !prev) {
        free(b.out), free(head), free(prev);
        return NULL;
    }
    for (size_t i = 0; i < (1u << HASH_BITS); i++) head[i] = -1;
    put_bits(&b, 1, 1);   // the last block
    put_bits(&b, 1, 2);   // fixed Huffman codes
#define HASH(p) ((((uint32_t)(p)[0] << 10) ^ ((uint32_t)(p)[1] << 5) ^ (p)[2]) & ((1u << HASH_BITS) - 1))
    size_t i = 0;
    while (i < n) {
        size_t best = 0, back = 0;
        if (n - i >= 3) {
            uint32_t h = HASH(src + i);
            int32_t cand = head[h];
            size_t limit = n - i < MAX_MATCH ? n - i : MAX_MATCH;
            for (int tries = 0; cand >= 0 && i - (size_t)cand <= WINDOW && tries < CHAIN; tries++) {
                const uint8_t *a = src + cand, *c = src + i;
                if (a[best] == c[best]) {   // only a longer match can win
                    size_t k = 0;
                    while (k < limit && a[k] == c[k]) k++;
                    if (k > best) {
                        best = k, back = i - (size_t)cand;
                        if (k == limit) break;
                    }
                }
                int32_t next = prev[cand & (WINDOW - 1)];
                if (next >= cand) break;   // an older entry overwritten by a newer one
                cand = next;
            }
        }
        size_t step = best >= 3 ? best : 1;
        if (best >= 3) put_match(&b, best, back);
        else put_literal(&b, src[i]);
        for (size_t k = 0; k < step; k++, i++)   // every position covered goes into the chains
            if (n - i >= 3) {
                uint32_t h = HASH(src + i);
                prev[i & (WINDOW - 1)] = head[h];
                head[h] = (int32_t)i;
            }
    }
#undef HASH
    put_literal(&b, 256);
    put_bits(&b, 0, 7);   // out to the byte
    free(head), free(prev);
    if (b.at > b.cap) {   // cannot happen with the bound above; kept as the guard put_bits relies on
        free(b.out);
        return NULL;
    }
    *out_n = b.at;
    return b.out;
}

// ---- writing a zip
static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffff), put16(p + 2, v >> 16); }
static unsigned get16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

static uint8_t *room(SzWriter *w, size_t k) {
    if (w->failed) return NULL;
    if (w->n + k > 0xffffffffu) {   // offsets and sizes are 32-bit without the 64-bit extension
        w->failed = 1;
        return NULL;
    }
    if (w->n + k > w->cap) {
        size_t cap = w->cap ? w->cap : 1 << 16;
        while (cap < w->n + k) cap *= 2;
        uint8_t *nb = realloc(w->buf, cap);
        if (!nb) {
            w->failed = 1;
            return NULL;
        }
        w->buf = nb, w->cap = cap;
    }
    uint8_t *p = w->buf + w->n;
    w->n += k;
    return p;
}

void sz_writer_init(SzWriter *w, int year, int month, int day, int hour, int minute, int second) {
    memset(w, 0, sizeof *w);
    if (year < 1980) year = 1980, month = 1, day = 1, hour = minute = second = 0;
    w->dos_time = (uint16_t)(hour << 11 | minute << 5 | second / 2);
    w->dos_date = (uint16_t)((year - 1980) << 9 | month << 5 | day);
}

void sz_writer_add(SzWriter *w, const char *name, const void *data, size_t n) {
    if (w->failed) return;
    size_t name_n = strlen(name);
    if (n > 0xffffffffu || name_n >= 0xffff) {
        w->failed = 1;
        return;
    }
    if (w->count == w->cap_m) {
        int cap = w->cap_m ? w->cap_m * 2 : 32;
        SzMember *m = realloc(w->m, (size_t)cap * sizeof *m);
        if (!m) {
            w->failed = 1;
            return;
        }
        w->m = m, w->cap_m = cap;
    }
    SzMember *m = &w->m[w->count];
    m->name = malloc(name_n + 1);
    if (!m->name) {
        w->failed = 1;
        return;
    }
    memcpy(m->name, name, name_n + 1);
    w->count++;
    m->crc = sz_crc32(0, data, n);
    m->usize = (uint32_t)n;
    size_t packed_n = 0;
    uint8_t *packed = n >= 64 ? sz_deflate(data, n, &packed_n) : NULL;
    if (packed && packed_n >= n) free(packed), packed = NULL;   // stored when deflate does not help
    m->method = packed ? 8 : 0;
    m->csize = (uint32_t)(packed ? packed_n : n);
    m->offset = (uint32_t)w->n;
    uint8_t *h = room(w, 30 + name_n);
    if (h) {
        put32(h, 0x04034b50);
        put16(h + 4, 20);   // version needed: deflate
        put16(h + 6, 0);
        put16(h + 8, m->method);
        put16(h + 10, w->dos_time);
        put16(h + 12, w->dos_date);
        put32(h + 14, m->crc);
        put32(h + 18, m->csize);
        put32(h + 22, m->usize);
        put16(h + 26, (unsigned)name_n);
        put16(h + 28, 0);
        memcpy(h + 30, name, name_n);
    }
    uint8_t *d = room(w, m->csize);
    if (d && m->csize) memcpy(d, packed ? packed : data, m->csize);
    free(packed);
}

uint8_t *sz_writer_finish(SzWriter *w, size_t *n) {
    size_t cd_at = w->n;
    for (int i = 0; i < w->count && !w->failed; i++) {
        SzMember *m = &w->m[i];
        size_t name_n = strlen(m->name);
        uint8_t *c = room(w, 46 + name_n);
        if (!c) break;
        put32(c, 0x02014b50);
        put16(c + 4, 20);   // made by: MS-DOS attributes, version 2.0
        put16(c + 6, 20);
        put16(c + 8, 0);
        put16(c + 10, m->method);
        put16(c + 12, w->dos_time);
        put16(c + 14, w->dos_date);
        put32(c + 16, m->crc);
        put32(c + 20, m->csize);
        put32(c + 24, m->usize);
        put16(c + 28, (unsigned)name_n);
        put16(c + 30, 0);   // extra
        put16(c + 32, 0);   // comment
        put16(c + 34, 0);   // disk
        put16(c + 36, 0);   // internal attributes
        put32(c + 38, 0);   // external attributes
        put32(c + 42, m->offset);
        memcpy(c + 46, m->name, name_n);
    }
    size_t cd_n = w->n - cd_at;
    uint8_t *e = w->count < 0xffff ? room(w, 22) : (w->failed = 1, NULL);
    if (e) {
        put32(e, 0x06054b50);
        put16(e + 4, 0);
        put16(e + 6, 0);
        put16(e + 8, (unsigned)w->count);
        put16(e + 10, (unsigned)w->count);
        put32(e + 12, (uint32_t)cd_n);
        put32(e + 16, (uint32_t)cd_at);
        put16(e + 20, 0);
    }
    for (int i = 0; i < w->count; i++) free(w->m[i].name);
    free(w->m);
    uint8_t *out = w->failed ? NULL : w->buf;
    if (w->failed) free(w->buf);
    *n = w->failed ? 0 : w->n;
    memset(w, 0, sizeof *w);
    return out;
}

// ---- reading a zip
int sz_read_dir(const uint8_t *zip, size_t n, SzEntry **out, int *count) {
    *out = NULL, *count = 0;
    if (n < 22) return SZ_E_NOT_ZIP;
    // the end record: its signature within the last 22 bytes plus the longest comment
    size_t e = n - 22, stop = n > 22 + 0xffff ? n - 22 - 0xffff : 0;
    while (get32(zip + e) != 0x06054b50 || 22 + get16(zip + e + 20) != n - e) {
        if (e == stop) return SZ_E_NOT_ZIP;
        e--;
    }
    unsigned total = get16(zip + e + 10);
    uint32_t cd_n = get32(zip + e + 12), cd_at = get32(zip + e + 16);
    if (get16(zip + e + 4) || get16(zip + e + 6) || get16(zip + e + 8) != total) return SZ_E_UNSUPPORTED;   // spanned
    if (total == 0xffff || cd_n == 0xffffffffu || cd_at == 0xffffffffu) return SZ_E_UNSUPPORTED;   // the 64-bit zip
    if (cd_at > e || cd_n > e - cd_at) return SZ_E_DAMAGED;
    SzEntry *list = calloc(total ? total : 1, sizeof *list);
    if (!list) return SZ_E_MEMORY;
    size_t p = cd_at;
    int r = SZ_OK;
    for (unsigned i = 0; i < total && r == SZ_OK; i++) {
        if (p + 46 > cd_at + cd_n || get32(zip + p) != 0x02014b50) {
            r = SZ_E_DAMAGED;
            break;
        }
        const uint8_t *c = zip + p;
        unsigned made_by = get16(c + 4), flags = get16(c + 8), name_n = get16(c + 28);
        size_t next = p + 46 + name_n + get16(c + 30) + get16(c + 32);
        uint32_t ext = get32(c + 38), local = get32(c + 42);
        SzEntry *en = &list[i];
        en->method = (uint16_t)get16(c + 10);
        en->crc = get32(c + 16);
        en->csize = get32(c + 20);
        en->usize = get32(c + 24);
        if (next > cd_at + cd_n) r = SZ_E_DAMAGED;
        else if (flags & 1 || (en->method != 0 && en->method != 8) || name_n >= sizeof en->name ||
                 en->csize == 0xffffffffu || en->usize == 0xffffffffu || local == 0xffffffffu)
            r = SZ_E_UNSUPPORTED;   // encrypted, another method, a long name, a 64-bit member
        if (r) break;
        memcpy(en->name, c + 46, name_n);
        en->name[name_n] = 0;
        if (memchr(en->name, 0, name_n)) {
            r = SZ_E_DAMAGED;
            break;
        }
        en->is_dir = name_n && en->name[name_n - 1] == '/';
        en->is_link = (made_by >> 8) == 3 && ((ext >> 16) & 0170000) == 0120000;   // Unix: S_IFLNK
        if (local > cd_at || cd_at - local < 30 || get32(zip + local) != 0x04034b50) {
            r = SZ_E_DAMAGED;
            break;
        }
        uint64_t data = (uint64_t)local + 30 + get16(zip + local + 26) + get16(zip + local + 28);
        if (data > cd_at || en->csize > cd_at - data) {
            r = SZ_E_DAMAGED;
            break;
        }
        en->data = data;
        p = next;
    }
    if (r) {
        free(list);
        return r;
    }
    *out = list, *count = (int)total;
    return SZ_OK;
}

uint8_t *sz_extract(const uint8_t *zip, size_t n, const SzEntry *e, int *err) {
    if (e->data > n || e->csize > n - e->data || e->usize > (uint64_t)SIZE_MAX - 1) {
        *err = SZ_E_DAMAGED;
        return NULL;
    }
    uint8_t *out = malloc((size_t)e->usize + 1);
    if (!out) {
        *err = SZ_E_MEMORY;
        return NULL;
    }
    const uint8_t *src = zip + e->data;
    int bad = e->method == 0 ? e->csize != e->usize : sz_inflate(src, (size_t)e->csize, out, (size_t)e->usize) != 0;
    if (!bad && e->method == 0) memcpy(out, src, (size_t)e->usize);
    if (!bad && sz_crc32(0, out, (size_t)e->usize) != e->crc) bad = 1;
    if (bad) {
        free(out);
        *err = SZ_E_DAMAGED;
        return NULL;
    }
    out[e->usize] = 0;
    *err = SZ_OK;
    return out;
}

// ---- the manifest's JSON
static const char *ws(const char *p) {
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
    return p;
}

static int hex4(const char *p) {
    int v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) return -1;
        v = v << 4 | d;
    }
    return v;
}

// The string at p (its opening quote) into out (NULL: skipped); *cut set when it did not fit. The byte after its
// closing quote, NULL when it is not a string.
static const char *json_string(const char *p, char *out, size_t cap, int *cut) {
    if (*p++ != '"') return NULL;
    size_t k = 0;
    for (;;) {
        char c = *p++;
        if (!c || (unsigned char)c < 0x20) return NULL;
        if (c == '"') break;
        if (c == '\\') {
            char esc = *p++;
            switch (esc) {
            case '"': case '\\': case '/': c = esc; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u': {
                int v = hex4(p);
                if (v < 0) return NULL;
                p += 4;
                c = v > 0 && v < 0x80 ? (char)v : '?';
                break;
            }
            default: return NULL;
            }
        }
        if (out && k + 1 < cap) out[k++] = c;
        else if (out && cut) *cut = 1;
    }
    if (out && cap) out[k] = 0;
    return p;
}

// Past one value: a string, an object or array (nested), or a number or literal.
static const char *json_skip(const char *p) {
    if (*p == '"') return json_string(p, NULL, 0, NULL);
    if (*p == '{' || *p == '[') {
        int depth = 0;
        while (*p) {
            if (*p == '"') {
                p = json_string(p, NULL, 0, NULL);
                if (!p) return NULL;
                continue;
            }
            if (*p == '{' || *p == '[') depth++;
            else if ((*p == '}' || *p == ']') && !--depth) return p + 1;
            p++;
        }
        return NULL;
    }
    const char *s = p;
    while (*p && *p != ',' && *p != '}' && *p != ']' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
    return p > s ? p : NULL;
}

// The value of key at the top level of the object json, NULL when it has none.
static const char *json_find(const char *json, const char *key) {
    const char *p = ws(json);
    if (*p++ != '{') return NULL;
    for (;;) {
        p = ws(p);
        char k[128];
        int cut = 0;
        p = json_string(p, k, sizeof k, &cut);
        if (!p) return NULL;
        p = ws(p);
        if (*p++ != ':') return NULL;
        p = ws(p);
        if (!cut && !strcmp(k, key)) return p;
        p = json_skip(p);
        if (!p) return NULL;
        p = ws(p);
        if (*p++ != ',') return NULL;
    }
}

int sz_json_str(const char *json, const char *key, char *out, size_t cap) {
    const char *v = json_find(json, key);
    return v && *v == '"' && json_string(v, out, cap, NULL) != NULL;
}

int sz_json_int(const char *json, const char *key, long long *out) {
    const char *v = json_find(json, key);
    if (!v) return 0;
    int neg = *v == '-';
    v += neg;
    if (*v < '0' || *v > '9') return 0;
    long long r = 0;
    for (; *v >= '0' && *v <= '9'; v++) {
        if (r > (0x7fffffffffffffffll - 9) / 10) return 0;
        r = r * 10 + (*v - '0');
    }
    if (*v == '.' || *v == 'e' || *v == 'E') return 0;
    *out = neg ? -r : r;
    return 1;
}

char *sz_json_escape(const char *s, char *out, size_t cap) {
    static const char hex[] = "0123456789abcdef";
    size_t k = 0;
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        char tmp[7];
        size_t m = 0;
        if (c == '"' || c == '\\') tmp[m++] = '\\', tmp[m++] = (char)c;
        else if (c < 0x20) tmp[m++] = '\\', tmp[m++] = 'u', tmp[m++] = '0', tmp[m++] = '0', tmp[m++] = hex[c >> 4],
                           tmp[m++] = hex[c & 15];
        else tmp[m++] = (char)c;
        if (k + m >= cap) break;
        memcpy(out + k, tmp, m);
        k += m;
    }
    if (cap) out[k] = 0;
    return out;
}
