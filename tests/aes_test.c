/* aes_test.c - AES-128 and AES-128-GCM against vectors made with Python's
 * cryptography package (tests/aes_vectors.h, tests/aes_vectors.py) */
#include <stdio.h>
#include <string.h>
#include "aes.h"
#include "aes_vectors.h"

int main(void)
{
    unsigned i, fails = 0;
    for (i = 0; i < sizeof(vec) / sizeof(vec[0]); i++) {
        const struct vector *v = &vec[i];
        uint8_t data[64], tag[16], rk[176], blk[16];
        aes_ctr_ctx c;
        aes128_expand(rk, v->key);
        aes128_encrypt(rk, v->pt, blk);
        if (memcmp(blk, v->ecb, 16)) {
            printf("FAIL ecb %u\n", i);
            fails++;
        }
        memcpy(data, v->pt, v->len);
        if (!aes_gcm_seal(v->key, v->nonce, v->aad, v->aad_len, data, v->len, tag) ||
            memcmp(data, v->ct, v->len) || memcmp(tag, v->tag, 16)) {
            printf("FAIL gcm %u\n", i);
            fails++;
        }
        /* decryption: the same keystream from counter 2, fed in odd pieces */
        c.ready = 0;
        aes_ctr_init(&c, v->key, v->nonce, 2);
        aes_ctr_xor(&c, data, 3);
        aes_ctr_xor(&c, data + 3, v->len > 3 ? v->len - 3 : 0);
        if (memcmp(data, v->pt, v->len)) {
            printf("FAIL ctr %u\n", i);
            fails++;
        }
    }
    printf("%s (%u vectors)\n", fails ? "FAILED" : "aes: all passed", i);
    return fails != 0;
}
