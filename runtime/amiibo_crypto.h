// Amiibo tag encryption: the keys file the player supplies, decrypting a 540-byte NTAG215 dump and checking its two
// signatures, and encrypting and signing it again after the game changed it (amiibo.c amiibo_flush_figure).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "amiibo.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AMIIBO_KEYS_SIZE 160   // two 80-byte master keys: the data key, then the tag key

// Loads the keys file. False when it is missing, has another size or fails the format check; the keys loaded before are
// dropped either way. Key bytes are never logged, printed or written anywhere.
bool amiibo_keys_load(const char *path);
bool amiibo_keys_loaded(void);
void amiibo_keys_clear(void);

// A decrypted dump. Its layout is private to amiibo_crypto.c; read and change it through amiibo_plain_read/_write.
typedef struct {
    uint8_t b[AMIIBO_DUMP_SIZE];
} AmiiboPlain;

// Decrypts tag into out and checks both signatures. False without keys or when a signature does not match: the figure
// is then used without its stored data (UID and model only).
bool amiibo_decrypt(const uint8_t tag[AMIIBO_DUMP_SIZE], AmiiboPlain *out);
// Encrypts and signs plain into tag. False without keys.
bool amiibo_encrypt(const AmiiboPlain *plain, uint8_t tag[AMIIBO_DUMP_SIZE]);

// The fields of a decrypted figure that nn::nfp reports or the game writes.
typedef struct {
    uint8_t flags;                 // AMIIBO_FLAG_*
    uint8_t country_code;
    uint16_t write_counter;        // the figure's writes
    uint16_t setup_year;           // owner registration date
    uint8_t setup_month, setup_day;
    uint16_t write_year;           // last write date
    uint8_t write_month, write_day;
    char nickname[41];             // UTF-8 (at most 10 characters), NUL-terminated
    uint8_t mii[0x60];             // the owner's Mii as the tag stores it
    uint64_t title_id;             // program that created the application area
    uint16_t app_write_counter;    // writes of the application area
    uint32_t access_id;            // the application area's owner
    uint8_t app_area[AMIIBO_APP_AREA_SIZE];
} AmiiboFields;

#define AMIIBO_FLAG_REGISTERED 0x10u   // owner and nickname set
#define AMIIBO_FLAG_APP_AREA   0x20u   // an application area exists

void amiibo_plain_read(const AmiiboPlain *p, AmiiboFields *out);
// Stores every field of in (the nickname encoded again) and leaves the rest of p as it was.
void amiibo_plain_write(AmiiboPlain *p, const AmiiboFields *in);

// Known-answer tests of AES-128, HMAC-SHA256 and a decrypt/encrypt round trip with made-up keys. Logs the first failure
// and returns false. Called by ov_probe.c.
bool amiibo_crypto_selftest(void);

#ifdef __cplusplus
}
#endif
