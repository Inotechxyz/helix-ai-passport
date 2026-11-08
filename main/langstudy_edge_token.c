// main/langstudy_edge_token.c
// C port of edge-tts 7.3.1 drm.py (generate_sec_ms_gec_v2). The original adds
// deliberately odd SHA-256 initial state, schedule words and a CS64 mixing
// pass; every array below is copied verbatim so the token stays bit-identical.
#include "langstudy_edge_token.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define WIN_EPOCH 11644473600ULL
#define TRUSTED_CLIENT_TOKEN "6A5AA1D4EAFF4E9FB37E23D68491D6F4"
#define U32_MASK 0xFFFFFFFFu

static const uint32_t SHA256_INITIAL[8] = {
    0x6A09E667u, 0xBB67AE85u, 0x3C6EF372u, 0xA54FF53Au,
    0x510E527Fu, 0x9B05688Cu, 0x1F83D9ABu, 0x5BE0CD19u,
};

static const uint32_t SHA256_STATE[8] = {
    0xFD9FBEA2u, 0x91243CE7u, 0xBE499094u, 0xCD2E1E0Au,
    0x6C006F49u, 0x04BD35ABu, 0xFEB0A7E4u, 0x03EFFBD2u,
};

static const uint32_t SHA256_SCHEDULE[14] = {
    0xEB24BF9Eu, 0x1558691Fu, 0xA0C09EE2u, 0x0CFA32BAu,
    0xA622EE8Cu, 0xD1DD9491u, 0xC0DD25A0u, 0xF9E70AB6u,
    0xF0C31573u, 0x7058CE4Eu, 0x7188E606u, 0x00E5AB33u,
    0xD258B060u, 0x72766564u,
};

static const uint32_t SHA256_ROUND[64] = {
    0x428A2F98u, 0x71374491u, 0xB5C0FBCFu, 0xE9B5DBA5u,
    0x3956C25Bu, 0x59F111F1u, 0x923F82A4u, 0xAB1C5ED5u,
    0xD807AA98u, 0x12835B01u, 0x243185BEu, 0x550C7DC3u,
    0x72BE5D74u, 0x80DEB1FEu, 0x9BDC06A7u, 0xC19BF174u,
    0xE49B69C1u, 0xEFBE4786u, 0x0FC19DC6u, 0x240CA1CCu,
    0x2DE92C6Fu, 0x4A7484AAu, 0x5CB0A9DCu, 0x76F988DAu,
    0x983E5152u, 0xA831C66Du, 0xB00327C8u, 0xBF597FC7u,
    0xC6E00BF3u, 0xD5A79147u, 0x06CA6351u, 0x14292967u,
    0x27B70A85u, 0x2E1B2138u, 0x4D2C6DFCu, 0x53380D13u,
    0x650A7354u, 0x766A0ABBu, 0x81C2C92Eu, 0x92722C85u,
    0xA2BFE8A1u, 0xA81A664Bu, 0xC24B8B70u, 0xC76C51A3u,
    0xD192E819u, 0xD6990624u, 0xF40E3585u, 0x106AA070u,
    0x19A4C116u, 0x1E376C08u, 0x2748774Cu, 0x34B0BCB5u,
    0x391C0CB3u, 0x4ED8AA4Au, 0x5B9CCA4Fu, 0x682E6FF3u,
    0x748F82EEu, 0x78A5636Fu, 0x84C87814u, 0x8CC70208u,
    0x90BEFFFAu, 0xA4506CEBu, 0xBEF9A3F7u, 0xC67178F2u,
};

static const uint32_t SHA256_ADJUSTMENT = 0xC9DC06A7u;
static const uint32_t SHA256_SIGMA0_ADJUSTMENT = 0x059C0B80u;
static const uint32_t SHA256_SIGMA1_ADJUSTMENT = 0x000B92C0u;

static const uint32_t CS64_COEFFICIENTS[14][2] = {
    {0xE82E91B1u, 0xB32700D0u}, {0x44DE24C6u, 0x88DFF51Du},
    {0x3BEB65BCu, 0x29514038u}, {0xC89C5933u, 0xB328CBDFu},
    {0x8D1EC211u, 0xD405B4D5u}, {0x314EF5D5u, 0x26E55F3Cu},
    {0xE92491B1u, 0x607801C6u}, {0xF0A5B9D5u, 0x82B2C1FBu},
    {0x06064CF3u, 0xFA51A3DAu}, {0xDA79FA60u, 0x207CC464u},
    {0xBED20560u, 0x3C3C74F3u}, {0x89F9C9F8u, 0xD2D14B61u},
    {0x35FA38C6u, 0xBB04A2C1u}, {0xCB49E3C6u, 0xC5626167u},
};
static const uint32_t CS64_FACTOR = 0x82893933u;
static const uint32_t CS64_ADJUSTMENT0 = 0xF2F90B1Au;
static const uint32_t CS64_ADJUSTMENT1 = 0x0B1A0000u;

static uint32_t rotr32(uint32_t value, unsigned bits)
{
    return (value >> bits) | (value << (32u - bits));
}

static uint32_t rol16(uint32_t value)
{
    return (value << 16) | (value >> 16);
}

static uint32_t sigma0(uint32_t value)
{
    return rotr32(value, 7) ^ rotr32(value, 18) ^ (value >> 3);
}

static uint32_t sigma1(uint32_t value)
{
    return rotr32(value, 17) ^ rotr32(value, 19) ^ (value >> 10);
}

static void expand_schedule(uint32_t *words, int first)
{
    for (int i = first; i < 64; i++) {
        uint32_t x = words[i - 2];
        uint32_t y = words[i - 15];
        words[i] = sigma1(x) + words[i - 7] + sigma0(y) + words[i - 16];
    }
}

static void sha256_rounds(uint32_t state[8], const uint32_t *words, int first)
{
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    uint32_t e = state[4], f = state[5], g = state[6], h = state[7];

    for (int i = first; i < 64; i++) {
        uint32_t constants = SHA256_ROUND[i];
        // The first block of the modified digest substitutes K[14]; the two
        // words before the unrolled region still use the ordinary value.
        if (first == 14 && i == 14) constants = SHA256_ADJUSTMENT;
        uint32_t t1 = h + (rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25)) +
                      (g ^ (e & (f ^ g))) + constants + words[i];
        uint32_t t2 = (rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22)) +
                      ((a & b) | (c & (a | b)));
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state[0] = a;
    state[1] = b;
    state[2] = c;
    state[3] = d;
    state[4] = e;
    state[5] = f;
    state[6] = g;
    state[7] = h;
}

static void sha256_state_from_prefix(const uint8_t prefix[7], uint32_t out[8])
{
    uint32_t words[64];
    memset(words, 0, sizeof(words));
    words[14] = ((uint32_t)prefix[0] << 16) | ((uint32_t)prefix[1] << 8) |
                prefix[2];
    words[15] = ((uint32_t)prefix[3] << 24) | ((uint32_t)prefix[4] << 16) |
                ((uint32_t)prefix[5] << 8) | prefix[6];

    for (int i = 16; i <= 29; i++) {
        uint32_t value = SHA256_SCHEDULE[i - 16];
        if (i - 2 >= 14) {
            value += (i == 16) ? (sigma1(words[i - 2]) ^
                                  SHA256_SIGMA1_ADJUSTMENT)
                               : sigma1(words[i - 2]);
        }
        if (i - 7 >= 14) value += words[i - 7];
        if (i - 15 >= 14) {
            value += (i == 29) ? (sigma0(words[i - 15]) ^
                                  SHA256_SIGMA0_ADJUSTMENT)
                               : sigma0(words[i - 15]);
        }
        words[i] = value;
    }

    words[30] = sigma1(words[28]) + words[23] + sigma0(words[15]) +
                words[14] + SHA256_ADJUSTMENT - SHA256_ROUND[14];
    expand_schedule(words, 31);

    uint32_t working[8];
    memcpy(working, SHA256_STATE, sizeof(working));
    sha256_rounds(working, words, 14);
    for (int i = 0; i < 8; i++) out[i] = SHA256_INITIAL[i] + working[i];
}

static void sha256_token_digest(const uint8_t *data, size_t len,
                                uint8_t digest[32])
{
    // drm.py pads data[7:] and writes (57 + len) * 8 as the bit length: not the
    // real message length, but part of the accepted token.
    size_t length = 57 + len;
    int64_t negative = 55 - (int64_t)length;
    size_t pad = (size_t)((negative % 64 + 64) % 64);

    uint8_t block[128];
    size_t body = len - 7;
    size_t total = body + 1 + pad + 8;
    memset(block, 0, sizeof(block));
    memcpy(block, data + 7, body);
    block[body] = 0x80;
    uint64_t bits = (uint64_t)length * 8u;
    for (int i = 0; i < 8; i++) {
        block[total - 1 - (size_t)i] = (uint8_t)(bits >> (i * 8));
    }

    uint32_t state[8];
    sha256_state_from_prefix(data, state);

    for (size_t offset = 0; offset < total; offset += 64) {
        uint32_t words[64];
        for (int i = 0; i < 16; i++) {
            const uint8_t *p = block + offset + (size_t)i * 4;
            words[i] = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                       ((uint32_t)p[2] << 8) | p[3];
        }
        expand_schedule(words, 16);
        uint32_t working[8];
        memcpy(working, state, sizeof(working));
        sha256_rounds(working, words, 0);
        for (int i = 0; i < 8; i++) state[i] += working[i];
    }

    for (int i = 0; i < 8; i++) {
        digest[i * 4] = (uint8_t)(state[i] >> 24);
        digest[i * 4 + 1] = (uint8_t)(state[i] >> 16);
        digest[i * 4 + 2] = (uint8_t)(state[i] >> 8);
        digest[i * 4 + 3] = (uint8_t)state[i];
    }
}

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void cs64_digest(const uint8_t *data, size_t len,
                        const uint8_t digest[32], uint32_t out[2])
{
    uint32_t key_a = load_le32(digest) | 1u;
    uint32_t key_b = load_le32(digest + 4) | 1u;
    uint32_t factors[2] = {key_a * CS64_FACTOR, key_b * CS64_FACTOR};

    // words_le4_even(b"\x00" + data): zero byte, little-endian u32 words,
    // padded to an even count.
    uint32_t words[40];
    size_t bytes = len + 1;
    size_t word_count = (bytes + 3) / 4;
    if (word_count > sizeof(words) / sizeof(words[0]) - 1) word_count = 39;
    uint8_t staging[160];
    memset(staging, 0, sizeof(staging));
    memcpy(staging + 1, data, len < sizeof(staging) - 1 ? len : sizeof(staging) - 1);
    for (size_t i = 0; i < word_count; i++) {
        words[i] = load_le32(staging + i * 4);
    }
    if (word_count % 2 != 0) words[word_count++] = 0;

    uint32_t terms[64];
    uint32_t offsets[64];
    if (14 + word_count > sizeof(terms) / sizeof(terms[0])) {
        word_count = sizeof(terms) / sizeof(terms[0]) - 14;
        word_count &= ~(size_t)1;
    }
    size_t count = 0;
    for (size_t i = 0; i < 14; i++, count++) {
        terms[count] = CS64_COEFFICIENTS[i][0] * factors[i % 2];
        offsets[count] = CS64_COEFFICIENTS[i][1];
    }
    for (size_t i = 0; i < word_count; i++, count++) {
        uint32_t word = words[i];
        uint32_t term = word * (i % 2 == 0 ? key_a : key_b);
        uint32_t offset =
            rol16(word) * (i % 2 == 0 ? 0xEF0569FBu : 0xC31713DBu);
        if (i == 0) {
            term += CS64_ADJUSTMENT0 * factors[0];
            offset += CS64_ADJUSTMENT1;
        }
        terms[count] = term;
        offsets[count] = offset;
    }

    uint32_t swap_acc = 0, swap_last = 0, state = 0, acc = 0;
    for (size_t i = 0; i < count; i += 2) {
        uint32_t t = rol16(offsets[i] + terms[i]);
        uint32_t q = t * 0x76F50F34u;
        swap_acc += rol16(q) * 0xC3EFEA97u + q;

        t = rol16(offsets[i + 1] + terms[i + 1]);
        q = t * 0x1881CE3Cu;
        swap_last = rol16(q) * 0x35BD1EC9u + q;
        swap_acc += swap_last;

        state = state * key_a + terms[i];
        state = rol16(state) * 0xCF98B111u;
        state = rol16(state) * 0x87085B9Fu;
        state = rol16(state) * 0x12CEB96Du;
        state = rol16(state) * 0x257E1D83u;
        acc = (acc + state) & U32_MASK;

        t = state * key_b + terms[i + 1];
        t = rol16(t) * 0xA27416F5u;
        t = rol16(t) * 0xD38396FFu;
        state = rol16(rol16(t) * 0x7C932B89u) * 0xBFA49F69u;
        acc = (acc + state) & U32_MASK;
    }

    out[0] = swap_last ^ state;
    out[1] = swap_acc ^ acc;
}

bool ls_edge_gec_token(uint64_t unix_seconds,
                       char out[LS_EDGE_GEC_TOKEN_LEN + 1])
{
    if (!out) return false;
    out[0] = '\0';
    if (unix_seconds < 1600000000ULL) return false;

    double ticks = (double)(unix_seconds + WIN_EPOCH);
    ticks -= fmod(ticks, 300.0);
    ticks *= 10000000.0;

    char source[64];
    int written = snprintf(source, sizeof(source), "%.0f%s", ticks,
                           TRUSTED_CLIENT_TOKEN);
    if (written <= 0 || (size_t)written >= sizeof(source)) return false;
    size_t len = (size_t)written;
    if (len < 7) return false;

    uint8_t digest[32];
    sha256_token_digest((const uint8_t *)source, len, digest);

    uint32_t cs[2];
    cs64_digest((const uint8_t *)source, len, digest, cs);

    int result = snprintf(out, LS_EDGE_GEC_TOKEN_LEN + 1,
                          "%08X%08X-%02X%02X%02X%02X%02X%02X%02X%02X"
                          "%02X%02X%02X%02X%02X%02X%02X%02X"
                          "%02X%02X%02X%02X%02X%02X%02X%02X"
                          "%02X%02X%02X%02X%02X%02X%02X%02X",
                          (unsigned)__builtin_bswap32(cs[0]),
                          (unsigned)__builtin_bswap32(cs[1]),
                          digest[0], digest[1], digest[2], digest[3],
                          digest[4], digest[5], digest[6], digest[7],
                          digest[8], digest[9], digest[10], digest[11],
                          digest[12], digest[13], digest[14], digest[15],
                          digest[16], digest[17], digest[18], digest[19],
                          digest[20], digest[21], digest[22], digest[23],
                          digest[24], digest[25], digest[26], digest[27],
                          digest[28], digest[29], digest[30], digest[31]);
    return result == LS_EDGE_GEC_TOKEN_LEN;
}
