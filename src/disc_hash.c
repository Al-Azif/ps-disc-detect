#include "disc_hash.h"

#include <string.h>

struct md5_step {
  uint8_t k;
  uint8_t s;
  uint8_t f;
};

// RFC 1321. f is 0=F, 1=G, 2=H, 3=I. k indexes the block word. s is the rotate.
static const struct md5_step MD5_STEPS[64] = {
  {0, 7, 0},  {1, 12, 0}, {2, 17, 0}, {3, 22, 0}, {4, 7, 0},   {5, 12, 0}, {6, 17, 0}, {7, 22, 0},
  {8, 7, 0},  {9, 12, 0}, {10, 17, 0}, {11, 22, 0}, {12, 7, 0}, {13, 12, 0}, {14, 17, 0}, {15, 22, 0},
  {1, 5, 1},  {6, 9, 1},  {11, 14, 1}, {0, 20, 1}, {5, 5, 1},   {10, 9, 1}, {15, 14, 1}, {4, 20, 1},
  {9, 5, 1},  {14, 9, 1}, {3, 14, 1},  {8, 20, 1}, {13, 5, 1},  {2, 9, 1},  {7, 14, 1},  {12, 20, 1},
  {5, 4, 2},  {8, 11, 2}, {11, 16, 2}, {14, 23, 2}, {1, 4, 2},  {4, 11, 2}, {7, 16, 2},  {10, 23, 2},
  {13, 4, 2}, {0, 11, 2}, {3, 16, 2},  {6, 23, 2}, {9, 4, 2},   {12, 11, 2}, {15, 16, 2}, {2, 23, 2},
  {0, 6, 3},  {7, 10, 3}, {14, 15, 3}, {5, 21, 3}, {12, 6, 3},  {3, 10, 3}, {10, 15, 3}, {1, 21, 3},
  {8, 6, 3},  {15, 10, 3}, {6, 15, 3}, {13, 21, 3}, {4, 6, 3},  {11, 10, 3}, {2, 15, 3}, {9, 21, 3},
};

static const uint32_t MD5_T[64] = {
  0xD76AA478u, 0xE8C7B756u, 0x242070DBu, 0xC1BDCEEEu, 0xF57C0FAFu, 0x4787C62Au, 0xA8304613u, 0xFD469501u,
  0x698098D8u, 0x8B44F7AFu, 0xFFFF5BB1u, 0x895CD7BEu, 0x6B901122u, 0xFD987193u, 0xA679438Eu, 0x49B40821u,
  0xF61E2562u, 0xC040B340u, 0x265E5A51u, 0xE9B6C7AAu, 0xD62F105Du, 0x02441453u, 0xD8A1E681u, 0xE7D3FBC8u,
  0x21E1CDE6u, 0xC33707D6u, 0xF4D50D87u, 0x455A14EDu, 0xA9E3E905u, 0xFCEFA3F8u, 0x676F02D9u, 0x8D2A4C8Au,
  0xFFFA3942u, 0x8771F681u, 0x6D9D6122u, 0xFDE5380Cu, 0xA4BEEA44u, 0x4BDECFA9u, 0xF6BB4B60u, 0xBEBFBC70u,
  0x289B7EC6u, 0xEAA127FAu, 0xD4EF3085u, 0x04881D05u, 0xD9D4D039u, 0xE6DB99E5u, 0x1FA27CF8u, 0xC4AC5665u,
  0xF4292244u, 0x432AFF97u, 0xAB9423A7u, 0xFC93A039u, 0x655B59C3u, 0x8F0CCC92u, 0xFFEFF47Du, 0x85845DD1u,
  0x6FA87E4Fu, 0xFE2CE6E0u, 0xA3014314u, 0x4E0811A1u, 0xF7537E82u, 0xBD3AF235u, 0x2AD7D2BBu, 0xEB86D391u,
};

// Table 0 is the byte table. Tables 1..3 step a reflected CRC by 2, 3, and 4 bytes.
static uint32_t crc_table[4][256];
static int crc_ready;

static uint32_t rotl(uint32_t value, unsigned n) {
  return (value << n) | (value >> (32u - n));
}

static uint32_t load_le(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t load_be(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void store_le(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static void store_be(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static void crc_init(void) {
  unsigned i;

  if (crc_ready) {
    return;
  }
  for (i = 0; i < 256u; i++) {
    uint32_t c = i;
    unsigned bit;

    for (bit = 0; bit < 8u; bit++) {
      if ((c & 1u) != 0) {
        c = 0xEDB88320u ^ (c >> 1);
      } else {
        c >>= 1;
      }
    }
    crc_table[0][i] = c;
  }
  for (i = 0; i < 256u; i++) {
    uint32_t c = crc_table[0][i];
    unsigned k;

    for (k = 1; k < 4u; k++) {
      c = crc_table[0][c & 0xFFu] ^ (c >> 8);
      crc_table[k][i] = c;
    }
  }
  crc_ready = 1;
}

static uint32_t crc_update(uint32_t crc, const uint8_t *p, size_t n) {
  while (n >= 4u) {
    uint32_t c = crc ^ load_le(p);

    crc = crc_table[3][c & 0xFFu] ^ crc_table[2][(c >> 8) & 0xFFu] ^ crc_table[1][(c >> 16) & 0xFFu] ^ crc_table[0][c >> 24];
    p += 4;
    n -= 4u;
  }
  while (n > 0) {
    crc = crc_table[0][(crc ^ *p) & 0xFFu] ^ (crc >> 8);
    p++;
    n--;
  }
  return crc;
}

static void md5_block(uint32_t state[4], const uint8_t block[64]) {
  uint32_t x[16];
  uint32_t v[4];
  unsigned i;

  for (i = 0; i < 16u; i++) {
    x[i] = load_le(block + i * 4u);
  }
  v[0] = state[0];
  v[1] = state[1];
  v[2] = state[2];
  v[3] = state[3];
  for (i = 0; i < 64u; i++) {
    unsigned a = (4u - (i & 3u)) & 3u;
    unsigned b = (a + 1u) & 3u;
    unsigned c = (a + 2u) & 3u;
    unsigned d = (a + 3u) & 3u;
    uint32_t f;

    switch (MD5_STEPS[i].f) {
    case 0:
      f = (v[b] & v[c]) | (~v[b] & v[d]);
      break;
    case 1:
      f = (v[b] & v[d]) | (v[c] & ~v[d]);
      break;
    case 2:
      f = v[b] ^ v[c] ^ v[d];
      break;
    default:
      f = v[c] ^ (v[b] | ~v[d]);
      break;
    }
    v[a] = v[b] + rotl(v[a] + f + x[MD5_STEPS[i].k] + MD5_T[i], MD5_STEPS[i].s);
  }
  state[0] += v[0];
  state[1] += v[1];
  state[2] += v[2];
  state[3] += v[3];
}

static void sha1_block(uint32_t state[5], const uint8_t block[64]) {
  uint32_t w[80];
  uint32_t a = state[0];
  uint32_t b = state[1];
  uint32_t c = state[2];
  uint32_t d = state[3];
  uint32_t e = state[4];
  unsigned t;

  for (t = 0; t < 16u; t++) {
    w[t] = load_be(block + t * 4u);
  }
  for (t = 16; t < 80u; t++) {
    w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
  }
  for (t = 0; t < 80u; t++) {
    uint32_t f;
    uint32_t k;
    uint32_t temp;

    if (t < 20u) {
      f = (b & c) | (~b & d);
      k = 0x5A827999u;
    } else if (t < 40u) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1u;
    } else if (t < 60u) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCu;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6u;
    }
    temp = rotl(a, 5) + f + e + k + w[t];
    e = d;
    d = c;
    c = rotl(b, 30);
    b = a;
    a = temp;
  }
  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
}

static void pad_md5(struct disc_hash_ctx *ctx) {
  uint64_t bits = ctx->size * 8ull;
  unsigned n = ctx->md5_n;
  unsigned i;

  ctx->md5_buf[n++] = 0x80;
  if (n > 56u) {
    memset(ctx->md5_buf + n, '\0', 64u - n);
    md5_block(ctx->md5_s, ctx->md5_buf);
    n = 0;
  }
  memset(ctx->md5_buf + n, '\0', 56u - n);
  for (i = 0; i < 8u; i++) {
    ctx->md5_buf[56u + i] = (uint8_t)(bits >> (8u * i));
  }
  md5_block(ctx->md5_s, ctx->md5_buf);
}

static void pad_sha1(struct disc_hash_ctx *ctx) {
  uint64_t bits = ctx->size * 8ull;
  unsigned n = ctx->sha1_n;
  unsigned i;

  ctx->sha1_buf[n++] = 0x80;
  if (n > 56u) {
    memset(ctx->sha1_buf + n, '\0', 64u - n);
    sha1_block(ctx->sha1_s, ctx->sha1_buf);
    n = 0;
  }
  memset(ctx->sha1_buf + n, '\0', 56u - n);
  for (i = 0; i < 8u; i++) {
    ctx->sha1_buf[56u + i] = (uint8_t)(bits >> (8u * (7u - i)));
  }
  sha1_block(ctx->sha1_s, ctx->sha1_buf);
}

void disc_hash_init(struct disc_hash_ctx *ctx) {
  crc_init();
  memset(ctx, '\0', sizeof(*ctx));
  ctx->crc = 0xFFFFFFFFu;
  ctx->md5_s[0] = 0x67452301u;
  ctx->md5_s[1] = 0xEFCDAB89u;
  ctx->md5_s[2] = 0x98BADCFEu;
  ctx->md5_s[3] = 0x10325476u;
  ctx->sha1_s[0] = 0x67452301u;
  ctx->sha1_s[1] = 0xEFCDAB89u;
  ctx->sha1_s[2] = 0x98BADCFEu;
  ctx->sha1_s[3] = 0x10325476u;
  ctx->sha1_s[4] = 0xC3D2E1F0u;
}

void disc_hash_update(struct disc_hash_ctx *ctx, const void *data, size_t n) {
  const uint8_t *p = data;

  if (n == 0) {
    return;
  }
  ctx->size += n;
  ctx->crc = crc_update(ctx->crc, p, n);

  // md5_n and sha1_n stay equal. A full block is hashed from the input.
  if (ctx->md5_n != 0) {
    unsigned room = 64u - ctx->md5_n;

    if (n < room) {
      memcpy(ctx->md5_buf + ctx->md5_n, p, n);
      memcpy(ctx->sha1_buf + ctx->sha1_n, p, n);
      ctx->md5_n += (unsigned)n;
      ctx->sha1_n += (unsigned)n;
      return;
    }
    memcpy(ctx->md5_buf + ctx->md5_n, p, room);
    memcpy(ctx->sha1_buf + ctx->sha1_n, p, room);
    md5_block(ctx->md5_s, ctx->md5_buf);
    sha1_block(ctx->sha1_s, ctx->sha1_buf);
    ctx->md5_n = 0;
    ctx->sha1_n = 0;
    p += room;
    n -= room;
  }

  while (n >= 64u) {
    md5_block(ctx->md5_s, p);
    sha1_block(ctx->sha1_s, p);
    p += 64;
    n -= 64u;
  }

  if (n != 0) {
    memcpy(ctx->md5_buf, p, n);
    memcpy(ctx->sha1_buf, p, n);
    ctx->md5_n = (unsigned)n;
    ctx->sha1_n = (unsigned)n;
  }
}

void disc_hash_final(struct disc_hash_ctx *ctx, struct disc_hash *out) {
  unsigned i;

  pad_md5(ctx);
  pad_sha1(ctx);
  out->crc = ctx->crc ^ 0xFFFFFFFFu;
  out->size = ctx->size;
  for (i = 0; i < 4u; i++) {
    store_le(out->md5 + i * 4u, ctx->md5_s[i]);
  }
  for (i = 0; i < 5u; i++) {
    store_be(out->sha1 + i * 4u, ctx->sha1_s[i]);
  }
}
