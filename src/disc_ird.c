#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "disc_ird.h"

// These switches hide unused miniz APIs. They do not change tdefl_compressor.
#ifndef MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ARCHIVE_APIS
#endif
#ifndef MINIZ_NO_INFLATE_APIS
#define MINIZ_NO_INFLATE_APIS
#endif
#ifndef MINIZ_NO_STDIO
#define MINIZ_NO_STDIO
#endif
#ifndef MINIZ_NO_TIME
#define MINIZ_NO_TIME
#endif
#ifndef MINIZ_NO_ZLIB_APIS
#define MINIZ_NO_ZLIB_APIS
#endif
#ifndef MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#endif
#include "miniz.h"

/*
 * IRD v9 ("3IRD") for one PS3 ISO.
 *
 * The region table, header, footer, and ISO CRC come from the raw dump.
 * File MD5s are the decrypted bytes read from the mount. PS4 and PS5 do not
 * expose the per-disc AES key, so Data1 and Data2 stay zero. A reader can
 * check the plaintext files and this ISO's regions, and cannot decrypt the
 * ISO on another machine.
 *
 * The layout matches LibIRD: little-endian integers, a 7-bit title length,
 * Joliet names when a supplementary descriptor is present, and gzip around
 * the header, the footer, and the finished IRD.
 */

_Static_assert(sizeof(off_t) >= 8, "off_t");

#define IRD_SECTOR 2048u
#define IRD_PIC_LEN 115
#define IRD_FILE_MAX 8192
#define IRD_EXT_MAX 16384
#define IRD_DIR_MAX (4u << 20)
#define IRD_DEPTH_MAX 24
#define IRD_PATH_MAX 1024
#define IRD_PATHS_MAX (1u << 20)
#define IRD_TITLE_MAX 1024
#define IRD_GZ_MAX (64u << 20)
#define IRD_MOUNT_MAX 400
#define IRD_PROBES 32

static const uint8_t IRD_GZIP_HDR[10] = {0x1F, 0x8B, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFF};
static const uint8_t IRD_EMPTY_MD5[16] = {0xD4, 0x1D, 0x8C, 0xD9, 0x8F, 0x00, 0xB2, 0x04, 0xE9, 0x80, 0x09, 0x98, 0xEC, 0xF8, 0x42, 0x7E};

struct ird_extent {
  uint64_t sector;
  uint64_t bytes;
};

struct ird_file {
  uint64_t key;
  uint64_t size;
  uint32_t path_off;
  uint16_t ext_index;
  uint16_t n_ext;
  uint8_t md5[16];
};

struct ird_gz {
  uint8_t *data;
  size_t len;
  size_t cap;
  uint64_t raw;
  uint32_t crc;
  uint32_t isize;
  int phase;
};

struct md5 {
  uint32_t s[4];
  uint64_t nbits;
  uint8_t buf[64];
  unsigned nbuf;
};

struct disc_ird {
  int failed;
  int sealed;
  int joliet;
  char mount[IRD_MOUNT_MAX];
  uint64_t media;
  uint64_t expect;
  uint64_t got;
  uint64_t header_end;
  uint64_t footer_start;
  uint32_t iso_crc;
  uint8_t nregions;
  int64_t region_start[63];
  int64_t region_end[63];
  uint64_t region_got[63];
  struct md5 region_md5[63];
  uint8_t region_hash[63][16];
  uint32_t nfiles;
  uint32_t next_ext;
  uint32_t paths_len;
  struct ird_file *files;
  struct ird_extent *extents;
  char *paths;
  char title[IRD_TITLE_MAX + 1];
  size_t title_len;
  uint8_t title_id[9];
  uint8_t sys_ver[4];
  uint8_t game_ver[5];
  uint8_t app_ver[5];
  uint8_t pic[IRD_PIC_LEN];
  struct ird_gz header;
  struct ird_gz footer;
  struct ird_gz *active;
  tdefl_compressor *comp;
};

struct ird_cursor {
  uint8_t *dst;
  size_t n;
  size_t cap;
  int counting;
  int err;
};

static uint32_t crc_table[256];

static uint32_t rd_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd_le16(const uint8_t *p) {
  return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_be32(const uint8_t *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void wr_le16(uint8_t *p, uint16_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
}

static void wr_le32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)v;
  p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16);
  p[3] = (uint8_t)(v >> 24);
}

static void wr_le64(uint8_t *p, uint64_t v) {
  wr_le32(p, (uint32_t)v);
  wr_le32(p + 4, (uint32_t)(v >> 32));
}

static int read_full(int fd, void *buf, size_t n) {
  uint8_t *p = buf;
  size_t got = 0;

  while (got < n) {
    ssize_t r = read(fd, p + got, n - got);

    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (r == 0) {
      return -1;
    }
    got += (size_t)r;
  }
  return 0;
}

static int read_at(int fd, uint64_t off, void *buf, size_t n) {
  if (off > (uint64_t)INT64_MAX || lseek(fd, (off_t)off, SEEK_SET) < 0) {
    return -1;
  }
  return read_full(fd, buf, n);
}

static int write_full(int fd, const void *buf, size_t n) {
  const uint8_t *p = buf;
  size_t off = 0;

  while (off < n) {
    ssize_t wrote = write(fd, p + off, n - off);

    if (wrote < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (wrote == 0) {
      return -1;
    }
    off += (size_t)wrote;
  }
  return 0;
}

static int fsync_retry(int fd) {
  while (fsync(fd) < 0) {
    if (errno != EINTR) {
      return -1;
    }
  }
  return 0;
}

static int close_keep(int fd) {
  if (close(fd) < 0 && errno != EINTR) {
    return -1;
  }
  return 0;
}

static void crc_init(void) {
  unsigned i;

  if (crc_table[1] != 0) {
    return;
  }
  for (i = 0; i < 256; i++) {
    uint32_t c = i;
    unsigned k;

    for (k = 0; k < 8; k++) {
      if (c & 1u) {
        c = 0xEDB88320u ^ (c >> 1);
      } else {
        c >>= 1;
      }
    }
    crc_table[i] = c;
  }
}

static void crc_feed(uint32_t *crc, const void *data, size_t n) {
  const uint8_t *p = data;
  uint32_t c = *crc;

  crc_init();
  while (n > 0) {
    c = crc_table[(c ^ *p++) & 0xFFu] ^ (c >> 8);
    n--;
  }
  *crc = c;
}

static uint32_t crc_done(uint32_t crc) {
  return crc ^ 0xFFFFFFFFu;
}

static uint32_t rotl32(uint32_t x, unsigned n) {
  return (x << n) | (x >> (32u - n));
}

static void md5_init(struct md5 *m) {
  m->s[0] = 0x67452301u;
  m->s[1] = 0xEFCDAB89u;
  m->s[2] = 0x98BADCFEu;
  m->s[3] = 0x10325476u;
  m->nbits = 0;
  m->nbuf = 0;
}

static void md5_block(struct md5 *m, const uint8_t block[64]) {
  static const uint32_t K[64] = {
    0xD76AA478u, 0xE8C7B756u, 0x242070DBu, 0xC1BDCEEEu, 0xF57C0FAFu, 0x4787C62Au, 0xA8304613u, 0xFD469501u, 0x698098D8u, 0x8B44F7AFu, 0xFFFF5BB1u, 0x895CD7BEu, 0x6B901122u, 0xFD987193u, 0xA679438Eu, 0x49B40821u,
    0xF61E2562u, 0xC040B340u, 0x265E5A51u, 0xE9B6C7AAu, 0xD62F105Du, 0x02441453u, 0xD8A1E681u, 0xE7D3FBC8u, 0x21E1CDE6u, 0xC33707D6u, 0xF4D50D87u, 0x455A14EDu, 0xA9E3E905u, 0xFCEFA3F8u, 0x676F02D9u, 0x8D2A4C8Au,
    0xFFFA3942u, 0x8771F681u, 0x6D9D6122u, 0xFDE5380Cu, 0xA4BEEA44u, 0x4BDECFA9u, 0xF6BB4B60u, 0xBEBFBC70u, 0x289B7EC6u, 0xEAA127FAu, 0xD4EF3085u, 0x04881D05u, 0xD9D4D039u, 0xE6DB99E5u, 0x1FA27CF8u, 0xC4AC5665u,
    0xF4292244u, 0x432AFF97u, 0xAB9423A7u, 0xFC93A039u, 0x655B59C3u, 0x8F0CCC92u, 0xFFEFF47Du, 0x85845DD1u, 0x6FA87E4Fu, 0xFE2CE6E0u, 0xA3014314u, 0x4E0811A1u, 0xF7537E82u, 0xBD3AF235u, 0x2AD7D2BBu, 0xEB86D391u
  };
  static const uint8_t S[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
  uint32_t M[16];
  uint32_t a = m->s[0];
  uint32_t b = m->s[1];
  uint32_t c = m->s[2];
  uint32_t d = m->s[3];
  unsigned i;

  for (i = 0; i < 16; i++) {
    M[i] = rd_le32(block + i * 4);
  }
  for (i = 0; i < 64; i++) {
    uint32_t f;
    unsigned g;
    uint32_t t;

    if (i < 16) {
      f = (b & c) | (~b & d);
      g = i;
    } else if (i < 32) {
      f = (b & d) | (c & ~d);
      g = (5u * i + 1u) % 16u;
    } else if (i < 48) {
      f = b ^ c ^ d;
      g = (3u * i + 5u) % 16u;
    } else {
      f = c ^ (b | ~d);
      g = (7u * i) % 16u;
    }
    t = d;
    d = c;
    c = b;
    b = b + rotl32(a + f + K[i] + M[g], S[i]);
    a = t;
  }
  m->s[0] += a;
  m->s[1] += b;
  m->s[2] += c;
  m->s[3] += d;
}

static void md5_update(struct md5 *m, const void *data, size_t n) {
  const uint8_t *p = data;

  m->nbits += (uint64_t)n * 8u;
  while (n > 0) {
    unsigned take = 64u - m->nbuf;

    if ((size_t)take > n) {
      take = (unsigned)n;
    }
    memcpy(m->buf + m->nbuf, p, take);
    m->nbuf += take;
    p += take;
    n -= take;
    if (m->nbuf == 64u) {
      md5_block(m, m->buf);
      m->nbuf = 0;
    }
  }
}

static void md5_final(struct md5 *m, uint8_t out[16]) {
  uint64_t bits = m->nbits;
  unsigned i;

  m->buf[m->nbuf++] = 0x80;
  if (m->nbuf == 64u) {
    md5_block(m, m->buf);
    m->nbuf = 0;
  } else if (m->nbuf > 56u) {
    while (m->nbuf < 64u) {
      m->buf[m->nbuf++] = 0;
    }
    md5_block(m, m->buf);
    m->nbuf = 0;
  }
  while (m->nbuf < 56u) {
    m->buf[m->nbuf++] = 0;
  }
  for (i = 0; i < 8; i++) {
    m->buf[56u + i] = (uint8_t)(bits >> (8u * i));
  }
  md5_block(m, m->buf);
  for (i = 0; i < 4; i++) {
    wr_le32(out + i * 4, m->s[i]);
  }
}

static int gz_append(struct ird_gz *gz, const void *data, size_t n) {
  if (n == 0) {
    return 0;
  }
  if (n > IRD_GZ_MAX || gz->len > IRD_GZ_MAX - n) {
    return -1;
  }
  if (gz->len + n > gz->cap) {
    size_t cap = gz->cap ? gz->cap : 4096u;
    uint8_t *next;

    while (cap < gz->len + n) {
      if (cap > IRD_GZ_MAX / 2u) {
        cap = IRD_GZ_MAX;
        break;
      }
      cap *= 2u;
    }
    if (cap < gz->len + n) {
      return -1;
    }
    next = realloc(gz->data, cap);
    if (!next) {
      return -1;
    }
    gz->data = next;
    gz->cap = cap;
  }
  memcpy(gz->data + gz->len, data, n);
  gz->len += n;
  return 0;
}

static mz_bool gz_put(const void *buf, int len, void *user) {
  struct disc_ird *ird = user;

  if (!ird || !ird->active || len < 0) {
    return MZ_FALSE;
  }
  if (gz_append(ird->active, buf, (size_t)len) != 0) {
    return MZ_FALSE;
  }
  return MZ_TRUE;
}

static int gz_begin(struct disc_ird *ird, struct ird_gz *gz) {
  if (!ird->comp || gz->phase != 0) {
    return -1;
  }
  gz->len = 0;
  gz->raw = 0;
  gz->crc = 0xFFFFFFFFu;
  gz->isize = 0;
  gz->phase = 1;
  ird->active = gz;
  if (gz_append(gz, IRD_GZIP_HDR, sizeof(IRD_GZIP_HDR)) != 0) {
    return -1;
  }
  if (tdefl_init(ird->comp, gz_put, ird, IRD_PROBES | TDEFL_GREEDY_PARSING_FLAG) != TDEFL_STATUS_OKAY) {
    return -1;
  }
  return 0;
}

static int gz_data(struct disc_ird *ird, struct ird_gz *gz, const void *data, size_t n) {
  if (n == 0) {
    return 0;
  }
  if (gz->phase != 1) {
    return -1;
  }
  ird->active = gz;
  if (tdefl_compress_buffer(ird->comp, data, n, TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY) {
    return -1;
  }
  crc_feed(&gz->crc, data, n);
  gz->isize += (uint32_t)n;
  gz->raw += n;
  return 0;
}

static int gz_end(struct disc_ird *ird, struct ird_gz *gz) {
  uint8_t dummy = 0;
  uint8_t tr[8];
  uint32_t crc;

  if (gz->phase != 1) {
    return -1;
  }
  ird->active = gz;
  if (tdefl_compress_buffer(ird->comp, &dummy, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE) {
    return -1;
  }
  crc = crc_done(gz->crc);
  wr_le32(tr, crc);
  wr_le32(tr + 4, gz->isize);
  if (gz_append(gz, tr, sizeof(tr)) != 0) {
    return -1;
  }
  gz->phase = 2;
  return 0;
}

static int consume_header(struct disc_ird *ird, uint64_t off, const uint8_t *data, size_t n) {
  size_t take;

  if (ird->header.phase == 2 || off >= ird->header_end) {
    return 0;
  }
  if (ird->header.phase != 1) {
    return -1;
  }
  take = n;
  if ((uint64_t)take > ird->header_end - off) {
    take = (size_t)(ird->header_end - off);
  }
  if (gz_data(ird, &ird->header, data, take) != 0) {
    return -1;
  }
  if (off + take == ird->header_end && gz_end(ird, &ird->header) != 0) {
    return -1;
  }
  return 0;
}

static int consume_footer(struct disc_ird *ird, uint64_t off, const uint8_t *data, size_t n) {
  uint64_t end = off + n;
  uint64_t start;

  if (end <= ird->footer_start) {
    return 0;
  }
  start = off > ird->footer_start ? off : ird->footer_start;
  if (ird->footer.phase == 0) {
    if (ird->header.phase != 2 || gz_begin(ird, &ird->footer) != 0) {
      return -1;
    }
  }
  if (ird->footer.phase != 1) {
    return -1;
  }
  return gz_data(ird, &ird->footer, data + (size_t)(start - off), (size_t)(end - start));
}

static void hash_regions(struct disc_ird *ird, uint64_t off, const uint8_t *data, size_t n) {
  uint64_t end = off + n;
  uint8_t i;

  for (i = 0; i < ird->nregions; i++) {
    int64_t rs = ird->region_start[i];
    int64_t re = ird->region_end[i];
    uint64_t b0;
    uint64_t b1;
    uint64_t lo;
    uint64_t hi;

    if (rs > re) {
      continue;
    }
    b0 = (uint64_t)rs * IRD_SECTOR;
    b1 = (uint64_t)(re + 1) * IRD_SECTOR;
    lo = off > b0 ? off : b0;
    hi = end < b1 ? end : b1;
    if (lo >= hi) {
      continue;
    }
    md5_update(&ird->region_md5[i], data + (size_t)(lo - off), (size_t)(hi - lo));
    ird->region_got[i] += hi - lo;
  }
}

static int joliet_escape(const uint8_t *sec) {
  unsigned i;

  for (i = 0; i + 2 < 32; i++) {
    if (sec[88 + i] == 0x25 && sec[89 + i] == 0x2F && (sec[90 + i] == 0x40 || sec[90 + i] == 0x43 || sec[90 + i] == 0x45)) {
      return 1;
    }
  }
  return 0;
}

static int vd_root(const uint8_t *sec, uint32_t *sector, uint32_t *bytes) {
  if (sec[156] < 34) {
    return -1;
  }
  *sector = rd_le32(sec + 158);
  *bytes = rd_le32(sec + 166);
  return 0;
}

static int find_tree(int fd, uint64_t media, int *joliet, uint32_t *sector, uint32_t *bytes) {
  uint8_t sec[IRD_SECTOR];
  int have = 0;
  uint32_t s;

  *joliet = 0;
  for (s = 16; s < 48; s++) {
    if ((uint64_t)(s + 1u) * IRD_SECTOR > media) {
      break;
    }
    if (read_at(fd, (uint64_t)s * IRD_SECTOR, sec, sizeof(sec)) != 0) {
      return -1;
    }
    if (memcmp(sec + 1, "CD001", 5) != 0) {
      break;
    }
    if (sec[0] == 255) {
      break;
    }
    if (sec[0] == 1 && !have) {
      if (vd_root(sec, sector, bytes) != 0) {
        return -1;
      }
      have = 1;
      *joliet = 0;
    } else if (sec[0] == 2 && joliet_escape(sec)) {
      if (vd_root(sec, sector, bytes) != 0) {
        return -1;
      }
      have = 1;
      *joliet = 1;
      break;
    }
  }
  return have ? 0 : -1;
}

static int name_bad(const char *name) {
  size_t i;

  if (name[0] == '\0') {
    return 1;
  }
  if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
    return 1;
  }
  for (i = 0; name[i] != '\0'; i++) {
    if (name[i] == '/' || name[i] == '\\') {
      return 1;
    }
  }
  return 0;
}

static int decode_name(int joliet, const uint8_t *raw, unsigned raw_len, char *out, size_t cap) {
  size_t o = 0;

  if (joliet) {
    unsigned i;

    if (raw_len % 2u != 0) {
      return -1;
    }
    for (i = 0; i + 1 < raw_len; i += 2) {
      uint32_t cp = ((uint32_t)raw[i] << 8) | raw[i + 1];

      if (cp == 0 || cp == (uint32_t)';') {
        break;
      }
      if (cp >= 0xD800u && cp <= 0xDFFFu) {
        return -1;
      }
      if (cp < 0x80u) {
        if (o + 1 >= cap) {
          return -1;
        }
        out[o++] = (char)cp;
      } else if (cp < 0x800u) {
        if (o + 2 >= cap) {
          return -1;
        }
        out[o++] = (char)(0xC0u | (cp >> 6));
        out[o++] = (char)(0x80u | (cp & 0x3Fu));
      } else {
        if (o + 3 >= cap) {
          return -1;
        }
        out[o++] = (char)(0xE0u | (cp >> 12));
        out[o++] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
        out[o++] = (char)(0x80u | (cp & 0x3Fu));
      }
    }
  } else {
    unsigned i;

    for (i = 0; i < raw_len; i++) {
      if (raw[i] == 0 || raw[i] == ';') {
        break;
      }
      if (raw[i] < 0x20 || raw[i] >= 0x7F) {
        return -1;
      }
      if (o + 1 >= cap) {
        return -1;
      }
      out[o++] = (char)raw[i];
    }
    while (o > 0 && out[o - 1] == ' ') {
      o--;
    }
  }
  out[o] = '\0';
  return name_bad(out) ? -1 : 0;
}

static int key_taken(const struct disc_ird *ird, uint64_t key) {
  uint32_t i;

  for (i = 0; i < ird->nfiles; i++) {
    if (ird->files[i].key == key) {
      return 1;
    }
  }
  return 0;
}

static int add_extent(struct disc_ird *ird, uint64_t sector, uint64_t bytes) {
  uint64_t end;

  if (ird->next_ext >= IRD_EXT_MAX || sector > UINT64_MAX / IRD_SECTOR) {
    return -1;
  }
  if (bytes > UINT64_MAX - sector * IRD_SECTOR) {
    return -1;
  }
  end = sector * IRD_SECTOR + bytes;
  if (end > ird->media) {
    return -1;
  }
  ird->extents[ird->next_ext].sector = sector;
  ird->extents[ird->next_ext].bytes = bytes;
  ird->next_ext++;
  return 0;
}

static int close_file(struct disc_ird *ird, uint32_t ext_index, uint16_t n_ext, const char *path) {
  uint64_t key;
  uint64_t size = 0;
  uint16_t j;
  size_t path_n;

  if (n_ext == 0) {
    return -1;
  }
  key = ird->extents[ext_index].sector;
  for (j = 0; j < n_ext; j++) {
    uint64_t sector = ird->extents[ext_index + j].sector;

    size += ird->extents[ext_index + j].bytes;
    if (sector < key) {
      key = sector;
    }
  }
  if (key_taken(ird, key)) {
    ird->next_ext = ext_index;
    return 0;
  }
  if (ird->nfiles >= IRD_FILE_MAX) {
    return -1;
  }
  path_n = strlen(path);
  if (ird->paths_len + path_n + 1 > IRD_PATHS_MAX) {
    return -1;
  }
  ird->files[ird->nfiles].key = key;
  ird->files[ird->nfiles].size = size;
  ird->files[ird->nfiles].ext_index = (uint16_t)ext_index;
  ird->files[ird->nfiles].n_ext = n_ext;
  ird->files[ird->nfiles].path_off = ird->paths_len;
  memcpy(ird->paths + ird->paths_len, path, path_n + 1);
  ird->paths_len += (uint32_t)(path_n + 1);
  ird->nfiles++;
  return 0;
}

struct ird_pending {
  int open;
  uint32_t ext_index;
  uint16_t n_ext;
  char path[IRD_PATH_MAX];
};

static int walk_dir(struct disc_ird *ird, int fd, uint32_t sector, uint32_t bytes, char *path, int depth);

static int take_record(struct disc_ird *ird, int fd, const uint8_t *rec, unsigned len, char *path, int depth, struct ird_pending *pend) {
  uint32_t sector;
  uint32_t size;
  unsigned name_len;
  uint8_t flags;
  char name[512];
  size_t base;

  if (len < 34 || rec[26] != 0) {
    return -1;
  }
  name_len = rec[32];
  if (33u + name_len > len) {
    return -1;
  }
  sector = rd_le32(rec + 2);
  size = rd_le32(rec + 10);
  flags = rec[25];
  if (name_len == 1 && (rec[33] == 0 || rec[33] == 1)) {
    return pend->open ? -1 : 0;
  }
  if (decode_name(ird->joliet, rec + 33, name_len, name, sizeof(name)) != 0) {
    return -1;
  }
  if (flags & 0x02) {
    if (pend->open) {
      return -1;
    }
    base = strlen(path);
    if (base + 1 + strlen(name) >= IRD_PATH_MAX) {
      return -1;
    }
    path[base] = '/';
    memcpy(path + base + 1, name, strlen(name) + 1);
    if (walk_dir(ird, fd, sector, size, path, depth + 1) != 0) {
      return -1;
    }
    path[base] = '\0';
    return 0;
  }

  if (!pend->open) {
    base = strlen(path);
    if (base + 1 + strlen(name) >= IRD_PATH_MAX) {
      return -1;
    }
    pend->ext_index = ird->next_ext;
    pend->n_ext = 0;
    pend->path[0] = '\0';
    memcpy(pend->path, path, base);
    pend->path[base] = '/';
    memcpy(pend->path + base + 1, name, strlen(name) + 1);
  }
  if (pend->n_ext == 65535u || add_extent(ird, sector, size) != 0) {
    return -1;
  }
  pend->n_ext++;
  if (flags & 0x80) {
    pend->open = 1;
    return 0;
  }
  if (close_file(ird, pend->ext_index, pend->n_ext, pend->path) != 0) {
    return -1;
  }
  pend->open = 0;
  return 0;
}

static int walk_dir(struct disc_ird *ird, int fd, uint32_t sector, uint32_t bytes, char *path, int depth) {
  uint8_t *buf = NULL;
  size_t pos = 0;
  int rc = -1;
  struct ird_pending pend;

  if (depth > IRD_DEPTH_MAX || bytes == 0 || bytes > IRD_DIR_MAX) {
    return -1;
  }
  if ((uint64_t)sector * IRD_SECTOR > ird->media || bytes > ird->media - (uint64_t)sector * IRD_SECTOR) {
    return -1;
  }
  buf = malloc(bytes);
  if (!buf || read_at(fd, (uint64_t)sector * IRD_SECTOR, buf, bytes) != 0) {
    free(buf);
    return -1;
  }
  memset(&pend, '\0', sizeof(pend));
  while (pos < bytes) {
    unsigned len = buf[pos];
    size_t mis;

    if (len == 0) {
      mis = pos % IRD_SECTOR;
      pos += mis == 0 ? IRD_SECTOR : IRD_SECTOR - mis;
      continue;
    }
    mis = pos % IRD_SECTOR;
    if (len < 34 || mis + len > IRD_SECTOR || pos + len > bytes) {
      goto done;
    }
    if (take_record(ird, fd, buf + pos, len, path, depth, &pend) != 0) {
      goto done;
    }
    pos += len;
  }
  if (!pend.open) {
    rc = 0;
  }
done:
  free(buf);
  return rc;
}

static int file_cmp(const void *a, const void *b) {
  const struct ird_file *fa = a;
  const struct ird_file *fb = b;

  if (fa->key < fb->key) {
    return -1;
  }
  if (fa->key > fb->key) {
    return 1;
  }
  return 0;
}

static int read_regions(struct disc_ird *ird, int fd) {
  uint8_t sec[IRD_SECTOR];
  uint32_t count;
  uint32_t nreg;
  uint32_t prev;
  uint32_t i;

  if (read_at(fd, 0, sec, sizeof(sec)) != 0) {
    return -1;
  }
  count = sec[3];
  if (count < 1 || count > 32) {
    return -1;
  }
  nreg = 2u * count - 1u;
  prev = rd_be32(sec + 8);
  for (i = 0; i < nreg; i++) {
    uint32_t next = rd_be32(sec + 8 + 4u * (i + 1u));

    if (i % 2u == 0) {
      ird->region_start[i] = (int64_t)prev;
      ird->region_end[i] = (int64_t)next;
    } else {
      ird->region_start[i] = (int64_t)prev + 1;
      ird->region_end[i] = (int64_t)next - 1;
    }
    prev = next;
  }
  ird->nregions = (uint8_t)nreg;
  return 0;
}

static size_t cstr_n(const uint8_t *s, size_t n) {
  size_t i = 0;

  while (i < n && s[i] != 0) {
    i++;
  }
  return i;
}

static size_t utf8_fit(const uint8_t *s, size_t n, size_t cap) {
  size_t i = 0;

  if (n > cap) {
    n = cap;
  }
  while (i < n) {
    uint8_t c = s[i];
    size_t need = 1;

    if ((c & 0x80u) == 0) {
      need = 1;
    } else if ((c & 0xE0u) == 0xC0u) {
      need = 2;
    } else if ((c & 0xF0u) == 0xE0u) {
      need = 3;
    } else if ((c & 0xF8u) == 0xF0u) {
      need = 4;
    } else {
      break;
    }
    if (i + need > n) {
      break;
    }
    i += need;
  }
  return i;
}

static int sfo_value(const uint8_t *buf, size_t n, const char *key, const uint8_t **val, size_t *len) {
  uint32_t key_off;
  uint32_t data_off;
  uint32_t entries;
  uint32_t e;
  size_t klen;

  if (n < 20 || memcmp(buf, "\0PSF", 4) != 0) {
    return -1;
  }
  key_off = rd_le32(buf + 8);
  data_off = rd_le32(buf + 12);
  entries = rd_le32(buf + 16);
  if (entries > 4096 || key_off > n || data_off > n || (uint64_t)entries * 16u + 20u > n) {
    return -1;
  }
  klen = strlen(key);
  for (e = 0; e < entries; e++) {
    size_t ent = 20u + (size_t)e * 16u;
    uint32_t kpos = key_off + rd_le16(buf + ent);
    uint32_t vlen = rd_le32(buf + ent + 4);
    uint32_t voff = rd_le32(buf + ent + 12);

    if ((uint64_t)kpos + klen + 1u > n || (uint64_t)data_off + voff + vlen > n) {
      continue;
    }
    if (memcmp(buf + kpos, key, klen) != 0 || buf[kpos + klen] != 0) {
      continue;
    }
    *val = buf + data_off + voff;
    *len = vlen;
    return 0;
  }
  return -1;
}

static int read_capped(const char *path, size_t max, uint8_t **out, size_t *out_n) {
  struct stat st;
  uint8_t *buf;
  int fd;
  size_t got = 0;

  *out = NULL;
  *out_n = 0;
  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > max) {
    return -1;
  }
  buf = malloc((size_t)st.st_size);
  if (!buf) {
    return -1;
  }
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    free(buf);
    return -1;
  }
  while (got < (size_t)st.st_size) {
    ssize_t r = read(fd, buf + got, (size_t)st.st_size - got);

    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(fd);
      free(buf);
      return -1;
    }
    if (r == 0) {
      break;
    }
    got += (size_t)r;
  }
  close(fd);
  if (got != (size_t)st.st_size) {
    free(buf);
    return -1;
  }
  *out = buf;
  *out_n = got;
  return 0;
}

static void copy_field(uint8_t *dst, size_t dst_n, const uint8_t *src, size_t src_n) {
  size_t n = cstr_n(src, src_n);

  memset(dst, '\0', dst_n);
  if (n > dst_n) {
    n = dst_n;
  }
  memcpy(dst, src, n);
}

static void read_sfo(struct disc_ird *ird) {
  char path[IRD_MOUNT_MAX + 32];
  uint8_t *buf = NULL;
  size_t n = 0;
  const uint8_t *val = NULL;
  size_t len = 0;
  int wr;

  wr = snprintf(path, sizeof(path), "%s/PS3_GAME/PARAM.SFO", ird->mount);
  if (wr < 0 || (size_t)wr >= sizeof(path) || read_capped(path, 0x8000, &buf, &n) != 0) {
    return;
  }
  if (sfo_value(buf, n, "TITLE_ID", &val, &len) == 0) {
    char raw[32];
    char *hy;
    size_t idn = cstr_n(val, len);

    if (idn > sizeof(raw) - 1) {
      idn = sizeof(raw) - 1;
    }
    memcpy(raw, val, idn);
    raw[idn] = '\0';
    hy = strchr(raw, '-');
    if (hy) {
      memmove(hy, hy + 1, strlen(hy + 1) + 1);
    }
    copy_field(ird->title_id, sizeof(ird->title_id), (const uint8_t *)raw, strlen(raw));
  }
  if (sfo_value(buf, n, "TITLE", &val, &len) == 0) {
    size_t tn = cstr_n(val, len);

    tn = utf8_fit(val, tn, IRD_TITLE_MAX);
    memcpy(ird->title, val, tn);
    ird->title[tn] = '\0';
    ird->title_len = tn;
  }
  if (sfo_value(buf, n, "VERSION", &val, &len) == 0) {
    copy_field(ird->game_ver, sizeof(ird->game_ver), val, len);
  }
  if (sfo_value(buf, n, "APP_VER", &val, &len) == 0) {
    copy_field(ird->app_ver, sizeof(ird->app_ver), val, len);
  }
  free(buf);
}

static void read_pup(struct disc_ird *ird) {
  char path[IRD_MOUNT_MAX + 40];
  uint8_t hdr[0x40];
  uint8_t ver[4];
  uint16_t off;
  int fd;
  int wr;
  unsigned i;

  wr = snprintf(path, sizeof(path), "%s/PS3_UPDATE/PS3UPDAT.PUP", ird->mount);
  if (wr < 0 || (size_t)wr >= sizeof(path)) {
    return;
  }
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    return;
  }
  if (read_full(fd, hdr, sizeof(hdr)) != 0 || memcmp(hdr, "SCEUF", 5) != 0) {
    close(fd);
    return;
  }
  off = (uint16_t)(((uint16_t)hdr[0x3E] << 8) | hdr[0x3F]);
  if (lseek(fd, (off_t)off, SEEK_SET) < 0 || read_full(fd, ver, sizeof(ver)) != 0) {
    close(fd);
    return;
  }
  close(fd);
  for (i = 0; i < 4; i++) {
    if (ver[i] < 0x20 || ver[i] > 0x7E) {
      return;
    }
  }
  memcpy(ird->sys_ver, ver, 4);
}

static void generate_pic(uint8_t pic[IRD_PIC_LEN], uint64_t media) {
  static const uint8_t prefix[12] = {0x10, 0x02, 0x00, 0x00, 0x44, 0x49, 0x01, 0x08, 0x00, 0x00, 0x20, 0x00};
  static const uint8_t mid[12] = {0x42, 0x44, 0x4F, 0x01, 0x11, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00};
  uint32_t sectors = (uint32_t)(media / IRD_SECTOR);
  uint32_t total = sectors + 1048575u;
  uint32_t end = sectors + 1048574u;

  memset(pic, '\0', IRD_PIC_LEN);
  memcpy(pic, prefix, sizeof(prefix));
  memcpy(pic + 12, mid, sizeof(mid));
  pic[24] = (uint8_t)(total >> 24);
  pic[25] = (uint8_t)(total >> 16);
  pic[26] = (uint8_t)(total >> 8);
  pic[27] = (uint8_t)total;
  pic[28] = 0x00;
  pic[29] = 0x10;
  pic[30] = 0x00;
  pic[31] = 0x00;
  pic[32] = (uint8_t)(end >> 24);
  pic[33] = (uint8_t)(end >> 16);
  pic[34] = (uint8_t)(end >> 8);
  pic[35] = (uint8_t)end;
}

static uint64_t footer_of(const struct disc_ird *ird) {
  uint64_t footer = 0;
  uint32_t i;

  for (i = 0; i < ird->nfiles; i++) {
    uint16_t j;

    for (j = 0; j < ird->files[i].n_ext; j++) {
      const struct ird_extent *ext = &ird->extents[ird->files[i].ext_index + j];
      uint64_t end = ext->sector * IRD_SECTOR + ext->bytes;

      if (end > footer) {
        footer = end;
      }
    }
  }
  return footer;
}

void disc_ird_free(struct disc_ird *ird) {
  if (!ird) {
    return;
  }
  free(ird->files);
  free(ird->extents);
  free(ird->paths);
  free(ird->header.data);
  free(ird->footer.data);
  free(ird->comp);
  free(ird);
}

struct disc_ird *disc_ird_prepare(int fd, uint64_t media_bytes, const char *mount) {
  struct disc_ird *ird;
  uint32_t root_sector = 0;
  uint32_t root_bytes = 0;
  char path[IRD_PATH_MAX];
  uint8_t i;

  if (fd < 0 || media_bytes == 0 || (media_bytes % IRD_SECTOR) != 0 || media_bytes / IRD_SECTOR > 0xFFFFFFFFu) {
    return NULL;
  }
  ird = calloc(1, sizeof(*ird));
  if (!ird) {
    return NULL;
  }
  ird->media = media_bytes;
  ird->iso_crc = 0xFFFFFFFFu;
  if (read_regions(ird, fd) != 0) {
    disc_ird_free(ird);
    return NULL;
  }
  if (!mount || mount[0] == '\0' || strlen(mount) >= sizeof(ird->mount)) {
    ird->failed = 1;
    return ird;
  }
  memcpy(ird->mount, mount, strlen(mount) + 1);
  ird->files = calloc(IRD_FILE_MAX, sizeof(*ird->files));
  ird->extents = calloc(IRD_EXT_MAX, sizeof(*ird->extents));
  ird->paths = malloc(IRD_PATHS_MAX);
  ird->comp = malloc(sizeof(*ird->comp));
  if (!ird->files || !ird->extents || !ird->paths || !ird->comp) {
    ird->failed = 1;
    return ird;
  }
  if (find_tree(fd, media_bytes, &ird->joliet, &root_sector, &root_bytes) != 0) {
    ird->failed = 1;
    return ird;
  }
  path[0] = '\0';
  if (walk_dir(ird, fd, root_sector, root_bytes, path, 0) != 0 || ird->nfiles == 0) {
    ird->failed = 1;
    return ird;
  }
  qsort(ird->files, ird->nfiles, sizeof(ird->files[0]), file_cmp);
  ird->header_end = ird->files[0].key * IRD_SECTOR;
  ird->footer_start = footer_of(ird);
  if (ird->header_end > ird->media || ird->footer_start > ird->media || ird->header_end > ird->footer_start) {
    ird->failed = 1;
    return ird;
  }
  ird->region_start[0] = (int64_t)ird->files[0].key;
  ird->region_end[ird->nregions - 1] = (int64_t)(ird->footer_start / IRD_SECTOR) - 1;
  for (i = 0; i < ird->nregions; i++) {
    md5_init(&ird->region_md5[i]);
  }
  read_sfo(ird);
  read_pup(ird);
  generate_pic(ird->pic, media_bytes);
  if (gz_begin(ird, &ird->header) != 0 || (ird->header_end == 0 && gz_end(ird, &ird->header) != 0)) {
    ird->failed = 1;
    return ird;
  }
  return ird;
}

void disc_ird_feed(struct disc_ird *ird, uint64_t offset, const void *data, size_t n) {
  if (!ird || ird->failed || ird->sealed || n == 0) {
    return;
  }
  if (offset != ird->expect || n > ird->media - ird->expect) {
    ird->failed = 1;
    return;
  }
  crc_feed(&ird->iso_crc, data, n);
  hash_regions(ird, offset, data, n);
  if (consume_header(ird, offset, data, n) != 0 || consume_footer(ird, offset, data, n) != 0) {
    ird->failed = 1;
    return;
  }
  ird->expect += n;
  ird->got += n;
}

void disc_ird_set_pic(struct disc_ird *ird, const uint8_t pic[IRD_PIC_LEN]) {
  if (!ird || !pic || ird->sealed) {
    return;
  }
  memcpy(ird->pic, pic, IRD_PIC_LEN);
}

static int hash_one(struct disc_ird *ird, struct ird_file *file, uint8_t *chunk, size_t chunk_n, uint64_t *done, uint64_t total, disc_dump_progress_fn progress, void *user) {
  char full[IRD_MOUNT_MAX + IRD_PATH_MAX];
  struct stat st;
  struct md5 md;
  int fd;
  uint64_t left;
  int wr;

  wr = snprintf(full, sizeof(full), "%s%s", ird->mount, ird->paths + file->path_off);
  if (wr < 0 || (size_t)wr >= sizeof(full)) {
    return -1;
  }
  if (stat(full, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size != file->size) {
    return -1;
  }
  md5_init(&md);
  left = file->size;
  if (left == 0) {
    md5_final(&md, file->md5);
    return 0;
  }
  fd = open(full, O_RDONLY);
  if (fd < 0) {
    return -1;
  }
  while (left > 0) {
    size_t want = chunk_n;
    ssize_t r;

    if ((uint64_t)want > left) {
      want = (size_t)left;
    }
    do {
      r = read(fd, chunk, want);
    } while (r < 0 && errno == EINTR);
    if (r <= 0) {
      close(fd);
      return -1;
    }
    md5_update(&md, chunk, (size_t)r);
    left -= (uint64_t)r;
    *done += (uint64_t)r;
    if (progress) {
      progress(*done, total, user);
    }
  }
  close(fd);
  md5_final(&md, file->md5);
  return 0;
}

static void cursor_put(struct ird_cursor *c, const void *src, size_t n) {
  if (c->err || n == 0) {
    return;
  }
  if (!c->counting) {
    if (n > c->cap - c->n) {
      c->err = 1;
      return;
    }
    memcpy(c->dst + c->n, src, n);
  }
  c->n += n;
}

static void cursor_u8(struct ird_cursor *c, uint8_t v) {
  cursor_put(c, &v, 1);
}

static void cursor_u7(struct ird_cursor *c, uint32_t v) {
  uint8_t tmp[5];
  size_t n = 0;

  while (v >= 0x80u) {
    tmp[n++] = (uint8_t)((v & 0x7Fu) | 0x80u);
    v >>= 7;
  }
  tmp[n++] = (uint8_t)v;
  cursor_put(c, tmp, n);
}

static void emit_body(const struct disc_ird *ird, struct ird_cursor *c) {
  uint8_t le[8];
  uint8_t i;
  uint32_t f;

  cursor_put(c, "3IRD", 4);
  cursor_u8(c, 9);
  cursor_put(c, ird->title_id, 9);
  cursor_u7(c, (uint32_t)ird->title_len);
  cursor_put(c, ird->title, ird->title_len);
  cursor_put(c, ird->sys_ver, 4);
  cursor_put(c, ird->game_ver, 5);
  cursor_put(c, ird->app_ver, 5);
  wr_le32(le, (uint32_t)ird->header.len);
  cursor_put(c, le, 4);
  cursor_put(c, ird->header.data, ird->header.len);
  wr_le32(le, (uint32_t)ird->footer.len);
  cursor_put(c, le, 4);
  cursor_put(c, ird->footer.data, ird->footer.len);
  cursor_u8(c, ird->nregions);
  for (i = 0; i < ird->nregions; i++) {
    cursor_put(c, ird->region_hash[i], 16);
  }
  wr_le32(le, ird->nfiles);
  cursor_put(c, le, 4);
  for (f = 0; f < ird->nfiles; f++) {
    wr_le64(le, ird->files[f].key);
    cursor_put(c, le, 8);
    cursor_put(c, ird->files[f].md5, 16);
  }
  wr_le16(le, 0);
  cursor_put(c, le, 2);
  wr_le16(le, 0);
  cursor_put(c, le, 2);
  cursor_put(c, ird->pic, IRD_PIC_LEN);
  memset(le, '\0', sizeof(le));
  cursor_put(c, le, 8);
  cursor_put(c, le, 8);
  cursor_put(c, le, 8);
  cursor_put(c, le, 8);
  wr_le32(le, crc_done(ird->iso_crc));
  cursor_put(c, le, 4);
}

struct file_sink {
  int fd;
  int failed;
};

static mz_bool file_put(const void *buf, int len, void *user) {
  struct file_sink *sink = user;

  if (!sink || sink->failed || len < 0) {
    return MZ_FALSE;
  }
  if (write_full(sink->fd, buf, (size_t)len) != 0) {
    sink->failed = 1;
    return MZ_FALSE;
  }
  return MZ_TRUE;
}

static int gzip_body(struct disc_ird *ird, int fd, const uint8_t *body, size_t n) {
  struct file_sink sink;
  uint32_t crc = 0xFFFFFFFFu;
  uint8_t tr[8];
  size_t off = 0;
  uint8_t dummy = 0;

  sink.fd = fd;
  sink.failed = 0;
  if (write_full(fd, IRD_GZIP_HDR, sizeof(IRD_GZIP_HDR)) != 0) {
    return -1;
  }
  if (tdefl_init(ird->comp, file_put, &sink, IRD_PROBES | TDEFL_GREEDY_PARSING_FLAG) != TDEFL_STATUS_OKAY) {
    return -1;
  }
  while (off < n) {
    size_t take = n - off;

    if (take > 65536u) {
      take = 65536u;
    }
    if (tdefl_compress_buffer(ird->comp, body + off, take, TDEFL_NO_FLUSH) != TDEFL_STATUS_OKAY) {
      return -1;
    }
    off += take;
  }
  if (tdefl_compress_buffer(ird->comp, &dummy, 0, TDEFL_FINISH) != TDEFL_STATUS_DONE || sink.failed) {
    return -1;
  }
  crc_feed(&crc, body, n);
  wr_le32(tr, crc_done(crc));
  wr_le32(tr + 4, (uint32_t)n);
  return write_full(fd, tr, sizeof(tr));
}

int disc_ird_write(struct disc_ird *ird, const char *path, disc_dump_progress_fn progress, void *user) {
  uint8_t *chunk = NULL;
  uint8_t *body = NULL;
  struct ird_cursor count;
  struct ird_cursor fill;
  uint64_t total = 0;
  uint64_t done = 0;
  uint32_t i;
  uint8_t r;
  int fd = -1;
  int rc = -1;
  uint32_t body_crc;

  if (!ird || ird->failed || ird->sealed || !path || path[0] == '\0') {
    return -1;
  }
  ird->sealed = 1;
  if (ird->got != ird->media || ird->header.phase != 2 || ird->header.raw != ird->header_end) {
    ird->failed = 1;
    return -1;
  }
  if (ird->footer_start < ird->media) {
    if (ird->footer.phase != 1 || gz_end(ird, &ird->footer) != 0) {
      ird->failed = 1;
      return -1;
    }
  } else if (ird->footer.phase != 0 || gz_begin(ird, &ird->footer) != 0 || gz_end(ird, &ird->footer) != 0) {
    ird->failed = 1;
    return -1;
  }
  if (ird->footer.raw != ird->media - ird->footer_start || ird->header.len > 0xFFFFFFFFu || ird->footer.len > 0xFFFFFFFFu) {
    ird->failed = 1;
    return -1;
  }
  for (r = 0; r < ird->nregions; r++) {
    uint64_t expect = 0;

    if (ird->region_start[r] <= ird->region_end[r]) {
      expect = (uint64_t)(ird->region_end[r] - ird->region_start[r] + 1) * IRD_SECTOR;
    }
    if (ird->region_got[r] != expect) {
      ird->failed = 1;
      return -1;
    }
    if (expect == 0) {
      memcpy(ird->region_hash[r], IRD_EMPTY_MD5, 16);
    } else {
      md5_final(&ird->region_md5[r], ird->region_hash[r]);
    }
  }
  for (i = 0; i < ird->nfiles; i++) {
    total += ird->files[i].size;
  }
  if (progress) {
    progress(0, total, user);
  }
  chunk = malloc(64u * 1024u);
  if (!chunk) {
    ird->failed = 1;
    return -1;
  }
  for (i = 0; i < ird->nfiles; i++) {
    if (hash_one(ird, &ird->files[i], chunk, 64u * 1024u, &done, total, progress, user) != 0) {
      free(chunk);
      ird->failed = 1;
      return -1;
    }
  }
  free(chunk);
  if (progress) {
    progress(done, total, user);
  }

  memset(&count, '\0', sizeof(count));
  count.counting = 1;
  emit_body(ird, &count);
  if (count.err || count.n > SIZE_MAX - 4u) {
    ird->failed = 1;
    return -1;
  }
  body = malloc(count.n + 4u);
  if (!body) {
    ird->failed = 1;
    return -1;
  }
  memset(&fill, '\0', sizeof(fill));
  fill.dst = body;
  fill.cap = count.n;
  emit_body(ird, &fill);
  if (fill.err || fill.n != count.n) {
    free(body);
    ird->failed = 1;
    return -1;
  }
  body_crc = 0xFFFFFFFFu;
  crc_feed(&body_crc, body, fill.n);
  wr_le32(body + fill.n, crc_done(body_crc));
  fill.n += 4u;

  free(ird->header.data);
  ird->header.data = NULL;
  free(ird->footer.data);
  ird->footer.data = NULL;

  fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) {
    free(body);
    ird->failed = 1;
    return -1;
  }
  if (gzip_body(ird, fd, body, fill.n) == 0 && fsync_retry(fd) == 0) {
    rc = 0;
  }
  if (close_keep(fd) != 0) {
    rc = -1;
  }
  free(body);
  if (rc != 0) {
    unlink(path);
    ird->failed = 1;
  }
  return rc;
}
