#include <stdint.h>
#include <string.h>

#include "disc_serial.h"
#include "sfo.h"

// TODO:
// - Port SFO parsing lib from another project we've worked on (?)

#define SFO_VERSION 0x101u
#define SFO_FMT_BITS 0x100010111ull

static uint16_t le16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static int sfo_fmt_ok(uint16_t fmt) {
  if (fmt <= 0xFFu) {
    return fmt <= 0x20u && ((SFO_FMT_BITS >> fmt) & 1ull);
  }
  if (fmt <= 0x203u) {
    uint16_t bit = (uint16_t)(fmt - 0x100u);
    return bit <= 0x20u && ((SFO_FMT_BITS >> bit) & 1ull);
  }
  if (fmt <= 0x403u) {
    return fmt == 0x204u || fmt == 0x304u;
  }
  return fmt == 0x404u || fmt == 0x504u;
}

int sfo_layout_ok(const unsigned char *buf, size_t n) {
  uint32_t key_off;
  uint32_t data_off;
  uint32_t entries;
  uint64_t index_end;
  size_t i;
  uint32_t seen;
  uint32_t e;

  if (n < 20 || memcmp(buf, "\0PSF", 4) != 0 || le32(buf + 4) != SFO_VERSION) {
    return 0;
  }
  key_off = le32(buf + 8);
  data_off = le32(buf + 12);
  entries = le32(buf + 16);
  index_end = 16ull * entries + 20;
  if (key_off >= n || data_off >= n || index_end >= n || data_off <= key_off) {
    return 0;
  }

  seen = 0;
  i = key_off;
  for (;;) {
    unsigned char c;

    if (i >= n) {
      return 0;
    }
    c = buf[i];
    if (c) {
      if (c < 0x21 || c == '#' || c == 0x7F) {
        return 0;
      }
      if (seen == entries) {
        break;
      }
    } else {
      seen++;
      if (seen == entries) {
        break;
      }
    }
    i++;
  }
  if (data_off != (uint32_t)((i + 4) & ~(size_t)3)) {
    return 0;
  }

  for (e = 0; e < entries; e++) {
    size_t ent = 20 + (size_t)e * 16;
    uint16_t koff;
    uint16_t fmt;
    uint32_t len;
    uint32_t maxl;
    uint32_t doff;
    uint64_t end;

    koff = le16(buf + ent);
    fmt = le16(buf + ent + 2);
    len = le32(buf + ent + 4);
    maxl = le32(buf + ent + 8);
    doff = le32(buf + ent + 12);
    if ((uint32_t)(key_off + koff) >= data_off || !sfo_fmt_ok(fmt) || maxl < len) {
      return 0;
    }
    end = (uint64_t)doff + maxl;
    if (end > n) {
      return 0;
    }
    if (e + 1 == entries) {
      if (end + data_off != n) {
        return 0;
      }
    } else if (end != le32(buf + ent + 28)) {
      return 0;
    }
  }
  return 1;
}

int sfo_copy_title(const unsigned char *buf, size_t n, uint32_t max_len, size_t copy_n, char *dest, size_t dest_len) {
  uint32_t key_off;
  uint32_t data_off;
  uint32_t entries;
  uint32_t e;
  static const char key[] = "TITLE_ID";

  if (!dest || dest_len == 0) {
    return 0;
  }
  dest[0] = '\0';
  key_off = le32(buf + 8);
  data_off = le32(buf + 12);
  entries = le32(buf + 16);
  for (e = 0; e < entries; e++) {
    size_t ent = 20 + (size_t)e * 16;
    uint32_t kpos;
    uint32_t len;
    uint32_t maxl;
    uint32_t val;
    size_t take;
    size_t i;

    kpos = key_off + le16(buf + ent);
    if (kpos >= n || n - kpos < sizeof(key)) {
      continue;
    }
    if (memcmp(buf + kpos, key, sizeof(key)) != 0) {
      continue;
    }
    len = le32(buf + ent + 4);
    maxl = le32(buf + ent + 8);
    if (maxl != max_len || len == 0 || len > max_len) {
      return 0;
    }
    val = data_off + le32(buf + ent + 12);
    if (val >= n) {
      return 0;
    }
    take = n - val;
    if (take > copy_n) {
      take = copy_n;
    }
    if (take >= dest_len) {
      take = dest_len - 1;
    }
    for (i = 0; i < take; i++) {
      if (buf[val + i] == '\0') {
        break;
      }
      dest[i] = (char)buf[val + i];
    }
    dest[i] = '\0';
    if (dest[0] == '\0') {
      return 0;
    }
    hyphenate_serial(dest, dest_len);
    return dest[0] != '\0';
  }
  return 0;
}
