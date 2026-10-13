/* ime.c - input methods (see ime.h).
 *
 * Japanese, Cyrillic, Greek, Arabic and Devanagari are transliterated:
 * typed letters collect in rom[] until they match an entry of the
 * language's table and no longer entry could still match, then the entry's
 * text is emitted.  Hangul follows the dubeolsik layout and builds the
 * syllable from its jamo. */
#include <string.h>
#include "ime.h"

uint8_t ime_lang, ime_upper;
uint16_t ime_enabled = 0xFFFF;
char ime_pre[IME_PRE_MAX + 1];

static char rom[8];             /* letters not converted yet */
static uint8_t rom_n;
static uint8_t kana_n;          /* Japanese: kana at the start of ime_pre */
static uint8_t hi_cons;         /* Devanagari: last letter was a consonant */
static char *out_p;             /* where emitted text goes */

/* tables: "latin\0text\0" ... "\0" */

/* Japanese romaji (as Microsoft's IME); katakana is hiragana + 60h */
static const char ja_bk[] =
    "a\0あ\0i\0い\0u\0う\0e\0え\0o\0お\0"
    "ka\0か\0ki\0き\0ku\0く\0ke\0け\0ko\0こ\0"
    "sa\0さ\0si\0し\0shi\0し\0su\0す\0se\0せ\0so\0そ\0"
    "ta\0た\0ti\0ち\0chi\0ち\0tu\0つ\0tsu\0つ\0te\0て\0to\0と\0"
    "na\0な\0ni\0に\0nu\0ぬ\0ne\0ね\0no\0の\0"
    "ha\0は\0hi\0ひ\0hu\0ふ\0fu\0ふ\0he\0へ\0ho\0ほ\0"
    "ma\0ま\0mi\0み\0mu\0む\0me\0め\0mo\0も\0"
    "ya\0や\0yu\0ゆ\0yo\0よ\0ye\0いぇ\0"
    "ra\0ら\0ri\0り\0ru\0る\0re\0れ\0ro\0ろ\0"
    "wa\0わ\0wi\0うぃ\0we\0うぇ\0wo\0を\0"
    "n\0ん\0nn\0ん\0n'\0ん\0"
    "ga\0が\0gi\0ぎ\0gu\0ぐ\0ge\0げ\0go\0ご\0"
    "za\0ざ\0zi\0じ\0ji\0じ\0zu\0ず\0ze\0ぜ\0zo\0ぞ\0"
    "da\0だ\0di\0ぢ\0du\0づ\0de\0で\0do\0ど\0"
    "ba\0ば\0bi\0び\0bu\0ぶ\0be\0べ\0bo\0ぼ\0"
    "pa\0ぱ\0pi\0ぴ\0pu\0ぷ\0pe\0ぺ\0po\0ぽ\0"
    "va\0ゔぁ\0vi\0ゔぃ\0vu\0ゔ\0ve\0ゔぇ\0vo\0ゔぉ\0"
    "kya\0きゃ\0kyi\0きぃ\0kyu\0きゅ\0kye\0きぇ\0kyo\0きょ\0"
    "sya\0しゃ\0syu\0しゅ\0sye\0しぇ\0syo\0しょ\0"
    "sha\0しゃ\0shu\0しゅ\0she\0しぇ\0sho\0しょ\0"
    "tya\0ちゃ\0tyu\0ちゅ\0tye\0ちぇ\0tyo\0ちょ\0"
    "cha\0ちゃ\0chu\0ちゅ\0che\0ちぇ\0cho\0ちょ\0"
    "cya\0ちゃ\0cyu\0ちゅ\0cyo\0ちょ\0"
    "nya\0にゃ\0nyu\0にゅ\0nyo\0にょ\0"
    "hya\0ひゃ\0hyu\0ひゅ\0hyo\0ひょ\0"
    "mya\0みゃ\0myu\0みゅ\0myo\0みょ\0"
    "rya\0りゃ\0ryu\0りゅ\0ryo\0りょ\0"
    "gya\0ぎゃ\0gyu\0ぎゅ\0gyo\0ぎょ\0"
    "zya\0じゃ\0zyu\0じゅ\0zyo\0じょ\0"
    "ja\0じゃ\0ju\0じゅ\0je\0じぇ\0jo\0じょ\0jya\0じゃ\0jyu\0じゅ\0jyo\0じょ\0"
    "dya\0ぢゃ\0dyu\0ぢゅ\0dyo\0ぢょ\0"
    "bya\0びゃ\0byu\0びゅ\0byo\0びょ\0"
    "pya\0ぴゃ\0pyu\0ぴゅ\0pyo\0ぴょ\0"
    "fa\0ふぁ\0fi\0ふぃ\0fe\0ふぇ\0fo\0ふぉ\0fyu\0ふゅ\0"
    "thi\0てぃ\0thu\0てゅ\0dhi\0でぃ\0dhu\0でゅ\0twu\0とぅ\0dwu\0どぅ\0"
    "tsa\0つぁ\0tsi\0つぃ\0tse\0つぇ\0tso\0つぉ\0"
    "wha\0うぁ\0whi\0うぃ\0whe\0うぇ\0who\0うぉ\0"
    "ca\0か\0ci\0し\0cu\0く\0ce\0せ\0co\0こ\0"
    "qa\0くぁ\0qi\0くぃ\0qu\0く\0qe\0くぇ\0qo\0くぉ\0"
    "xa\0ぁ\0xi\0ぃ\0xu\0ぅ\0xe\0ぇ\0xo\0ぉ\0"
    "la\0ぁ\0li\0ぃ\0lu\0ぅ\0le\0ぇ\0lo\0ぉ\0"
    "xya\0ゃ\0xyu\0ゅ\0xyo\0ょ\0lya\0ゃ\0lyu\0ゅ\0lyo\0ょ\0"
    "xtu\0っ\0ltu\0っ\0xtsu\0っ\0xwa\0ゎ\0lwa\0ゎ\0xka\0ゕ\0xke\0ゖ\0"
    "-\0ー\0,\0、\0.\0。\0?\0？\0!\0！\0[\0「\0]\0」\0~\0〜\0/\0・\0"
    "\0";

/* Russian */
static const char ru_bk[] =
    "a\0а\0b\0б\0v\0в\0g\0г\0d\0д\0e\0е\0yo\0ё\0jo\0ё\0zh\0ж\0z\0з\0"
    "i\0и\0j\0й\0k\0к\0l\0л\0m\0м\0n\0н\0o\0о\0p\0п\0r\0р\0s\0с\0"
    "t\0т\0u\0у\0f\0ф\0h\0х\0x\0х\0kh\0х\0c\0ц\0ch\0ч\0sh\0ш\0shh\0щ\0shch\0щ\0sch\0щ\0"
    "w\0щ\0y\0ы\0'\0ь\0''\0ъ\0e'\0э\0yu\0ю\0ju\0ю\0ya\0я\0ja\0я\0"
    "\0";

/* Greek ("Greeklish"); s before a non-letter is the final sigma */
static const char el_bk[] =
    "a\0α\0b\0β\0v\0β\0g\0γ\0d\0δ\0e\0ε\0z\0ζ\0h\0η\0th\0θ\0i\0ι\0"
    "k\0κ\0l\0λ\0m\0μ\0n\0ν\0x\0ξ\0ks\0ξ\0o\0ο\0p\0π\0r\0ρ\0s\0σ\0"
    "t\0τ\0u\0υ\0y\0υ\0f\0φ\0c\0χ\0ch\0χ\0ps\0ψ\0w\0ω\0"
    "a'\0ά\0e'\0έ\0h'\0ή\0i'\0ί\0o'\0ό\0u'\0ύ\0y'\0ύ\0w'\0ώ\0"
    "\0";

/* Arabic (doubled letters for the emphatics and h) */
static const char ar_bk[] =
    "a\0ا\0aa\0آ\0a'\0أ\0i'\0إ\0b\0ب\0t\0ت\0th\0ث\0j\0ج\0hh\0ح\0kh\0خ\0"
    "d\0د\0dh\0ذ\0r\0ر\0z\0ز\0s\0س\0sh\0ش\0ss\0ص\0dd\0ض\0tt\0ط\0zz\0ظ\0"
    "e\0ع\0gh\0غ\0f\0ف\0q\0ق\0k\0ك\0l\0ل\0m\0م\0n\0ن\0h\0ه\0"
    "w\0و\0u\0و\0w'\0ؤ\0y\0ي\0i\0ي\0y'\0ئ\0v\0ى\0o\0ة\0x\0ء\0"
    "p\0پ\0c\0چ\0g\0گ\0,\0،\0?\0؟\0;\0؛\0"
    "\0";

/* Devanagari: C consonant, V vowel (letter|sign after a consonant),
 * O other */
static const char hi_bk[] =
    "a\0Vअ|\0aa\0Vआ|ा\0i\0Vइ|ि\0ii\0Vई|ी\0ee\0Vई|ी\0u\0Vउ|ु\0uu\0Vऊ|ू\0"
    "oo\0Vऊ|ू\0e\0Vए|े\0ai\0Vऐ|ै\0o\0Vओ|ो\0au\0Vऔ|ौ\0"
    "k\0Cक\0kh\0Cख\0g\0Cग\0gh\0Cघ\0ch\0Cच\0chh\0Cछ\0j\0Cज\0jh\0Cझ\0"
    "tt\0Cट\0tth\0Cठ\0dd\0Cड\0ddh\0Cढ\0nn\0Cण\0"
    "t\0Cत\0th\0Cथ\0d\0Cद\0dh\0Cध\0n\0Cन\0"
    "p\0Cप\0ph\0Cफ\0f\0Cफ़\0b\0Cब\0bh\0Cभ\0m\0Cम\0"
    "y\0Cय\0r\0Cर\0l\0Cल\0v\0Cव\0w\0Cव\0sh\0Cश\0shh\0Cष\0s\0Cस\0h\0Cह\0"
    "z\0Cज़\0q\0Cक़\0x\0Cक्ष\0gy\0Cज्ञ\0"
    ".n\0Oं\0.c\0Oँ\0.h\0O्\0.s\0Oः\0.\0O।\0..\0O॥\0"
    "\0";

static const char *table(void)
{
    switch (ime_lang) {
    case IME_JA: case IME_KATA: return ja_bk;
    case IME_RU: return ru_bk;
    case IME_EL: return el_bk;
    case IME_AR: return ar_bk;
    case IME_HI: return hi_bk;
    }
    return NULL;
}

static const char *const labels_bk[IME_N] = {
    "", "あ", "ア", "한", "Я", "Ω", "ع", "ह"
};
/* as drawn: Arabic in its contextual forms from right to left, the
 * Devanagari vowel sign i before its consonant */
static const char *const titles_bk[IME_N] = {
    "", "日本語 (ひらがな)", "カタカナ", "한국어", "Русский", "Ελληνικά",
    "ﺔﻴﺑﺮﻌﻟﺍ", "िहन्दी"
};

static const char *const labels_lower_bk[IME_N] = {
    "", "あ", "ア", "한", "я", "ω", "ع", "ह"
};

const char *ime_label(uint8_t l)
{
    if (l >= IME_N)
        return "";
    return ime_upper ? labels_bk[l] : labels_lower_bk[l];
}

const char *ime_title(uint8_t l)
{
    return l < IME_N ? titles_bk[l] : "";
}

uint8_t ime_has_case(uint8_t l)
{
    return l == IME_RU || l == IME_EL;
}

uint8_t ime_converts(uint8_t l)
{
    return l == IME_JA;
}

/* ---- UTF-8 ---- */

static void put_cp(uint16_t cp)
{
    if (cp < 0x80) {
        *out_p++ = (char)cp;
    } else if (cp < 0x800) {
        *out_p++ = (char)(0xC0 | (cp >> 6));
        *out_p++ = (char)(0x80 | (cp & 0x3F));
    } else {
        *out_p++ = (char)(0xE0 | (cp >> 12));
        *out_p++ = (char)(0x80 | ((cp >> 6) & 0x3F));
        *out_p++ = (char)(0x80 | (cp & 0x3F));
    }
    *out_p = 0;
}

static const char *get_cp(const char *s, uint16_t *cp)
{
    uint8_t c = (uint8_t)*s++;
    if (c < 0x80) {
        *cp = c;
    } else if (c < 0xE0) {
        *cp = (uint16_t)(c & 0x1F) << 6 | (*s++ & 0x3F);
    } else {
        *cp = (uint16_t)(c & 0x0F) << 12;
        *cp |= (uint16_t)(*s++ & 0x3F) << 6;
        *cp |= *s++ & 0x3F;
    }
    return s;
}

/* capitals of Cyrillic and Greek */
static uint16_t upper(uint16_t cp)
{
    if (cp >= 0x430 && cp <= 0x44F)
        return cp - 0x20;
    if (cp >= 0x450 && cp <= 0x45F)
        return cp - 0x50;
    if (cp >= 0x3B1 && cp <= 0x3C9 && cp != 0x3C2)
        return cp - 0x20;
    switch (cp) {
    case 0x3C2: return 0x3A3;
    case 0x3AC: return 0x386;
    case 0x3AD: return 0x388;
    case 0x3AE: return 0x389;
    case 0x3AF: return 0x38A;
    case 0x3CC: return 0x38C;
    case 0x3CD: return 0x38E;
    case 0x3CE: return 0x38F;
    }
    return cp;
}

/* text of an entry to the output (Japanese: to the composition); n bytes
 * of s, or up to the NUL */
static void emit_text(const char *s, uint8_t n)
{
    const char *end = s + n;
    uint16_t cp;
    if (ime_lang == IME_JA) {
        if (kana_n + n <= IME_PRE_MAX - sizeof(rom)) {
            memcpy(ime_pre + kana_n, s, n);
            kana_n += n;
        }
        return;
    }
    while (s < end) {
        s = get_cp(s, &cp);
        if (ime_lang == IME_KATA && cp >= 0x3041 && cp <= 0x3096)
            cp += 0x60;
        if (ime_upper)
            cp = upper(cp);
        put_cp(cp);
    }
}

static void emit(const char *t)
{
    uint8_t n = (uint8_t)strlen(t);
    if (ime_lang == IME_HI) {
        const char *bar;
        char k = *t++;
        n--;
        if (k == 'C') {
            if (hi_cons)
                emit_text("्", 3);
            emit_text(t, n);
            hi_cons = 1;
            return;
        }
        if (k == 'V') {
            bar = strchr(t, '|');
            if (hi_cons)
                emit_text(bar + 1, (uint8_t)strlen(bar + 1));
            else
                emit_text(t, (uint8_t)(bar - t));
        } else {
            emit_text(t, n);
        }
        hi_cons = 0;
        return;
    }
    emit_text(t, n);
}

static void emit_literal(char c)
{
    char s[2];
    s[0] = c;
    s[1] = 0;
    hi_cons = 0;
    emit_text(s, 1);
}

static void rom_drop(uint8_t n)
{
    rom_n -= n;
    memmove(rom, rom + n, rom_n);
    rom[rom_n] = 0;
}

static uint8_t is_letter(char c)
{
    return c >= 'a' && c <= 'z';
}

/* turn as much of rom[] into text as is certain; force: all of it */
static void resolve(uint8_t force)
{
    const char *tab = table(), *t, *best;
    uint8_t len, best_n, longer;
    while (rom_n) {
        /* Japanese: a doubled consonant is a small tsu */
        if ((ime_lang == IME_JA || ime_lang == IME_KATA) && rom_n >= 2 &&
            rom[0] == rom[1] && is_letter(rom[0]) && !strchr("aiueon", rom[0])) {
            emit_text("っ", 3);
            rom_drop(1);
            continue;
        }
        best = NULL;
        best_n = 0;
        longer = 0;
        for (t = tab; *t; t += strlen(t) + 1, t += strlen(t) + 1) {
            len = (uint8_t)strlen(t);
            if (len > rom_n) {
                if (!memcmp(t, rom, rom_n))
                    longer = 1;
            } else if (len > best_n && !memcmp(t, rom, len)) {
                best = t;
                best_n = len;
            }
        }
        /* Greek: wait for the letter after s */
        if (ime_lang == IME_EL && rom_n == 1 && rom[0] == 's')
            longer = 1;
        if (longer && !force)
            return;
        if (!best) {
            emit_literal(rom[0]);
            rom_drop(1);
            continue;
        }
        t = best + best_n + 1;
        if (ime_lang == IME_EL && best_n == 1 && rom[0] == 's' && !is_letter(rom[1]))
            t = "ς";
        emit(t);
        rom_drop(best_n);
    }
}

/* ---- Hangul (dubeolsik) ---- */

/* keys a-z: initial consonant (0-18) + 1, or vowel (0-20) + 0x41 */
static const uint8_t ko_key_bk[26] = {
    7, 0x52, 15, 12, 4, 6, 19, 0x49, 0x43, 0x45, 0x41, 0x55, 0x53,  /* a-m */
    0x4E, 0x42, 0x46, 8, 1, 3, 10, 0x47, 18, 13, 17, 0x4D, 16       /* n-z */
};
/* initial consonant -> final (0 none) */
static const uint8_t ko_cho_jong_bk[19] = {
    1, 2, 4, 7, 0, 8, 16, 17, 0, 19, 20, 21, 22, 0, 23, 24, 25, 26, 27
};
/* final -> initial for moving it to the next syllable */
static const int8_t ko_jong_cho_bk[28] = {
    -1, 0, 1, -1, 2, -1, -1, 3, 5, -1, -1, -1, -1, -1, -1, -1,
    6, 7, -1, 9, 10, 11, 12, 14, 15, 16, 17, 18
};
/* compound finals: first, second, compound */
static const uint8_t ko_jong2_bk[] = {
    1, 19, 3,  4, 22, 5,  4, 27, 6,  8, 1, 9,  8, 16, 10,  8, 17, 11,
    8, 19, 12, 8, 25, 13, 8, 26, 14, 8, 27, 15, 17, 19, 18,
    1, 1, 2,  19, 19, 20, 0
};
/* compound vowels: first, second, compound */
static const uint8_t ko_jung2_bk[] = {
    8, 0, 9,  8, 1, 10,  8, 20, 11,  13, 4, 14,  13, 5, 15,  13, 20, 16,
    18, 20, 19,  2, 20, 3,  6, 20, 7, 0xFF
};
/* compatibility jamo of the initials */
static const uint8_t ko_cho_compat_bk[19] = {
    0x31, 0x32, 0x34, 0x37, 0x38, 0x39, 0x41, 0x42, 0x43, 0x45, 0x46,
    0x47, 0x48, 0x49, 0x4A, 0x4B, 0x4C, 0x4D, 0x4E
};
/* tense initials from a doubled one: plain, tense */
static const uint8_t ko_tense_bk[] = { 0, 1, 3, 4, 7, 8, 9, 10, 12, 13, 0xFF };

static int8_t ko_cho = -1, ko_jung = -1;
static uint8_t ko_jong;
static uint8_t ko_hist[8][3], ko_hn;    /* states before each key, for DEL */

static uint8_t ko_pair(const uint8_t *t, uint8_t a, uint8_t b)
{
    for (; *t != 0 && *t != 0xFF; t += 3) {
        if (t[0] == a && t[1] == b)
            return t[2];
    }
    return 0xFF;
}

static void ko_show(void)
{
    char *save = out_p;
    out_p = ime_pre;
    *out_p = 0;
    if (ko_cho >= 0 && ko_jung >= 0)
        put_cp(0xAC00 + ((uint16_t)ko_cho * 21 + ko_jung) * 28 + ko_jong);
    else if (ko_cho >= 0)
        put_cp(0x3100 + ko_cho_compat_bk[ko_cho]);
    else if (ko_jung >= 0)
        put_cp(0x314F + ko_jung);
    out_p = save;
}

static void ko_commit(void)
{
    strcpy(out_p, ime_pre);
    out_p += strlen(out_p);
    ko_cho = ko_jung = -1;
    ko_jong = 0;
    ko_hn = 0;
    ime_pre[0] = 0;
}

static void ko_key(char c)
{
    uint8_t k = ko_key_bk[c - 'a'], v;
    if (ko_hn == 8) {
        memmove(ko_hist[0], ko_hist[1], 7 * 3);
        ko_hn = 7;
    }
    ko_hist[ko_hn][0] = (uint8_t)ko_cho;
    ko_hist[ko_hn][1] = (uint8_t)ko_jung;
    ko_hist[ko_hn][2] = ko_jong;
    ko_hn++;
    if (k < 0x41) {                     /* consonant */
        k--;
        if (ko_cho >= 0 && ko_jung < 0) {
            for (v = 0; ko_tense_bk[v] != 0xFF; v += 2)
                if (ko_tense_bk[v] == k && ko_cho == (int8_t)k)
                    break;
            if (ko_tense_bk[v] != 0xFF) {
                ko_cho = (int8_t)ko_tense_bk[v + 1];
                ko_show();
                return;
            }
        } else if (ko_cho >= 0 && !ko_jong && ko_cho_jong_bk[k]) {
            ko_jong = ko_cho_jong_bk[k];
            ko_show();
            return;
        } else if (ko_jong && ko_cho_jong_bk[k] &&
                   (v = ko_pair(ko_jong2_bk, ko_jong, ko_cho_jong_bk[k])) != 0xFF) {
            ko_jong = v;
            ko_show();
            return;
        }
        ko_commit();
        ko_cho = (int8_t)k;
        ko_hist[0][0] = 0xFF;
        ko_hist[0][1] = 0xFF;
        ko_hist[0][2] = 0;
        ko_hn = 1;
        ko_show();
        return;
    }
    k -= 0x41;                          /* vowel */
    if (ko_jong) {
        /* the final consonant (or its second half) starts the next syllable */
        uint8_t first = 0, j = ko_jong, *t;
        for (t = (uint8_t *)ko_jong2_bk; *t; t += 3) {
            if (t[2] == ko_jong && t[0] != t[1]) {
                first = t[0];
                j = t[1];
                break;
            }
        }
        ko_jong = first;
        ko_show();
        ko_commit();
        ko_cho = ko_jong_cho_bk[j];
        ko_hist[0][0] = (uint8_t)ko_cho;
        ko_hist[0][1] = 0xFF;
        ko_hist[0][2] = 0;
        ko_hn = 1;
        ko_jung = (int8_t)k;
        ko_show();
        return;
    }
    if (ko_jung >= 0) {
        v = ko_pair(ko_jung2_bk, (uint8_t)ko_jung, k);
        if (v != 0xFF) {
            ko_jung = (int8_t)v;
            ko_show();
            return;
        }
        ko_commit();
        ko_jung = (int8_t)k;
        ko_hn = 0;
        ko_show();
        return;
    }
    ko_jung = (int8_t)k;
    ko_show();
}

/* ---- the interface ---- */

static void show(void)
{
    if (ime_lang == IME_KO)
        return;
    memcpy(ime_pre + kana_n, rom, rom_n + 1);
}

void ime_reset(void)
{
    rom_n = kana_n = hi_cons = 0;
    rom[0] = 0;
    ko_cho = ko_jung = -1;
    ko_jong = ko_hn = 0;
    ime_pre[0] = 0;
}

void ime_flush(char *out)
{
    out_p = out;
    *out = 0;
    if (ime_lang == IME_KO) {
        ko_commit();
    } else {
        resolve(1);
        if (kana_n) {
            memcpy(out_p, ime_pre, kana_n);
            out_p[kana_n] = 0;
        }
    }
    ime_reset();
}

void ime_feed(char c, char *out)
{
    out_p = out;
    *out = 0;
    if (ime_lang == IME_KO) {
        if (is_letter(c)) {
            ko_key(c);
            return;
        }
        ko_commit();
        emit_literal(c);
        return;
    }
    if (c == ' ') {
        ime_flush(out);
        out_p = out + strlen(out);
        emit_literal(' ');
        return;
    }
    if (rom_n == sizeof(rom) - 1)
        resolve(1);
    rom[rom_n++] = c;
    rom[rom_n] = 0;
    resolve(0);
    show();
}

uint8_t ime_back(void)
{
    if (ime_lang == IME_KO) {
        if (!ko_hn) {
            if (ko_cho < 0 && ko_jung < 0)
                return 0;
            ko_cho = ko_jung = -1;
            ko_jong = 0;
        } else {
            ko_hn--;
            ko_cho = (int8_t)ko_hist[ko_hn][0];
            ko_jung = (int8_t)ko_hist[ko_hn][1];
            ko_jong = ko_hist[ko_hn][2];
        }
        ko_show();
        return 1;
    }
    if (rom_n) {
        rom[--rom_n] = 0;
    } else if (kana_n) {
        /* the last kana: drop its UTF-8 continuation bytes too */
        do
            kana_n--;
        while (kana_n && ((uint8_t)ime_pre[kana_n] & 0xC0) == 0x80);
    } else {
        return 0;
    }
    show();
    return 1;
}
