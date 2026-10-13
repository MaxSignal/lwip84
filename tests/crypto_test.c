/* crypto_test.c - host test of lib/crypto/ against RFC test vectors:
 *   gcc -O2 -I lib/crypto -o /tmp/crypto_test tests/crypto_test.c lib/crypto/sha256.c lib/crypto/chacha.c lib/crypto/x25519.c
 *   /tmp/crypto_test */
#include <stdio.h>
#include <string.h>
#include "sha256.h"
#include "chacha.h"
#include "x25519.h"

static int fails;

static void unhex(uint8_t *out, const char *h)
{
    while (*h) {
        unsigned v;
        sscanf(h, "%2x", &v);
        *out++ = (uint8_t)v;
        h += 2;
    }
}

static void check(const char *name, const uint8_t *got, const char *want)
{
    uint8_t w[256];
    size_t n = strlen(want) / 2;
    unhex(w, want);
    if (memcmp(got, w, n)) {
        size_t i;
        printf("FAIL %s\n  got  ", name);
        for (i = 0; i < n; i++)
            printf("%02x", got[i]);
        printf("\n  want %s\n", want);
        fails++;
    } else {
        printf("ok   %s\n", name);
    }
}

int main(void)
{
    uint8_t out[64], key[32], nonce[12], buf[256], zero[32] = {0};
    sha256(  "abc", 3, out);
    check("sha256 abc", out, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    {
        static uint8_t m[1000];
        memset(m, 'a', sizeof(m));
        sha256_ctx c;
        int i;
        sha256_init(&c);
        for (i = 0; i < 1000; i++)
            sha256_update(&c, m, 1000);
        sha256_final(&c, out);
        check("sha256 1M a", out, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
    }
    hmac_sha256((const uint8_t *)"Jefe", 4, "what do ya want for nothing?", 28, out);
    check("hmac rfc4231 #2", out, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
    hkdf_extract(zero, zero, key);
    check("tls13 early secret", key, "33ad0a1c607ec03b09e6cd9893680ce210adf300aa1f2660e1b22e10f170f92a");
    sha256("", 0, buf);
    hkdf_expand_label(key, "derived", buf, 32, out, 32);
    check("tls13 derived", out, "6f2615a108c702c5678f54fc9dbab69716c076189c48250cebeac3576c3611ba");

    {
        chacha_ctx c;
        const char *pt = "Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.";
        size_t n = strlen(pt), i;
        for (i = 0; i < 32; i++)
            key[i] = (uint8_t)i;
        unhex(nonce, "000000000000004a00000000");
        memcpy(buf, pt, n);
        chacha_init(&c, key, nonce, 12, 1);
        chacha_xor(&c, buf, 10);
        chacha_xor(&c, buf + 10, n - 10);
        check("chacha20 rfc8439 2.4.2", buf,
              "6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab8f593dabcd62b3571639d624e65152ab8f530c359f0861d807ca0dbf500d6a6156a38e088a22b65e52bc514d16ccf806818ce91ab77937365af90bbf74a35be6b40b8eedf2785e42874d");
    }
    {
        poly1305_ctx p;
        unhex(key, "85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b");
        poly1305_init(&p, key);
        poly1305_update(&p, (const uint8_t *)"Cryptographic Forum ", 20);
        poly1305_update(&p, (const uint8_t *)"Research Group", 14);
        poly1305_final(&p, out);
        check("poly1305 rfc8439 2.5.2", out, "a8061dc1305136c6c22b8baf0c0127a9");
    }
    {
        uint8_t s[32], u[32];
        unhex(s, "a546e36bf0527c9d3b16154b82465edd62144c0ac1fc5a18506a2244ba449ac4");
        unhex(u, "e6db6867583030db3594c1a424b15f7c726624ec26b3353b10a903a6d0ab1c4c");
        x25519(out, s, u);
        check("x25519 rfc7748 #1", out, "c3da55379de9c6908e94ea4df28d084f32eccf03491c71f754b4075577a28552");
        unhex(s, "77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
        memset(u, 0, 32);
        u[0] = 9;
        x25519(out, s, u);
        check("x25519 alice pub", out, "8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a");
    }
    printf("%s\n", fails ? "FAILED" : "all passed");
    return fails != 0;
}
