// The co-op wire format (coop_proto.h): room secrets and their keys, HMAC tags, the datagram header, the client and
// relay messages and the sealed player messages. Pure functions on bytes.
#include "coop_proto.h"
#include "coop_offer.h"   // coop_offer_shown: who the puppets show

#include <string.h>

#include "sha256.h"
#include "third_party/monocypher/monocypher.h"

static const char secret_alphabet[] = "23456789BCDFGHJKMNPRSTVWXYZ";

static int secret_char(char c) { return c && strchr(secret_alphabet, c) != NULL; }

// The length of the string at s, at most max.
static size_t bounded_len(const char *s, size_t max) {
    size_t n = 0;
    while (n < max && s[n]) n++;
    return n;
}

int coop_secret_normalize(const char *text, char out[COOP_SECRET_LEN + 1]) {
    size_t n = 0;
    for (const char *p = text; *p; p++) {
        char c = *p;
        if (c == '-' || c == ' ') continue;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        if (n == COOP_SECRET_LEN || !secret_char(c)) return 0;
        out[n++] = c;
    }
    if (n != COOP_SECRET_LEN) return 0;
    out[n] = 0;
    return 1;
}

void coop_secret_format(const char secret[COOP_SECRET_LEN], char out[COOP_SECRET_TEXT + 1]) {
    for (int g = 0, at = 0; g < 3; g++) {
        if (g) out[at++] = '-';
        memcpy(out + at, secret + 6 * g, 6);
        at += 6;
    }
    out[COOP_SECRET_TEXT] = 0;
}

int coop_secret_from_random(const uint8_t random[32], char out[COOP_SECRET_LEN + 1]) {
    int n = 0;
    for (int i = 0; i < 32 && n < COOP_SECRET_LEN; i++)
        if (random[i] < 243) out[n++] = secret_alphabet[random[i] % 27];   // 243 = 9 x 27: every symbol as likely
    out[n] = 0;
    return n == COOP_SECRET_LEN;
}

// Three characters of the secret's alphabet taken from a hash of the secret, not from the secret itself: every game
// in the room gets the same label, and a log that is passed on gives away none of the secret's characters.
void coop_secret_short(const char secret[COOP_SECRET_LEN], char out[8]) {
    static const char label_key[] = "nx2navi co-op room label";
    uint8_t h[4];
    coop_blake2b(h, sizeof h, label_key, sizeof label_key - 1, secret, COOP_SECRET_LEN);
    uint32_t v = (uint32_t)h[0] | (uint32_t)h[1] << 8 | (uint32_t)h[2] << 16;
    for (int i = 0; i < 3; i++, v /= 27) out[i] = secret_alphabet[v % 27];
    memcpy(out + 3, "\xe2\x80\xa6", 4);   // U+2026 and the NUL
}

// ---- keys and tags

static void sha_two(Sha256 *s, const void *a, size_t an, const void *b, size_t bn) {
    if (an) sha256_update(s, a, an);
    if (bn) sha256_update(s, b, bn);
}

void coop_hmac_sha256(const void *key, size_t klen, const void *a, size_t an, const void *b, size_t bn, uint8_t out[32]) {
    uint8_t k[64] = {0}, pad[64], inner[32];
    Sha256 s;
    if (klen > sizeof k) sha256(key, klen, k);
    else if (klen) memcpy(k, key, klen);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x36;
    sha256_init(&s);
    sha256_update(&s, pad, sizeof pad);
    sha_two(&s, a, an, b, bn);
    sha256_final(&s, inner);
    for (int i = 0; i < 64; i++) pad[i] = k[i] ^ 0x5c;
    sha256_init(&s);
    sha256_update(&s, pad, sizeof pad);
    sha256_update(&s, inner, sizeof inner);
    sha256_final(&s, out);
}

void coop_room_keys(const char secret[COOP_SECRET_LEN], uint8_t locator[COOP_KEY_LEN], uint8_t msg_key[COOP_MSG_KEY_LEN]) {
    static const char room[] = "nx2navi-coop/2 room", messages[] = "nx2navi-coop/2 messages";
    if (locator) crypto_blake2b_keyed(locator, COOP_KEY_LEN, (const uint8_t *)secret, COOP_SECRET_LEN, (const uint8_t *)room, sizeof room - 1);
    if (msg_key)
        crypto_blake2b_keyed(msg_key, COOP_MSG_KEY_LEN, (const uint8_t *)secret, COOP_SECRET_LEN, (const uint8_t *)messages,
                             sizeof messages - 1);
}

static void make_tag(const uint8_t key[COOP_KEY_LEN], const uint8_t *hdr16, const uint8_t *payload, size_t n,
                     uint8_t tag[COOP_TAG_LEN]) {
    uint8_t full[32];
    coop_hmac_sha256(key, COOP_KEY_LEN, hdr16, 16, payload, n, full);
    memcpy(tag, full, COOP_TAG_LEN);
}

// ---- little-endian fields

static void put16(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, v), put16(p + 2, v >> 16); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v), put32(p + 4, (uint32_t)(v >> 32)); }
static uint32_t get16(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | get16(p + 2) << 16; }
static uint64_t get64(const uint8_t *p) { return get32(p) | (uint64_t)get32(p + 4) << 32; }

// ---- datagrams

size_t coop_dgram_pack(uint8_t *out, size_t cap, const CoopHdr *h, const void *payload, size_t n,
                       const uint8_t key[COOP_KEY_LEN]) {
    if (n > COOP_MAX_PAYLOAD || cap < COOP_HDR_LEN + n) return 0;
    put16(out, COOP_MAGIC);
    out[2] = h->ver, out[3] = h->type;
    put32(out + 4, h->room);
    out[8] = h->src, out[9] = h->dst;
    put16(out + 10, (uint32_t)n);
    put32(out + 12, h->seq);
    if (n) memmove(out + COOP_HDR_LEN, payload, n);   // the payload may already sit where it goes
    if (key) make_tag(key, out, out + COOP_HDR_LEN, n, out + 16);
    else memset(out + 16, 0, COOP_TAG_LEN);
    return COOP_HDR_LEN + n;
}

int coop_dgram_parse(const uint8_t *d, size_t n, CoopHdr *h, const uint8_t **payload) {
    if (n < COOP_HDR_LEN || n > COOP_MAX_DGRAM || get16(d) != COOP_MAGIC) return 0;
    uint32_t len = get16(d + 10);
    if (len != n - COOP_HDR_LEN) return 0;
    h->ver = d[2], h->type = d[3];
    h->room = get32(d + 4);
    h->src = d[8], h->dst = d[9];
    h->len = (uint16_t)len;
    h->seq = get32(d + 12);
    *payload = d + COOP_HDR_LEN;
    return 1;
}

int coop_dgram_verify(const uint8_t *d, size_t n, const uint8_t key[COOP_KEY_LEN]) {
    uint8_t tag[COOP_TAG_LEN], diff = 0;
    if (n < COOP_HDR_LEN) return 0;
    make_tag(key, d, d + COOP_HDR_LEN, n - COOP_HDR_LEN, tag);
    for (int i = 0; i < COOP_TAG_LEN; i++) diff |= (uint8_t)(tag[i] ^ d[16 + i]);
    return diff == 0;
}

int coop_dgram_tag_zero(const uint8_t *d) {
    uint8_t any = 0;
    for (int i = 0; i < COOP_TAG_LEN; i++) any |= d[16 + i];
    return any == 0;
}

// ---- names

// The length of the valid UTF-8 character at p (n bytes left), 0 when the bytes there are not one: no overlong forms,
// no surrogates, nothing above U+10FFFF.
static size_t utf8_len(const uint8_t *p, size_t n) {
    uint8_t c = p[0];
    if (c < 0x80) return 1;
    size_t len = c >= 0xC2 && c <= 0xDF ? 2 : c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
    if (!len || len > n) return 0;
    for (size_t i = 1; i < len; i++)
        if ((p[i] & 0xC0) != 0x80) return 0;
    if (c == 0xE0 && p[1] < 0xA0) return 0;    // overlong
    if (c == 0xED && p[1] > 0x9F) return 0;    // a surrogate
    if (c == 0xF0 && p[1] < 0x90) return 0;    // overlong
    if (c == 0xF4 && p[1] > 0x8F) return 0;    // above U+10FFFF
    return len;
}

// A character a log or a console may act on: C0, DEL, C1 (U+0080-U+009F), and the bidi controls U+061C, U+200E,
// U+200F, U+202A-U+202E and U+2066-U+2069.
static int control_char(const uint8_t *p, size_t len) {
    if (len == 1) return p[0] < 0x20 || p[0] == 0x7F;
    if (len == 2) return (p[0] == 0xC2 && p[1] < 0xA0) || (p[0] == 0xD8 && p[1] == 0x9C);
    if (len == 3 && p[0] == 0xE2 && p[1] == 0x80) return p[2] == 0x8E || p[2] == 0x8F || (p[2] >= 0xAA && p[2] <= 0xAE);
    if (len == 3 && p[0] == 0xE2 && p[1] == 0x81) return p[2] >= 0xA6 && p[2] <= 0xA9;
    return 0;
}

// At most max bytes of raw as valid UTF-8 into out (cap bytes, NUL-terminated): a control character and each byte that is
// not valid UTF-8 become '?', and the text is cut before the character that would not fit.
static size_t text_clean(char *out, size_t cap, const uint8_t *raw, size_t n, size_t max) {
    size_t at = 0, i = 0;
    if (!cap) return 0;
    while (i < n) {
        size_t len = utf8_len(raw + i, n - i), take = len ? len : 1;
        int bad = !len || control_char(raw + i, len);
        if (at + (bad ? 1 : take) > max || at + (bad ? 1 : take) + 1 > cap) break;
        if (bad) out[at++] = '?';
        else memcpy(out + at, raw + i, len), at += len;
        i += take;
    }
    out[at] = 0;
    return at;
}

size_t coop_name_clean(char *out, size_t cap, const uint8_t *raw, size_t n) {
    return text_clean(out, cap, raw, n, COOP_NAME_MAX);
}

// ---- client and relay messages

enum { CONNECT_FIXED = 43 };   // the fields before the name

size_t coop_connect_pack(uint8_t *out, size_t cap, const CoopConnect *c) {
    size_t nlen = bounded_len(c->name, COOP_NAME_MAX);
    size_t len = CONNECT_FIXED + nlen + 2 * COOP_COOKIE_LEN;
    size_t padded = len < COOP_CONNECT_MIN - COOP_HDR_LEN ? COOP_CONNECT_MIN - COOP_HDR_LEN : len;
    if (cap < padded) return 0;
    memset(out, 0, padded);
    out[0] = c->intent, out[1] = c->flags;
    memcpy(out + 2, c->locator, COOP_KEY_LEN);
    put64(out + 18, c->client_id);
    put64(out + 26, c->session);
    put64(out + 34, c->nonce);
    out[42] = (uint8_t)nlen;
    memcpy(out + CONNECT_FIXED, c->name, nlen);
    memcpy(out + CONNECT_FIXED + nlen, c->cookie, COOP_COOKIE_LEN);
    memcpy(out + CONNECT_FIXED + nlen + COOP_COOKIE_LEN, c->token, COOP_COOKIE_LEN);
    return padded;
}

int coop_connect_parse(const uint8_t *p, size_t n, CoopConnect *c) {
    if (n < CONNECT_FIXED) return 0;
    size_t nlen = p[42];
    if (nlen > COOP_NAME_MAX || CONNECT_FIXED + nlen + 2 * COOP_COOKIE_LEN > n) return 0;
    memset(c, 0, sizeof *c);
    c->intent = p[0], c->flags = p[1];
    memcpy(c->locator, p + 2, COOP_KEY_LEN);
    c->client_id = get64(p + 18);
    c->session = get64(p + 26);
    c->nonce = get64(p + 34);
    coop_name_clean(c->name, sizeof c->name, p + CONNECT_FIXED, nlen);
    memcpy(c->cookie, p + CONNECT_FIXED + nlen, COOP_COOKIE_LEN);
    memcpy(c->token, p + CONNECT_FIXED + nlen + COOP_COOKIE_LEN, COOP_COOKIE_LEN);
    return 1;
}

size_t coop_challenge_pack(uint8_t *out, size_t cap, uint64_t nonce, const uint8_t cookie[COOP_COOKIE_LEN]) {
    if (cap < 8 + COOP_COOKIE_LEN) return 0;
    put64(out, nonce);
    memcpy(out + 8, cookie, COOP_COOKIE_LEN);
    return 8 + COOP_COOKIE_LEN;
}

int coop_challenge_parse(const uint8_t *p, size_t n, uint64_t *nonce, uint8_t cookie[COOP_COOKIE_LEN]) {
    if (n != 8 + COOP_COOKIE_LEN) return 0;
    *nonce = get64(p);
    memcpy(cookie, p + 8, COOP_COOKIE_LEN);
    return 1;
}

size_t coop_roster_pack(uint8_t *out, size_t cap, const CoopRoster *r) {
    size_t at = 1;
    if (!cap || r->n > COOP_MAX_MEMBERS) return 0;
    out[0] = r->n;
    for (int i = 0; i < r->n; i++) {
        size_t nlen = bounded_len(r->m[i].name, COOP_NAME_MAX);
        if (at + 11 + nlen > cap) return 0;
        out[at] = r->m[i].id, out[at + 1] = r->m[i].flags;
        put64(out + at + 2, r->m[i].session);
        out[at + 10] = (uint8_t)nlen;
        memcpy(out + at + 11, r->m[i].name, nlen);
        at += 11 + nlen;
    }
    return at;
}

int coop_roster_parse(const uint8_t *p, size_t n, CoopRoster *r, size_t *used) {
    if (n < 1 || p[0] > COOP_MAX_MEMBERS) return 0;
    size_t at = 1;
    memset(r, 0, sizeof *r);
    r->n = p[0];
    for (int i = 0; i < r->n; i++) {
        if (at + 11 > n) return 0;
        size_t nlen = p[at + 10];
        if (nlen > COOP_NAME_MAX || at + 11 + nlen > n) return 0;
        r->m[i].id = p[at], r->m[i].flags = p[at + 1];
        r->m[i].session = get64(p + at + 2);
        coop_name_clean(r->m[i].name, sizeof r->m[i].name, p + at + 11, nlen);
        at += 11 + nlen;
    }
    if (used) *used = at;
    return 1;
}

size_t coop_joined_pack(uint8_t *out, size_t cap, const CoopJoined *j) {
    if (cap < 9 + COOP_COOKIE_LEN) return 0;
    out[0] = j->member;
    put64(out + 1, j->nonce);
    memcpy(out + 9, j->token, COOP_COOKIE_LEN);
    size_t k = coop_roster_pack(out + 9 + COOP_COOKIE_LEN, cap - 9 - COOP_COOKIE_LEN, &j->roster);
    return k ? 9 + COOP_COOKIE_LEN + k : 0;
}

int coop_joined_parse(const uint8_t *p, size_t n, CoopJoined *j) {
    if (n < 9 + COOP_COOKIE_LEN) return 0;
    memset(j, 0, sizeof *j);
    j->member = p[0];
    j->nonce = get64(p + 1);
    memcpy(j->token, p + 9, COOP_COOKIE_LEN);
    return coop_roster_parse(p + 9 + COOP_COOKIE_LEN, n - 9 - COOP_COOKIE_LEN, &j->roster, NULL);
}

size_t coop_error_pack(uint8_t *out, size_t cap, const CoopError *e) {
    size_t tlen = bounded_len(e->text, sizeof e->text - 1);
    if (cap < 10 + tlen) return 0;
    out[0] = e->reason;
    put64(out + 1, e->nonce);
    out[9] = (uint8_t)tlen;
    memcpy(out + 10, e->text, tlen);
    return 10 + tlen;
}

int coop_error_parse(const uint8_t *p, size_t n, CoopError *e) {
    if (n < 10 || 10u + p[9] > n) return 0;
    memset(e, 0, sizeof *e);
    e->reason = p[0];
    e->nonce = get64(p + 1);
    text_clean(e->text, sizeof e->text, p + 10, p[9], sizeof e->text - 1);   // for the log: printable UTF-8 only
    return 1;
}

size_t coop_kick_pack(uint8_t *out, size_t cap, uint8_t member, uint64_t session) {
    if (cap < 9) return 0;
    out[0] = member;
    put64(out + 1, session);
    return 9;
}

int coop_kick_parse(const uint8_t *p, size_t n, uint8_t *member, uint64_t *session) {
    if (n != 9) return 0;
    *member = p[0];
    *session = get64(p + 1);
    return 1;
}

size_t coop_u32_pack(uint8_t *out, size_t cap, uint32_t v) {
    if (cap < 4) return 0;
    put32(out, v);
    return 4;
}

int coop_u32_parse(const uint8_t *p, size_t n, uint32_t *v) {
    if (n < 4) return 0;
    *v = get32(p);
    return 1;
}

// ---- sealed player messages

static void env_nonce(uint8_t nonce[24], uint64_t session, uint32_t counter, uint8_t src) {
    memset(nonce, 0, 24);
    put64(nonce, session);
    put32(nonce + 8, counter);
    nonce[12] = src;
}

size_t coop_seal(uint8_t *out, size_t cap, const uint8_t key[COOP_MSG_KEY_LEN], uint8_t src, uint8_t dst, uint64_t session,
                 uint32_t counter, uint8_t kind, const void *bytes, size_t n) {
    size_t total = COOP_ENV_OVERHEAD + 1 + n;
    if (n > COOP_MAX_MESSAGE || cap < total) return 0;
    uint8_t nonce[24], ad[2] = {dst, COOP_ENV_V1};
    out[0] = COOP_ENV_V1;
    put64(out + 1, session);
    put32(out + 9, counter);
    uint8_t *plain = out + COOP_ENV_HDR;
    if (n) memmove(plain + 1, bytes, n);
    plain[0] = kind;
    env_nonce(nonce, session, counter, src);
    crypto_aead_lock(plain, out + COOP_ENV_HDR + 1 + n, key, nonce, ad, sizeof ad, plain, 1 + n);
    return total;
}

int coop_envelope_peek(const uint8_t *p, size_t n, uint64_t *session, uint32_t *counter) {
    if (n < COOP_ENV_OVERHEAD + 1 || p[0] != COOP_ENV_V1) return 0;
    *session = get64(p + 1);
    *counter = get32(p + 9);
    return 1;
}

int coop_open(const uint8_t *p, size_t n, const uint8_t key[COOP_MSG_KEY_LEN], uint8_t src, uint8_t dst, uint8_t *kind,
              uint8_t *out, size_t cap, size_t *nout) {
    uint64_t session;
    uint32_t counter;
    if (!coop_envelope_peek(p, n, &session, &counter)) return 0;
    size_t len = n - COOP_ENV_OVERHEAD;   // kind and bytes
    uint8_t plain[COOP_MAX_PAYLOAD], nonce[24], ad[2] = {dst, COOP_ENV_V1};
    if (len > sizeof plain || cap < len - 1) return 0;
    env_nonce(nonce, session, counter, src);
    if (crypto_aead_unlock(plain, p + n - COOP_ENV_MAC, key, nonce, ad, sizeof ad, p + COOP_ENV_HDR, len)) return 0;
    *kind = plain[0];
    if (len > 1) memcpy(out, plain + 1, len - 1);
    *nout = len - 1;
    crypto_wipe(plain, len);
    return 1;
}

// ---- signatures

void coop_sign_keypair(const uint8_t seed[32], uint8_t sk[COOP_SK_LEN], uint8_t pk[COOP_PK_LEN]) {
    uint8_t s[32];
    memcpy(s, seed, sizeof s);   // the library wipes the seed it is given
    crypto_eddsa_key_pair(sk, pk, s);
}

void coop_sign(uint8_t sig[COOP_SIG_LEN], const uint8_t sk[COOP_SK_LEN], const void *msg, size_t n) {
    crypto_eddsa_sign(sig, sk, msg, n);
}

int coop_sign_check(const uint8_t sig[COOP_SIG_LEN], const uint8_t pk[COOP_PK_LEN], const void *msg, size_t n) {
    return crypto_eddsa_check(sig, pk, msg, n) == 0;
}

void coop_blake2b(uint8_t *out, size_t outlen, const void *key, size_t klen, const void *msg, size_t n) {
    crypto_blake2b_keyed(out, outlen, key, klen, msg, n);
}

// ---- POS

static void putf(uint8_t *p, float f) {
    uint32_t v;
    memcpy(&v, &f, 4);
    put32(p, v);
}

// The f32 at p into *f; 0 when it is an infinity or a NaN (all exponent bits set), which a pose never holds.
static int getf(const uint8_t *p, float *f) {
    uint32_t v = get32(p);
    memcpy(f, &v, 4);
    return (v & 0x7f800000u) != 0x7f800000u;
}

// 1 when p[0..n) is printable ASCII: a POS's names reach the log and the game's animation lookup.
static int printable(const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (p[i] < 0x20 || p[i] > 0x7e) return 0;
    return 1;
}

enum { POS_HEAD = 11 };   // version, pseq, t_ms, flags, area length

size_t coop_pos_pack(uint8_t *out, size_t cap, const CoopPos *p) {
    size_t alen = bounded_len(p->area, COOP_POS_TEXT), n = POS_HEAD + alen + 17;
    int slots = p->nslots < COOP_POS_SLOTS ? p->nslots : COOP_POS_SLOTS;
    for (int i = 0; i < slots; i++) n += 1 + bounded_len(p->slot[i].node, COOP_POS_TEXT) + 4;
    if (cap < n) return 0;
    out[0] = COOP_POS_VERSION;
    put32(out + 1, p->pseq);
    put32(out + 5, p->t_ms);
    out[9] = p->flags, out[10] = (uint8_t)alen;
    memcpy(out + POS_HEAD, p->area, alen);
    uint8_t *q = out + POS_HEAD + alen;
    for (int k = 0; k < 3; k++) putf(q + 4 * k, p->pos[k]);
    putf(q + 12, p->yaw);
    q[16] = (uint8_t)slots, q += 17;
    for (int i = 0; i < slots; i++) {
        size_t len = bounded_len(p->slot[i].node, COOP_POS_TEXT);
        q[0] = (uint8_t)len;
        memcpy(q + 1, p->slot[i].node, len);
        putf(q + 1 + len, p->slot[i].frame);
        q += 1 + len + 4;
    }
    return n;
}

int coop_pos_parse(const uint8_t *p, size_t n, CoopPos *out) {
    if (n < POS_HEAD || p[0] != COOP_POS_VERSION || p[10] > COOP_POS_TEXT || n < POS_HEAD + p[10] + 17u ||
        !printable(p + POS_HEAD, p[10]))
        return 0;
    CoopPos r;
    memset(&r, 0, sizeof r);
    r.pseq = get32(p + 1), r.t_ms = get32(p + 5), r.flags = p[9];
    memcpy(r.area, p + POS_HEAD, p[10]);
    size_t at = POS_HEAD + (size_t)p[10];
    int finite = 1;
    for (int k = 0; k < 3; k++) finite &= getf(p + at + 4 * k, &r.pos[k]);
    finite &= getf(p + at + 12, &r.yaw);
    r.nslots = p[at + 16], at += 17;
    if (!finite || r.nslots > COOP_POS_SLOTS) return 0;
    for (int i = 0; i < r.nslots; i++) {
        if (at >= n || p[at] > COOP_POS_TEXT || at + 1 + p[at] + 4 > n || !printable(p + at + 1, p[at]) ||
            !getf(p + at + 1 + p[at], &r.slot[i].frame))
            return 0;
        memcpy(r.slot[i].node, p + at + 1, p[at]);
        at += 1u + p[at] + 4;
    }
    if (at != n) return 0;
    *out = r;
    return 1;
}

int coop_pos_fresh(CoopPosSeen *s, uint64_t session, uint32_t pseq, uint64_t now_ms) {
    int again = !s->any || s->session != session || now_ms - s->at_ms >= COOP_POS_QUIET_MS;
    if (!again && pseq <= s->last) return 0;
    s->any = 1, s->session = session, s->last = pseq, s->at_ms = now_ms;
    return again ? 2 : 1;
}

uint32_t coop_offer_shown(const CoopOffer *o, int (*accepted)(const char *key, void *user), void *user) {
    uint32_t shown = 0;
    for (int i = 0; i < o->n && i < COOP_MAX_MEMBERS; i++) {
        const CoopOfferMember *m = &o->member[i];
        if (m->id >= 1 && m->id <= COOP_MAX_MEMBERS && m->heard && !m->removed && !m->accepted_as[0] && m->key[0] &&
            accepted(m->key, user))
            shown |= 1u << m->id;
    }
    return shown;
}
