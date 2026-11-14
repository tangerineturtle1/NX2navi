// Amiibo tag encryption (amiibo_crypto.h): the player's keys file, decrypting and verifying a 540-byte NTAG215 dump,
// and signing and encrypting it again after amiibo.c changed the figure's data.
//
// Two 80-byte master keys make the keys file: the data key, which encrypts and signs the figure's data, then the tag
// key, which signs the parts of the tag that never change. Neither is used directly. Each dump derives its own AES key,
// AES counter and HMAC key from each master key and from the tag's UID, salt and (for the data key) write counter, so
// the same figure data encrypts differently on every tag and after every write.
//
// An AmiiboPlain keeps the tag's own byte layout with the two encrypted regions decrypted in place: everything outside
// them, the two signatures included, sits where the tag has it. Encrypting a plain image that nobody changed gives back
// the dump it came from, byte for byte.
//
// Key bytes, derived keys and decrypted data never reach the log. Buffers that held them are wiped before they go out
// of scope (wipe, which the compiler cannot drop as a dead store).
#include "amiibo_crypto.h"

#include <stdlib.h>
#include <string.h>

#include "aes128.h"
#include "host.h"
#include "platform/platform.h"
#include "sha256.h"

// ---- one master key, 80 bytes
enum {
    MK_SIZE = 80,
    MK_HMAC_KEY = 0x00,     // 16 bytes: the key of the derivation HMAC
    MK_TYPE = 0x10,         // 14 bytes: a NUL-terminated name; it and its NUL start every derivation seed
    MK_RESERVED = 0x1E,     // 1 byte, unused
    MK_MAGIC_SIZE = 0x1F,   // how many of the 16 magic bytes the seed takes, at most 16
    MK_MAGIC = 0x20,        // 16 bytes
    MK_XOR_PAD = 0x30,      // 32 bytes, XORed with the tag's salt in the seed
};

// ---- the tag (and the plain image, which has the same layout)
enum {
    T_UID = 0x000,          // 8 bytes: UID bytes 0-2, check byte 0, UID bytes 3-6; signed and part of the seed
    T_HEADER = 0x010,       // 0xA5, not signed
    T_COUNTER = 0x011,      // u16 BE: writes of the figure. Seeds the data key, so each write re-keys the data
    T_VERSION = 0x013,      // format version, 0
    T_SETTINGS = 0x014,     // first encrypted region, 0x20 bytes:
    T_FLAGS = 0x014,        //   AMIIBO_FLAG_* in bits 4-5, the font region in bits 0-3
    T_COUNTRY = 0x015,      //   country code of the console that registered the owner
    T_CRC_COUNTER = 0x016,  //   u16 BE: changes of the console checksum below
    T_SETUP_DATE = 0x018,   //   packed date, u16 BE: owner registration
    T_WRITE_DATE = 0x01A,   //   packed date, u16 BE: last write
    T_CONSOLE_CRC = 0x01C,  //   u32 BE checksum of the last console that wrote the figure
    T_NICKNAME = 0x020,     //   10 UTF-16 BE code units, NUL-padded, not terminated when all 10 are used
    T_TAG_HMAC = 0x034,     // 32 bytes: HMAC of the tag key over UID, model info and salt
    T_MODEL = 0x054,        // 8 bytes model info, then 4 more signed bytes; part of the static signed block
    T_SALT = 0x060,         // 32 bytes chosen at manufacture; seeds both keys
    T_DATA_HMAC = 0x080,    // 32 bytes: HMAC of the data key over the header, the plain data and the static block
    T_DATA = 0x0A0,         // second encrypted region, 0x168 bytes, up to 0x208:
    T_MII = 0x0A0,          //   0x60 bytes: the owner's Mii and its checksum, kept raw
    T_TITLE_ID = 0x100,     //   u64 BE: the program that created the application area
    T_APP_COUNTER = 0x108,  //   u16 BE: writes of the application area
    T_ACCESS_ID = 0x10A,    //   u32 BE: the owner of the application area
    T_APP_AREA = 0x130,     //   AMIIBO_APP_AREA_SIZE bytes of game data
    T_DATA_END = 0x208,     // dynamic lock bytes and the NTAG215 configuration follow, not signed
};

#define SETTINGS_SIZE 0x20
#define DATA_SIZE (T_DATA_END - T_DATA)
#define STATIC_SIZE (T_SALT + 32 - T_MODEL)                       // model info, 4 bytes, salt: 0x2C
#define TAG_SIGNED_SIZE (8 + STATIC_SIZE)                          // 0x34
#define DATA_SIGNED_SIZE ((T_TAG_HMAC - T_COUNTER) + DATA_SIZE + 32 + TAG_SIGNED_SIZE)   // 0x1DF
#define SEED_MAX (14 + 16 + 16 + 32)

_Static_assert(T_SETTINGS + SETTINGS_SIZE == T_TAG_HMAC, "settings run up to the tag signature");
_Static_assert(T_APP_AREA + AMIIBO_APP_AREA_SIZE == T_DATA_END, "the application area ends the encrypted data");
_Static_assert(DATA_SIZE == 0x168 && STATIC_SIZE == 0x2C && DATA_SIGNED_SIZE == 0x1DF, "signed sizes");
_Static_assert(T_DATA_END <= AMIIBO_DUMP_SIZE && AMIIBO_KEYS_SIZE == 2 * MK_SIZE, "sizes");
_Static_assert(T_NICKNAME + 20 == T_TAG_HMAC && T_MII + 0x60 == T_TITLE_ID, "field sizes");

static PlatMutex g_keys_lock = PLAT_MUTEX_INIT;
static uint8_t g_keys[AMIIBO_KEYS_SIZE];
static bool g_have_keys;
static uint32_t g_keys_gen;   // bumped by every change of the keys, so the self-test restores only what it replaced

static void wipe(void *p, size_t n) {
    volatile uint8_t *v = p;
    while (n--) *v++ = 0;
}

// Equality without an early exit, so the time taken does not tell how many leading bytes of a signature matched.
static bool same(const uint8_t *a, const uint8_t *b, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= (uint8_t)(a[i] ^ b[i]);
    return d == 0;
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) << 32 | get32(p + 4); }
static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }
static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24), p[1] = (uint8_t)(v >> 16), p[2] = (uint8_t)(v >> 8), p[3] = (uint8_t)v;
}
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)(v >> 32)), put32(p + 4, (uint32_t)v); }

// ---- keys

static bool master_key_valid(const uint8_t *mk) {
    return memchr(mk + MK_TYPE, 0, 14) != NULL && mk[MK_MAGIC_SIZE] <= 16;
}

static bool keys_valid(const uint8_t *k) { return master_key_valid(k) && master_key_valid(k + MK_SIZE); }

bool amiibo_keys_load(const char *path) {
    uint64_t size = 0;
    uint8_t *raw = path && path[0] ? plat_file_read(path, &size) : NULL;
    enum { LOADED, MISSING, BAD_SIZE, BAD_FORMAT } why =
        !raw ? MISSING : size != AMIIBO_KEYS_SIZE ? BAD_SIZE : !keys_valid(raw) ? BAD_FORMAT : LOADED;
    plat_mutex_lock(&g_keys_lock);
    if (why == LOADED) memcpy(g_keys, raw, AMIIBO_KEYS_SIZE);
    else wipe(g_keys, sizeof g_keys);
    g_have_keys = why == LOADED;
    g_keys_gen++;
    plat_mutex_unlock(&g_keys_lock);
    if (raw) {
        wipe(raw, (size_t)size);
        free(raw);
    }
    switch (why) {
    case LOADED: rt_log("amiibo: keys loaded"); break;
    case MISSING: rt_log("amiibo: no keys file at %s", path && path[0] ? path : "(no path)"); break;
    case BAD_SIZE:
        rt_log("amiibo: keys file %s has %llu bytes, not %d", path, (unsigned long long)size, AMIIBO_KEYS_SIZE);
        break;
    case BAD_FORMAT: rt_log("amiibo: keys file %s is not in the expected format", path); break;
    }
    return why == LOADED;
}

bool amiibo_keys_loaded(void) {
    plat_mutex_lock_shared(&g_keys_lock);
    bool have = g_have_keys;
    plat_mutex_unlock_shared(&g_keys_lock);
    return have;
}

void amiibo_keys_clear(void) {
    plat_mutex_lock(&g_keys_lock);
    wipe(g_keys, sizeof g_keys);
    g_have_keys = false;
    g_keys_gen++;
    plat_mutex_unlock(&g_keys_lock);
}

// A private copy of the keys, so the crypto runs outside the lock. False without keys.
static bool keys_copy(uint8_t out[AMIIBO_KEYS_SIZE]) {
    plat_mutex_lock_shared(&g_keys_lock);
    bool have = g_have_keys;
    if (have) memcpy(out, g_keys, AMIIBO_KEYS_SIZE);
    plat_mutex_unlock_shared(&g_keys_lock);
    return have;
}

// ---- derivation

typedef struct {
    uint8_t aes_key[16], aes_iv[16], hmac_key[16];
} DerivedKey;

// 64 bytes that tie the derived keys to this tag: the write counter, 14 zero bytes, the UID page twice, the salt.
static void base_seed(const uint8_t *t, uint8_t seed[64]) {
    memset(seed, 0, 64);
    memcpy(seed, t + T_COUNTER, 2);
    memcpy(seed + 16, t + T_UID, 8);
    memcpy(seed + 24, t + T_UID, 8);
    memcpy(seed + 32, t + T_SALT, 32);
}

// The seed is the master key's type string with its NUL, the first 16 - magic size bytes of the base seed followed by
// the magic bytes (16 bytes together), base seed bytes 0x10-0x1F, and base seed bytes 0x20-0x3F XOR the pad. Two blocks
// of HMAC(master HMAC key, u16 BE block number || seed) give 64 bytes, of which the first 48 are the derived AES key,
// AES counter and HMAC key.
static void derive(const uint8_t *mk, const uint8_t base[64], DerivedKey *dk) {
    uint8_t msg[2 + SEED_MAX], out[64];
    const uint8_t *nul = memchr(mk + MK_TYPE, 0, 14);
    size_t ms = mk[MK_MAGIC_SIZE];
    if (!nul || ms > 16) {   // amiibo_keys_load refuses such keys; this only keeps the copies below in bounds
        memset(dk, 0, sizeof *dk);
        return;
    }
    size_t tl = (size_t)(nul - (mk + MK_TYPE)) + 1, n = 2;
    memcpy(msg + n, mk + MK_TYPE, tl), n += tl;
    memcpy(msg + n, base, 16 - ms), n += 16 - ms;
    memcpy(msg + n, mk + MK_MAGIC, ms), n += ms;
    memcpy(msg + n, base + 16, 16), n += 16;
    for (size_t i = 0; i < 32; i++) msg[n + i] = base[32 + i] ^ mk[MK_XOR_PAD + i];
    n += 32;
    for (int blk = 0; blk < 2; blk++) {
        msg[0] = 0, msg[1] = (uint8_t)blk;
        hmac_sha256(mk + MK_HMAC_KEY, 16, msg, n, out + 32 * blk);
    }
    memcpy(dk->aes_key, out, 16);
    memcpy(dk->aes_iv, out + 16, 16);
    memcpy(dk->hmac_key, out + 32, 16);
    wipe(msg, sizeof msg);
    wipe(out, sizeof out);
}

// AES-128-CTR over both encrypted regions as one stream: the settings, then the data. Its own inverse.
static void crypt_regions(const DerivedKey *dk, uint8_t *t) {
    uint8_t buf[SETTINGS_SIZE + DATA_SIZE];
    Aes128 aes;
    memcpy(buf, t + T_SETTINGS, SETTINGS_SIZE);
    memcpy(buf + SETTINGS_SIZE, t + T_DATA, DATA_SIZE);
    aes128_init(&aes, dk->aes_key);
    aes128_ctr(&aes, dk->aes_iv, buf, buf, sizeof buf);
    memcpy(t + T_SETTINGS, buf, SETTINGS_SIZE);
    memcpy(t + T_DATA, buf + SETTINGS_SIZE, DATA_SIZE);
    wipe(buf, sizeof buf);
    wipe(&aes, sizeof aes);
}

// The tag signature: UID page, then model info, the 4 bytes after it and the salt.
static void sign_tag(const DerivedKey *tk, const uint8_t *t, uint8_t mac[32]) {
    uint8_t msg[TAG_SIGNED_SIZE];
    memcpy(msg, t + T_UID, 8);
    memcpy(msg + 8, t + T_MODEL, STATIC_SIZE);
    hmac_sha256(tk->hmac_key, 16, msg, sizeof msg, mac);
}

// The data signature over the plain image: write counter, version and settings, the data, the tag signature, then the
// tag-signed block (UID page, model info, salt).
static void sign_data(const DerivedKey *dk, const uint8_t *t, uint8_t mac[32]) {
    uint8_t msg[DATA_SIGNED_SIZE];
    size_t n = 0;
    memcpy(msg + n, t + T_COUNTER, T_TAG_HMAC - T_COUNTER), n += T_TAG_HMAC - T_COUNTER;
    memcpy(msg + n, t + T_DATA, DATA_SIZE), n += DATA_SIZE;
    memcpy(msg + n, t + T_TAG_HMAC, 32), n += 32;
    memcpy(msg + n, t + T_UID, 8), n += 8;
    memcpy(msg + n, t + T_MODEL, STATIC_SIZE), n += STATIC_SIZE;
    hmac_sha256(dk->hmac_key, 16, msg, n, mac);
    wipe(msg, sizeof msg);
}

static bool decrypt_with(const uint8_t *keys, const uint8_t *tag, AmiiboPlain *out) {
    AmiiboPlain p;
    uint8_t seed[64], mac[32];
    DerivedKey dk, tk;
    memcpy(p.b, tag, AMIIBO_DUMP_SIZE);   // before out is cleared: out may be the tag itself
    memset(out, 0, sizeof *out);
    base_seed(p.b, seed);
    derive(keys, seed, &dk);
    derive(keys + MK_SIZE, seed, &tk);
    crypt_regions(&dk, p.b);
    sign_tag(&tk, p.b, mac);
    bool ok = same(mac, p.b + T_TAG_HMAC, 32);
    sign_data(&dk, p.b, mac);
    ok = same(mac, p.b + T_DATA_HMAC, 32) && ok;
    if (ok) memcpy(out->b, p.b, AMIIBO_DUMP_SIZE);
    wipe(&p, sizeof p);
    wipe(seed, sizeof seed);
    wipe(mac, sizeof mac);
    wipe(&dk, sizeof dk);
    wipe(&tk, sizeof tk);
    return ok;
}

// The tag signature first, since the data signature covers it; both over the plain bytes; then the encryption.
static void encrypt_with(const uint8_t *keys, const AmiiboPlain *plain, uint8_t *tag) {
    AmiiboPlain p;
    uint8_t seed[64];
    DerivedKey dk, tk;
    memcpy(p.b, plain->b, AMIIBO_DUMP_SIZE);
    base_seed(p.b, seed);
    derive(keys, seed, &dk);
    derive(keys + MK_SIZE, seed, &tk);
    sign_tag(&tk, p.b, p.b + T_TAG_HMAC);
    sign_data(&dk, p.b, p.b + T_DATA_HMAC);
    crypt_regions(&dk, p.b);
    memcpy(tag, p.b, AMIIBO_DUMP_SIZE);
    wipe(&p, sizeof p);
    wipe(seed, sizeof seed);
    wipe(&dk, sizeof dk);
    wipe(&tk, sizeof tk);
}

bool amiibo_decrypt(const uint8_t tag[AMIIBO_DUMP_SIZE], AmiiboPlain *out) {
    uint8_t keys[AMIIBO_KEYS_SIZE];
    if (!keys_copy(keys)) {
        memset(out, 0, sizeof *out);
        return false;
    }
    bool ok = decrypt_with(keys, tag, out);
    wipe(keys, sizeof keys);
    if (!ok) rt_log("amiibo: dump signature does not match the keys");
    return ok;
}

bool amiibo_encrypt(const AmiiboPlain *plain, uint8_t tag[AMIIBO_DUMP_SIZE]) {
    uint8_t keys[AMIIBO_KEYS_SIZE];
    if (!keys_copy(keys)) return false;
    encrypt_with(keys, plain, tag);
    wipe(keys, sizeof keys);
    return true;
}

// ---- fields

// Packed date: years since 2000 in bits 9-15, month in bits 5-8, day in bits 0-4.
static void date_unpack(uint16_t v, uint16_t *year, uint8_t *month, uint8_t *day) {
    *year = (uint16_t)(2000 + (v >> 9));
    *month = (uint8_t)(v >> 5 & 0x0F);
    *day = (uint8_t)(v & 0x1F);
}

static uint16_t date_pack(uint16_t year, uint8_t month, uint8_t day) {
    unsigned y = year < 2000 ? 0 : year - 2000u > 127 ? 127 : year - 2000u;
    return (uint16_t)(y << 9 | (month & 0x0Fu) << 5 | (day & 0x1Fu));
}

// Appends cp as UTF-8 when it fits with room left for the NUL; returns the new length (n when it did not fit).
static size_t utf8_put(char *out, size_t cap, size_t n, uint32_t cp) {
    uint8_t t[4];
    size_t len;
    if (cp < 0x80) t[0] = (uint8_t)cp, len = 1;
    else if (cp < 0x800) t[0] = (uint8_t)(0xC0 | cp >> 6), t[1] = (uint8_t)(0x80 | (cp & 0x3F)), len = 2;
    else if (cp < 0x10000)
        t[0] = (uint8_t)(0xE0 | cp >> 12), t[1] = (uint8_t)(0x80 | (cp >> 6 & 0x3F)),
        t[2] = (uint8_t)(0x80 | (cp & 0x3F)), len = 3;
    else
        t[0] = (uint8_t)(0xF0 | cp >> 18), t[1] = (uint8_t)(0x80 | (cp >> 12 & 0x3F)),
        t[2] = (uint8_t)(0x80 | (cp >> 6 & 0x3F)), t[3] = (uint8_t)(0x80 | (cp & 0x3F)), len = 4;
    if (n + len >= cap) return n;
    memcpy(out + n, t, len);
    return n + len;
}

// One code point from the n bytes at s (n >= 1); returns the bytes it took. Malformed input reads as U+FFFD: a bad lead
// or continuation byte takes 1 byte, a well-formed overlong, surrogate or out-of-range sequence takes all its bytes.
static size_t utf8_get(const uint8_t *s, size_t n, uint32_t *cp) {
    uint32_t c = s[0], v, min;
    size_t len;
    if (c < 0x80) {
        *cp = c;
        return 1;
    }
    if (c >= 0xC2 && c <= 0xDF) len = 2, v = c & 0x1F, min = 0x80;
    else if (c >= 0xE0 && c <= 0xEF) len = 3, v = c & 0x0F, min = 0x800;
    else if (c >= 0xF0 && c <= 0xF4) len = 4, v = c & 0x07, min = 0x10000;
    else {
        *cp = 0xFFFD;
        return 1;
    }
    if (len > n) {
        *cp = 0xFFFD;
        return 1;
    }
    for (size_t i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *cp = 0xFFFD;
            return 1;
        }
        v = v << 6 | (s[i] & 0x3Fu);
    }
    *cp = v < min || v > 0x10FFFF || (v >= 0xD800 && v <= 0xDFFF) ? 0xFFFD : v;
    return len;
}

// 10 UTF-16 BE code units, up to the first zero unit, as NUL-terminated UTF-8. A surrogate without its partner reads
// as U+FFFD. cap >= 31 always holds the whole name (at most 3 bytes per unit).
static void nickname_decode(const uint8_t *b, char *out, size_t cap) {
    size_t n = 0;
    for (int i = 0; i < 10; i++) {
        uint32_t u = get16(b + 2 * i), cp = u;
        if (u == 0) break;
        if (u >= 0xD800 && u <= 0xDBFF) {
            uint32_t lo = i + 1 < 10 ? get16(b + 2 * (i + 1)) : 0;
            if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00), i++;
            else cp = 0xFFFD;
        } else if (u >= 0xDC00 && u <= 0xDFFF) cp = 0xFFFD;
        size_t m = utf8_put(out, cap, n, cp);
        if (m == n) break;
        n = m;
    }
    out[n] = 0;
}

// UTF-8 (up to cap bytes or the NUL) as 10 UTF-16 BE code units, zero-padded. Characters past the 10th unit are
// dropped, and a character needing a surrogate pair is dropped whole when only one unit is left.
static void nickname_encode(const char *s, size_t cap, uint8_t *b) {
    const uint8_t *u = (const uint8_t *)s;
    size_t n = 0, i = 0;
    int k = 0;
    while (n < cap && u[n]) n++;
    memset(b, 0, 20);
    while (i < n && k < 10) {
        uint32_t cp;
        i += utf8_get(u + i, n - i, &cp);
        if (cp >= 0x10000) {
            if (k + 2 > 10) break;
            put16(b + 2 * k, (uint16_t)(0xD800 + ((cp - 0x10000) >> 10)));
            put16(b + 2 * k + 2, (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF)));
            k += 2;
        } else {
            put16(b + 2 * k, (uint16_t)cp);
            k++;
        }
    }
}

void amiibo_plain_read(const AmiiboPlain *p, AmiiboFields *out) {
    const uint8_t *b = p->b;
    memset(out, 0, sizeof *out);
    out->flags = b[T_FLAGS];
    out->country_code = b[T_COUNTRY];
    out->write_counter = get16(b + T_COUNTER);
    date_unpack(get16(b + T_SETUP_DATE), &out->setup_year, &out->setup_month, &out->setup_day);
    date_unpack(get16(b + T_WRITE_DATE), &out->write_year, &out->write_month, &out->write_day);
    nickname_decode(b + T_NICKNAME, out->nickname, sizeof out->nickname);
    memcpy(out->mii, b + T_MII, sizeof out->mii);
    out->title_id = get64(b + T_TITLE_ID);
    out->app_write_counter = get16(b + T_APP_COUNTER);
    out->access_id = get32(b + T_ACCESS_ID);
    memcpy(out->app_area, b + T_APP_AREA, sizeof out->app_area);
}

void amiibo_plain_write(AmiiboPlain *p, const AmiiboFields *in) {
    const uint8_t known = AMIIBO_FLAG_REGISTERED | AMIIBO_FLAG_APP_AREA;
    uint8_t *b = p->b;
    b[T_FLAGS] = (uint8_t)((b[T_FLAGS] & ~known) | (in->flags & known));   // the font region and other bits stay
    b[T_COUNTRY] = in->country_code;
    put16(b + T_COUNTER, in->write_counter);
    put16(b + T_SETUP_DATE, date_pack(in->setup_year, in->setup_month, in->setup_day));
    put16(b + T_WRITE_DATE, date_pack(in->write_year, in->write_month, in->write_day));
    nickname_encode(in->nickname, sizeof in->nickname, b + T_NICKNAME);
    memcpy(b + T_MII, in->mii, sizeof in->mii);
    put64(b + T_TITLE_ID, in->title_id);
    put16(b + T_APP_COUNTER, in->app_write_counter);
    put32(b + T_ACCESS_ID, in->access_id);
    memcpy(b + T_APP_AREA, in->app_area, sizeof in->app_area);
}

// ---- self-test. Every key here is made up from a constant pattern.

static bool fail(const char *what) {
    rt_log("amiibo crypto selftest failed: %s", what);
    return false;
}

static bool all_zero(const uint8_t *p, size_t n) {
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) d |= p[i];
    return d == 0;
}

static bool selftest_primitives(void) {
    // FIPS-197 appendix C.1
    static const uint8_t fips_pt[16] = {0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
                                        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
    static const uint8_t fips_ct[16] = {0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
                                        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a};
    // SP 800-38A F.5.1, CTR-AES128: the counter carries out of the last byte between blocks 1 and 2
    static const uint8_t sp_key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
                                       0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c};
    static const uint8_t sp_iv[16] = {0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
                                      0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff};
    static const uint8_t sp_pt[64] = {
        0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96, 0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
        0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c, 0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
        0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11, 0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
        0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17, 0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10};
    static const uint8_t sp_ct[64] = {
        0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26, 0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce,
        0x98, 0x06, 0xf6, 0x6b, 0x79, 0x70, 0xfd, 0xff, 0x86, 0x17, 0x18, 0x7b, 0xb9, 0xff, 0xfd, 0xff,
        0x5a, 0xe4, 0xdf, 0x3e, 0xdb, 0xd5, 0xd3, 0x5e, 0x5b, 0x4f, 0x09, 0x02, 0x0d, 0xb0, 0x3e, 0xab,
        0x1e, 0x03, 0x1d, 0xda, 0x2f, 0xbe, 0x03, 0xd1, 0x79, 0x21, 0x70, 0xa0, 0xf3, 0x00, 0x9c, 0xee};
    // key 10 11 .. 1f, counter ff .. ff fe, plaintext 00 01 .. 27 (2.5 blocks): the counter wraps through all 16
    // bytes to zero after the second block
    static const uint8_t wrap_ct[40] = {
        0x44, 0xe2, 0x12, 0x1f, 0xc4, 0x9c, 0x71, 0x90, 0x06, 0xe2, 0xa0, 0x00, 0xb4, 0x0f, 0xf0, 0x66,
        0xea, 0x51, 0x3d, 0xc7, 0x13, 0x7b, 0xbf, 0x74, 0x97, 0x91, 0xf1, 0xb4, 0xe8, 0x7e, 0x84, 0x8f,
        0xcd, 0x82, 0x12, 0xda, 0x2a, 0xc9, 0xf7, 0x4b};
    // RFC 4231 test cases 2 (short key) and 6 (131-byte key, hashed first)
    static const uint8_t hmac2[32] = {0x5b, 0xdc, 0xc1, 0x46, 0xbf, 0x60, 0x75, 0x4e, 0x6a, 0x04, 0x24,
                                      0x26, 0x08, 0x95, 0x75, 0xc7, 0x5a, 0x00, 0x3f, 0x08, 0x9d, 0x27,
                                      0x39, 0x83, 0x9d, 0xec, 0x58, 0xb9, 0x64, 0xec, 0x38, 0x43};
    static const uint8_t hmac6[32] = {0x60, 0xe4, 0x31, 0x59, 0x1e, 0xe0, 0xb6, 0x7f, 0x0d, 0x8a, 0x26,
                                      0xaa, 0xcb, 0xf5, 0xb7, 0x7f, 0x8e, 0x0b, 0xc6, 0x21, 0x37, 0x28,
                                      0xc5, 0x14, 0x05, 0x46, 0x04, 0x0f, 0x0e, 0xe3, 0x7f, 0x54};
    static const char msg2[] = "what do ya want for nothing?";
    static const char msg6[] = "Test Using Larger Than Block-Size Key - Hash Key First";
    uint8_t key[16], buf[64], iv[16], mac[32], long_key[131];
    Aes128 aes;

    for (int i = 0; i < 16; i++) key[i] = (uint8_t)i;
    aes128_init(&aes, key);
    aes128_encrypt_block(&aes, fips_pt, buf);
    if (memcmp(buf, fips_ct, 16)) return fail("AES-128 FIPS-197 C.1");

    aes128_init(&aes, sp_key);
    aes128_ctr(&aes, sp_iv, sp_pt, buf, sizeof sp_pt);
    if (memcmp(buf, sp_ct, sizeof sp_ct)) return fail("AES-128-CTR SP 800-38A F.5.1");
    aes128_ctr(&aes, sp_iv, buf, buf, sizeof sp_ct);   // in place, and its own inverse
    if (memcmp(buf, sp_pt, sizeof sp_pt)) return fail("AES-128-CTR in place");

    for (int i = 0; i < 16; i++) key[i] = (uint8_t)(0x10 + i), iv[i] = 0xff;
    iv[15] = 0xfe;
    for (int i = 0; i < 40; i++) buf[i] = (uint8_t)i;
    aes128_init(&aes, key);
    aes128_ctr(&aes, iv, buf, buf, 40);
    if (memcmp(buf, wrap_ct, sizeof wrap_ct)) return fail("AES-128-CTR counter wrap");

    hmac_sha256("Jefe", 4, msg2, sizeof msg2 - 1, mac);
    if (memcmp(mac, hmac2, 32)) return fail("HMAC-SHA256 RFC 4231 case 2");
    memset(long_key, 0xaa, sizeof long_key);
    hmac_sha256(long_key, sizeof long_key, msg6, sizeof msg6 - 1, mac);
    if (memcmp(mac, hmac6, 32)) return fail("HMAC-SHA256 RFC 4231 case 6");
    return true;
}

// Two made-up master keys: a pattern, with type strings of two lengths and both magic sizes in use (14 for the data
// key, so the write counter enters its seed; 16 for the tag key).
static void selftest_keys(uint8_t k[AMIIBO_KEYS_SIZE]) {
    for (int i = 0; i < AMIIBO_KEYS_SIZE; i++) k[i] = (uint8_t)(i * 7 + 3);
    memcpy(k + MK_TYPE, "test data key", 14);
    k[MK_RESERVED] = 0, k[MK_MAGIC_SIZE] = 14;
    memcpy(k + MK_SIZE + MK_TYPE, "tag key", 8);
    k[MK_SIZE + MK_RESERVED] = 0, k[MK_SIZE + MK_MAGIC_SIZE] = 16;
}

static void selftest_pattern(AmiiboPlain *p) {
    for (int i = 0; i < AMIIBO_DUMP_SIZE; i++) p->b[i] = (uint8_t)(i * 13 + 5);
}

static void selftest_fields(AmiiboFields *f) {
    static const char name[] = "Zelda\xc3\xa9\xf0\x9f\x98\x80";   // ASCII, a 2-byte and a 4-byte character
    memset(f, 0, sizeof *f);
    f->flags = AMIIBO_FLAG_REGISTERED | AMIIBO_FLAG_APP_AREA;
    f->country_code = 0x31;
    f->write_counter = 0x1234;
    f->setup_year = 2024, f->setup_month = 3, f->setup_day = 15;
    f->write_year = 2026, f->write_month = 11, f->write_day = 14;
    memcpy(f->nickname, name, sizeof name);
    for (int i = 0; i < (int)sizeof f->mii; i++) f->mii[i] = (uint8_t)(i ^ 0x5a);
    f->title_id = 0x0100F00000ABC000ull;
    f->app_write_counter = 0x0102;
    f->access_id = 0x12345678;
    for (int i = 0; i < AMIIBO_APP_AREA_SIZE; i++) f->app_area[i] = (uint8_t)(i * 3 + 1);
}

static bool fields_equal(const AmiiboFields *a, const AmiiboFields *b) {
    return a->flags == b->flags && a->country_code == b->country_code && a->write_counter == b->write_counter &&
           a->setup_year == b->setup_year && a->setup_month == b->setup_month && a->setup_day == b->setup_day &&
           a->write_year == b->write_year && a->write_month == b->write_month && a->write_day == b->write_day &&
           strcmp(a->nickname, b->nickname) == 0 && memcmp(a->mii, b->mii, sizeof a->mii) == 0 &&
           a->title_id == b->title_id && a->app_write_counter == b->app_write_counter &&
           a->access_id == b->access_id && memcmp(a->app_area, b->app_area, sizeof a->app_area) == 0;
}

// The bytes amiibo_plain_write may change.
static bool field_byte(int i) {
    return (i >= T_COUNTER && i < T_VERSION) || i == T_FLAGS || i == T_COUNTRY ||
           (i >= T_SETUP_DATE && i < T_CONSOLE_CRC) || (i >= T_NICKNAME && i < T_TAG_HMAC) ||
           (i >= T_MII && i < T_ACCESS_ID + 4) || (i >= T_APP_AREA && i < T_DATA_END);
}

static bool nickname_case(const char *in, const char *expect) {
    AmiiboPlain p;
    AmiiboFields f, r;
    size_t n = strlen(in);
    memset(&p, 0, sizeof p);
    memset(&f, 0, sizeof f);
    memcpy(f.nickname, in, n < sizeof f.nickname ? n : sizeof f.nickname - 1);
    amiibo_plain_write(&p, &f);
    amiibo_plain_read(&p, &r);
    return strcmp(r.nickname, expect) == 0;
}

static bool selftest_plain(void) {
    static const uint8_t nick[20] = {0x00, 0x5a, 0x00, 0x65, 0x00, 0x6c, 0x00, 0x64, 0x00, 0x61,
                                     0x00, 0xe9, 0xd8, 0x3d, 0xde, 0x00, 0x00, 0x00, 0x00, 0x00};
    static const uint8_t title[8] = {0x01, 0x00, 0xf0, 0x00, 0x00, 0xab, 0xc0, 0x00};
    AmiiboPlain orig, p;
    AmiiboFields f, r, expect;
    selftest_pattern(&orig);
    p = orig;
    selftest_fields(&f);
    amiibo_plain_write(&p, &f);
    expect = f;
    expect.flags = (uint8_t)((orig.b[T_FLAGS] & 0xCF) | 0x30);
    amiibo_plain_read(&p, &r);
    if (!fields_equal(&r, &expect)) return fail("fields read back");
    if (p.b[T_COUNTER] != 0x12 || p.b[T_COUNTER + 1] != 0x34) return fail("write counter bytes");
    if (get16(p.b + T_SETUP_DATE) != 0x306f || get16(p.b + T_WRITE_DATE) != 0x356e) return fail("date bytes");
    if (memcmp(p.b + T_NICKNAME, nick, 20)) return fail("nickname bytes");
    if (memcmp(p.b + T_TITLE_ID, title, 8) || get16(p.b + T_APP_COUNTER) != 0x0102 ||
        get32(p.b + T_ACCESS_ID) != 0x12345678)
        return fail("application area header bytes");
    for (int i = 0; i < AMIIBO_DUMP_SIZE; i++)
        if (!field_byte(i) && p.b[i] != orig.b[i]) return fail("a byte outside the fields changed");

    if (!nickname_case("abcdefghijk", "abcdefghij")) return fail("nickname cut at 10 units");
    if (!nickname_case("abcdefghi\xf0\x9f\x98\x80", "abcdefghi")) return fail("nickname surrogate pair at the end");
    if (!nickname_case("a\xff" "b", "a\xef\xbf\xbd" "b")) return fail("nickname invalid UTF-8");
    memset(&p, 0, sizeof p);
    put16(p.b + T_NICKNAME, 0xD800);   // a lone high surrogate, then 'A'
    put16(p.b + T_NICKNAME + 2, 0x0041);
    amiibo_plain_read(&p, &r);
    if (strcmp(r.nickname, "\xef\xbf\xbd" "A")) return fail("nickname lone surrogate");
    return true;
}

// The format with explicit keys: a known answer computed by an independent model of the format, round trips, and
// single-byte changes that must break a signature.
static bool selftest_tag(const uint8_t *keys) {
    static const uint8_t expect_tag_hmac[32] = {
        0x13, 0x98, 0x45, 0x32, 0x7c, 0xd0, 0xda, 0x1e, 0x1f, 0x6c, 0x81, 0xe9, 0x9f, 0x85, 0xca, 0xd4,
        0x3c, 0x2d, 0xe2, 0x22, 0x17, 0x2b, 0xa0, 0x6e, 0x65, 0x03, 0xd3, 0xc5, 0xc6, 0x7a, 0xb3, 0x03};
    static const uint8_t expect_data_hmac[32] = {
        0x9e, 0x06, 0xd9, 0xd1, 0x16, 0xb6, 0xe1, 0x62, 0x84, 0x97, 0x4a, 0xc8, 0xc0, 0xe5, 0x16, 0x9b,
        0xc9, 0xf2, 0x3b, 0xbb, 0xba, 0xf5, 0xc5, 0x59, 0xc6, 0x60, 0x16, 0x3d, 0x20, 0x5e, 0xef, 0x60};
    static const uint8_t expect_sha[32] = {
        0x1a, 0xda, 0x4b, 0xe1, 0x0d, 0xc4, 0xea, 0x31, 0x9a, 0xbe, 0xc8, 0xe9, 0x10, 0x62, 0xb0, 0x10,
        0xbe, 0x66, 0xa7, 0xcc, 0x18, 0xc3, 0xa3, 0xcc, 0xb9, 0xa1, 0x4c, 0x51, 0xf7, 0xd5, 0x9f, 0x8a};
    // a byte in each of: data, UID, tag signature, write counter, salt, settings, model info
    static const int flips[] = {0x150, 0x003, 0x040, 0x011, 0x060, 0x014, 0x055};
    uint8_t bad[AMIIBO_KEYS_SIZE], tag[AMIIBO_DUMP_SIZE], again[AMIIBO_DUMP_SIZE], digest[32];
    AmiiboPlain plain, back;

    memcpy(bad, keys, sizeof bad);
    if (!keys_valid(bad)) return fail("made-up keys rejected");
    bad[MK_SIZE + MK_MAGIC_SIZE] = 17;
    if (keys_valid(bad)) return fail("magic size 17 accepted");
    memcpy(bad, keys, sizeof bad);
    memset(bad + MK_TYPE, 'x', 14);
    if (keys_valid(bad)) return fail("type string without NUL accepted");

    selftest_pattern(&plain);
    encrypt_with(keys, &plain, tag);
    if (memcmp(tag + T_TAG_HMAC, expect_tag_hmac, 32)) return fail("tag signature known answer");
    if (memcmp(tag + T_DATA_HMAC, expect_data_hmac, 32)) return fail("data signature known answer");
    sha256(tag, sizeof tag, digest);
    if (memcmp(digest, expect_sha, 32)) return fail("encrypted tag known answer");
    for (int i = 0; i < AMIIBO_DUMP_SIZE; i++) {
        bool changed = (i >= T_SETTINGS && i < T_MODEL) || (i >= T_DATA_HMAC && i < T_DATA_END);
        if (!changed && tag[i] != plain.b[i]) return fail("encryption changed a byte it must keep");
    }

    if (!decrypt_with(keys, tag, &back)) return fail("decrypt of an encrypted tag");
    for (int i = 0; i < AMIIBO_DUMP_SIZE; i++) {
        bool sig = (i >= T_TAG_HMAC && i < T_MODEL) || (i >= T_DATA_HMAC && i < T_DATA);
        if (!sig && back.b[i] != plain.b[i]) return fail("decrypt does not give the plain image back");
    }
    encrypt_with(keys, &back, again);
    if (memcmp(again, tag, sizeof tag)) return fail("encrypting the decrypted image changed the tag");

    for (size_t f = 0; f < sizeof flips / sizeof flips[0]; f++) {
        memcpy(again, tag, sizeof tag);
        again[flips[f]] ^= 0x01;
        memset(&back, 0x5a, sizeof back);
        if (decrypt_with(keys, again, &back)) return fail("a changed byte kept the signatures valid");
        if (!all_zero(back.b, sizeof back.b)) return fail("a failed decrypt left data behind");
    }
    return true;
}

// Replaces the loaded keys with k (have false: none) unless another caller changed them since *gen was taken; then
// *gen is the new generation. False when the keys changed meanwhile, and nothing is replaced.
static bool keys_swap(const uint8_t *k, bool have, uint32_t *gen) {
    plat_mutex_lock(&g_keys_lock);
    bool mine = g_keys_gen == *gen;
    if (mine) {
        if (have) memcpy(g_keys, k, AMIIBO_KEYS_SIZE);
        else wipe(g_keys, sizeof g_keys);
        g_have_keys = have;
        *gen = ++g_keys_gen;
    }
    plat_mutex_unlock(&g_keys_lock);
    return mine;
}

// The public calls: without keys, then with the made-up keys, then the player's keys go back. While the made-up keys
// are in place (well under a millisecond) a dump decrypted on another thread would fail and load without its data; the
// caller, ov_probe.c under --probe-services, runs no game code that loads one.
static bool selftest_public(const uint8_t *keys) {
    uint8_t saved[AMIIBO_KEYS_SIZE], tag[AMIIBO_DUMP_SIZE];
    bool saved_have, ok = true;
    uint32_t gen;
    AmiiboPlain plain, back;
    AmiiboFields f, r, expect;

    selftest_pattern(&plain);
    selftest_fields(&f);
    amiibo_plain_write(&plain, &f);
    expect = f;
    expect.flags = plain.b[T_FLAGS];

    plat_mutex_lock_shared(&g_keys_lock);
    memcpy(saved, g_keys, sizeof saved);
    saved_have = g_have_keys;
    gen = g_keys_gen;
    plat_mutex_unlock_shared(&g_keys_lock);

    bool mine = keys_swap(NULL, false, &gen);
    if (mine) {
        memset(&back, 0x5a, sizeof back);
        memset(tag, 0x5a, sizeof tag);
        if (amiibo_keys_loaded()) ok = fail("keys reported after clearing");
        else if (amiibo_decrypt(plain.b, &back) || !all_zero(back.b, sizeof back.b)) ok = fail("decrypt without keys");
        else if (amiibo_encrypt(&plain, tag) || tag[0] != 0x5a || tag[AMIIBO_DUMP_SIZE - 1] != 0x5a)
            ok = fail("encrypt without keys");
    }
    if (mine && ok) mine = keys_swap(keys, true, &gen);
    if (mine && ok) {
        if (!amiibo_keys_loaded()) ok = fail("made-up keys not reported");
        else if (!amiibo_encrypt(&plain, tag)) ok = fail("encrypt with keys");
        else if (!amiibo_decrypt(tag, &back)) ok = fail("decrypt with keys");
        else {
            amiibo_plain_read(&back, &r);
            if (!fields_equal(&r, &expect)) ok = fail("fields after an encrypt and decrypt");
        }
    }
    if (mine) keys_swap(saved, saved_have, &gen);
    else rt_log("amiibo crypto selftest: the keys were reloaded during the test; that part was skipped");
    wipe(saved, sizeof saved);
    wipe(&plain, sizeof plain);
    wipe(&back, sizeof back);
    return ok;
}

bool amiibo_crypto_selftest(void) {
    uint8_t keys[AMIIBO_KEYS_SIZE];
    selftest_keys(keys);
    bool ok = selftest_primitives() && selftest_plain() && selftest_tag(keys) && selftest_public(keys);
    wipe(keys, sizeof keys);
    return ok;
}
