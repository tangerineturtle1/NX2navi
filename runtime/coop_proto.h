// The co-op wire format, as pure functions on bytes: no sockets, no threads, no game state. docs/coop.md ("The room's
// secret and the wire") is the description a reviewer checks this file against.
//
// A room is named by a secret the players share out of band: 18 characters of a 27-symbol alphabet (85 bits), shown
// XXXXXX-XXXXXX-XXXXXX. The relay never sees it. Two values are derived from it with keyed BLAKE2b (the key is the
// secret's 18 canonical characters): the room's locator (16 bytes), which the relay knows the room by and which keys
// the tag of every datagram between a client and the relay, and the message key (32 bytes), which seals every message
// between the players (XChaCha20-Poly1305), so that the relay and anyone on the path can neither read nor change nor
// make one.
//
// A datagram is a 24-byte header (little-endian) and a payload; the header carries an 8-byte tag, the start of an
// HMAC-SHA256 over the first 16 header bytes and the payload, keyed with the locator. The client and relay messages
// are CONNECT .. LEAVE and KICK (tools/coop_relay.py, the same format in Python); DATA carries a sealed player message:
// the envelope (sender's session, counter) and, sealed, one kind byte and the message bytes the layers above coop.c
// define. tests/coop_vectors.txt holds byte vectors that this file and the relay's Python are checked against
// (--probe-coop, tests/coop_relay_test.py). Every parser checks every length against the bytes it was given and fails
// closed.
#pragma once
#include <stddef.h>
#include <stdint.h>

enum {
    COOP_MAGIC = 0x434E,         // bytes 4E 43
    COOP_PROTO = 2,              // protocol major: a relay and a client talk only when it is equal
    COOP_HDR_LEN = 24,
    COOP_MAX_DGRAM = 1200,       // a datagram's size at most: it crosses any path without fragmenting
    COOP_MAX_PAYLOAD = COOP_MAX_DGRAM - COOP_HDR_LEN,
    COOP_TAG_LEN = 8,
    COOP_KEY_LEN = 16,           // the locator, which keys the tags
    COOP_MSG_KEY_LEN = 32,
    COOP_DST_ALL = 0xFF,         // dst: every other member of the room
    COOP_SECRET_LEN = 18,        // characters of a room secret
    COOP_SECRET_TEXT = COOP_SECRET_LEN + 2,   // shown with two '-'
    COOP_MAX_MEMBERS = 8,
    COOP_NAME_MAX = 24,          // a player's name in bytes of UTF-8
    COOP_COOKIE_LEN = 16,
    COOP_CONNECT_MIN = 64,       // a CONNECT datagram's smallest size, so that no answer to it is larger
    COOP_ENV_V1 = 1,             // the envelope of a player message
    COOP_ENV_HDR = 13,           // u8 envelope version, u64 session, u32 counter
    COOP_ENV_MAC = 16,
    COOP_ENV_OVERHEAD = COOP_ENV_HDR + COOP_ENV_MAC,
    COOP_MAX_MESSAGE = COOP_MAX_PAYLOAD - COOP_ENV_OVERHEAD - 1,   // a player message's bytes after its kind, at most
};

// Datagram types. CONNECT .. LEAVE and KICK are between a client and the relay, DATA carries the players' messages.
enum { COOP_T_CONNECT = 1, COOP_T_CHALLENGE = 2, COOP_T_JOINED = 3, COOP_T_ERROR = 5, COOP_T_ROSTER = 6, COOP_T_PING = 7,
       COOP_T_PONG = 8, COOP_T_LEAVE = 9, COOP_T_KICK = 11, COOP_T_DATA = 0x10 };
enum { COOP_INTENT_CREATE = 1, COOP_INTENT_JOIN = 2 };   // CONNECT: make the room of a locator or join it
enum { COOP_E_NO_ROOM = 1, COOP_E_ROOM_FULL = 2, COOP_E_PROTO = 3, COOP_E_RATE = 4, COOP_E_BAD_TAG = 5, COOP_E_SERVER_FULL = 6,
       COOP_E_ROOM_EXISTS = 7, COOP_E_REMOVED = 8, COOP_E_SESSION_USED = 9 };   // 9: a session used in the room before
enum { COOP_FLAG_AWAY = 1, COOP_FLAG_MAKER = 2 };   // roster flags: not heard from for a while; made the room

// The kinds of player messages (the first byte of a sealed message). coop.c only carries them; HELLO, POS, DIGEST,
// SECTION, SYNC_NEED and NOTICE belong to the layers that send them. A kind of 0xF0 and above is free for tests.
enum { COOP_K_HELLO = 1, COOP_K_POS = 2, COOP_K_DIGEST = 3, COOP_K_SECTION = 4, COOP_K_SYNC_NEED = 5, COOP_K_NOTICE = 6,
       COOP_K_BYE = 7, COOP_K_TEST = 0xF0 };

typedef struct {
    uint8_t ver, type;
    uint8_t src, dst;       // member ids 1..8; src 0 is the relay; dst COOP_DST_ALL: every other member
    uint16_t len;           // payload bytes
    uint32_t room, seq;     // room 0 until joined; seq counts the sender's datagrams
} CoopHdr;

// ---- room secrets: 18 characters of "23456789BCDFGHJKMNPRSTVWXYZ" (27 symbols, 85.6 bits; no vowels, 0/1/O/I/L, Q or
// E, so a secret spoken or typed does not read as a word), shown as XXXXXX-XXXXXX-XXXXXX. Any ASCII case, '-' and spaces
// ignored. 1 and the canonical 18 characters plus a NUL in out when text is a secret.
int coop_secret_normalize(const char *text, char out[COOP_SECRET_LEN + 1]);
void coop_secret_format(const char secret[COOP_SECRET_LEN], char out[COOP_SECRET_TEXT + 1]);
// A new secret from 32 random bytes (each symbol from one byte below 243, the rest skipped); 0 when they ran out.
int coop_secret_from_random(const uint8_t random[32], char out[COOP_SECRET_LEN + 1]);
// The secret's first characters and an ellipsis, for logs: "XXX…".
void coop_secret_short(const char secret[COOP_SECRET_LEN], char out[8]);

// ---- keys. BLAKE2b keyed with the secret's canonical characters: locator = BLAKE2b-128("nx2navi-coop/2 room"),
// message key = BLAKE2b-256("nx2navi-coop/2 messages"). The tag of a datagram: HMAC-SHA256(locator, header[0:16] +
// payload)[0:8].
void coop_hmac_sha256(const void *key, size_t klen, const void *a, size_t an, const void *b, size_t bn, uint8_t out[32]);
void coop_room_keys(const char secret[COOP_SECRET_LEN], uint8_t locator[COOP_KEY_LEN], uint8_t msg_key[COOP_MSG_KEY_LEN]);

// ---- datagrams
// The datagram of hdr (its len is ignored: n is the payload length) and payload into out. key NULL: a zero tag (what a
// relay of another protocol version answers). Bytes written, 0 when it does not fit cap or n is too long.
size_t coop_dgram_pack(uint8_t *out, size_t cap, const CoopHdr *hdr, const void *payload, size_t n,
                       const uint8_t key[COOP_KEY_LEN]);
// 1 when d is exactly one datagram of this format (magic, a length that matches); *payload points into d. The version
// and the tag are not checked here.
int coop_dgram_parse(const uint8_t *d, size_t n, CoopHdr *hdr, const uint8_t **payload);
// 1 when the tag of the parsed datagram d (n bytes) is the one key makes; time independent of where they differ.
int coop_dgram_verify(const uint8_t *d, size_t n, const uint8_t key[COOP_KEY_LEN]);
int coop_dgram_tag_zero(const uint8_t *d);

// ---- names: a player's name from untrusted bytes. At most COOP_NAME_MAX bytes of valid UTF-8; a control character
// (C0, DEL, C1) and each byte that is not valid UTF-8 become '?', and so do the characters that reorder text (bidi
// controls). out is NUL-terminated (cap at least COOP_NAME_MAX + 1); the length.
size_t coop_name_clean(char *out, size_t cap, const uint8_t *raw, size_t n);

// ---- client and relay messages
// CONNECT: u8 intent, u8 flags, u8[16] locator, u64 client id, u64 session, u64 nonce, u8 name length, name,
// u8[16] cookie (zero, or the CHALLENGE's), u8[16] rejoin token (zero, or the JOINED's of this room).
typedef struct {
    uint8_t intent, flags;
    uint8_t locator[COOP_KEY_LEN];
    uint64_t client_id;     // random for each start of the game; the relay knows a rejoin of the same member by it
    uint64_t session;       // random for each join: the others take messages only of the session the roster names
    uint64_t nonce;         // random for each attempt: every answer carries it
    char name[COOP_NAME_MAX + 1];
    uint8_t cookie[COOP_COOKIE_LEN], token[COOP_COOKIE_LEN];
} CoopConnect;
size_t coop_connect_pack(uint8_t *out, size_t cap, const CoopConnect *c);   // zero-padded to COOP_CONNECT_MIN as a datagram
int coop_connect_parse(const uint8_t *p, size_t n, CoopConnect *c);

// CHALLENGE: u64 nonce, u8[16] cookie.
size_t coop_challenge_pack(uint8_t *out, size_t cap, uint64_t nonce, const uint8_t cookie[COOP_COOKIE_LEN]);
int coop_challenge_parse(const uint8_t *p, size_t n, uint64_t *nonce, uint8_t cookie[COOP_COOKIE_LEN]);

// ROSTER: u8 n, n x (u8 id, u8 flags, u64 session, u8 name length, name).
typedef struct { uint8_t id, flags; uint64_t session; char name[COOP_NAME_MAX + 1]; } CoopMemberInfo;
typedef struct { uint8_t n; CoopMemberInfo m[COOP_MAX_MEMBERS]; } CoopRoster;
size_t coop_roster_pack(uint8_t *out, size_t cap, const CoopRoster *r);
// used (may be NULL): the bytes the roster took.
int coop_roster_parse(const uint8_t *p, size_t n, CoopRoster *r, size_t *used);

// JOINED: u8 member, u64 nonce, u8[16] rejoin token, roster.
typedef struct { uint8_t member; uint64_t nonce; uint8_t token[COOP_COOKIE_LEN]; CoopRoster roster; } CoopJoined;
size_t coop_joined_pack(uint8_t *out, size_t cap, const CoopJoined *j);
int coop_joined_parse(const uint8_t *p, size_t n, CoopJoined *j);

// ERROR: u8 reason, u64 nonce (of the CONNECT it answers, 0 else), u8 text length, text.
typedef struct { uint8_t reason; uint64_t nonce; char text[61]; } CoopError;   // text for the log, at most 60 bytes
size_t coop_error_pack(uint8_t *out, size_t cap, const CoopError *e);
int coop_error_parse(const uint8_t *p, size_t n, CoopError *e);

// KICK (the room's maker to the relay): u8 member, u64 its session.
size_t coop_kick_pack(uint8_t *out, size_t cap, uint8_t member, uint64_t session);
int coop_kick_parse(const uint8_t *p, size_t n, uint8_t *member, uint64_t *session);

size_t coop_u32_pack(uint8_t *out, size_t cap, uint32_t v);   // PING and PONG: the token
int coop_u32_parse(const uint8_t *p, size_t n, uint32_t *v);

// ---- player messages: the payload of a DATA datagram, sealed with the message key.
//   u8 envelope version (COOP_ENV_V1), u64 sender's session, u32 counter, ciphertext of (u8 kind, bytes), u8[16] tag
// XChaCha20-Poly1305: nonce = session (8 bytes LE), counter (4 LE), src (1), 11 zero bytes; associated data = dst and
// the envelope version. A session and counter are used once (a new session for every join, the counter from 1), so a
// nonce never repeats under the room's key. A later envelope version can carry other fields; the kind byte and the
// message inside stay as they are, so a new kind (a pose, say) only needs a number.
// Seal: the payload into out (cap); its length, 0 when n is above COOP_MAX_MESSAGE or it does not fit.
size_t coop_seal(uint8_t *out, size_t cap, const uint8_t key[COOP_MSG_KEY_LEN], uint8_t src, uint8_t dst, uint64_t session,
                 uint32_t counter, uint8_t kind, const void *bytes, size_t n);
// The envelope's clear fields; 0 when p is not an envelope of a version this build reads.
int coop_envelope_peek(const uint8_t *p, size_t n, uint64_t *session, uint32_t *counter);
// Open: 1 and the kind and bytes (into out, cap at least n) when the tag is right for this key, src and dst.
int coop_open(const uint8_t *p, size_t n, const uint8_t key[COOP_MSG_KEY_LEN], uint8_t src, uint8_t dst, uint8_t *kind,
              uint8_t *out, size_t cap, size_t *nout);

// ---- signatures: EdDSA over Curve25519 with BLAKE2b (the library's crypto_eddsa). The room's message key cannot tell
// one member from another, and the sync layer's state travels through every member, so what only one member may make
// is signed with that member's key: its identity in the room, the maker's rules, and each pool contributor's counts
// (coop_state.c). Every member can then pass them on, and nobody but the key's holder can make or change them.
enum { COOP_PK_LEN = 32, COOP_SK_LEN = 64, COOP_SIG_LEN = 64 };
void coop_sign_keypair(const uint8_t seed[32], uint8_t sk[COOP_SK_LEN], uint8_t pk[COOP_PK_LEN]);
void coop_sign(uint8_t sig[COOP_SIG_LEN], const uint8_t sk[COOP_SK_LEN], const void *msg, size_t n);
int coop_sign_check(const uint8_t sig[COOP_SIG_LEN], const uint8_t pk[COOP_PK_LEN], const void *msg, size_t n);   // 1: valid
// BLAKE2b of msg keyed with key (klen at most 64, 0 for none), outlen bytes (1 to 64): the sync layer's derivations.
void coop_blake2b(uint8_t *out, size_t outlen, const void *key, size_t klen, const void *msg, size_t n);

// ---- POS (COOP_K_POS): where a player is and what Link plays, for showing the other players (nothing sends it in this build), meant to be sent about 15
// times a second as a sealed player message (above). The bytes after the kind, little endian: u8 version
// (COOP_POS_VERSION), u32 pseq (the sender's counter: one more for each POS it sends), u32 t_ms (the sender's clock), u8
// flags, u8 area length (<= COOP_POS_TEXT) and the area's name (the game's current-area text), f32 x y z, f32 yaw
// (radians, 0 facing +z), u8 slot count (<= COOP_POS_SLOTS), per AS slot u8 node name length (<= COOP_POS_TEXT), the
// node's name, f32 frame. A parser takes exactly one message of its version: another version is another format this
// build cannot read (version 1 had a u16 pseq). The names come from another player's game and reach this one's log and
// its animation lookup, so a name with a byte outside printable ASCII (0x20..0x7e) is refused, and so is a number that
// is not finite.
#define COOP_POS_VERSION 2
#define COOP_POS_TEXT 47
#define COOP_POS_SLOTS 4
enum { COOP_POS_ADULT = 1, COOP_POS_IN_WORLD = 2 };
typedef struct {
    uint32_t pseq;
    uint32_t t_ms;
    uint8_t flags, nslots;
    char area[COOP_POS_TEXT + 1];
    float pos[3], yaw;
    struct { char node[COOP_POS_TEXT + 1]; float frame; } slot[COOP_POS_SLOTS];
} CoopPos;
size_t coop_pos_pack(uint8_t *out, size_t cap, const CoopPos *p);   // 0 when it does not fit cap
int coop_pos_parse(const uint8_t *p, size_t n, CoopPos *out);

// The receiver's counter of one POS sender (coop.h: a handler is not called once only). A pose is taken only above the
// last one taken from the same member id in the same session, so neither a pose sent again nor one recorded earlier
// in the session moves the puppet back while that sender's poses keep being taken; the first pose of another session
// (another player given the id, the member back in a new session, or a roster that lists an earlier session) starts the
// counter again. After COOP_POS_QUIET_MS with none taken (below), whoever holds the sender's datagrams back that long can
// send its recorded poses of the session again (no secret needed), and they are taken: only the puppet moves. Someone
// with the room's secret can seal a POS in a member's name with a counter far ahead: it becomes the newest pose, so the
// puppet stands where it says (or is hidden, if it names another area) and the member's own poses are kept out until
// nothing was taken from the sender for COOP_POS_QUIET_MS; then the next pose is taken whatever its counter and starts
// the counter again. One forged pose every COOP_POS_QUIET_MS keeps the member's own out while the forger goes on.
// session: the one the roster lists for the member (CoopView). Returns 0 to drop the pose, 1 to take it, 2 to take it
// and drop what was kept of the sender's earlier poses.
#define COOP_POS_QUIET_MS 3000
typedef struct {
    uint64_t session, at_ms;    // the session of the last pose taken, and when it was taken
    uint32_t last;              // its counter
    int any;                    // a pose was taken
} CoopPosSeen;
int coop_pos_fresh(CoopPosSeen *s, uint64_t session, uint32_t pseq, uint64_t now_ms);
