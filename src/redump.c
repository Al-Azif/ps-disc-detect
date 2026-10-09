#include "redump.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <sqlite3.h>

#if defined(__ORBIS__) && defined(__PROSPERO__)
#error "Define only one of __ORBIS__ or __PROSPERO__"
#elif defined(__ORBIS__)
#include <ps4/klog.h>
#elif defined(__PROSPERO__)
#include <ps5/klog.h>
#else
#error "Build for PS4 (__ORBIS__) or PS5 (__PROSPERO__)"
#endif

extern const unsigned char redump_db[];
extern const unsigned char redump_db_end[];

static sqlite3 *db;
static sqlite3_stmt *name_stmt;
static sqlite3_stmt *image_stmt;
static sqlite3_stmt *list_stmt;
static sqlite3_stmt *sum_stmt;
static sqlite3_stmt *track_stmt;
static sqlite3_stmt *size_stmt;
static sqlite3_stmt *size_name_stmt;
static int db_state;

static void copy_exact(char *dest, size_t dest_len, const char *src) {
  size_t n;

  if (!src) {
    dest[0] = '\0';
    return;
  }
  n = strlen(src);
  if (n + 1 > dest_len) {
    dest[0] = '\0';
    return;
  }
  memcpy(dest, src, n + 1);
}

// A regular SQLite file here replaces the embedded database for the rest of the run.
#define REDUMP_DB_PATH "/data/redump.db"
// Newer Redump serial sets still fit. A larger file is left unused.
#define REDUMP_DB_MAX (64ull * 1024ull * 1024ull)

static void close_db(void) {
  sqlite3_finalize(name_stmt);
  sqlite3_finalize(image_stmt);
  sqlite3_finalize(list_stmt);
  sqlite3_finalize(sum_stmt);
  sqlite3_finalize(track_stmt);
  sqlite3_finalize(size_stmt);
  sqlite3_finalize(size_name_stmt);
  name_stmt = NULL;
  image_stmt = NULL;
  list_stmt = NULL;
  sum_stmt = NULL;
  track_stmt = NULL;
  size_stmt = NULL;
  size_name_stmt = NULL;
  sqlite3_close(db);
  db = NULL;
}

static int prepare_db(void) {
  int rc;

  rc = sqlite3_prepare_v2(db, "SELECT name FROM serials WHERE serial = ?1 LIMIT 2", -1, &name_stmt, NULL);
  if (rc == SQLITE_OK) {
    rc = sqlite3_prepare_v2(db, "SELECT name FROM images WHERE sha1 = ?1 AND size = ?2 AND crc = ?3 AND md5 = ?4 AND kind = ?5 LIMIT 1", -1, &image_stmt, NULL);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_prepare_v2(db, "SELECT name FROM serials WHERE serial = ?1", -1, &list_stmt, NULL);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_prepare_v2(db, "SELECT COUNT(*), COALESCE(SUM(size), 0) FROM tracks WHERE name = ?1", -1, &sum_stmt, NULL);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_prepare_v2(db, "SELECT size, crc, md5, sha1 FROM tracks WHERE name = ?1 ORDER BY track", -1, &track_stmt, NULL);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_prepare_v2(db, "SELECT t1.name, t1.size, t1.crc, t1.md5, t1.sha1 FROM tracks AS t1 WHERE t1.track = 1 AND t1.name IN (SELECT name FROM tracks GROUP BY name HAVING SUM(size) = ?1) ORDER BY t1.size, t1.name", -1, &size_stmt, NULL);
  }
  if (rc == SQLITE_OK) {
    rc = sqlite3_prepare_v2(db, "SELECT name FROM images WHERE size = ?1 AND kind = ?2 UNION SELECT name FROM tracks GROUP BY name HAVING SUM(size) = ?1 AND ?2 = 'bin' LIMIT 2", -1, &size_name_stmt, NULL);
  }
  if (rc != SQLITE_OK) {
    close_db();
    return 0;
  }
  return 1;
}

// copy comes from sqlite3_malloc64. FREEONCLOSE frees it, including when deserialize fails.
// rollback forces header bytes 18 and 19 to 1 so a WAL file opens in rollback mode.
static int adopt_image(unsigned char *copy, sqlite3_int64 n, int rollback) {
  int rc;

  if (rollback && n >= 20) {
    copy[18] = 1;
    copy[19] = 1;
  }

  rc = sqlite3_open_v2(":memory:", &db, SQLITE_OPEN_READWRITE, NULL);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    db = NULL;
    sqlite3_free(copy);
    return 0;
  }

  rc = sqlite3_deserialize(db, "main", copy, n, n, SQLITE_DESERIALIZE_FREEONCLOSE | SQLITE_DESERIALIZE_READONLY);
  if (rc != SQLITE_OK) {
    sqlite3_close(db);
    db = NULL;
    return 0;
  }
  return prepare_db();
}

static int load_embedded(void) {
  sqlite3_int64 n = (sqlite3_int64)(redump_db_end - redump_db);
  unsigned char *copy;

  if (n <= 0) {
    return 0;
  }
  // The embedded redump_db bytes stay read-only.
  copy = sqlite3_malloc64((sqlite3_uint64)n);
  if (!copy) {
    return 0;
  }
  memcpy(copy, redump_db, (size_t)n);
  return adopt_image(copy, n, 0);
}

// 1: loaded.
// 0: path is absent.
// -1: present, but not a usable Redump database.
static int load_redump_file(void) {
  struct stat st;
  unsigned char *copy;
  int fd;
  size_t got = 0;
  sqlite3_int64 n;

  if (stat(REDUMP_DB_PATH, &st) != 0) {
    return errno == ENOENT ? 0 : -1;
  }
  if (!S_ISREG(st.st_mode) || st.st_size < 16 || (unsigned long long)st.st_size > REDUMP_DB_MAX) {
    return -1;
  }

  n = (sqlite3_int64)st.st_size;
  copy = sqlite3_malloc64((sqlite3_uint64)n);
  if (!copy) {
    return -1;
  }
  fd = open(REDUMP_DB_PATH, O_RDONLY);
  if (fd < 0) {
    sqlite3_free(copy);
    return -1;
  }
  while (got < (size_t)n) {
    ssize_t r = read(fd, copy + got, (size_t)n - got);

    if (r < 0) {
      if (errno == EINTR) {
        continue;
      }
      close(fd);
      sqlite3_free(copy);
      return -1;
    }
    if (r == 0) {
      break;
    }
    got += (size_t)r;
  }
  close(fd);
  if (got != (size_t)n || memcmp(copy, "SQLite format 3", 16) != 0) {
    sqlite3_free(copy);
    return -1;
  }
  if (!adopt_image(copy, n, 1)) {
    return -1;
  }
  return 1;
}

static int open_db(void) {
  int external;

  if (db_state != 0) {
    return db_state == 1;
  }
  db_state = -1;
  external = load_redump_file();
  if (external < 0) {
    klog_printf("[Redump] /data/redump.db unusable\n");
  }
  if (external == 1 || load_embedded()) {
    if (external == 1) {
      klog_printf("[Redump] /data/redump.db\n");
    }
    db_state = 1;
    return 1;
  }
  return 0;
}

static int hash_span(int fd, uint64_t offset, uint64_t size, struct disc_hash *out) {
  uint8_t *buf = malloc(64u * 1024u);
  struct disc_hash_ctx ctx;
  uint64_t left = size;

  if (!buf) {
    return -1;
  }
  if (lseek(fd, (off_t)offset, SEEK_SET) < 0) {
    free(buf);
    return -1;
  }
  disc_hash_init(&ctx);
  while (left > 0) {
    size_t want = left > 64u * 1024u ? 64u * 1024u : (size_t)left;
    ssize_t got;

    do {
      got = read(fd, buf, want);
    } while (got < 0 && errno == EINTR);
    if (got <= 0) {
      free(buf);
      return -1;
    }
    disc_hash_update(&ctx, buf, (size_t)got);
    left -= (uint64_t)got;
  }
  disc_hash_final(&ctx, out);
  free(buf);
  return 0;
}

static int same_hash(const struct disc_hash *got, uint64_t size, uint32_t crc, const void *md5, int md5_n, const void *sha1, int sha1_n) {
  if (!md5 || !sha1 || md5_n != 16 || sha1_n != 20) {
    return 0;
  }
  return got->size == size && got->crc == crc && memcmp(got->md5, md5, 16) == 0 && memcmp(got->sha1, sha1, 20) == 0;
}

static int match_image(const struct disc_hash *whole, int iso, char *dest, size_t dest_len) {
  const char *kind = iso ? "iso" : "bin";

  dest[0] = '\0';
  if (sqlite3_bind_blob(image_stmt, 1, whole->sha1, 20, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_bind_int64(image_stmt, 2, (sqlite3_int64)whole->size) != SQLITE_OK || sqlite3_bind_int64(image_stmt, 3, (sqlite3_int64)whole->crc) != SQLITE_OK || sqlite3_bind_blob(image_stmt, 4, whole->md5, 16, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_bind_text(image_stmt, 5, kind, -1, SQLITE_STATIC) != SQLITE_OK) {
    sqlite3_reset(image_stmt);
    return 0;
  }
  if (sqlite3_step(image_stmt) == SQLITE_ROW) {
    const unsigned char *text = sqlite3_column_text(image_stmt, 0);

    if (text) {
      copy_exact(dest, dest_len, (const char *)text);
    }
  }
  sqlite3_reset(image_stmt);
  return dest[0] != '\0';
}

// size_col is the size column. crc, md5, and sha1 follow it.
static int row_matches(sqlite3_stmt *stmt, int size_col, const struct disc_hash *got) {
  sqlite3_int64 size = sqlite3_column_int64(stmt, size_col);
  uint32_t crc;
  const void *md5;
  const void *sha1;
  int md5_n;
  int sha1_n;

  if (size < 0) {
    return 0;
  }
  crc = (uint32_t)sqlite3_column_int64(stmt, size_col + 1);
  md5 = sqlite3_column_blob(stmt, size_col + 2);
  md5_n = sqlite3_column_bytes(stmt, size_col + 2);
  sha1 = sqlite3_column_blob(stmt, size_col + 3);
  sha1_n = sqlite3_column_bytes(stmt, size_col + 3);
  return same_hash(got, (uint64_t)size, crc, md5, md5_n, sha1, sha1_n);
}

// Track rows are the Redump split bins, in order. The dumped bin is those bytes concatenated.
// lead, when set, is the hash of the first lead_n bytes and is not read again.
static int match_tracks(int fd, const char *game, const struct disc_hash *whole, const struct disc_hash *lead, uint64_t lead_n) {
  sqlite3_int64 count;
  sqlite3_int64 sum;
  uint64_t offset = 0;
  int first = 1;

  if (sqlite3_bind_text(sum_stmt, 1, game, -1, SQLITE_TRANSIENT) != SQLITE_OK || sqlite3_step(sum_stmt) != SQLITE_ROW) {
    sqlite3_reset(sum_stmt);
    return 0;
  }
  count = sqlite3_column_int64(sum_stmt, 0);
  sum = sqlite3_column_int64(sum_stmt, 1);
  sqlite3_reset(sum_stmt);
  if (count <= 0 || (uint64_t)sum != whole->size) {
    return 0;
  }

  // One track the size of the bin is the dump hash. The file does not need another read.
  if (count == 1) {
    int ok = 0;

    if (sqlite3_bind_text(track_stmt, 1, game, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
      sqlite3_reset(track_stmt);
      return 0;
    }
    if (sqlite3_step(track_stmt) == SQLITE_ROW && row_matches(track_stmt, 0, whole) && sqlite3_step(track_stmt) == SQLITE_DONE) {
      ok = 1;
    }
    sqlite3_reset(track_stmt);
    return ok;
  }

  if (sqlite3_bind_text(track_stmt, 1, game, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    sqlite3_reset(track_stmt);
    return 0;
  }
  for (;;) {
    int step = sqlite3_step(track_stmt);
    sqlite3_int64 size;
    struct disc_hash got;

    if (step == SQLITE_DONE) {
      break;
    }
    if (step != SQLITE_ROW) {
      sqlite3_reset(track_stmt);
      return 0;
    }
    size = sqlite3_column_int64(track_stmt, 0);
    if (size < 0) {
      sqlite3_reset(track_stmt);
      return 0;
    }
    if (first && lead && lead_n == (uint64_t)size && row_matches(track_stmt, 0, lead)) {
      first = 0;
      offset += (uint64_t)size;
      continue;
    }
    first = 0;
    if (hash_span(fd, offset, (uint64_t)size, &got) != 0 || !row_matches(track_stmt, 0, &got)) {
      sqlite3_reset(track_stmt);
      return 0;
    }
    offset += (uint64_t)size;
  }
  sqlite3_reset(track_stmt);
  return offset == whole->size;
}

static int match_serial_tracks(int fd, const char *serial, const struct disc_hash *whole, char *dest, size_t dest_len) {
  if (sqlite3_bind_text(list_stmt, 1, serial, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    sqlite3_reset(list_stmt);
    return 0;
  }
  for (;;) {
    int step = sqlite3_step(list_stmt);
    char game[512];
    const unsigned char *text;

    if (step == SQLITE_DONE) {
      break;
    }
    if (step != SQLITE_ROW) {
      break;
    }
    text = sqlite3_column_text(list_stmt, 0);
    copy_exact(game, sizeof(game), text ? (const char *)text : NULL);
    if (game[0] != '\0' && match_tracks(fd, game, whole, NULL, 0)) {
      copy_exact(dest, dest_len, game);
      sqlite3_reset(list_stmt);
      return dest[0] != '\0';
    }
  }
  sqlite3_reset(list_stmt);
  return 0;
}

// Games that share a merged-bin size. Track 1 is hashed once per distinct length.
static int match_size_tracks(int fd, const struct disc_hash *whole, char *dest, size_t dest_len) {
  uint64_t image_size = whole->size;
  uint64_t hashed_size = 0;
  struct disc_hash prefix = {0};
  int prefix_ok = 0;

  if (sqlite3_bind_int64(size_stmt, 1, (sqlite3_int64)image_size) != SQLITE_OK) {
    sqlite3_reset(size_stmt);
    return 0;
  }
  for (;;) {
    int step = sqlite3_step(size_stmt);
    char game[512];
    const unsigned char *text;
    sqlite3_int64 size;

    if (step == SQLITE_DONE) {
      break;
    }
    if (step != SQLITE_ROW) {
      break;
    }
    size = sqlite3_column_int64(size_stmt, 1);
    if (size <= 0 || (uint64_t)size > image_size) {
      continue;
    }
    if ((uint64_t)size == image_size) {
      prefix = *whole;
      prefix_ok = 1;
      hashed_size = image_size;
    } else if ((uint64_t)size != hashed_size) {
      prefix_ok = hash_span(fd, 0, (uint64_t)size, &prefix) == 0;
      hashed_size = (uint64_t)size;
    }
    if (!prefix_ok || !row_matches(size_stmt, 1, &prefix)) {
      continue;
    }
    text = sqlite3_column_text(size_stmt, 0);
    copy_exact(game, sizeof(game), text ? (const char *)text : NULL);
    if (game[0] != '\0' && match_tracks(fd, game, whole, &prefix, (uint64_t)size)) {
      copy_exact(dest, dest_len, game);
      sqlite3_reset(size_stmt);
      return dest[0] != '\0';
    }
  }
  sqlite3_reset(size_stmt);
  return 0;
}

void redump_name(const char *serial, char *dest, size_t dest_len) {
  int rows = 0;

  if (!dest || dest_len == 0) {
    return;
  }
  dest[0] = '\0';
  if (!serial || serial[0] == '\0' || strncmp(serial, "GENERIC_", 8) == 0 || !open_db()) {
    return;
  }
  if (sqlite3_bind_text(name_stmt, 1, serial, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    sqlite3_reset(name_stmt);
    return;
  }
  while (sqlite3_step(name_stmt) == SQLITE_ROW && rows < 2) {
    const unsigned char *text = sqlite3_column_text(name_stmt, 0);

    rows++;
    if (rows == 1 && text) {
      copy_exact(dest, dest_len, (const char *)text);
    }
  }
  if (rows != 1) {
    dest[0] = '\0';
  }
  sqlite3_reset(name_stmt);
}

int redump_each_name(const char *serial, int (*visit)(const char *name, void *user), void *user) {
  if (!visit || !serial || serial[0] == '\0' || strncmp(serial, "GENERIC_", 8) == 0 || !open_db()) {
    return 0;
  }
  if (sqlite3_bind_text(list_stmt, 1, serial, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
    sqlite3_reset(list_stmt);
    return 0;
  }
  for (;;) {
    int step = sqlite3_step(list_stmt);
    const unsigned char *text;
    int stop;

    if (step != SQLITE_ROW) {
      break;
    }
    text = sqlite3_column_text(list_stmt, 0);
    if (!text) {
      continue;
    }
    stop = visit((const char *)text, user);
    if (stop != 0) {
      sqlite3_reset(list_stmt);
      return stop;
    }
  }
  sqlite3_reset(list_stmt);
  return 0;
}

void redump_name_for_size(uint64_t size, int iso, char *dest, size_t dest_len) {
  const char *kind = iso ? "iso" : "bin";
  int rows = 0;

  if (!dest || dest_len == 0) {
    return;
  }
  dest[0] = '\0';
  if (size == 0 || !open_db()) {
    return;
  }
  if (sqlite3_bind_int64(size_name_stmt, 1, (sqlite3_int64)size) != SQLITE_OK || sqlite3_bind_text(size_name_stmt, 2, kind, -1, SQLITE_STATIC) != SQLITE_OK) {
    sqlite3_reset(size_name_stmt);
    return;
  }
  while (sqlite3_step(size_name_stmt) == SQLITE_ROW && rows < 2) {
    const unsigned char *text = sqlite3_column_text(size_name_stmt, 0);

    rows++;
    if (rows == 1 && text) {
      copy_exact(dest, dest_len, (const char *)text);
    }
  }
  if (rows != 1) {
    dest[0] = '\0';
  }
  sqlite3_reset(size_name_stmt);
}

int redump_verify(const char *serial, const char *image_path, const struct disc_hash *whole, int iso, char *name, size_t name_len) {
  int fd;
  int matched;
  int unnamed;

  if (!name || name_len == 0) {
    return 0;
  }
  name[0] = '\0';
  if (!whole || !image_path || image_path[0] == '\0' || !open_db()) {
    return 0;
  }
  // A whole-file match identifies the disc even when the serial is missing or shared.
  if (match_image(whole, iso, name, name_len)) {
    return 1;
  }
  if (iso) {
    return 0;
  }
  // GENERIC_* is not a Redump serial, so the track search uses the bin size.
  unnamed = !serial || serial[0] == '\0' || strncmp(serial, "GENERIC_", 8) == 0;
  fd = open(image_path, O_RDONLY);
  if (fd < 0) {
    return 0;
  }
  if (unnamed) {
    matched = match_size_tracks(fd, whole, name, name_len);
  } else {
    matched = match_serial_tracks(fd, serial, whole, name, name_len);
  }
  close(fd);
  return matched;
}
