/* ime_test.c - the input methods on the host:
 *   gcc -DDICT_HOST -I lib/ime -o ime_test lib/ime/tests/ime_test.c lib/ime/ime.c \
 *       lib/ime/dict.c
 *   ime_test [DICT.bin]
 * With the dictionaries of tools/mkdict84.py it also checks conversion. */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "ime.h"
#include "dict.h"

static uint8_t *dict_blob;
static long dict_len;

void far_read(uint8_t page, uint16_t addr, void *dst, uint16_t len)
{
    long off = (long)(page - DICT_PAGE) * 0x4000 + (addr - 0x4000);
    memset(dst, 0xFF, len);
    if (dict_blob && off >= 0 && off + len <= dict_len)
        memcpy(dst, dict_blob + off, len);
}

static int fails;

/* keys: letters as typed, '<' is DEL, '>' next candidate, '^' previous,
 * '[' ']' shorter/longer part, '=' commit the candidate; the composition is
 * flushed at the end */
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
        if (*k == '>' || *k == '^') {
            ime_conv_next(*k == '>' ? 1 : -1);
            continue;
        }
        if (*k == '[' || *k == ']') {
            ime_conv_resize(*k == ']' ? 1 : -1);
            continue;
        }
        if (*k == '=') {
            ime_conv_commit(out);
            strcat(all, out);
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

int main(int argc, char **argv)
{
    if (argc > 1) {
        FILE *f = fopen(argv[1], "rb");
        if (!f)
            return 1;
        dict_blob = malloc(1 << 20);
        dict_len = (long)fread(dict_blob, 1, 1 << 20, f);
        fclose(f);
        dict_init();
    }
    check(IME_JA, 0, "nihongo", "にほんご");
    check(IME_JA, 0, "kyouhaiitenkidesune.", "きょうはいいてんきですね。");
    if (!dict_ok)
        check(IME_JA, 0, "kitte gakkou shinbun konnnichiha", "きってがっこうしんぶんこんにちは");
    check(IME_KATA, 0, "kitte gakkou", "キッテ ガッコウ");
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
    if (dict_ok) {
        char l[200];
        check(IME_JA, 0, "watashi =", "私");
        check(IME_JA, 0, "gakusei =desu", "学生です");
        check(IME_JA, 0, "tenki =", "天気");
        check(IME_JA, 0, "kanji =", "感じ");
        check(IME_JA, 0, "kanji >=", "漢字");
        check(IME_JA, 0, "toukyouniiku =", "東京にいく");
        check(IME_JA, 0, "kanji ^=", "カンジ");
        check(IME_JA, 0, "kanji ^^=", "かんじ");
        check(IME_ZH, 0, "ni =hao =", "你好");
        check(IME_ZH, 0, "zhongguo =", "中guo");
        check(IME_ZH, 0, "zhongguo = =", "中国");
        check(IME_ZH, 0, "wo, ", "wo， ");
        ime_lang = IME_JA;
        ime_reset();
        {
            char o[IME_OUT_MAX];
            const char *k;
            for (k = "kanji "; *k; k++)
                ime_feed(*k, o);
        }
        ime_cand_list(l, 60);
        printf("candidates: %s\n", l);
    }
    printf(fails ? "ime: %d failed\n" : "ime: all passed\n", fails);
    return fails != 0;
}
