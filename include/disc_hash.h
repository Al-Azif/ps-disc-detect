#ifndef DISC_HASH_H_
#define DISC_HASH_H_

#include <stddef.h>
#include <stdint.h>

// CRC-32, MD5, and SHA-1 of one image. Redump stores all three.
struct disc_hash {
  uint32_t crc;
  uint8_t md5[16];
  uint8_t sha1[20];
  uint64_t size;
};

struct disc_hash_ctx {
  uint32_t crc;
  uint64_t size;
  uint32_t md5_s[4];
  unsigned md5_n;
  uint8_t md5_buf[64];
  uint32_t sha1_s[5];
  unsigned sha1_n;
  uint8_t sha1_buf[64];
};

void disc_hash_init(struct disc_hash_ctx *ctx);
void disc_hash_update(struct disc_hash_ctx *ctx, const void *data, size_t n);
void disc_hash_final(struct disc_hash_ctx *ctx, struct disc_hash *out);

#endif
