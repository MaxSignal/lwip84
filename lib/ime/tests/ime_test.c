/* ime_test.c - the input methods on the host:
 *   gcc -I lib/ime -o ime_test lib/ime/tests/ime_test.c lib/ime/ime.c */
#include <stdio.h>
#include <string.h>
#include "ime.h"

static int fails;

/* keys: letters as typed, '<' is DEL; the composition is flushed at the end */
static void check(uint8_t lang, uint8_t up, const char *keys, const char *want)
{
    char out[IME_OUT_MAX], all[256] = "";
    const char *k;
    ime_lang = lang;
    ime_upper = up;
    ime_reset();
    for (k = keys; *k; k++) {
        if (*k == '<') {
            ime_back();
            continue;
        }
        ime_feed(*k, out);
        strcat(all, out);
    }
    ime_flush(out);
    strcat(all, out);
    if (strcmp(all, want)) {
        printf("FAIL %s: got %s, want %s\n", keys, all, want);
        fails++;
    }
}

int main(void)
{
    check(IME_JA, 0, "nihongo", "にほんご");
    check(IME_JA, 0, "kyouhaiitenkidesune.", "きょうはいいてんきですね。");
    check(IME_JA, 0, "kitte gakkou shinbun konnnichiha", "きって がっこう しんぶん こんにちは");
    check(IME_JA, 0, "kak<ta", "かた");
    check(IME_KATA, 0, "konpyu-ta-", "コンピューター");
    check(IME_KO, 0, "gksrmf dkssudgktpdy", "한글 안녕하세요");
    check(IME_KO, 0, "rkqt gottdj", "값 했어");
    check(IME_KO, 0, "rhkdgh<k", "광하");
    check(IME_KO, 0, "rrk", "까");
    check(IME_RU, 0, "privet, mir! shchuka", "привет, мир! щука");
    check(IME_RU, 1, "moskva", "МОСКВА");
    check(IME_EL, 0, "kalhme'ra ko'smos", "καλημέρα κόσμος");
    check(IME_EL, 1, "athhna", "ΑΘΗΝΑ");
    check(IME_AR, 0, "slam elykm", "سلام عليكم");
    check(IME_HI, 0, "namaste hindii bhaarat kamal", "नमस्ते हिन्दी भारत कमल");
    printf(fails ? "ime: %d failed\n" : "ime: all passed\n", fails);
    return fails != 0;
}
