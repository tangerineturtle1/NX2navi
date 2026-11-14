// AES-128 encryption (FIPS-197) and counter mode (SP 800-38A) with a 128-bit big-endian counter. Portable C; the
// amiibo tag cipher (amiibo_crypto.c) is its only user. Decryption is not needed: counter mode only encrypts.
#pragma once
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t rk[176];   // the 11 round keys, 16 bytes each
} Aes128;

void aes128_init(Aes128 *a, const uint8_t key[16]);
void aes128_encrypt_block(const Aes128 *a, const uint8_t in[16], uint8_t out[16]);   // in and out may be the same
// out = in XOR the key stream of the counter blocks iv, iv + 1, ... (the whole 16 bytes as one big-endian number,
// wrapping at 2^128). n need not be a multiple of 16; in and out may be the same buffer.
void aes128_ctr(const Aes128 *a, const uint8_t iv[16], const uint8_t *in, uint8_t *out, size_t n);

#ifdef __cplusplus
}
#endif
