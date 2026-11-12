// --probe-coop: the co-op layer without a window, a Vulkan device or game files.
//
// 1. The platform's UDP sockets and addresses on 127.0.0.1.
// 2. The wire format (coop_proto.c): HMAC against known values, XChaCha20-Poly1305 against a known answer, sealing and
//    opening (a changed byte, another sender or receiver, another key: nothing opens), the byte vectors of
//    tests/coop_vectors.txt (the file named by NX2NAVI_COOP_VECTORS; the Python of tools/coop_relay.py is checked against
//    the same file), random bytes into every parser, and the puppets' POS counter per sender and session.
// 3. Two sessions (coop.c) in this process, Alice and Bob, through a relay: Alice makes a room of a secret, Bob joins it,
//    they exchange messages both ways, are hit with garbage and forged datagrams from the relay's address (a changed tag,
//    a message the relay sealed itself, a message of Bob's recorded before he joined again, answers with a wrong nonce
//    while a player connects), lose the relay for good (a restart) and find each other again, the maker removes a
//    member, and they leave. The relay is the real tools/coop_relay.py when NX2NAVI_COOP_RELAY=host:port names one (with
//    NX2NAVI_COOP_RESTART=stdin the probe prints "coop-probe: restart the relay now" and waits for a line on its input,
//    which tests/coop_relay_test.py answers after restarting it), else a small relay of this file on a thread, which can
//    also do what a hostile relay would. NX2NAVI_COOP_DROP=30 makes both sessions throw away that percentage of the
//    datagrams they receive.
// 4. Progress sync's merge engine (coop_state.c) between fake games on a fake network: coop_sync_probe.c.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "coop.h"
#include "coop_offer.h"
#include "coop_state.h"
#include "host.h"
#include "platform/platform.h"
#include "third_party/monocypher/monocypher.h"

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; fprintf(stderr, "FAIL coop: "); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)
// A member learns of a join or a leave from a roster datagram that may be lost; a relay sends it again within a second and every 5 s,
// so this wait holds eleven of them: at 30 % loss they are all lost once in half a million waits.
#define ROSTER_WAIT_MS 40000
// With no loss (NX2NAVI_COOP_DROP unset) a roster step has no lost datagram to wait for, so it is bounded as the code
// promises: a spoofed or listed-elsewhere session settles within 4 s (a rejoin once per backoff, the rosters after it;
// it took under 2.5 s, the probe's earlier fixed wait), and after a roster forged a little ahead the relay's are taken
// again within the silence time (1.5 s here) and 4 s (docs/coop.md, "the client and relay datagrams"), plus up to 1 s
// for the mock relay's next roster and 1 s for a loaded machine. Lost datagrams make the waits long at 30 % loss.
#define SETTLE_NO_LOSS_MS 4000
#define STALE_NO_LOSS_MS (1500 + 4000 + 1000 + 1000)
#define FAIL_RET(...) do { CHECK(0, __VA_ARGS__); return; } while (0)
#define WAIT_FOR(cond, ms) ({ uint64_t t_ = plat_now_ms() + (ms); int ok_; while (!(ok_ = (cond)) && plat_now_ms() < t_) plat_sleep_ms(10); ok_; })

static const char ROOM_SECRET[] = "H7K3WMBCDFGH234567";   // the probe's room (tests/coop_vectors.txt uses it too)

// ---- hex and a random source

static int hex_digit(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

// "-" is empty. The bytes into out (cap), their count, -1 when text is not hex or does not fit.
static int unhex(const char *text, uint8_t *out, size_t cap) {
    if (!text) return -1;
    if (!strcmp(text, "-")) return 0;
    size_t n = strlen(text);
    if (n % 2 || n / 2 > cap) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int a = hex_digit(text[2 * i]), b = hex_digit(text[2 * i + 1]);
        if (a < 0 || b < 0) return -1;
        out[i] = (uint8_t)(a << 4 | b);
    }
    return (int)(n / 2);
}

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;
static uint32_t rnd32(void) {
    rng_state ^= rng_state >> 12, rng_state ^= rng_state << 25, rng_state ^= rng_state >> 27;
    return (uint32_t)((rng_state * 0x2545F4914F6CDD1Dull) >> 32);
}

// ---- 1. sockets

static void probe_sockets(void) {
    PlatAddr a;
    uint8_t ip[16] = {0}, back[16];
    uint32_t scope = 0;
    char text[96];
    ip[10] = ip[11] = 0xff, ip[12] = 10, ip[13] = 1, ip[14] = 2, ip[15] = 3;
    plat_addr_set(&a, ip, 24680, 0);
    CHECK(plat_addr_port(&a) == 24680, "address port");
    plat_addr_get(&a, back, &scope);
    CHECK(!memcmp(ip, back, 16) && !scope, "address round trip");
    CHECK(plat_addr_str(&a, text, sizeof text) && !strcmp(text, "10.1.2.3:24680"), "IPv4 address text: %s", text);
    PlatAddr b = a;
    CHECK(plat_addr_eq(&a, &b), "equal addresses");
    plat_addr_set(&b, ip, 24681, 0);
    CHECK(!plat_addr_eq(&a, &b), "ports differ");
    uint8_t v6[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1};
    plat_addr_set(&a, v6, 5, 0);
    CHECK(plat_addr_str(&a, text, sizeof text) && !strcmp(text, "[2001:db8::1]:5"), "IPv6 address text: %s", text);
    uint8_t lo[16] = {0};
    lo[15] = 1;
    plat_addr_set(&a, lo, 80, 3);
    CHECK(plat_addr_str(&a, text, sizeof text) && !strcmp(text, "[::1%3]:80"), "loopback with a scope: %s", text);
    CHECK(!plat_addr_str(&a, text, 5), "an address that does not fit is refused");
    uint8_t zero[16] = {0};
    plat_addr_set(&a, zero, 0, 0);
    CHECK(plat_addr_str(&a, text, sizeof text) && !strcmp(text, "[::]:0"), "the unspecified address: %s", text);

    PlatAddr loop;
    CHECK(plat_net_resolve("127.0.0.1", 4000, &loop), "resolve 127.0.0.1");
    CHECK(plat_addr_str(&loop, text, sizeof text) && !strcmp(text, "127.0.0.1:4000"), "resolved text: %s", text);
    CHECK(!plat_net_resolve("", 1, &loop) && !plat_net_resolve("no such host.invalid", 1, &loop), "a name that does not resolve fails");

    PlatUdp *u1 = plat_udp_open(0, 1), *u2 = plat_udp_open(0, 0);
    CHECK(u1 && u2, "open two sockets");
    if (!u1 || !u2) return;
    CHECK(plat_udp_port(u1) && plat_udp_port(u2) && plat_udp_port(u1) != plat_udp_port(u2), "bound to free ports");
    uint8_t buf[2048], msg[300];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)i;
    PlatAddr to1, from;
    plat_net_resolve("127.0.0.1", plat_udp_port(u1), &to1);
    uint64_t t0 = plat_now_ms();
    CHECK(plat_udp_recv(u1, buf, sizeof buf, &from, 50) == 0, "nothing to receive is a timeout");
    CHECK(plat_now_ms() - t0 >= 40, "the timeout waited");
    CHECK(plat_udp_send(u2, &to1, msg, sizeof msg), "send IPv4 to a dual-stack socket");
    int n = plat_udp_recv(u1, buf, sizeof buf, &from, 1000);
    CHECK(n == (int)sizeof msg && !memcmp(buf, msg, sizeof msg), "the datagram arrives whole (%d bytes)", n);
    PlatAddr expect;
    plat_net_resolve("127.0.0.1", plat_udp_port(u2), &expect);
    CHECK(plat_addr_eq(&from, &expect), "the sender is reported as 127.0.0.1:port whatever the socket's family");
    CHECK(plat_udp_send(u1, &from, msg, 10), "send back from the dual-stack socket to an IPv4-only one");
    n = plat_udp_recv(u2, buf, sizeof buf, &from, 1000);
    CHECK(n == 10, "reply arrives (%d)", n);
    plat_udp_send(u2, &to1, msg, sizeof msg);
    n = plat_udp_recv(u1, buf, 100, NULL, 1000);
    CHECK(n == 100, "a datagram longer than the buffer is cut to it (%d)", n);
    static uint8_t big[70000];
    CHECK(!plat_udp_send(u2, &to1, big, sizeof big), "a datagram that is too long can never be sent");
    PlatAddr far6;
    plat_addr_set(&far6, v6, 9, 0);
    CHECK(!plat_udp_send(u2, &far6, msg, 10), "an IPv4-only socket cannot reach an IPv6 address");
    plat_udp_close(u1);
    plat_udp_close(u2);
    plat_udp_close(NULL);
}

// ---- 2. the wire format

static void probe_crypto(void) {
    // RFC 4231 test cases 1 and 2
    static const uint8_t key1[20] = {0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b, 0x0b};
    uint8_t mac[32], expect[64];
    coop_hmac_sha256(key1, sizeof key1, "Hi There", 8, NULL, 0, mac);
    CHECK(unhex("b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7", expect, 32) == 32 && !memcmp(mac, expect, 32),
          "HMAC-SHA256, RFC 4231 case 1");
    coop_hmac_sha256("Jefe", 4, "what do ya want ", 16, "for nothing?", 12, mac);   // the message in two pieces
    CHECK(unhex("5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", expect, 32) == 32 && !memcmp(mac, expect, 32),
          "HMAC-SHA256, RFC 4231 case 2 (two pieces)");
    // XChaCha20-Poly1305 (the library's lock) against a known answer from the library's own test vectors
    uint8_t key[32], nonce[24], out[17], ct[1];
    unhex("8019e8999c815c5723dbfbde05e6c71f118afc0dedb5b9f8dea398b2d764bca6", key, 32);
    unhex("8dfc023a9821939d389e38a072cf1b413bb1517c3fe83abe", nonce, 24);
    unhex("374190382975907a68e8a341faa0772aa0", out, 17);
    uint8_t plain = 0x86, tag[16];
    crypto_aead_lock(ct, tag, key, nonce, NULL, 0, &plain, 1);
    CHECK(!memcmp(tag, out, 16) && ct[0] == out[16], "XChaCha20-Poly1305 known answer");

    // seal and open: the largest message round trips; a changed byte, another sender, receiver or key opens nothing
    uint8_t lk[COOP_KEY_LEN], mk[COOP_MSG_KEY_LEN], other[COOP_MSG_KEY_LEN];
    coop_room_keys(ROOM_SECRET, lk, mk);
    coop_room_keys("23BCDF23BCDF23BCDF", NULL, other);
    static uint8_t msg[COOP_MAX_MESSAGE], box[COOP_MAX_PAYLOAD], got[COOP_MAX_PAYLOAD];
    for (size_t i = 0; i < sizeof msg; i++) msg[i] = (uint8_t)(i * 13);
    size_t n = coop_seal(box, sizeof box, mk, 3, COOP_DST_ALL, 0x1234, 77, 0x42, msg, sizeof msg), ng = 0;
    uint8_t kind = 0;
    CHECK(n == COOP_MAX_PAYLOAD, "the largest message fills a datagram (%zu)", n);
    CHECK(!coop_seal(box, sizeof box, mk, 3, COOP_DST_ALL, 1, 1, 0x42, msg, sizeof msg + 1), "a larger one is refused");
    CHECK(coop_open(box, n, mk, 3, COOP_DST_ALL, &kind, got, sizeof got, &ng) && kind == 0x42 && ng == sizeof msg && !memcmp(got, msg, ng),
          "sealed and opened");
    int opened = 0;
    for (size_t i = 0; i < n; i += 37) {
        box[i] ^= 0x10;
        opened += coop_open(box, n, mk, 3, COOP_DST_ALL, &kind, got, sizeof got, &ng);
        box[i] ^= 0x10;
    }
    CHECK(!opened, "a changed byte anywhere opens nothing (%d did)", opened);
    CHECK(!coop_open(box, n, mk, 4, COOP_DST_ALL, &kind, got, sizeof got, &ng) && !coop_open(box, n, mk, 3, 2, &kind, got, sizeof got, &ng) &&
              !coop_open(box, n, other, 3, COOP_DST_ALL, &kind, got, sizeof got, &ng),
          "another sender, receiver or room key opens nothing");
    // secrets: from random bytes, every one valid, every symbol about as often
    unsigned counts[27] = {0}, made = 0;
    for (int i = 0; i < 2000; i++) {
        uint8_t r[32];
        char s[COOP_SECRET_LEN + 1], again[COOP_SECRET_LEN + 1];
        for (int k = 0; k < 32; k++) r[k] = (uint8_t)rnd32();
        if (!coop_secret_from_random(r, s)) continue;
        made++;
        CHECK(coop_secret_normalize(s, again) && !strcmp(s, again), "a new secret is one");
        for (int k = 0; k < COOP_SECRET_LEN; k++) counts[strchr("23456789BCDFGHJKMNPRSTVWXYZ", s[k]) - "23456789BCDFGHJKMNPRSTVWXYZ"]++;
    }
    unsigned lo = ~0u, hi = 0;
    for (int k = 0; k < 27; k++) lo = counts[k] < lo ? counts[k] : lo, hi = counts[k] > hi ? counts[k] : hi;
    CHECK(made > 1990 && lo > hi * 3 / 4, "random secrets: %u made, symbols %u..%u times", made, lo, hi);
    char text[COOP_SECRET_TEXT + 1], shortened[8];
    coop_secret_format(ROOM_SECRET, text);
    coop_secret_short(ROOM_SECRET, shortened);
    char again_short[8], other_short[8];
    coop_secret_short(ROOM_SECRET, again_short);
    coop_secret_short("27K3WMBCDFGH234567", other_short);
    int label_ok = strlen(shortened) == 6 && !strcmp(shortened + 3, "\xe2\x80\xa6") && !strcmp(shortened, again_short);
    for (int k = 0; k < 3; k++) label_ok &= shortened[k] && strchr("23456789BCDFGHJKMNPRSTVWXYZ", shortened[k]) != NULL;
    CHECK(!strcmp(text, "H7K3WM-BCDFGH-234567") && label_ok && strncmp(shortened, "H7K", 3) && strcmp(shortened, other_short),
          "a secret is shown %s, logged %s (a label hashed from it, not its first characters)", text, shortened);
}

static void probe_server_parse(void) {
    static const struct { const char *text, *host; unsigned port; int ok; } cases[] = {
        {"relay.example", "relay.example", 24680, 1}, {"relay.example:5000", "relay.example", 5000, 1},
        {"  relay.example  ", "relay.example", 24680, 1}, {"127.0.0.1:65535", "127.0.0.1", 65535, 1},
        {"[::1]", "::1", 24680, 1}, {"[::1]:99", "::1", 99, 1}, {"2001:db8::1", "2001:db8::1", 24680, 1},
        {"", "", 0, 0}, {"   ", "", 0, 0}, {"host:", "", 0, 0}, {"host:0", "", 0, 0}, {"host:65536", "", 0, 0},
        {"host:12x", "", 0, 0}, {"[::1", "", 0, 0}, {"[::1]x", "", 0, 0}, {":80", "", 0, 0}, {"[]:80", "", 0, 0},
    };
    for (size_t i = 0; i < sizeof cases / sizeof *cases; i++) {
        char host[128] = "";
        uint16_t port = 0;
        int ok = coop_server_parse(cases[i].text, host, sizeof host, &port);
        CHECK(ok == cases[i].ok && (!ok || (!strcmp(host, cases[i].host) && port == cases[i].port)), "server \"%s\" parses as %d %s:%u",
              cases[i].text, ok, host, port);
    }
    char small[4];
    uint16_t port;
    CHECK(!coop_server_parse("relay.example", small, sizeof small, &port), "a host that does not fit is refused");
}

// ---- the byte vectors

static char *next_token(char **at) {
    char *p = *at;
    while (*p == ' ') p++;
    if (!*p) return NULL;
    char *start = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
    *at = p;
    return start;
}

// "id:flags:session:namehex" entries into r; their count, -1 when malformed.
static int entries_parse(char **at, CoopRoster *r) {
    memset(r, 0, sizeof *r);
    char *t;
    while ((t = next_token(at))) {
        unsigned id, flags;
        unsigned long long session;
        char name_hex[80];
        if (r->n >= COOP_MAX_MEMBERS || sscanf(t, "%u:%u:%llx:%79s", &id, &flags, &session, name_hex) != 4) return -1;
        uint8_t raw[40];
        int k = unhex(name_hex, raw, sizeof raw);
        if (k < 0 || k > COOP_NAME_MAX) return -1;
        r->m[r->n].id = (uint8_t)id, r->m[r->n].flags = (uint8_t)flags, r->m[r->n].session = session;
        memcpy(r->m[r->n].name, raw, (size_t)k);
        r->n++;
    }
    return r->n;
}

static int roster_equal(const CoopRoster *a, const CoopRoster *b) {
    if (a->n != b->n) return 0;
    for (int i = 0; i < a->n; i++)
        if (a->m[i].id != b->m[i].id || a->m[i].flags != b->m[i].flags || a->m[i].session != b->m[i].session || strcmp(a->m[i].name, b->m[i].name))
            return 0;
    return 1;
}

static uint64_t hex64(const char *t) { return t ? strtoull(t, NULL, 16) : 0; }

static void check_vector(const char *kind, char *rest, int line) {
    static uint8_t a[1300], b[1300], c[1300];
    char *name = next_token(&rest);
    if (!strcmp(kind, "hmac")) {
        char *ks = next_token(&rest), *ms = next_token(&rest), *macs = next_token(&rest);
        int kn = unhex(ks, a, sizeof a), mn = unhex(ms, b, sizeof b), cn = unhex(macs, c, 32);
        uint8_t mac[32];
        if (kn < 0 || mn < 0 || cn != 32) FAIL_RET("vector line %d: bad hmac line", line);
        coop_hmac_sha256(a, (size_t)kn, b, (size_t)mn, NULL, 0, mac);
        CHECK(!memcmp(mac, c, 32), "vector hmac %s (line %d)", name, line);
    } else if (!strcmp(kind, "secret")) {   // secret <canonical|-> <text, the rest of the line>
        char out[COOP_SECRET_LEN + 1] = "";
        int ok = coop_secret_normalize(rest, out);
        CHECK(!strcmp(name, "-") ? !ok : ok && !strcmp(out, name), "vector secret \"%s\" is %s (line %d)", rest, name, line);
    } else if (!strcmp(kind, "keys")) {
        uint8_t lk[COOP_KEY_LEN], mk[COOP_MSG_KEY_LEN];
        coop_room_keys(name, lk, mk);
        CHECK(unhex(next_token(&rest), a, 16) == 16 && !memcmp(lk, a, 16) && unhex(next_token(&rest), b, 32) == 32 && !memcmp(mk, b, 32),
              "vector keys %s (line %d)", name, line);
    } else if (!strcmp(kind, "dgram") || !strcmp(kind, "badtag")) {
        int bad = !strcmp(kind, "badtag");
        char *secret = next_token(&rest), *tok[7] = {0}, *dg;
        if (!bad)
            for (int i = 0; i < 7; i++) tok[i] = next_token(&rest);
        dg = next_token(&rest);
        int n = unhex(dg, a, sizeof a);
        CoopHdr h;
        const uint8_t *payload;
        if (n < 0 || !coop_dgram_parse(a, (size_t)n, &h, &payload)) FAIL_RET("vector %s %s does not parse (line %d)", kind, name, line);
        uint8_t key[COOP_KEY_LEN];
        int keyed = strcmp(secret, "-") != 0;
        if (keyed) coop_room_keys(secret, key, NULL);
        if (bad) {
            CHECK(!coop_dgram_verify(a, (size_t)n, key), "vector badtag %s verifies (line %d)", name, line);
            return;
        }
        int pn = unhex(tok[6], b, sizeof b);
        CHECK(h.ver == atoi(tok[0]) && h.type == atoi(tok[1]) && h.room == (uint32_t)strtoull(tok[2], NULL, 10) && h.src == atoi(tok[3]) &&
                  h.dst == atoi(tok[4]) && h.seq == (uint32_t)strtoull(tok[5], NULL, 10) && pn == (int)h.len && (!pn || !memcmp(payload, b, (size_t)pn)),
              "vector dgram %s: header fields (line %d)", name, line);
        CHECK(keyed ? coop_dgram_verify(a, (size_t)n, key) : coop_dgram_tag_zero(a), "vector dgram %s: tag (line %d)", name, line);
        size_t k = coop_dgram_pack(c, sizeof c, &h, b, (size_t)(pn < 0 ? 0 : pn), keyed ? key : NULL);
        CHECK(k == (size_t)n && !memcmp(c, a, k), "vector dgram %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "connect")) {
        // connect <name> <intent> <flags> <locator> <cid> <session> <nonce> <namehex|-> <cookie> <token> <payload>
        char *t[10];
        for (int i = 0; i < 10; i++) t[i] = next_token(&rest);
        uint8_t loc[16], cookie[16], token[16];
        int pn = unhex(t[9], a, sizeof a), nn = unhex(t[6], b, 32);
        CoopConnect cn;
        if (pn < 0 || nn < 0 || unhex(t[2], loc, 16) != 16 || unhex(t[7], cookie, 16) != 16 || unhex(t[8], token, 16) != 16 ||
            !coop_connect_parse(a, (size_t)pn, &cn))
            FAIL_RET("vector connect %s does not parse (line %d)", name, line);
        CHECK(cn.intent == atoi(t[0]) && cn.flags == atoi(t[1]) && !memcmp(cn.locator, loc, 16) && cn.client_id == hex64(t[3]) &&
                  cn.session == hex64(t[4]) && cn.nonce == hex64(t[5]) && strlen(cn.name) == (size_t)nn && !memcmp(cn.name, b, (size_t)nn) &&
                  !memcmp(cn.cookie, cookie, 16) && !memcmp(cn.token, token, 16),
              "vector connect %s: fields (line %d)", name, line);
        size_t k = coop_connect_pack(c, sizeof c, &cn);
        CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector connect %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "challenge")) {
        char *payload = next_token(&rest), *nonce = next_token(&rest), *cookie = next_token(&rest);
        int pn = unhex(payload, a, sizeof a);
        uint64_t got;
        uint8_t ck[16], want[16];
        CHECK(pn > 0 && unhex(cookie, want, 16) == 16 && coop_challenge_parse(a, (size_t)pn, &got, ck) && got == hex64(nonce) && !memcmp(ck, want, 16) &&
                  coop_challenge_pack(c, sizeof c, got, ck) == (size_t)pn && !memcmp(c, a, (size_t)pn),
              "vector challenge %s (line %d)", name, line);
    } else if (!strcmp(kind, "roster") || !strcmp(kind, "joined")) {
        int joined = !strcmp(kind, "joined");
        char *payload = next_token(&rest);
        int pn = unhex(payload, a, sizeof a);
        char *member = joined ? next_token(&rest) : NULL, *nonce = joined ? next_token(&rest) : NULL, *token = joined ? next_token(&rest) : NULL;
        CoopRoster want;
        if (pn < 0 || entries_parse(&rest, &want) < 0 || (joined && !token)) FAIL_RET("vector %s %s is malformed (line %d)", kind, name, line);
        if (joined) {
            CoopJoined j;
            uint8_t tk[16];
            CHECK(unhex(token, tk, 16) == 16 && coop_joined_parse(a, (size_t)pn, &j) && j.member == atoi(member) && j.nonce == hex64(nonce) &&
                      !memcmp(j.token, tk, 16) && roster_equal(&j.roster, &want),
                  "vector joined %s: fields (line %d)", name, line);
            size_t k = coop_joined_pack(c, sizeof c, &j);
            CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector joined %s: packs to the same bytes (line %d)", name, line);
        } else {
            CoopRoster r;
            size_t used = 0;
            CHECK(coop_roster_parse(a, (size_t)pn, &r, &used) && used == (size_t)pn && roster_equal(&r, &want), "vector roster %s: fields (line %d)", name, line);
            size_t k = coop_roster_pack(c, sizeof c, &r);
            CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector roster %s: packs to the same bytes (line %d)", name, line);
        }
    } else if (!strcmp(kind, "error")) {
        char *payload = next_token(&rest), *reason = next_token(&rest), *nonce = next_token(&rest), *text = next_token(&rest);
        int pn = unhex(payload, a, sizeof a), tn = unhex(text, b, 64);
        CoopError e;
        if (pn < 0 || tn < 0 || !coop_error_parse(a, (size_t)pn, &e)) FAIL_RET("vector error %s does not parse (line %d)", name, line);
        CHECK(e.reason == atoi(reason) && e.nonce == hex64(nonce) && strlen(e.text) == (size_t)tn && !memcmp(e.text, b, (size_t)tn),
              "vector error %s: fields (line %d)", name, line);
        size_t k = coop_error_pack(c, sizeof c, &e);
        CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector error %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "kick")) {
        char *payload = next_token(&rest), *member = next_token(&rest), *session = next_token(&rest);
        int pn = unhex(payload, a, sizeof a);
        uint8_t m;
        uint64_t s;
        CHECK(pn > 0 && coop_kick_parse(a, (size_t)pn, &m, &s) && m == atoi(member) && s == hex64(session) && coop_kick_pack(c, sizeof c, m, s) == (size_t)pn &&
                  !memcmp(c, a, (size_t)pn),
              "vector kick %s (line %d)", name, line);
    } else if (!strcmp(kind, "sealed")) {
        // sealed <name> <secret> <src> <dst> <session> <counter> <kind> <message> <payload>
        char *t[8];
        for (int i = 0; i < 8; i++) t[i] = next_token(&rest);
        uint8_t mk[COOP_MSG_KEY_LEN], got_kind = 0;
        coop_room_keys(t[0], NULL, mk);
        int mn = unhex(t[6], b, sizeof b), pn = unhex(t[7], a, sizeof a);
        size_t ng = 0;
        uint8_t src = (uint8_t)atoi(t[1]), dst = (uint8_t)atoi(t[2]), kd = (uint8_t)strtoul(t[5], NULL, 0);
        if (mn < 0 || pn < 0) FAIL_RET("vector sealed %s is malformed (line %d)", name, line);
        CHECK(coop_open(a, (size_t)pn, mk, src, dst, &got_kind, c, sizeof c, &ng) && got_kind == kd && ng == (size_t)mn && !memcmp(c, b, ng),
              "vector sealed %s opens (line %d)", name, line);
        size_t k = coop_seal(c, sizeof c, mk, src, dst, hex64(t[3]), (uint32_t)strtoul(t[4], NULL, 0), kd, b, (size_t)mn);
        CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector sealed %s: seals to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "pos")) {
        // pos <name> <payload> <pseq> <t_ms> <flags> <area hex|-> <x> <y> <z> <yaw> <node hex:frame>...
        char *payload = next_token(&rest), *pseq = next_token(&rest), *t_ms = next_token(&rest), *flags = next_token(&rest);
        char *area = next_token(&rest), *f[4];
        for (int i = 0; i < 4; i++) f[i] = next_token(&rest);
        int pn = unhex(payload, a, sizeof a), an = area ? unhex(area, b, COOP_POS_TEXT) : -1;
        CoopPos p;
        if (pn < 0 || an < 0 || !f[3]) FAIL_RET("vector pos %s does not parse (line %d)", name, line);
        CHECK(coop_pos_parse(a, (size_t)pn, &p), "vector pos %s: parses (line %d)", name, line);
        int ok = p.pseq == (uint32_t)strtoul(pseq, NULL, 10) && p.t_ms == (uint32_t)strtoul(t_ms, NULL, 10) && p.flags == atoi(flags) &&
                 strlen(p.area) == (size_t)an && !memcmp(p.area, b, (size_t)an) && p.pos[0] == strtof(f[0], NULL) &&
                 p.pos[1] == strtof(f[1], NULL) && p.pos[2] == strtof(f[2], NULL) && p.yaw == strtof(f[3], NULL);
        int ns = 0;
        for (char *s; (s = next_token(&rest)); ns++) {
            char *colon = strchr(s, ':');
            if (!colon || ns >= COOP_POS_SLOTS) FAIL_RET("vector pos %s: bad slot %s (line %d)", name, s, line);
            *colon = 0;
            int nn = unhex(s, c, COOP_POS_TEXT);
            ok &= nn >= 0 && strlen(p.slot[ns].node) == (size_t)nn && !memcmp(p.slot[ns].node, c, (size_t)nn) &&
                  p.slot[ns].frame == strtof(colon + 1, NULL);
        }
        CHECK(ok && p.nslots == ns, "vector pos %s: fields (line %d)", name, line);
        size_t k = coop_pos_pack(c, sizeof c, &p);
        CHECK(k == (size_t)pn && !memcmp(c, a, k), "vector pos %s: packs to the same bytes (line %d)", name, line);
    } else if (!strcmp(kind, "reject")) {
        char *what = name, *nm = next_token(&rest), *bytes = next_token(&rest);
        int n = unhex(bytes, a, sizeof a);
        if (n < 0) FAIL_RET("vector reject %s: bad hex (line %d)", nm, line);
        CoopHdr h;
        const uint8_t *p;
        CoopConnect cn;
        CoopRoster r;
        CoopJoined j;
        CoopError e;
        uint64_t u64;
        uint32_t u32;
        uint8_t u8, ck[16];
        int parsed;
        if (!strcmp(what, "dgram")) parsed = coop_dgram_parse(a, (size_t)n, &h, &p);
        else if (!strcmp(what, "connect")) parsed = coop_connect_parse(a, (size_t)n, &cn);
        else if (!strcmp(what, "challenge")) parsed = coop_challenge_parse(a, (size_t)n, &u64, ck);
        else if (!strcmp(what, "roster")) parsed = coop_roster_parse(a, (size_t)n, &r, NULL);
        else if (!strcmp(what, "joined")) parsed = coop_joined_parse(a, (size_t)n, &j);
        else if (!strcmp(what, "error")) parsed = coop_error_parse(a, (size_t)n, &e);
        else if (!strcmp(what, "kick")) parsed = coop_kick_parse(a, (size_t)n, &u8, &u64);
        else if (!strcmp(what, "pos")) parsed = coop_pos_parse(a, (size_t)n, &(CoopPos){0});
        else parsed = coop_envelope_peek(a, (size_t)n, &u64, &u32);
        CHECK(!parsed, "vector reject %s %s parses (line %d)", what, nm, line);
    } else {
        CHECK(0, "vector line %d: unknown kind %s", line, kind);
    }
}

static int probe_vectors(void) {
    const char *path = cfg_get("NX2NAVI_COOP_VECTORS");
    if (!path || !*path) {
        printf("coop-probe: no NX2NAVI_COOP_VECTORS: the byte vectors are not checked\n");
        return 0;
    }
    uint64_t size = 0;
    char *text = (char *)plat_file_read(path, &size);
    if (!text) {
        CHECK(0, "cannot read the vectors file %s", path);
        return 0;
    }
    int lines = 0, line = 0;
    for (char *p = text, *next; *p; p = next, line++) {
        next = strchr(p, '\n');
        if (next) *next++ = 0;
        else next = p + strlen(p);
        if (!*p || *p == '#') continue;
        char *rest = p;
        char *kind = next_token(&rest);
        if (!kind) continue;
        check_vector(kind, rest, line + 1);
        lines++;
    }
    free(text);
    CHECK(lines > 60, "the vectors file has %d lines of vectors", lines);
    printf("coop-probe: %d byte vectors checked\n", lines);
    return lines;
}

static void probe_fuzz(void) {
    static uint8_t buf[1400], out[1400], opened[1400];
    uint8_t key[COOP_KEY_LEN], mk[COOP_MSG_KEY_LEN];
    coop_room_keys(ROOM_SECRET, key, mk);
    uint8_t seeds[8][400];
    size_t seed_len[8];
    CoopRoster r = {.n = 2};
    r.m[0] = (CoopMemberInfo){1, COOP_FLAG_MAKER, 5, "Alice"}, r.m[1] = (CoopMemberInfo){2, 1, 6, "Bob"};
    uint8_t payload[400];
    CoopHdr h = {.ver = COOP_PROTO, .room = 7, .src = 1, .dst = 2, .seq = 5};
    size_t n = coop_roster_pack(payload, sizeof payload, &r);
    h.type = COOP_T_ROSTER, seed_len[0] = coop_dgram_pack(seeds[0], sizeof seeds[0], &h, payload, n, key);
    CoopJoined j = {.member = 2, .nonce = 9, .roster = r};
    n = coop_joined_pack(payload, sizeof payload, &j);
    h.type = COOP_T_JOINED, seed_len[1] = coop_dgram_pack(seeds[1], sizeof seeds[1], &h, payload, n, key);
    CoopConnect c = {.intent = 2, .client_id = 99, .session = 3, .nonce = 4};
    memcpy(c.locator, key, sizeof key);
    strcpy(c.name, "Bob");
    n = coop_connect_pack(payload, sizeof payload, &c);
    h.type = COOP_T_CONNECT, seed_len[2] = coop_dgram_pack(seeds[2], sizeof seeds[2], &h, payload, n, key);
    CoopError e = {.reason = 1, .nonce = 4};
    strcpy(e.text, "no such room");
    n = coop_error_pack(payload, sizeof payload, &e);
    h.type = COOP_T_ERROR, seed_len[3] = coop_dgram_pack(seeds[3], sizeof seeds[3], &h, payload, n, key);
    n = coop_seal(payload, sizeof payload, mk, 1, 2, 5, 1, COOP_K_TEST, "hello", 5);
    h.type = COOP_T_DATA, seed_len[4] = coop_dgram_pack(seeds[4], sizeof seeds[4], &h, payload, n, key);
    h.type = COOP_T_PONG, seed_len[5] = coop_dgram_pack(seeds[5], sizeof seeds[5], &h, "abcd", 4, key);
    n = coop_kick_pack(payload, sizeof payload, 2, 6);
    h.type = COOP_T_KICK, seed_len[6] = coop_dgram_pack(seeds[6], sizeof seeds[6], &h, payload, n, key);
    CoopPos pos = {.pseq = 7, .t_ms = 1000, .flags = COOP_POS_IN_WORLD, .area = "HyruleField", .pos = {1, 2, 3}, .nslots = 2,
                   .slot = {{"Walk", 12}, {"Lower_Walk", 3.5f}}};
    n = coop_pos_pack(payload, sizeof payload, &pos);   // a POS as the payload, so that changed copies reach its parser
    h.type = COOP_T_DATA, seed_len[7] = coop_dgram_pack(seeds[7], sizeof seeds[7], &h, payload, n, key);
    long parsed = 0, tag_ok = 0, open_ok = 0;
    for (int it = 0; it < 400000; it++) {
        size_t len;
        int mode = it % 4;
        if (mode == 0) {   // random bytes
            len = rnd32() % 1300;
            for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)(rnd32() % 5 ? rnd32() : 0);
        } else {           // a valid datagram with bytes changed, or cut short
            int s = (int)(rnd32() % 8);
            len = seed_len[s];
            memcpy(buf, seeds[s], len);
            for (unsigned k = rnd32() % 4; k && len; k--) buf[rnd32() % len] = (uint8_t)rnd32();
            if (mode == 3) len = rnd32() % (len + 1);
        }
        CoopHdr hh;
        const uint8_t *p;
        if (!coop_dgram_parse(buf, len, &hh, &p)) {
            p = buf, hh.len = (uint16_t)len;   // the payload parsers on whatever bytes there are
        } else {
            parsed++;
            tag_ok += coop_dgram_verify(buf, len, key);
        }
        CoopConnect cn;
        CoopRoster rr;
        CoopJoined jj;
        CoopError ee;
        uint32_t v;
        uint64_t s64;
        uint8_t kind, ck[16];
        size_t no;
        char nameb[COOP_NAME_MAX + 1], secretb[COOP_SECRET_LEN + 1];
        if (coop_connect_parse(p, hh.len, &cn)) coop_connect_pack(out, sizeof out, &cn);
        if (coop_roster_parse(p, hh.len, &rr, NULL)) coop_roster_pack(out, sizeof out, &rr);
        if (coop_joined_parse(p, hh.len, &jj)) coop_joined_pack(out, sizeof out, &jj);
        if (coop_error_parse(p, hh.len, &ee)) coop_error_pack(out, sizeof out, &ee);
        if (coop_challenge_parse(p, hh.len, &s64, ck)) coop_challenge_pack(out, sizeof out, s64, ck);
        coop_kick_parse(p, hh.len, &kind, &s64);
        coop_u32_parse(p, hh.len, &v);
        coop_envelope_peek(p, hh.len, &s64, &v);
        CoopPos pp;   // a POS that parses packs to the same bytes: nothing in it is dropped or changed on the way in
        if (coop_pos_parse(p, hh.len, &pp))
            CHECK(coop_pos_pack(out, sizeof out, &pp) == hh.len && !memcmp(out, p, hh.len), "a POS that parses packs to other bytes");
        open_ok += coop_open(p, hh.len, mk, hh.src, hh.dst, &kind, opened, sizeof opened, &no);
        coop_name_clean(nameb, sizeof nameb, p, hh.len);
        char text[40];
        size_t tl = len < sizeof text - 1 ? len : sizeof text - 1;
        memcpy(text, buf, tl);
        text[tl] = 0;
        coop_secret_normalize(text, secretb);
    }
    CHECK(parsed > 1000, "the fuzz reached the payload parsers (%ld datagrams parsed)", parsed);
    CHECK(tag_ok < parsed, "a changed datagram's tag no longer verifies");
    printf("coop-probe: parser fuzz: 400000 inputs, %ld parsed as datagrams, %ld sealed messages opened (unchanged copies)\n", parsed, open_ok);
}

// The POS counter (coop_pos_fresh, for whatever takes poses): the session layer can deliver a message again
// (coop.h), so a pose sent again or recorded earlier in the session must not move a puppet back, also in a session long
// enough for a 16-bit counter to wrap; the first poses of another session in the member id are taken whatever their
// counter; and a pose forged far ahead keeps the sender's own out only until nothing was taken for COOP_POS_QUIET_MS.
static void probe_pos_counter(void) {
    CoopPosSeen s;
    memset(&s, 0, sizeof s);
    const uint64_t s1 = 0x5e55000000000001ull, s2 = 0x5e55000000000002ull;
    uint64_t t = 1000;
    int took = 0;
    for (uint32_t c = 1; c <= 200; c++, t += 67) took += coop_pos_fresh(&s, s1, c, t) != 0;
    CHECK(took == 200, "POS counter: of 200 poses of one session in order, %d are taken", took);
    int again = coop_pos_fresh(&s, s1, 200, t), older = coop_pos_fresh(&s, s1, 150, t + 10);
    CHECK(!again && !older, "POS counter: a pose sent again (%d) or an older one (%d) of the same session is taken", again, older);
    // another session in member id 1 (another player given it, or the member back in a new session) starts lower
    t += 67;
    int first = coop_pos_fresh(&s, s2, 1, t), second = coop_pos_fresh(&s, s2, 2, t + 67);
    CHECK(first && second, "POS counter: the first poses of another session in the same member id are not taken (%d %d)", first,
          second);
    // 40000 poses are 44 minutes at 15 a second: one recorded 35000 poses earlier stays older
    t += 134;
    int late = coop_pos_fresh(&s, s2, 40000, t), recorded = coop_pos_fresh(&s, s2, 5000, t + 67);
    CHECK(late && !recorded, "POS counter: after pose 40000 of a session, its pose 5000 sent again is taken (%d %d)", late, recorded);
    // a pose forged 30000 ahead (someone with the room's secret), then the sender's own every 67 ms
    t += 134;
    const uint64_t forged_at = t;
    int forged = coop_pos_fresh(&s, s2, 70000, t), held = 0, back = 0;
    uint64_t back_at = 0;
    uint32_t c = 40001;
    for (; c < 40001 + 15 * 6 && !back; c++) {
        t += 67;
        int f = coop_pos_fresh(&s, s2, c, t);
        if (f) back = f, back_at = t;
        else held++;
    }
    const long long waited = back ? (long long)(back_at - forged_at) : -1;   // -1: never taken again within 6 s
    CHECK(forged && back == 2 && waited >= COOP_POS_QUIET_MS && waited < COOP_POS_QUIET_MS + 67,
          "POS counter: after a pose forged 30000 ahead the sender's own are taken again %lld ms later (%d held out, start again "
          "%d), not %d to %d ms later", waited, held, back, COOP_POS_QUIET_MS, COOP_POS_QUIET_MS + 66);
    int next = coop_pos_fresh(&s, s2, c, t + 67), forged_again = coop_pos_fresh(&s, s2, 70000 - 1, t + 100) == 1;
    CHECK(next == 1, "POS counter: the sender's next pose after that is not taken (%d)", next);
    printf("coop-probe: POS counter: a pose sent again or older is dropped, also 35000 back; another session starts again; "
           "a pose forged 30000 ahead held the sender's own out %lld ms (%d poses)%s\n", waited, held,
           forged_again ? "; a forger that keeps sending holds them out again, while it lasts" : "");
}

// Which members a game would show (coop_offer_shown, which the game publishes): only a member identified
// by a key this player accepted under the name it signs now. Not one that never said who it is, nor one whose session
// two keys dispute (both show as not heard in the offer), nor one with a key not accepted, nor one removed, nor an
// accepted key that now signs another name.
static int pos_accepted(const char *key, void *user) {
    (void)user;
    return !strcmp(key, "a11c-e000-0000-0001") || !strcmp(key, "c4r0-0000-0000-0003") || !strcmp(key, "da7e-0000-0000-0004");
}

static void probe_puppet_shown(void) {
    CoopOffer o;
    memset(&o, 0, sizeof o);
    struct { int id, heard, removed; const char *key, *accepted_as; } m[] = {
        {1, 1, 0, "a11c-e000-0000-0001", ""},       // accepted and identified: shown
        {2, 0, 0, "", ""},                          // never said who it is, or disputed between two keys
        {3, 1, 0, "b0b0-0000-0000-0002", ""},       // identified, its key not accepted
        {4, 1, 1, "c4r0-0000-0000-0003", ""},       // accepted, then removed by the maker
        {5, 1, 0, "da7e-0000-0000-0004", "Dave"},   // a key accepted as Dave, now under another name
    };
    for (size_t i = 0; i < sizeof m / sizeof *m; i++) {
        CoopOfferMember *e = &o.member[o.n++];
        e->id = m[i].id, e->heard = m[i].heard, e->removed = m[i].removed;
        snprintf(e->key, sizeof e->key, "%s", m[i].key);
        snprintf(e->accepted_as, sizeof e->accepted_as, "%s", m[i].accepted_as);
    }
    uint32_t shown = coop_offer_shown(&o, pos_accepted, NULL);
    CHECK(shown == 1u << 1, "puppets: shown for members %#x, not only for member 1, accepted and identified (2 not "
                            "identified, 3 not accepted, 4 removed, 5 accepted under another name)", shown);
    printf("coop-probe: puppets: shown only for a member accepted and identified (members %#x of 1 to 5)\n", shown);
}

// ---- 3a. a relay of this file

typedef struct {
    int used, maker;
    uint64_t cid, session;
    char name[COOP_NAME_MAX + 1];
    PlatAddr addr;
    uint32_t connect_seq;
    uint8_t token[COOP_COOKIE_LEN];
} MockMember;
typedef struct { int used; uint32_t id; uint8_t locator[COOP_KEY_LEN]; uint64_t banned[4]; MockMember m[COOP_MAX_MEMBERS + 1]; } MockRoom;

typedef struct {
    PlatUdp *sock;
    PlatThread *thread;
    uint16_t port;
    volatile int stop, restart, corrupt_next, fuzz, forge, lie, record_from, replay, steal, forge_ahead;
    volatile int mark, spoof;   // the roster names this member the maker (toggles), or lists another session for it
    volatile uint32_t forge_roster;      // a roster to member 1 this far ahead of the mock's counter, another session for member 2
    volatile uint64_t rate_cid;          // CONNECTs of this client are refused for the network's joins
    volatile uint64_t watch_cid;         // CONNECTs of this client with their proof (a cookie or token) are counted in watch_n
    volatile unsigned watch_n;
    volatile unsigned forwarded;
    MockRoom rooms[4];
    uint8_t secret[32];
    uint32_t seq;
    uint64_t next_roster;
    uint8_t recorded[COOP_MAX_DGRAM];   // a DATA datagram of member record_from, for a replay later
    size_t recorded_n;
    uint32_t counter[COOP_MAX_MEMBERS + 1];   // the envelope counter of each member's last DATA forwarded
} MockRelay;

static void mock_send(MockRelay *r, const PlatAddr *to, uint8_t type, uint32_t room, uint8_t dst, const void *payload, size_t n, const uint8_t *key) {
    uint8_t d[COOP_MAX_DGRAM];
    CoopHdr h = {.ver = COOP_PROTO, .type = type, .room = room, .dst = dst, .seq = ++r->seq};
    size_t k = coop_dgram_pack(d, sizeof d, &h, payload, n, key);
    if (k) plat_udp_send(r->sock, to, d, k);
}

static void mock_error(MockRelay *r, const PlatAddr *to, uint8_t reason, uint64_t nonce, const uint8_t *key) {
    CoopError e = {.reason = reason, .nonce = nonce};
    strcpy(e.text, "refused");
    uint8_t payload[80];
    mock_send(r, to, COOP_T_ERROR, 0, 0, payload, coop_error_pack(payload, sizeof payload, &e), key);
}

static void mock_cookie(MockRelay *r, const PlatAddr *a, uint8_t out[COOP_COOKIE_LEN]) {
    uint8_t mac[32];
    coop_hmac_sha256(r->secret, sizeof r->secret, a->b, sizeof a->b, NULL, 0, mac);
    memcpy(out, mac, COOP_COOKIE_LEN);
}

static void mock_roster_of(const MockRoom *room, CoopRoster *out) {
    memset(out, 0, sizeof *out);
    for (int i = 1; i <= COOP_MAX_MEMBERS; i++)
        if (room->m[i].used) {
            CoopMemberInfo *m = &out->m[out->n++];
            m->id = (uint8_t)i, m->flags = room->m[i].maker ? COOP_FLAG_MAKER : 0, m->session = room->m[i].session;
            memcpy(m->name, room->m[i].name, sizeof m->name);
        }
}

static void mock_roster_to_all(MockRelay *r, MockRoom *room, int skip) {
    CoopRoster ro;
    uint8_t payload[300];
    mock_roster_of(room, &ro);
    size_t n = coop_roster_pack(payload, sizeof payload, &ro);
    for (int i = 1; i <= COOP_MAX_MEMBERS; i++)
        if (room->m[i].used && i != skip) mock_send(r, &room->m[i].addr, COOP_T_ROSTER, room->id, (uint8_t)i, payload, n, room->locator);
}

static void mock_connect(MockRelay *r, const uint8_t *d, size_t n, const CoopHdr *h, const uint8_t *p, const PlatAddr *from) {
    CoopConnect c;
    if (n < COOP_CONNECT_MIN || !coop_connect_parse(p, h->len, &c) || (c.intent != COOP_INTENT_CREATE && c.intent != COOP_INTENT_JOIN)) return;
    MockRoom *room = NULL;
    for (int i = 0; i < 4; i++)
        if (r->rooms[i].used && !memcmp(r->rooms[i].locator, c.locator, COOP_KEY_LEN)) room = &r->rooms[i];
    int id = 0;
    // the same member with its token (its id, no cookie needed), or the same CONNECT sent again because the JOINED was lost
    for (int i = 1; room && i <= COOP_MAX_MEMBERS; i++)
        if (room->m[i].used && room->m[i].cid == c.client_id &&
            (!memcmp(room->m[i].token, c.token, COOP_COOKIE_LEN) || (room->m[i].connect_seq == h->seq && plat_addr_eq(&room->m[i].addr, from))))
            id = i;
    uint8_t cookie[COOP_COOKIE_LEN];
    mock_cookie(r, from, cookie);
    if (!id && memcmp(cookie, c.cookie, sizeof cookie)) {
        uint8_t payload[32];
        mock_send(r, from, COOP_T_CHALLENGE, 0, 0, payload, coop_challenge_pack(payload, sizeof payload, c.nonce, cookie), c.locator);
        return;
    }
    if (!coop_dgram_verify(d, n, c.locator)) return;
    if (r->watch_cid && c.client_id == r->watch_cid) r->watch_n++;
    if (r->rate_cid && c.client_id == r->rate_cid) return mock_error(r, from, COOP_E_RATE, c.nonce, c.locator);
    if (r->lie) {   // answers this attempt never asked for: another nonce, no tag, another room; then the real one
        r->lie = 0;
        mock_error(r, from, COOP_E_ROOM_FULL, c.nonce ^ 1, c.locator);
        mock_error(r, from, COOP_E_REMOVED, c.nonce, NULL);
        uint8_t d2[64], other[COOP_KEY_LEN];
        CoopHdr eh = {.ver = 1, .type = COOP_T_ERROR, .seq = ++r->seq};
        size_t k = coop_dgram_pack(d2, sizeof d2, &eh, "\x03\x00", 2, NULL);   // a relay of another version
        plat_udp_send(r->sock, from, d2, k);
        coop_room_keys("ZZZZZZZZZZZZZZZZZZ", other, NULL);
        CoopJoined fake = {.member = 5, .nonce = c.nonce};
        uint8_t payload[300];
        size_t pn = coop_joined_pack(payload, sizeof payload, &fake);
        CoopHdr jh = {.ver = COOP_PROTO, .type = COOP_T_JOINED, .room = 0xBAD, .dst = 5, .seq = ++r->seq};
        uint8_t d3[400];
        k = coop_dgram_pack(d3, sizeof d3, &jh, payload, pn, other);
        plat_udp_send(r->sock, from, d3, k);
    }
    if (!room && c.intent == COOP_INTENT_JOIN) return mock_error(r, from, COOP_E_NO_ROOM, c.nonce, c.locator);
    if (!room && c.intent == COOP_INTENT_CREATE && r->steal) {   // someone else made the room first (after a restart)
        r->steal = 0;
        for (int i = 0; i < 4 && !room; i++)
            if (!r->rooms[i].used) room = &r->rooms[i];
        if (!room) return;
        memset(room, 0, sizeof *room);
        room->used = 1, room->id = 0xC0DE0000u + rnd32() % 0xFFFF + 1;
        memcpy(room->locator, c.locator, COOP_KEY_LEN);
        MockMember *thief = &room->m[1];
        thief->used = 1, thief->maker = 1, thief->cid = 0x7417, thief->session = 0x7417;
        strcpy(thief->name, "Thief");
        plat_net_resolve("127.0.0.1", r->port, &thief->addr);   // the mock's own address: what it sends there it drops
        return mock_error(r, from, COOP_E_ROOM_EXISTS, c.nonce, c.locator);
    }
    if (room && c.intent == COOP_INTENT_CREATE && !id) return mock_error(r, from, COOP_E_ROOM_EXISTS, c.nonce, c.locator);
    if (!room) {
        for (int i = 0; i < 4 && !room; i++)
            if (!r->rooms[i].used) room = &r->rooms[i];
        if (!room) return;
        memset(room, 0, sizeof *room);
        room->used = 1, room->id = 0xC0DE0000u + rnd32() % 0xFFFF + 1;
        memcpy(room->locator, c.locator, COOP_KEY_LEN);
    }
    for (int i = 0; i < 4; i++)
        if (room->banned[i] && room->banned[i] == c.client_id) return mock_error(r, from, COOP_E_REMOVED, c.nonce, room->locator);
    int fresh = !id, maker = 1;
    for (int i = 1; i <= COOP_MAX_MEMBERS; i++) maker &= !room->m[i].used;
    for (int i = 1; i <= COOP_MAX_MEMBERS && !id; i++)
        if (!room->m[i].used) id = i;
    if (!id) return mock_error(r, from, COOP_E_ROOM_FULL, c.nonce, room->locator);
    MockMember *m = &room->m[id];
    if (fresh) {
        memset(m, 0, sizeof *m);
        m->maker = maker && c.intent == COOP_INTENT_CREATE;
        m->cid = c.client_id;
        plat_random(m->token, sizeof m->token);
    }
    fresh |= m->connect_seq != h->seq || !m->used;
    m->used = 1, m->addr = *from, m->connect_seq = h->seq, m->session = c.session;
    memcpy(m->name, c.name, sizeof m->name);
    CoopJoined j = {.member = (uint8_t)id, .nonce = c.nonce};
    memcpy(j.token, m->token, COOP_COOKIE_LEN);
    mock_roster_of(room, &j.roster);
    uint8_t payload[400];
    mock_send(r, from, COOP_T_JOINED, room->id, (uint8_t)id, payload, coop_joined_pack(payload, sizeof payload, &j), room->locator);
    if (fresh) mock_roster_to_all(r, room, id);
}

static void mock_handle(MockRelay *r, uint8_t *d, size_t n, const PlatAddr *from) {
    CoopHdr h;
    const uint8_t *p;
    if (!coop_dgram_parse(d, n, &h, &p) || h.ver != COOP_PROTO) return;
    if (h.type == COOP_T_CONNECT) return mock_connect(r, d, n, &h, p, from);
    MockRoom *room = NULL;
    for (int i = 0; i < 4; i++)
        if (r->rooms[i].used && r->rooms[i].id == h.room) room = &r->rooms[i];
    if (!room || h.src < 1 || h.src > COOP_MAX_MEMBERS || !room->m[h.src].used || !plat_addr_eq(&room->m[h.src].addr, from)) return;
    if (!coop_dgram_verify(d, n, room->locator)) return;
    uint8_t member;
    uint64_t session;
    if (h.type == COOP_T_PING) {
        mock_send(r, from, COOP_T_PONG, room->id, h.src, p, h.len > 4 ? 4 : h.len, room->locator);
    } else if (h.type == COOP_T_LEAVE) {
        room->m[h.src].used = 0;
        mock_roster_to_all(r, room, 0);
    } else if (h.type == COOP_T_KICK && room->m[h.src].maker && coop_kick_parse(p, h.len, &member, &session) && member >= 1 &&
               member <= COOP_MAX_MEMBERS && room->m[member].used && room->m[member].session == session) {
        for (int i = 0; i < 4; i++)
            if (!room->banned[i]) {
                room->banned[i] = room->m[member].cid;
                break;
            }
        CoopError e = {.reason = COOP_E_REMOVED};
        uint8_t payload[80];
        mock_send(r, &room->m[member].addr, COOP_T_ERROR, room->id, member, payload, coop_error_pack(payload, sizeof payload, &e), room->locator);
        room->m[member].used = 0;
        mock_roster_to_all(r, room, 0);
    } else if (h.type == COOP_T_DATA) {
        uint32_t c;
        if (coop_envelope_peek(p, h.len, &session, &c)) r->counter[h.src] = c;
        if (r->record_from == h.src && n <= sizeof r->recorded) {   // kept for a replay after its sender joined again
            memcpy(r->recorded, d, n);
            r->recorded_n = n;
            r->record_from = 0;
        }
        if (r->corrupt_next) {   // one forwarded datagram with a flipped tag bit: no receiver may deliver it
            r->corrupt_next = 0;
            d[n - 1] ^= 1;
        }
        for (int i = 1; i <= COOP_MAX_MEMBERS; i++)
            if (room->m[i].used && i != h.src && (h.dst == COOP_DST_ALL || h.dst == i)) {
                plat_udp_send(r->sock, &room->m[i].addr, d, n);
                r->forwarded++;
            }
    }
}

static MockRoom *mock_room_of(MockRelay *r, int member) {
    for (int i = 0; i < 4; i++)
        if (r->rooms[i].used && r->rooms[i].m[member].used) return &r->rooms[i];
    return NULL;
}

// What a hostile relay can do with what it knows (the locator, every member's session, every datagram it forwards):
// a message to member 1 in member 2's name and session, sealed with a key it made up, tagged right; and the DATA
// datagram recorded earlier, sent again.
static void mock_forge(MockRelay *r) {
    MockRoom *room = mock_room_of(r, 1);
    if (!room || !room->m[2].used) return;
    uint8_t fake_key[COOP_MSG_KEY_LEN], payload[COOP_MAX_PAYLOAD], d[COOP_MAX_DGRAM];
    plat_random(fake_key, sizeof fake_key);
    uint8_t m[9] = {'p', 'r', 'o', 'b', 'e', 0};
    coop_u32_pack(m + 5, 4, 600);
    size_t n = coop_seal(payload, sizeof payload, fake_key, 2, COOP_DST_ALL, room->m[2].session, 0x7FFFFFF0u, COOP_K_TEST, m, sizeof m);
    CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_DATA, .room = room->id, .src = 2, .dst = COOP_DST_ALL, .seq = ++r->seq};
    size_t k = coop_dgram_pack(d, sizeof d, &h, payload, n, room->locator);
    plat_udp_send(r->sock, &room->m[1].addr, d, k);
}

// What a relay run by someone who knows the room's secret (or a sender from its address on member 1's network) can do
// with the message key: one message to member 1 in member 2's name and session, sealed with that key, its counter
// 2^31 - 1 past member 2's last one, of a kind nobody handles. Three copies: the test's loss.
static void mock_forge_ahead(MockRelay *r) {
    MockRoom *room = mock_room_of(r, 1);
    if (!room || !room->m[2].used) return;
    uint8_t key[COOP_KEY_LEN], mk[COOP_MSG_KEY_LEN], payload[COOP_MAX_PAYLOAD], d[COOP_MAX_DGRAM];
    coop_room_keys(ROOM_SECRET, key, mk);
    size_t n = coop_seal(payload, sizeof payload, mk, 2, COOP_DST_ALL, room->m[2].session, r->counter[2] + 0x7FFFFFFFu, 0xF1, "x", 1);
    CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_DATA, .room = room->id, .src = 2, .dst = COOP_DST_ALL, .seq = ++r->seq};
    size_t k = coop_dgram_pack(d, sizeof d, &h, payload, n, room->locator);
    for (int i = 0; i < 3; i++) plat_udp_send(r->sock, &room->m[1].addr, d, k);
}

static void mock_replay(MockRelay *r) {
    MockRoom *room = mock_room_of(r, 1);
    if (room && r->recorded_n) plat_udp_send(r->sock, &room->m[1].addr, r->recorded, r->recorded_n);
}

// Datagrams of every kind to member 1 of the first room from the relay's own address, as a hostile or broken relay would: bytes
// at random, valid ones changed or cut, and ones carrying the room's tag with a random payload (stale roster counters, and
// DATA no key opens).
static void mock_fuzz(MockRelay *r) {
    MockRoom *room = mock_room_of(r, 1);
    if (!room) return;
    PlatAddr to = room->m[1].addr;
    uint8_t d[1400], payload[COOP_MAX_PAYLOAD];
    for (int it = 0; it < 6000 && !r->stop; it++) {
        size_t n;
        int mode = it % 3;
        if (mode == 0) {
            n = rnd32() % 1300;
            for (size_t i = 0; i < n; i++) d[i] = (uint8_t)(rnd32() % 4 ? rnd32() : 0);
            if (n > 12 && rnd32() % 2) d[0] = 0x4E, d[1] = 0x43, d[2] = COOP_PROTO;
        } else {
            size_t pn = rnd32() % 200;
            for (size_t i = 0; i < pn; i++) payload[i] = (uint8_t)rnd32();
            static const uint8_t types[] = {COOP_T_CHALLENGE, COOP_T_JOINED, COOP_T_ERROR, COOP_T_ROSTER, COOP_T_PONG, COOP_T_DATA, COOP_T_PING, 0x77};
            uint8_t type = types[rnd32() % sizeof types];
            if (type == COOP_T_DATA && pn > 13) payload[0] = COOP_ENV_V1, memcpy(payload + 1, &room->m[2].session, 8);
            CoopHdr h = {.ver = COOP_PROTO, .type = type, .room = mode == 1 ? room->id : rnd32(), .src = (uint8_t)(rnd32() % 10),
                         .dst = (uint8_t)(rnd32() % 3 == 0 ? COOP_DST_ALL : 1), .seq = type == COOP_T_ROSTER ? 0 : rnd32()};
            n = coop_dgram_pack(d, sizeof d, &h, payload, pn, room->locator);
            if (mode == 2 && n) d[rnd32() % n] ^= (uint8_t)(1u << (rnd32() % 8));
        }
        plat_udp_send(r->sock, &to, d, n);
        if (it % 40 == 0) plat_sleep_ms(1);
    }
}

static void mock_main(void *arg) {
    MockRelay *r = arg;
    uint8_t buf[2048];
    PlatAddr from;
    while (!r->stop) {
        if (r->restart) {   // the process went away and came back: rooms gone, a new cookie secret, 400 ms of silence
            memset(r->rooms, 0, sizeof r->rooms);
            plat_random(r->secret, sizeof r->secret);
            uint64_t until = plat_now_ms() + 400;
            while (plat_now_ms() < until) plat_udp_recv(r->sock, buf, sizeof buf, &from, 20);
            r->restart = 0;
        }
        if (r->fuzz) mock_fuzz(r), r->fuzz = 0;
        if (r->forge) mock_forge(r), r->forge = 0;
        if (r->forge_ahead) mock_forge_ahead(r), r->forge_ahead = 0;
        for (; r->replay; r->replay--) mock_replay(r);   // several copies: the test's loss may drop some
        if (r->mark || r->spoof) {
            MockRoom *room = mock_room_of(r, r->mark ? r->mark : r->spoof);
            if (room && r->mark) room->m[r->mark].maker = !room->m[r->mark].maker;
            if (room && r->spoof) room->m[r->spoof].session ^= 0x5005;
            if (room) mock_roster_to_all(r, room, 0);
            r->mark = r->spoof = 0;
        }
        if (r->forge_roster) {   // someone on the path who saw a CONNECT: a roster with a number of its choosing
            MockRoom *room = mock_room_of(r, 1);
            if (room && room->m[2].used) {
                CoopRoster ro;
                mock_roster_of(room, &ro);
                for (int i = 0; i < ro.n; i++)
                    if (ro.m[i].id == 2) ro.m[i].session ^= 0xBAD;
                uint8_t payload[300], d[COOP_MAX_DGRAM];
                size_t pn = coop_roster_pack(payload, sizeof payload, &ro);
                CoopHdr h = {.ver = COOP_PROTO, .type = COOP_T_ROSTER, .room = room->id, .dst = 1, .seq = r->seq + r->forge_roster};
                size_t k = coop_dgram_pack(d, sizeof d, &h, payload, pn, room->locator);
                for (int i = 0; i < 3; i++) plat_udp_send(r->sock, &room->m[1].addr, d, k);   // three copies: the test's loss
            }
            r->forge_roster = 0;
        }
        int n = plat_udp_recv(r->sock, buf, sizeof buf, &from, 10);
        if (n > 0) mock_handle(r, buf, (size_t)n, &from);
        if (plat_now_ms() >= r->next_roster) {   // the roster again every second: a lost update repairs itself
            r->next_roster = plat_now_ms() + 1000;
            for (int i = 0; i < 4; i++)
                if (r->rooms[i].used) mock_roster_to_all(r, &r->rooms[i], 0);
        }
    }
}

static MockRelay *mock_start(void) {
    MockRelay *r = calloc(1, sizeof *r);
    if (!r) return NULL;
    r->sock = plat_udp_open(0, 0);
    if (!r->sock) {
        free(r);
        return NULL;
    }
    r->port = plat_udp_port(r->sock);
    plat_random(r->secret, sizeof r->secret);
    r->thread = plat_thread_new(mock_main, r, "coop-mock-relay");
    return r;
}

static void mock_stop(MockRelay *r) {
    r->stop = 1;
    plat_thread_join(r->thread);
    plat_udp_close(r->sock);
    free(r);
}

// ---- 3b. two sessions

typedef struct {
    const char *name;
    CoopSess *sess;
    PlatThread *thread;
    volatile int stop;
    volatile unsigned views, saw_reconnect;   // saw_reconnect: times the session went into RECONNECTING (rejoins)
    int last_state;                           // the state of the last view (the view hook's thread only)
    volatile unsigned got, dups, byes, from_wrong;   // test messages received, delivered twice, BYEs, from a member other than the expected
    uint32_t seen[512];
    int expect_from;
} Player;

static void player_loop(void *arg) {
    Player *p = arg;
    while (!p->stop) coop_sess_step(p->sess);
}

static void on_test_message(int from, const uint8_t *bytes, size_t n, void *user) {
    Player *p = user;
    uint32_t seq;
    if (n != 9 || memcmp(bytes, "probe", 4) || !coop_u32_parse(bytes + 5, 4, &seq) || seq >= 512 * 32) return;
    if (p->expect_from && from != p->expect_from) __atomic_add_fetch(&p->from_wrong, 1, __ATOMIC_RELAXED);
    uint32_t bit = 1u << (seq % 32);
    if (p->seen[seq / 32] & bit) __atomic_add_fetch(&p->dups, 1, __ATOMIC_RELAXED);
    p->seen[seq / 32] |= bit;
    __atomic_add_fetch(&p->got, 1, __ATOMIC_RELAXED);
}

static void on_bye(int from, const uint8_t *bytes, size_t n, void *user) {
    (void)from, (void)bytes, (void)n;
    __atomic_add_fetch(&((Player *)user)->byes, 1, __ATOMIC_RELAXED);
}

static void on_view_change(const CoopView *v, void *user) {
    Player *p = user;
    __atomic_add_fetch(&p->views, 1, __ATOMIC_RELAXED);
    // a rejoin publishes a view at each change while it lasts (its status text, the roster): count the step into
    // RECONNECTING, not every view in it
    if (v->state == COOP_RECONNECTING && p->last_state != COOP_RECONNECTING) __atomic_add_fetch(&p->saw_reconnect, 1, __ATOMIC_RELAXED);
    p->last_state = v->state;
}

static uint16_t free_port(void) {
    PlatUdp *u = plat_udp_open(0, 0);
    uint16_t port = u ? plat_udp_port(u) : 0;
    plat_udp_close(u);
    return port;
}

static void player_start(Player *p, const char *name, uint64_t cid, const char *server, unsigned drop, int retry, uint16_t local_port) {
    CoopParams cp = {.tag = name, .server = server, .name = name, .client_id = cid, .on = 1, .drop_pct = drop, .retry = retry,
                     .local_port = local_port, .ping_ms = 200, .silence_ms = 1500, .burst_gap_ms = 100, .backoff_first_ms = 150,
                     .backoff_max_ms = 500};
    memset(p, 0, sizeof *p);
    p->name = name;
    p->sess = coop_sess_new(&cp);
    if (!p->sess) return;
    coop_sess_on_message(p->sess, COOP_K_TEST, on_test_message, p);
    coop_sess_on_message(p->sess, COOP_K_BYE, on_bye, p);
    coop_sess_on_view(p->sess, on_view_change, p);
    p->thread = plat_thread_new(player_loop, p, "coop-probe");
}

static void player_end(Player *p) {
    if (!p->sess) return;
    coop_sess_stop(p->sess);
    coop_sess_wait_off(p->sess, 1000);
    p->stop = 1;
    plat_thread_join(p->thread);
    coop_sess_free(p->sess);
    p->sess = NULL;
}

static CoopView view_of(Player *p) {
    CoopView v;
    coop_sess_view(p->sess, &v);
    return v;
}

static int in_room(Player *p, int members) {
    CoopView v = view_of(p);
    return v.state == COOP_JOINED && v.n == members;
}

// p's roster names q's present session, both in the room: p takes q's messages. With q = p: the last roster p took
// lists its own session, so no rejoin of p waits for its turn (roster_rejoin in coop.c).
static int names_session(Player *p, Player *q) {
    CoopView vp = view_of(p), vq = view_of(q);
    if (vp.state != COOP_JOINED || vq.state != COOP_JOINED) return 0;
    for (int i = 0; i < vp.n; i++)
        if (vp.member[i].id == vq.self) return vp.member[i].session == vq.session;
    return 0;
}

// The mock relay lists each player's present session, and both rosters name both: no roster still to come lists
// another session for either (one sent before has a lower number and is refused), so neither throws the other's
// messages away or joins again until the relay is made to lie again.
static int settled(MockRelay *mock, Player *a, Player *b) {
    Player *both[2] = {a, b};
    for (int i = 0; i < 2; i++) {
        CoopView v = view_of(both[i]);
        MockRoom *room = v.state == COOP_JOINED ? mock_room_of(mock, v.self) : NULL;
        if (!room || room->m[v.self].session != v.session) return 0;
    }
    return names_session(a, a) && names_session(a, b) && names_session(b, a) && names_session(b, b);
}

// A stop is signalled only once the view that says the session is off is published, so that whoever waited for it
// (coop_shutdown's caller, a page that switched co-op off) cannot read the view of the step before. One session stepped
// on this thread, no relay: a view hook that sees "off" notes whether the stop was signalled already.
typedef struct {
    CoopSess *sess;
    int off_seen, signalled_before;
} StopOrder;

static void on_view_stop_order(const CoopView *v, void *user) {
    StopOrder *o = user;
    if (v->state != COOP_OFF || o->off_seen) return;
    o->off_seen = 1;
    o->signalled_before = coop_sess_wait_off(o->sess, 0);
}

static void probe_stop_order(void) {
    CoopParams cp = {.tag = "Stop", .server = "", .name = "Stop", .client_id = 0x5709, .on = 1};
    StopOrder o = {0};
    o.sess = coop_sess_new(&cp);
    if (!o.sess) FAIL_RET("a session did not start");
    coop_sess_on_view(o.sess, on_view_stop_order, &o);
    CoopView v;
    coop_sess_view(o.sess, &v);
    CHECK(v.state == COOP_LEFT && !coop_sess_wait_off(o.sess, 0), "stop order: a session that is on is in no room and not stopped (state %d)", v.state);
    coop_sess_stop(o.sess);
    coop_sess_step(o.sess);
    coop_sess_view(o.sess, &v);
    CHECK(o.off_seen && !o.signalled_before,
          "stop order: the stop is signalled only after the view that says the session is off is published (a view hook saw off %d, the stop already signalled then %d)",
          o.off_seen, o.signalled_before);
    CHECK(coop_sess_wait_off(o.sess, 0) && v.state == COOP_OFF, "stop order: after the step the stop is signalled and the view says off (state %d)", v.state);
    coop_sess_free(o.sess);
    printf("coop-probe: stop order: a stop wakes its waiter only once the view says the session is off\n");
}

// n numbered messages from a to b, 15 ms apart, until b has got the number a count asks for (or they ran out).
// to: the member id they are addressed to, or 0 for all.
static unsigned send_messages(Player *a, Player *b, int to, int n, uint32_t first) {
    unsigned before = b->got;
    for (int i = 0; i < n; i++) {
        uint8_t m[9];
        memcpy(m, "probe", 5);
        coop_u32_pack(m + 5, 4, first + (uint32_t)i);
        m[4] = 0;
        coop_sess_send(a->sess, to, COOP_K_TEST, m, sizeof m);
        plat_sleep_ms(15);
    }
    plat_sleep_ms(300);
    return b->got - before;
}

static void exchange(Player *a, Player *b, unsigned drop, const char *what, uint32_t *first) {
    int ida = view_of(a).self, idb = view_of(b).self;
    a->expect_from = idb, b->expect_from = ida;
    unsigned min = drop ? 20 : 55;
    unsigned ab = send_messages(a, b, 0, 60, *first);
    *first += 60;
    unsigned ba = send_messages(b, a, ida, 60, *first);   // addressed to one member
    *first += 60;
    CHECK(ab >= min, "%s: Alice's messages reached Bob: %u of 60 (at least %u)", what, ab, min);
    CHECK(ba >= min, "%s: Bob's messages reached Alice: %u of 60 (at least %u)", what, ba, min);
    CHECK(!a->dups && !b->dups, "%s: no message was delivered twice (%u, %u)", what, a->dups, b->dups);
    CHECK(!a->from_wrong && !b->from_wrong, "%s: every message came from the right member", what);
    printf("coop-probe: %s: Alice -> Bob %u of 60, Bob -> Alice %u of 60\n", what, ab, ba);
}

// The process's session, driven by the settings as the game does it: COOP, COOP_SERVER, COOP_NAME, COOP_AUTO and COOP_ROOM in the
// environment (cfg_setenv bumps cfg_version, which the coop thread polls), then coop_init, the commands, a rename, switching
// COOP off and on again, and coop_shutdown. alice is in the room of ROOM_SECRET with Bob.
static void probe_process(const char *server, unsigned drop, Player *alice, uint32_t *first) {
    char drop_text[16];
    snprintf(drop_text, sizeof drop_text, "%u", drop);
    cfg_setenv("NX2NAVI_COOP_NAME", NULL);
    cfg_setenv("NX2NAVI_COOP_SERVER", server);
    cfg_setenv("NX2NAVI_COOP_AUTO", "join");
    cfg_setenv("NX2NAVI_COOP_ROOM", "h7k3wm-bcdfgh 234567");   // a secret as a player types it
    cfg_setenv("NX2NAVI_COOP_DROP", drop_text);
    cfg_setenv("NX2NAVI_COOP", "0");
    CoopView v;
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF, "the process's session is off until COOP is on (state %d)", v.state);
    CHECK(!coop_send(COOP_K_TEST, "x", 1) && !coop_command(COOP_CMD_CREATE, NULL), "nothing is sent and no command taken while off");
    Player proc;
    memset(&proc, 0, sizeof proc);
    proc.name = "process";
    CHECK(coop_on_message(COOP_K_TEST, on_test_message, &proc) && coop_on_view(on_view_change, &proc), "handlers register");
    uint64_t id1 = coop_client_id();
    CHECK(id1 && id1 == coop_client_id(), "the client id stays the same within a start of the game");
    coop_init();
    coop_init();   // twice is the same as once
    cfg_setenv("NX2NAVI_COOP", "1");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED), 30000), "COOP_AUTO=join joins the room of COOP_ROOM (state %d, %s)", v.state, v.status);
    CHECK(v.self == 3 && v.n == 3 && !strcmp(v.code, "H7K3WM-BCDFGH-234567") && v.maker == 1 && !v.made,
          "as member 3 of H7K3WM-BCDFGH-234567, made by member 1 (self %d, %d members, %s, maker %d)", v.self, v.n, v.code, v.maker);
    CHECK(!strncmp(v.member[2].name, "Player-", 7) && strlen(v.member[2].name) == 11, "under the default name Player-xxxx (%s)", v.member[2].name);
    CHECK(WAIT_FOR(in_room(alice, 3), ROSTER_WAIT_MS), "Alice sees three members");
    CHECK(proc.views > 0, "the view hook ran");

    // messages both ways through the interface other layers use
    alice->expect_from = 3, proc.expect_from = 1;
    unsigned got_before = proc.got;
    for (int i = 0; i < 40; i++) {
        uint8_t m[9];
        memcpy(m, "probe", 5);
        m[4] = 0;
        coop_u32_pack(m + 5, 4, *first + (uint32_t)i);
        coop_sess_send(alice->sess, 3, COOP_K_TEST, m, sizeof m);
        coop_u32_pack(m + 5, 4, *first + 100 + (uint32_t)i);
        coop_send(COOP_K_TEST, m, sizeof m);   // to everyone: Alice and Bob
        plat_sleep_ms(15);
    }
    plat_sleep_ms(300);
    unsigned min = drop ? 14 : 37;
    CHECK(proc.got - got_before >= min && !proc.dups && !proc.from_wrong, "the process got %u of 40 from Alice (%u twice, %u from the wrong member)",
          proc.got - got_before, proc.dups, proc.from_wrong);
    CHECK(alice->got >= min, "Alice got messages from the process: %u", alice->got);
    *first += 200;
    alice->got = 0;
    static uint8_t too_long[COOP_MAX_MESSAGE + 1];
    CHECK(coop_send_to(1, COOP_K_TEST, "abc", 3) && !coop_send_to(0, COOP_K_TEST, "abc", 3) && !coop_send(COOP_K_TEST, too_long, sizeof too_long),
          "coop_send_to takes a member, a message that is too long is refused");
    CHECK(!coop_remove_member(1) || WAIT_FOR(in_room(alice, 3), 3000), "a member that did not make the room removes nobody");

    // a new name joins the room again under it
    cfg_setenv("NX2NAVI_COOP_NAME", "Process");
    CHECK(WAIT_FOR(in_room(alice, 3) && !strcmp(view_of(alice).member[2].name, "Process"), ROSTER_WAIT_MS), "a new COOP_NAME shows in Alice's roster (%s)",
          view_of(alice).member[2].name);
    coop_view_get(&v);
    CHECK(v.state == COOP_JOINED || WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED), 15000), "and the process is back in the room");

    // commands
    CHECK(!coop_command(COOP_CMD_JOIN, "not a secret") && !coop_command(COOP_CMD_JOIN, NULL) && !coop_command(COOP_CMD_CREATE, "H7K3WM"),
          "a bad room secret is refused");
    CHECK(coop_command(COOP_CMD_LEAVE, NULL), "leave is taken");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_LEFT) && in_room(alice, 2), ROSTER_WAIT_MS), "the process left (state %d)", v.state);
    CHECK(coop_command(COOP_CMD_JOIN, "H7K3WM BCDFGH 234567"), "join is taken");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED) && in_room(alice, 3), ROSTER_WAIT_MS), "and the process joined again (state %d, %s)", v.state, v.status);

    // COOP off and on again
    cfg_setenv("NX2NAVI_COOP", "0");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_OFF) && in_room(alice, 2), ROSTER_WAIT_MS), "COOP = 0 leaves the room and turns the session off (state %d)", v.state);
    cfg_setenv("NX2NAVI_COOP", "1");
    CHECK(WAIT_FOR((coop_view_get(&v), v.state == COOP_JOINED) && in_room(alice, 3), ROSTER_WAIT_MS), "COOP = 1 starts COOP_AUTO again (state %d, %s)", v.state, v.status);

    // the process ends: LEAVE and BYE
    uint64_t t0 = plat_now_ms();
    coop_shutdown();
    CHECK(plat_now_ms() - t0 < 1500, "coop_shutdown returns within a second and a half (%u ms)", (unsigned)(plat_now_ms() - t0));
    coop_view_get(&v);
    CHECK(v.state == COOP_OFF, "and leaves the session off (state %d)", v.state);
    CHECK(WAIT_FOR(in_room(alice, 2), ROSTER_WAIT_MS), "Alice sees the process go");
    cfg_setenv("NX2NAVI_COOP", "0");
    cfg_setenv("NX2NAVI_COOP_AUTO", NULL);
    cfg_setenv("NX2NAVI_COOP_ROOM", NULL);
}

// The hostile relay of this file (the mock): what it forwards changed, a message it seals itself, a recorded message of
// Bob's sent again after Bob joined again, and lies to a player who connects. Alice and Bob are in a room.
static void probe_hostile_relay(MockRelay *mock, Player *alice, Player *bob, uint16_t alice_port, unsigned drop, uint32_t *first) {
    uint8_t m[9] = {'p', 'r', 'o', 'b', 'e', 0};
    unsigned got_before = alice->got;
    mock->corrupt_next = 1;
    coop_u32_pack(m + 5, 4, 500);
    coop_sess_send(bob->sess, 0, COOP_K_TEST, m, sizeof m);
    plat_sleep_ms(300);
    CHECK(alice->got == got_before && !(alice->seen[500 / 32] & (1u << (500 % 32))), "a message changed in transit is not delivered");

    mock->forge = 1;
    CHECK(WAIT_FOR(!mock->forge, 5000), "the relay forged a message");
    plat_sleep_ms(300);
    CHECK(!(alice->seen[600 / 32] & (1u << (600 % 32))), "a message the relay sealed itself, in Bob's name and session, is not delivered");

    // a message of Bob's recorded, then Bob joins again (a new session), then the recorded one comes again
    mock->record_from = view_of(bob).self;
    coop_u32_pack(m + 5, 4, 700);
    for (int i = 0; i < 20 && mock->record_from; i++) coop_sess_send(bob->sess, 0, COOP_K_TEST, m, sizeof m), plat_sleep_ms(50);
    CHECK(WAIT_FOR(!mock->record_from && mock->recorded_n, 3000), "the relay recorded a message of Bob's");
    CHECK(drop || WAIT_FOR(alice->seen[700 / 32] & (1u << (700 % 32)), 3000), "and Alice got it once");
    unsigned dups = alice->dups;
    mock->replay = 5;   // the same session still: a replay within the window
    CHECK(WAIT_FOR(!mock->replay, 3000), "replayed");
    plat_sleep_ms(300);
    CHECK(alice->dups == dups, "a message sent again in the same session is not delivered twice");
    // Alice joins the room again (under a new name); Bob's session has not changed, so his recorded message is still one
    // she has seen
    char srv[32];
    snprintf(srv, sizeof srv, "127.0.0.1:%u", mock->port);
    CoopParams ap = {.tag = "Alice", .server = srv, .name = "Alice-again", .client_id = 0xA11CE, .on = 1, .drop_pct = drop, .retry = 1,
                     .local_port = alice_port};
    unsigned recon = alice->saw_reconnect;
    coop_sess_configure(alice->sess, &ap);
    CHECK(WAIT_FOR(alice->saw_reconnect > recon && in_room(alice, 2), ROSTER_WAIT_MS + 3000), "Alice joined the room again under a new name");
    plat_sleep_ms(500);
    mock->replay = 5;
    CHECK(WAIT_FOR(!mock->replay, 3000), "replayed after Alice joined again");
    plat_sleep_ms(300);
    CHECK(alice->dups == dups, "a message of Bob's present session is not delivered again after Alice joined the room again (%u twice)",
          alice->dups - dups);
    ap.name = "Alice";
    recon = alice->saw_reconnect;
    coop_sess_configure(alice->sess, &ap);
    CHECK(WAIT_FOR(alice->saw_reconnect > recon && in_room(alice, 2) && !strcmp(view_of(alice).member[0].name, "Alice"), ROSTER_WAIT_MS + 3000),
          "Alice is Alice again");
    coop_sess_command(bob->sess, COOP_CMD_JOIN, ROOM_SECRET);   // Bob joins again: a new session
    CHECK(WAIT_FOR(in_room(bob, 2) && in_room(alice, 2), ROSTER_WAIT_MS), "Bob is back");
    plat_sleep_ms(1500);   // the roster with Bob's new session has reached Alice
    uint32_t lost = view_of(alice).lost;
    mock->replay = 5;
    CHECK(WAIT_FOR(!mock->replay, 3000), "replayed after the rejoin");
    plat_sleep_ms(300);
    CHECK(alice->dups == dups && view_of(alice).lost > lost, "a message recorded before Bob joined again is refused after it (%u thrown away)",
          view_of(alice).lost - lost);

    // garbage and datagrams with the room's tag from the relay's address
    lost = view_of(alice).lost;
    mock->fuzz = 1;
    CHECK(WAIT_FOR(!mock->fuzz, 60000), "the garbage was sent");
    plat_sleep_ms(300);
    CHECK(view_of(alice).lost > lost, "the garbage was counted as thrown away (%u)", view_of(alice).lost - lost);
    CHECK(WAIT_FOR(in_room(alice, 2) && in_room(bob, 2), ROSTER_WAIT_MS), "after the garbage both are still in the room");
    exchange(alice, bob, drop, "after the hostile relay", first);

    // the relay marks Bob as the room's maker: Bob's game did not make the room, so it does not take it as made
    int bob_id = view_of(bob).self;
    mock->mark = bob_id;
    CHECK(WAIT_FOR(view_of(bob).maker == bob_id, ROSTER_WAIT_MS) && !view_of(bob).made, "F5: a member the relay marks as the maker does not take the room as made by it (made %d)",
          view_of(bob).made);
    mock->mark = bob_id;
    CHECK(WAIT_FOR(view_of(bob).maker != bob_id, ROSTER_WAIT_MS), "and the mark is gone again");
    // someone on the path rejoined Alice with a session of its own: the relay lists that one; Alice notices and rejoins,
    // and Bob takes her messages again once a roster with her new session reaches him. Each of those datagrams can be
    // lost: the wait holds as many rosters as ROSTER_WAIT_MS does.
    recon = alice->saw_reconnect;
    mock->spoof = view_of(alice).self;
    uint64_t spoofed_at = plat_now_ms();
    CHECK(WAIT_FOR(alice->saw_reconnect > recon && settled(mock, alice, bob), ROSTER_WAIT_MS),
          "F9: Alice joined again, and the relay and both rosters list her new session");
    double settle_s = (double)(plat_now_ms() - spoofed_at) / 1000;
    // with no loss nothing waits for a lost datagram: Alice joins again at the roster (once per backoff) and the next
    // rosters name her new session (the probe's old fixed wait was 2.5 s); a rejoin or roster that comes late fails here
    CHECK(drop || settle_s * 1000 <= SETTLE_NO_LOSS_MS, "F9: with no loss Alice's new session settled %.1f s after the spoofed "
          "rejoin, not within %.1f s", settle_s, SETTLE_NO_LOSS_MS / 1000.0);
    printf("coop-probe: F9: Alice's new session settled %.1f s after the spoofed rejoin\n", settle_s);
    exchange(alice, bob, drop, "F9: after a spoofed rejoin of Alice", first);

    // an outsider's datagrams to Alice's port are not the relay's: thrown away
    PlatUdp *evil = plat_udp_open(0, 0);
    PlatAddr to;
    plat_net_resolve("127.0.0.1", alice_port, &to);
    uint8_t junk[200];
    lost = view_of(alice).lost;
    for (int i = 0; evil && i < 50; i++) {
        for (size_t k = 0; k < sizeof junk; k++) junk[k] = (uint8_t)rnd32();
        plat_udp_send(evil, &to, junk, 10 + (size_t)(rnd32() % 190));
    }
    plat_udp_close(evil);
    CHECK(WAIT_FOR(view_of(alice).lost >= lost + 20, 5000), "datagrams from another address are thrown away");
    CHECK(in_room(alice, 2), "and change nothing");

    // the fourth review's finding 4. Three rosters within 300 ms list another session for Alice: she joins again once
    // in that time, not three times (each join is one of her network's)
    if (*first < 1000) *first = 1000;   // past the numbers of the messages above
    unsigned recon2 = alice->saw_reconnect;
    for (int i = 0; i < 3; i++) {
        mock->spoof = view_of(alice).self;
        WAIT_FOR(!mock->spoof, 1000);
        plat_sleep_ms(100);
    }
    unsigned rejoins = alice->saw_reconnect - recon2;
    CHECK(rejoins <= 1, "H4 F4: three rosters that list another session for Alice make her join again once (%u times)", rejoins);
    uint64_t spoofs_at = plat_now_ms();
    CHECK(WAIT_FOR(in_room(alice, 2) && in_room(bob, 2) && settled(mock, alice, bob), ROSTER_WAIT_MS),
          "and both are in the room after it, the relay and both rosters listing each one's present session");
    settle_s = (double)(plat_now_ms() - spoofs_at) / 1000;
    CHECK(drop || settle_s * 1000 <= SETTLE_NO_LOSS_MS, "H4 F4: with no loss the rosters settled %.1f s after the three that "
          "list another session for Alice, not within %.1f s", settle_s, SETTLE_NO_LOSS_MS / 1000.0);
    printf("coop-probe: H4 F4: the rosters settled %.1f s after the three that list another session for Alice\n", settle_s);
    exchange(alice, bob, drop, "H4 F4: after the rosters listing another session for Alice", first);
    // someone on the path sends Alice a roster far ahead of the relay's counter that lists another session for Bob: it
    // is not taken, and Bob's messages keep reaching her
    mock->forge_roster = 1u << 30;
    CHECK(WAIT_FOR(!mock->forge_roster, 2000), "a roster 2^30 ahead was sent");
    plat_sleep_ms(300);
    exchange(alice, bob, drop, "H4 F4: after a forged roster 2^30 ahead", first);
    // one a little ahead is taken (the relay's counter could be there), and the relay's are taken again once none was
    // for the silence time and 4 s (1.5 s and 4 s here): the next of the relay's that arrives then. The mock sends one
    // a second, so the wait holds at least ten of them after the 5.5 s; all lost at 30 % loss is one run in 170000.
    mock->forge_roster = 1000;
    CHECK(WAIT_FOR(!mock->forge_roster, 2000), "a roster 1000 ahead was sent");
    uint64_t forged_at = plat_now_ms();
    int forged_taken = WAIT_FOR(!names_session(alice, bob), 1000);   // its three copies can all be lost at 30 % loss
    CHECK(drop || forged_taken, "H4 F4: a roster 1000 ahead is taken");
    CHECK(WAIT_FOR(names_session(alice, bob), 1500 + 4000 + 11000), "H4 F4: the relay's rosters are taken again after a forged one 1000 ahead");
    const uint64_t retaken_ms = plat_now_ms() - forged_at;
    // with no loss the guide's bound holds: the silence time and 4 s, then the mock's next roster
    CHECK(drop || !forged_taken || retaken_ms <= STALE_NO_LOSS_MS, "H4 F4: with no loss the relay's rosters were taken again "
          "%.1f s after a forged one 1000 ahead, not within %.1f s (the silence time and 4 s, the mock's next roster, 1 s)",
          (double)retaken_ms / 1000, STALE_NO_LOSS_MS / 1000.0);
    char what[96];
    snprintf(what, sizeof what, "H4 F4: %.1f s after a forged roster 1000 ahead%s", (double)retaken_ms / 1000,
             forged_taken ? "" : ", all of whose copies were lost");
    exchange(alice, bob, drop, what, first);
    // the sixth review's finding 1: one message in Bob's name and session, sealed with the room's key (which everyone
    // who knows the secret has), its counter 2^31 - 1 ahead of Bob's. Bob's own messages reach Alice again after a few
    mock->forge_ahead = 1;
    CHECK(WAIT_FOR(!mock->forge_ahead, 2000), "an envelope 2^31 - 1 ahead was sent");
    plat_sleep_ms(300);
    exchange(alice, bob, drop, "H6 F1: after one envelope in Bob's name 2^31 - 1 ahead, sealed with the room's key", first);

    // finding 3: a session that keeps its room waits a minute when the relay refuses it for its network's joins, and
    // stays; and it asks for a room that is not there at most every 10 s
    Player erin, fay;
    mock->watch_cid = 0xE41, mock->watch_n = 0, mock->rate_cid = 0xE41;
    player_start(&erin, "Erin", 0xE41, srv, drop, 1, 0);
    coop_sess_command(erin.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(strstr(view_of(&erin).status, "minute") != NULL, 5000), "H4 F3: a refusal for the network's joins says when it is tried again: %s (state %d)",
          view_of(&erin).status, view_of(&erin).state);
    unsigned asked = mock->watch_n;
    plat_sleep_ms(3000);
    CoopView ev = view_of(&erin);
    CHECK(ev.state == COOP_CONNECTING && mock->watch_n - asked <= 3, "H4 F3: a session that keeps its room stays after it and does not ask again at once "
          "(state %d, %s; %u more CONNECTs)", ev.state, ev.status, mock->watch_n - asked);
    player_end(&erin);
    mock->rate_cid = 0, mock->watch_cid = 0xFA1, mock->watch_n = 0;
    player_start(&fay, "Fay", 0xFA1, srv, drop, 1, 0);
    coop_sess_command(fay.sess, COOP_CMD_JOIN, "BCDFGHBCDFGHBCDFGH");
    CHECK(WAIT_FOR(strstr(view_of(&fay).status, "not found") != NULL, 5000), "H4 F3: a room that is not there is waited for: %s", view_of(&fay).status);
    asked = mock->watch_n;
    plat_sleep_ms(4000);
    CHECK(view_of(&fay).state == COOP_CONNECTING && mock->watch_n - asked <= 3, "H4 F3: a session waiting for a room that is not there asks at most "
          "every 10 s (%u questions in 4 s)", mock->watch_n - asked);
    player_end(&fay);
    mock->watch_cid = 0;
}

static void probe_sessions(void) {
    unsigned drop = 0;
    const char *d = cfg_get("NX2NAVI_COOP_DROP");
    if (d) drop = (unsigned)atoi(d);
    const char *relay_env = cfg_get("NX2NAVI_COOP_RELAY");
    int real = relay_env && *relay_env;
    const char *restart_mode = cfg_get("NX2NAVI_COOP_RESTART");
    MockRelay *mock = NULL;
    char server[64];
    if (real) {
        snprintf(server, sizeof server, "%s", relay_env);
    } else {
        mock = mock_start();
        if (!mock) FAIL_RET("the mock relay did not start");
        snprintf(server, sizeof server, "127.0.0.1:%u", mock->port);
    }
    printf("coop-probe: sessions through %s relay %s, %u%% of received datagrams dropped\n", real ? "the real" : "the mock", server, drop);

    Player alice, bob;
    uint16_t alice_port = free_port();
    player_start(&alice, "Alice", 0xA11CE, server, drop, 1, alice_port);
    player_start(&bob, "Bob", 0xB0B, server, drop, 1, 0);
    if (!alice.sess || !bob.sess) FAIL_RET("a session did not start");
    CoopView v = view_of(&alice);
    CHECK(v.state == COOP_LEFT, "a session that is on starts in no room (state %d)", v.state);

    // --- a room is made and joined
    coop_sess_command(alice.sess, COOP_CMD_CREATE, ROOM_SECRET);
    CHECK(WAIT_FOR(view_of(&alice).state == COOP_JOINED, 30000), "Alice joined her room (state %d, %s)", view_of(&alice).state, view_of(&alice).status);
    v = view_of(&alice);
    CHECK(!strcmp(v.code, "H7K3WM-BCDFGH-234567") && v.self == 1 && v.n == 1 && v.made && v.maker == 1 && !strcmp(v.member[0].name, "Alice"),
          "Alice is member 1 and the maker of room %s (self %d, %d members, made %d)", v.code, v.self, v.n, v.made);
    coop_sess_command(bob.sess, COOP_CMD_JOIN, "h7k3wm-bcdfgh-234567");
    CHECK(WAIT_FOR(in_room(&bob, 2) && in_room(&alice, 2), ROSTER_WAIT_MS), "both see two members (Alice %d, Bob %d)", view_of(&alice).n, view_of(&bob).n);
    v = view_of(&bob);
    CHECK(v.self == 2 && v.maker == 1 && !v.made && v.n == 2 && !strcmp(v.member[0].name, "Alice") && !strcmp(v.member[1].name, "Bob"),
          "Bob is member 2 and sees Alice as the maker (self %d, maker %d, %d members)", v.self, v.maker, v.n);
    CHECK(WAIT_FOR(view_of(&bob).ping_ms > 0, 5000), "the round trip to the relay is measured");
    Player other;
    player_start(&other, "Other", 0x07E, server, 0, 0, 0);
    coop_sess_command(other.sess, COOP_CMD_CREATE, ROOM_SECRET);
    CHECK(WAIT_FOR(view_of(&other).state == COOP_LEFT && strstr(view_of(&other).status, "already open"), 30000),
          "a create of a room that is open is refused, not joined: %s", view_of(&other).status);
    player_end(&other);

    uint32_t first = 0;
    exchange(&alice, &bob, drop, "in the room", &first);
    CHECK(WAIT_FOR(view_of(&alice).member[1].heard_ms != UINT32_MAX, 3000), "a member whose messages opened counts as heard");

    if (mock) probe_hostile_relay(mock, &alice, &bob, alice_port, drop, &first);

    // --- the relay is lost for good and comes back
    int restarted = 1;
    if (mock) {
        mock->restart = 1;
        printf("coop-probe: the mock relay restarts\n");
    } else if (restart_mode && !strcmp(restart_mode, "stdin")) {
        printf("coop-probe: restart the relay now\n");
        fflush(stdout);
        char line[64] = "";
        if (!fgets(line, sizeof line, stdin) || strncmp(line, "restarted", 9)) CHECK(0, "the relay was not restarted (got \"%s\")", line);
    } else {
        printf("coop-probe: restart phase skipped: the relay is not ours (NX2NAVI_COOP_RESTART=stdin to do it by hand)\n");
        restarted = 0;
    }
    if (restarted) {
        CHECK(WAIT_FOR(alice.saw_reconnect && bob.saw_reconnect, 15000), "both sessions noticed that the relay was gone (%u, %u)",
              alice.saw_reconnect, bob.saw_reconnect);
        CHECK(WAIT_FOR(in_room(&alice, 2) && in_room(&bob, 2), 60000),
              "after a relay restart both are in a room again (Alice state %d %s, Bob state %d %s)", view_of(&alice).state,
              view_of(&alice).status, view_of(&bob).state, view_of(&bob).status);
        v = view_of(&bob);
        CHECK(!strcmp(v.code, "H7K3WM-BCDFGH-234567") && v.maker == view_of(&alice).self, "the maker made the room again under its secret (%s, maker %d)",
              v.code, v.maker);
        exchange(&alice, &bob, drop, "after the restart", &first);
    }

    // --- Bob leaves, and comes back
    coop_sess_command(bob.sess, COOP_CMD_LEAVE, "");
    CHECK(WAIT_FOR(view_of(&bob).state == COOP_LEFT, 5000), "Bob left (state %d)", view_of(&bob).state);
    CHECK(WAIT_FOR(in_room(&alice, 1), ROSTER_WAIT_MS), "Alice sees the room with only her in it (%d members)", view_of(&alice).n);
    if (!drop) CHECK(alice.byes > 0, "Alice got Bob's BYE");
    coop_sess_command(bob.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(in_room(&bob, 2) && in_room(&alice, 2), ROSTER_WAIT_MS), "Bob joined again");

    // --- the process's own session, with the settings the game reads
    probe_process(server, drop, &alice, &first);

    // --- a player who connects while the relay lies to it (the mock), and is then removed by the maker
    Player carol;
    player_start(&carol, "Carol", 0xCA201, server, 0, 0, 0);
    if (mock) mock->lie = 1;
    coop_sess_command(carol.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(in_room(&carol, 3), ROSTER_WAIT_MS), "Carol joins%s (state %d, %s)", mock ? " though the relay first answered with lies" : "",
          view_of(&carol).state, view_of(&carol).status);
    CHECK(WAIT_FOR(in_room(&alice, 3), ROSTER_WAIT_MS), "Alice sees Carol");
    int carol_id = view_of(&carol).self;
    CHECK(!coop_sess_remove(bob.sess, carol_id) || WAIT_FOR(in_room(&carol, 3), 2000), "Bob, who did not make the room, cannot remove Carol");
    plat_sleep_ms(500);
    CHECK(in_room(&carol, 3), "Carol is still in the room");
    CHECK(coop_sess_remove(alice.sess, carol_id), "the maker's removal is taken");
    CHECK(WAIT_FOR(view_of(&carol).state == COOP_LEFT && strstr(view_of(&carol).status, "removed"), ROSTER_WAIT_MS),
          "Carol is removed by the maker: %s", view_of(&carol).status);
    CHECK(WAIT_FOR(in_room(&alice, 2), ROSTER_WAIT_MS), "and gone from Alice's roster");
    coop_sess_command(carol.sess, COOP_CMD_JOIN, ROOM_SECRET);
    CHECK(WAIT_FOR(view_of(&carol).state == COOP_LEFT && strstr(view_of(&carol).status, "removed"), 30000) && in_room(&alice, 2),
          "and cannot come back as the same player: %s", view_of(&carol).status);
    player_end(&carol);

    // --- a room that is not there, a server that is not there
    Player dave;
    player_start(&carol, "Carol", 0xCA202, server, drop, 0, 0);
    coop_sess_command(carol.sess, COOP_CMD_JOIN, "BCDFGHBCDFGHBCDFGH");
    CHECK(WAIT_FOR(strstr(view_of(&carol).status, "no such room") != NULL, 30000), "Carol, joining a room that does not exist, is told so: %s",
          view_of(&carol).status);
    CHECK(view_of(&carol).state == COOP_LEFT, "and ends in no room (state %d)", view_of(&carol).state);
    player_end(&carol);
    uint16_t dead_port = free_port();
    char dead[40];
    snprintf(dead, sizeof dead, "127.0.0.1:%u", dead_port);
    player_start(&dave, "Dave", 0xDA7E, dead, 0, 0, 0);
    CHECK(coop_sess_command(dave.sess, COOP_CMD_CREATE, ""), "a create without a secret makes a new one");
    plat_sleep_ms(2500);
    v = view_of(&dave);
    char secret[COOP_SECRET_LEN + 1];
    CHECK(v.state == COOP_CONNECTING && strstr(v.status, "no answer") && coop_secret_normalize(v.code, secret) && strcmp(secret, ROOM_SECRET),
          "Dave, with nobody at the server, keeps trying with a new secret: state %d, %s", v.state, v.status);
    coop_sess_command(dave.sess, COOP_CMD_LEAVE, "");
    CHECK(WAIT_FOR(view_of(&dave).state == COOP_LEFT, 5000), "and leaves when told to");
    coop_sess_configure(dave.sess, &(CoopParams){.server = "", .name = "Dave", .on = 1});
    coop_sess_command(dave.sess, COOP_CMD_CREATE, "");
    CHECK(WAIT_FOR(view_of(&dave).state == COOP_LEFT && strstr(view_of(&dave).status, "no server"), 5000), "with no server configured a create says so: %s", view_of(&dave).status);
    player_end(&dave);

    // --- the end: LEAVE and BYE, state off
    player_end(&bob);
    CHECK(WAIT_FOR(in_room(&alice, 1), ROSTER_WAIT_MS), "Alice sees Bob go when his session ends");
    if (mock) {   // the relay restarts and someone else makes the room first: its maker joins it instead of leaving
        mock->steal = 1;
        mock->restart = 1;
        CHECK(WAIT_FOR(view_of(&alice).state == COOP_JOINED && !view_of(&alice).made && view_of(&alice).n == 2, 30000),
              "after a restart in which another made the room first, its maker joins it (state %d, made %d, %d members, %s)",
              view_of(&alice).state, view_of(&alice).made, view_of(&alice).n, view_of(&alice).status);
    }
    player_end(&alice);
    if (mock) mock_stop(mock);
}

int probe_coop(void) {
    probe_sockets();
    probe_crypto();
    probe_server_parse();
    probe_vectors();
    probe_fuzz();
    probe_pos_counter();
    probe_puppet_shown();
    fails += probe_coop_sync();   // 4. progress sync (coop_sync_probe.c)
    probe_stop_order();
    probe_sessions();
    printf("coop-probe: %s\n", fails ? "FAILED" : "passed");
    return fails != 0;
}
