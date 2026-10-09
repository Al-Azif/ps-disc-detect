#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <unistd.h>

#include "detect_disc.h"
#include "disc_dump.h"
#include "disc_hash.h"
#include "disc_ird.h"
#include "disc_mount.h"

/*
 * Copy the optical drive to an image file.
 *
 * DVD and BD come from the /dev/cd0 streaming block device. Those logical
 * sectors are written to path, which is an ISO when the sectors are 2048 bytes.
 *
 * A CD is not that byte stream. Each sector is 2352 bytes, and the tracks are
 * described by a cue sheet. A path ending in .iso, .bin, or .cue names the
 * stem: the image is stem.bin and the sheet is stem.cue. Track times in the
 * sheet are relative to the first track, which is the start of the bin.
 *
 * Returns 0 when the image is complete, or -1 if it could not be written.
 * A failed attempt removes the files it created.
 *
 * progress, when set, is called with the image bytes written so far and the
 * total size. The total is 0 when the drive did not report one. The disc
 * type selects the image format. hash, when set, receives the checksums of
 * the ISO or the CD bin.
 */

static const char DISC_DEV[] = "/dev/cd0";

// One kernel transfer. MAXPHYS on these systems is 128KB(?)
#define DUMP_CHUNK (128 * 1024)
#define CD_SECTOR 2352
#define CD_CHUNK 16
#define CD_TRACKS 99

static const uint8_t CD_SYNC[12] = {0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00};

struct cd_track {
  uint8_t number;
  uint32_t lba;
  int data;
  int mode;
};

struct cd_toc {
  struct cd_track tracks[CD_TRACKS];
  unsigned count;
  uint32_t leadout;
};

static int write_full(int fd, const unsigned char *buf, size_t n) {
  size_t off = 0;

  while (off < n) {
    ssize_t wrote = write(fd, buf + off, n - off);

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

// FreeBSD leaves the descriptor state unspecified after EINTR, so a second close is not safe.
static int close_keep(int fd) {
  if (close(fd) < 0 && errno != EINTR) {
    return -1;
  }
  return 0;
}

static void report_progress(disc_dump_progress_fn progress, void *user, uint64_t done, uint64_t total) {
  if (progress) {
    progress(done, total, user);
  }
}

// Optional raw-image hook. offset is the byte position of data, before it is counted as done.
typedef void (*disc_feed_fn)(uint64_t offset, const void *data, size_t n, void *user);

// total < 0 reads until the device ends. total >= 0 reads that many bytes.
static int copy_stream(int dev, int iso, unsigned char *buf, size_t chunk, off_t total, disc_dump_progress_fn progress, void *user, disc_feed_fn feed, void *feed_user) {
  off_t done = 0;
  uint64_t total_bytes = total > 0 ? (uint64_t)total : 0;

  report_progress(progress, user, 0, total_bytes);
  for (;;) {
    size_t want = chunk;
    ssize_t got;

    if (total >= 0) {
      off_t left = total - done;

      if (left == 0) {
        return 0;
      }
      if ((off_t)want > left) {
        want = (size_t)left;
      }
    }

    do {
      got = read(dev, buf, want);
    } while (got < 0 && errno == EINTR);

    if (got < 0) {
      return -1;
    }
    if (got == 0) {
      return (total < 0 && done > 0) ? 0 : -1;
    }
    if (write_full(iso, buf, (size_t)got) < 0) {
      return -1;
    }
    if (feed) {
      feed((uint64_t)done, buf, (size_t)got, feed_user);
    }
    done += got;
    report_progress(progress, user, (uint64_t)done, total_bytes);
  }
}

struct image_feed {
  struct disc_ird *ird;
  struct disc_hash_ctx *hash;
};

static void image_feed(uint64_t offset, const void *data, size_t n, void *user) {
  struct image_feed *feed = user;

  if (feed->ird) {
    disc_ird_feed(feed->ird, offset, data, n);
  }
  if (feed->hash) {
    disc_hash_update(feed->hash, data, n);
  }
}

// READ DISC STRUCTURE, format 0. A BD physical-info block starts 10 02 .. DI.
static int read_pic(uint8_t pic[115]) {
  struct disc_pass pass;
  uint8_t cdb[12];
  uint8_t data[256];

  pass.fd = -1;
  if (disc_pass_open(&pass) != 0) {
    return -1;
  }
  memset(cdb, '\0', sizeof(cdb));
  memset(data, '\0', sizeof(data));
  cdb[0] = 0xAD;
  cdb[8] = 0x01;
  if (disc_pass_scsi(&pass, cdb, 12, data, sizeof(data), 10000, NULL) != 0) {
    disc_pass_close(&pass);
    return -1;
  }
  disc_pass_close(&pass);
  if (data[4] == 0x10 && data[5] == 0x02 && data[8] == 0x44 && data[9] == 0x49) {
    memcpy(pic, data + 4, 115);
    return 0;
  }
  return -1;
}

// ird_out, when set, receives a PS3 IRD builder after a successful ISO. NULL means ISO only.
// hash_out, when set, receives the checksums of the bytes written.
static int dump_block(const char *path, disc_dump_progress_fn progress, void *user, struct disc_ird **ird_out, struct disc_hash *hash_out) {
  int dev = -1;
  int iso = -1;
  unsigned char *buf = NULL;
  struct disc_ird *ird = NULL;
  struct disc_hash_ctx hash_ctx;
  struct image_feed feed;
  disc_feed_fn feed_fn = NULL;
  off_t media = -1;
  unsigned int sector = 0;
  size_t chunk = DUMP_CHUNK;
  int tried_ird = 0;
  int rc = -1;

  if (ird_out) {
    *ird_out = NULL;
  }

  dev = open(DISC_DEV, O_RDONLY);
  if (dev < 0) {
    return -1;
  }

  if (ioctl(dev, DIOCGMEDIASIZE, &media) < 0 || media <= 0) {
    media = -1;
  }
  if (ioctl(dev, DIOCGSECTORSIZE, &sector) < 0) {
    sector = 0;
  }
  if (sector > 0 && sector <= DUMP_CHUNK && (chunk % sector) != 0) {
    chunk -= chunk % sector;
  }
  if (media > 0 && sector > 0 && media / (off_t)sector > 1) {
    disc_request_read_speed((uint32_t)(media / (off_t)sector - 1));
  } else {
    disc_request_read_speed(0);
  }

  if (ird_out && sector == 2048 && media > 0 && (media % (off_t)2048) == 0) {
    uint8_t pic[115];

    tried_ird = 1;
    ird = disc_ird_prepare(dev, (uint64_t)media, disc_mount_root());
    if (ird && read_pic(pic) == 0) {
      disc_ird_set_pic(ird, pic);
    }
  }
  // prepare seeks around the disc. A failed rewind would copy from the wrong place.
  if (tried_ird && lseek(dev, 0, SEEK_SET) < 0) {
    disc_ird_free(ird);
    close(dev);
    return -1;
  }

  buf = malloc(chunk);
  if (!buf) {
    disc_ird_free(ird);
    close(dev);
    return -1;
  }

  iso = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (iso < 0) {
    disc_ird_free(ird);
    free(buf);
    close(dev);
    return -1;
  }

  disc_hash_init(&hash_ctx);
  feed.ird = ird;
  feed.hash = hash_out ? &hash_ctx : NULL;
  if (feed.ird || feed.hash) {
    feed_fn = image_feed;
  }
  if (copy_stream(dev, iso, buf, chunk, media, progress, user, feed_fn, feed_fn ? &feed : NULL) == 0 && fsync_retry(iso) == 0) {
    rc = 0;
  }
  if (close_keep(iso) != 0) {
    rc = -1;
  }
  if (rc == 0 && hash_out) {
    disc_hash_final(&hash_ctx, hash_out);
  }
  if (rc != 0) {
    disc_ird_free(ird);
    unlink(path);
  } else if (ird_out) {
    *ird_out = ird;
  } else {
    disc_ird_free(ird);
  }
  free(buf);
  close(dev);
  return rc;
}

static int profile_is_cd(enum disc_type type) {
  unsigned profile = (unsigned)type;

  return profile == 0x08u || profile == 0x09u || profile == 0x0Au || profile == (unsigned)DISC_PS1_CD_ROM || profile == (unsigned)DISC_PS2_CD_ROM;
}

static int ends_with(const char *s, const char *suf) {
  size_t n = strlen(s);
  size_t m = strlen(suf);
  size_t i;

  if (n < m) {
    return 0;
  }
  for (i = 0; i < m; i++) {
    if (tolower((unsigned char)s[n - m + i]) != tolower((unsigned char)suf[i])) {
      return 0;
    }
  }
  return 1;
}

static int cd_paths(const char *path, char *bin, size_t bin_len, char *cue, size_t cue_len) {
  size_t n = strlen(path);

  if (ends_with(path, ".iso") || ends_with(path, ".bin") || ends_with(path, ".cue")) {
    n -= 4;
  }
  if (n == 0 || n + 5 > bin_len || n + 5 > cue_len) {
    return -1;
  }
  memcpy(bin, path, n);
  memcpy(bin + n, ".bin", 5);
  memcpy(cue, path, n);
  memcpy(cue + n, ".cue", 5);
  return 0;
}

static const char *base_name(const char *path) {
  const char *slash = strrchr(path, '/');

  return slash ? slash + 1 : path;
}

static int cue_name_ok(const char *name) {
  return name[0] != '\0' && strpbrk(name, "\"\r\n") == NULL;
}

static void read_cd_cdb(uint8_t cdb[12], uint32_t lba, uint32_t count, int data) {
  memset(cdb, '\0', 12);
  cdb[0] = 0xBE;
  cdb[2] = (uint8_t)(lba >> 24);
  cdb[3] = (uint8_t)(lba >> 16);
  cdb[4] = (uint8_t)(lba >> 8);
  cdb[5] = (uint8_t)lba;
  cdb[6] = (uint8_t)(count >> 16);
  cdb[7] = (uint8_t)(count >> 8);
  cdb[8] = (uint8_t)count;
  // Data: sync, headers, user data, and EDC/ECC. Audio: the 2352 user bytes.
  cdb[9] = data ? 0xF8 : 0x10;
}

static int read_toc(struct disc_pass *pass, struct cd_toc *toc) {
  uint8_t data[2048];
  uint8_t cdb[10];
  unsigned total;
  unsigned count = 0;
  unsigned i;
  int saw_lead = 0;

  memset(toc, '\0', sizeof(*toc));
  memset(cdb, '\0', sizeof(cdb));
  cdb[0] = 0x43;
  cdb[7] = 0x08;
  memset(data, '\0', sizeof(data));
  if (disc_pass_scsi(pass, cdb, 10, data, sizeof(data), 10000, NULL) != 0) {
    return -1;
  }

  total = (((unsigned)data[0] << 8) | data[1]) + 2;
  if (total > sizeof(data)) {
    total = sizeof(data);
  }
  if (total < 12) {
    return -1;
  }

  for (i = 4; i + 8 <= total; i += 8) {
    uint8_t number = data[i + 2];
    uint32_t lba = ((uint32_t)data[i + 4] << 24) | ((uint32_t)data[i + 5] << 16) | ((uint32_t)data[i + 6] << 8) | data[i + 7];

    if (number == 0xAA) {
      toc->leadout = lba;
      saw_lead = 1;
      break;
    }
    if (number == 0 || count >= CD_TRACKS) {
      continue;
    }
    toc->tracks[count].number = number;
    toc->tracks[count].lba = lba;
    toc->tracks[count].data = (data[i + 1] & 0x04) != 0;
    toc->tracks[count].mode = 2;
    count++;
  }
  if (!saw_lead || count == 0) {
    return -1;
  }
  toc->count = count;
  for (i = 0; i < count; i++) {
    uint32_t next = (i + 1 < count) ? toc->tracks[i + 1].lba : toc->leadout;

    if (next <= toc->tracks[i].lba) {
      return -1;
    }
  }
  return 0;
}

int disc_image_bytes(enum disc_type type, uint64_t *out) {
  if (!out || type == DISC_NOT_PRESENT || type == DISC_TIMEOUT || type == DISC_ERROR) {
    return -1;
  }
  *out = 0;
  if (profile_is_cd(type)) {
    struct disc_pass pass;
    struct cd_toc toc;

    pass.fd = -1;
    if (disc_pass_open(&pass) != 0 || read_toc(&pass, &toc) != 0 || toc.leadout <= toc.tracks[0].lba) {
      disc_pass_close(&pass);
      return -1;
    }
    disc_pass_close(&pass);
    *out = (uint64_t)(toc.leadout - toc.tracks[0].lba) * CD_SECTOR;
    return 0;
  } else {
    int dev = open(DISC_DEV, O_RDONLY);
    off_t media = -1;

    if (dev < 0) {
      return -1;
    }
    if (ioctl(dev, DIOCGMEDIASIZE, &media) < 0 || media <= 0) {
      close(dev);
      return -1;
    }
    close(dev);
    *out = (uint64_t)media;
    return 0;
  }
}

static int sector_mode(const uint8_t *sector) {
  if (memcmp(sector, CD_SYNC, sizeof(CD_SYNC)) == 0 && sector[15] == 1) {
    return 1;
  }
  return 2;
}

static int probe_mode(struct disc_pass *pass, struct cd_track *track) {
  uint8_t sector[CD_SECTOR];
  uint8_t cdb[12];

  memset(sector, '\0', sizeof(sector));
  read_cd_cdb(cdb, track->lba, 1, 1);
  if (disc_pass_scsi(pass, cdb, 12, sector, sizeof(sector), 20000, NULL) != 0) {
    return -1;
  }
  track->mode = sector_mode(sector);
  return 0;
}

static int read_cd(struct disc_pass *pass, uint32_t lba, uint32_t count, int data, uint8_t *buf) {
  uint8_t cdb[12];

  read_cd_cdb(cdb, lba, count, data);
  return disc_pass_scsi(pass, cdb, 12, buf, count * CD_SECTOR, 20000, NULL);
}

static int dump_tracks(struct disc_pass *pass, int bin, const struct cd_toc *toc, struct disc_hash_ctx *hash, disc_dump_progress_fn progress, void *user) {
  uint8_t *buf = malloc((size_t)CD_CHUNK * CD_SECTOR);
  uint64_t total = (uint64_t)(toc->leadout - toc->tracks[0].lba) * CD_SECTOR;
  uint64_t done = 0;
  unsigned i;

  if (!buf) {
    return -1;
  }
  report_progress(progress, user, 0, total);
  for (i = 0; i < toc->count; i++) {
    uint32_t lba = toc->tracks[i].lba;
    uint32_t end = (i + 1 < toc->count) ? toc->tracks[i + 1].lba : toc->leadout;

    while (lba < end) {
      uint32_t n = end - lba;

      if (n > CD_CHUNK) {
        n = CD_CHUNK;
      }
      if (read_cd(pass, lba, n, toc->tracks[i].data, buf) != 0 || write_full(bin, buf, (size_t)n * CD_SECTOR) != 0) {
        free(buf);
        return -1;
      }
      if (hash) {
        disc_hash_update(hash, buf, (size_t)n * CD_SECTOR);
      }
      lba += n;
      done += (uint64_t)n * CD_SECTOR;
      report_progress(progress, user, done, total);
    }
  }
  free(buf);
  return 0;
}

static int write_cue(int fd, const char *bin_name, const struct cd_toc *toc) {
  char line[640];
  unsigned i;
  uint32_t origin = toc->tracks[0].lba;

  int n = snprintf(line, sizeof(line), "FILE \"%s\" BINARY\n", bin_name);
  if (n < 0 || (size_t)n >= sizeof(line) || write_full(fd, (const unsigned char *)line, (size_t)n) != 0) {
    return -1;
  }
  for (i = 0; i < toc->count; i++) {
    uint32_t rel = toc->tracks[i].lba - origin;
    unsigned m = rel / (75u * 60u);
    unsigned s = (rel / 75u) % 60u;
    unsigned f = rel % 75u;

    if (toc->tracks[i].data) {
      n = snprintf(line, sizeof(line), "  TRACK %02u MODE%d/2352\n    INDEX 01 %02u:%02u:%02u\n", (unsigned)toc->tracks[i].number, toc->tracks[i].mode, m, s, f);
    } else {
      n = snprintf(line, sizeof(line), "  TRACK %02u AUDIO\n    INDEX 01 %02u:%02u:%02u\n", (unsigned)toc->tracks[i].number, m, s, f);
    }
    if (n < 0 || (size_t)n >= sizeof(line) || write_full(fd, (const unsigned char *)line, (size_t)n) != 0) {
      return -1;
    }
  }
  return 0;
}

static int dump_cd(const char *path, struct disc_hash *hash_out, disc_dump_progress_fn progress, void *user) {
  struct disc_pass pass;
  struct cd_toc toc;
  struct disc_hash_ctx hash_ctx;
  char bin_path[512];
  char cue_path[512];
  int bin = -1;
  int cue = -1;
  int rc = -1;
  unsigned i;

  pass.fd = -1;
  if (cd_paths(path, bin_path, sizeof(bin_path), cue_path, sizeof(cue_path)) != 0 || !cue_name_ok(base_name(bin_path))) {
    return -1;
  }
  if (disc_pass_open(&pass) != 0) {
    return -1;
  }
  disc_request_read_speed(0);
  if (read_toc(&pass, &toc) != 0) {
    disc_pass_close(&pass);
    return -1;
  }
  for (i = 0; i < toc.count; i++) {
    if (toc.tracks[i].data && probe_mode(&pass, &toc.tracks[i]) != 0) {
      disc_pass_close(&pass);
      return -1;
    }
  }

  bin = open(bin_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (bin < 0) {
    disc_pass_close(&pass);
    return -1;
  }
  disc_hash_init(&hash_ctx);
  if (dump_tracks(&pass, bin, &toc, hash_out ? &hash_ctx : NULL, progress, user) == 0 && fsync_retry(bin) == 0) {
    cue = open(cue_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (cue >= 0 && write_cue(cue, base_name(bin_path), &toc) == 0 && fsync_retry(cue) == 0) {
      rc = 0;
    }
  }

  if (cue >= 0 && close_keep(cue) != 0) {
    rc = -1;
  }
  if (close_keep(bin) != 0) {
    rc = -1;
  }
  if (rc == 0 && hash_out) {
    disc_hash_final(&hash_ctx, hash_out);
  }
  if (rc != 0) {
    unlink(bin_path);
    unlink(cue_path);
  }
  disc_pass_close(&pass);
  return rc;
}

int dump_disc(enum disc_type type, const char *path, struct disc_hash *hash, disc_dump_progress_fn progress, void *user) {
  if (!path || path[0] == '\0' || type == DISC_NOT_PRESENT || type == DISC_TIMEOUT || type == DISC_ERROR) {
    return -1;
  }
  if (profile_is_cd(type)) {
    return dump_cd(path, hash, progress, user);
  }
  return dump_block(path, progress, user, NULL, hash);
}

int dump_ps3(const char *path, struct disc_ird **ird_out, struct disc_hash *hash, disc_dump_progress_fn progress, void *user) {
  if (!path || path[0] == '\0') {
    return -1;
  }
  return dump_block(path, progress, user, ird_out, hash);
}
