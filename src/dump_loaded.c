#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "disc_dump.h"
#include "disc_serial.h"
#include "dump_loaded.h"
#include "redump.h"

#if defined(__ORBIS__) && defined(__PROSPERO__)
#error "Define only one of __ORBIS__ or __PROSPERO__"
#elif defined(__ORBIS__)
#include <ps4/klog.h>
#elif defined(__PROSPERO__)
#include <ps5/klog.h>
#else
#error "Build for PS4 (__ORBIS__) or PS5 (__PROSPERO__)"
#endif

// The cue this payload writes fits well under this. A larger sheet is left unchanged.
#define CUE_REWRITE_MAX 32768u

struct dump_log {
  const char *label;
  unsigned last_pct;
  uint64_t last_mark;
  uint64_t done;
  uint64_t start_ms;
  uint64_t sample_ms;
  uint64_t sample_done;
};

static int serial_filename(const char *serial) {
  const unsigned char *p;

  if (!serial || serial[0] == '\0') {
    return 0;
  }
  for (p = (const unsigned char *)serial; *p != '\0'; p++) {
    if (!isalnum(*p) && *p != '-' && *p != '_') {
      return 0;
    }
  }
  return 1;
}

// GENERIC_PS1 and the other unknown-title ids are not a Redump serial.
static int generic_serial(const char *serial) {
  return serial && strncmp(serial, "GENERIC_", 8) == 0;
}

// Redump stems may contain spaces and parentheses. Reject a name that cannot be one path component.
static int dump_name_ok(const char *name, size_t path_len) {
  const unsigned char *p;
  size_t n = 0;

  if (!name || name[0] == '\0' || name[0] == '.') {
    return 0;
  }
  for (p = (const unsigned char *)name; *p != '\0'; p++) {
    if (*p < 0x20 || *p == '/' || *p == '\\' || *p == '"') {
      return 0;
    }
    n++;
  }
  // "/data/" + name + ".cue" and the terminating NUL.
  return n + 11 <= path_len;
}

static uint64_t mono_ms(void) {
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
    return 0;
  }
  return (uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull;
}

static void format_span(char *dst, size_t n, uint64_t ms) {
  unsigned sec = (unsigned)(ms / 1000ull);
  unsigned min = sec / 60u;
  unsigned hr = min / 60u;

  sec %= 60u;
  min %= 60u;
  if (hr > 0) {
    snprintf(dst, n, "%u:%02u:%02u", hr, min, sec);
  } else {
    snprintf(dst, n, "%u:%02u", min, sec);
  }
}

static void format_bytes(char *dst, size_t n, uint64_t bytes) {
  const uint64_t mib = 1024ull * 1024ull;
  const uint64_t gib = mib * 1024ull;

  if (bytes >= gib) {
    uint64_t tenths = (bytes * 10ull) / gib;
    snprintf(dst, n, "%u.%u GiB", (unsigned)(tenths / 10ull), (unsigned)(tenths % 10ull));
  } else if (bytes >= mib) {
    uint64_t tenths = (bytes * 10ull) / mib;
    snprintf(dst, n, "%u.%u MiB", (unsigned)(tenths / 10ull), (unsigned)(tenths % 10ull));
  } else if (bytes >= 1024ull) {
    snprintf(dst, n, "%u KiB", (unsigned)(bytes / 1024ull));
  } else {
    snprintf(dst, n, "%u B", (unsigned)bytes);
  }
}

static void format_speed(char *dst, size_t n, uint64_t bytes, uint64_t ms) {
  uint64_t per_sec;
  uint64_t tenths;

  if (ms == 0) {
    ms = 1;
  }
  per_sec = (bytes * 1000ull) / ms;
  tenths = (per_sec * 10ull) / (1024ull * 1024ull);
  snprintf(dst, n, "%u.%u MiB/s", (unsigned)(tenths / 10ull), (unsigned)(tenths % 10ull));
}

static void log_dump_progress(struct dump_log *log, uint64_t done, const char *mark) {
  char span[16];
  char speed[24];
  uint64_t now;
  uint64_t elapsed;
  uint64_t dt;
  uint64_t delta;

  now = mono_ms();
  elapsed = now >= log->start_ms ? now - log->start_ms : 0;
  dt = now >= log->sample_ms ? now - log->sample_ms : 0;
  delta = done >= log->sample_done ? done - log->sample_done : 0;
  format_span(span, sizeof(span), elapsed);
  format_speed(speed, sizeof(speed), delta, dt);
  log->sample_ms = now;
  log->sample_done = done;
  klog_printf("[Dump] %s %s %s %s\n", log->label, mark, span, speed);
}

static void on_dump_progress(uint64_t done, uint64_t total, void *user) {
  struct dump_log *log = user;
  char mark[32];

  log->done = done;
  if (total > 0) {
    unsigned pct = (unsigned)((done * 100ull) / total);

    if (pct > 100u) {
      pct = 100u;
    }
    if (pct == log->last_pct) {
      return;
    }
    log->last_pct = pct;
    snprintf(mark, sizeof(mark), "%u%%", pct);
    log_dump_progress(log, done, mark);
    return;
  }
  if (done < log->last_mark + ((uint64_t)64 << 20)) {
    return;
  }
  log->last_mark = done;
  snprintf(mark, sizeof(mark), "%u MiB", (unsigned)(done / (1024u * 1024u)));
  log_dump_progress(log, done, mark);
}

static void log_dump_result(const struct dump_log *log, int ok) {
  char span[16];
  char size[24];
  char speed[24];
  uint64_t now;
  uint64_t elapsed;

  now = mono_ms();
  elapsed = now >= log->start_ms ? now - log->start_ms : 0;
  format_span(span, sizeof(span), elapsed);
  format_bytes(size, sizeof(size), log->done);
  format_speed(speed, sizeof(speed), log->done, elapsed);
  klog_printf("[Dump] %s %s %s in %s, average %s\n", log->label, ok ? "done" : "failed", size, span, speed);
}

static int write_bytes(const char *path, const void *data, size_t n) {
  FILE *fp = fopen(path, "wb");
  size_t wrote;

  if (!fp) {
    return -1;
  }
  wrote = fwrite(data, 1, n, fp);
  if (fclose(fp) != 0 || wrote != n) {
    return -1;
  }
  return 0;
}

// The cue's first line names the bin. A renamed dump has to keep that name in step with the file.
static int rewrite_cue_bin(const char *cue_path, const char *bin_name) {
  FILE *fp;
  char *old;
  char *neu;
  char extra;
  char *quote;
  char *nl;
  size_t n;
  size_t tail;
  int head;
  int rc = -1;

  old = malloc(CUE_REWRITE_MAX);
  neu = malloc(CUE_REWRITE_MAX);
  if (!old || !neu) {
    free(old);
    free(neu);
    return -1;
  }
  fp = fopen(cue_path, "rb");
  if (!fp) {
    goto out;
  }
  n = fread(old, 1, CUE_REWRITE_MAX - 1, fp);
  if (n == 0 || ferror(fp) || fread(&extra, 1, 1, fp) == 1) {
    fclose(fp);
    goto out;
  }
  fclose(fp);
  old[n] = '\0';
  if (n < 6 || memcmp(old, "FILE \"", 6) != 0) {
    goto out;
  }
  quote = strchr(old + 6, '"');
  nl = strchr(old, '\n');
  if (!quote || !nl || quote > nl) {
    goto out;
  }
  head = snprintf(neu, CUE_REWRITE_MAX, "FILE \"%s\" BINARY\n", bin_name);
  if (head < 0 || (size_t)head >= CUE_REWRITE_MAX) {
    goto out;
  }
  tail = n - (size_t)(nl + 1 - old);
  if ((size_t)head + tail >= CUE_REWRITE_MAX) {
    goto out;
  }
  memcpy(neu + head, nl + 1, tail);
  if (write_bytes(cue_path, neu, (size_t)head + tail) != 0) {
    write_bytes(cue_path, old, n);
    goto out;
  }
  rc = 0;
out:
  free(old);
  free(neu);
  return rc;
}

static int data_file(char *dst, size_t dst_len, const char *stem, const char *ext) {
  int n = snprintf(dst, dst_len, "/data/%s%s", stem, ext);

  if (n < 0 || (size_t)n >= dst_len) {
    return -1;
  }
  return 0;
}

// size 0 accepts any non-empty regular file. A positive size must match.
static int file_is_image(const char *path, uint64_t size) {
  struct stat st;

  if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    return 0;
  }
  if (size == 0) {
    return 1;
  }
  return (uint64_t)st.st_size == size;
}

// A finished CD dump is the bin plus its cue. DVD and BD are the iso.
// A shorter bin or iso is a partial dump and is not kept.
static int stem_dumped(const char *stem, int cd, uint64_t size) {
  char path[512];

  if (!dump_name_ok(stem, sizeof(path))) {
    return 0;
  }
  if (data_file(path, sizeof(path), stem, cd ? ".bin" : ".iso") != 0 || !file_is_image(path, size)) {
    return 0;
  }
  if (!cd) {
    return 1;
  }
  if (data_file(path, sizeof(path), stem, ".cue") != 0) {
    return 0;
  }
  return file_is_image(path, 0);
}

static void log_already(int cd, const char *stem) {
  klog_printf("[Dump] /data/%s.%s already dumped\n", stem, cd ? "bin" : "iso");
}

struct dumped_name {
  int cd;
  uint64_t size;
  char stem[512];
};

static int visit_dumped(const char *name, void *user) {
  struct dumped_name *hit = user;
  size_t n;

  if (!stem_dumped(name, hit->cd, hit->size)) {
    return 0;
  }
  n = strlen(name);
  if (n + 1 > sizeof(hit->stem)) {
    return 0;
  }
  memcpy(hit->stem, name, n + 1);
  return 1;
}

static int rename_dump_files(int cd, const char *from_stem, const char *to_stem) {
  char from[512];
  char to[512];
  char bin_from[512];
  char bin_to[512];
  char bin_name[520];
  int n;

  if (strcmp(from_stem, to_stem) == 0) {
    return 0;
  }
  if (!cd) {
    if (data_file(from, sizeof(from), from_stem, ".iso") != 0 || data_file(to, sizeof(to), to_stem, ".iso") != 0) {
      return -1;
    }
    return rename(from, to);
  }
  if (data_file(from, sizeof(from), from_stem, ".cue") != 0 || data_file(to, sizeof(to), to_stem, ".cue") != 0 ||
      data_file(bin_from, sizeof(bin_from), from_stem, ".bin") != 0 || data_file(bin_to, sizeof(bin_to), to_stem, ".bin") != 0) {
    return -1;
  }
  n = snprintf(bin_name, sizeof(bin_name), "%s.bin", to_stem);
  if (n < 0 || (size_t)n >= sizeof(bin_name) || rewrite_cue_bin(from, bin_name) != 0) {
    return -1;
  }
  if (rename(bin_from, bin_to) != 0) {
    snprintf(bin_name, sizeof(bin_name), "%s.bin", from_stem);
    rewrite_cue_bin(from, bin_name);
    return -1;
  }
  if (rename(from, to) != 0) {
    rename(bin_to, bin_from);
    snprintf(bin_name, sizeof(bin_name), "%s.bin", from_stem);
    rewrite_cue_bin(from, bin_name);
    return -1;
  }
  return 0;
}

static void log_dat_result(int cd, const char *serial, const char *name, const char *image_path, const struct disc_hash *hash) {
  char matched[512];
  const char *final_name = name;
  const char *ext = cd ? "bin" : "iso";

  matched[0] = '\0';
  if (redump_verify(serial, image_path, hash, cd ? 0 : 1, matched, sizeof(matched)) && dump_name_ok(matched, sizeof(matched))) {
    if (strcmp(matched, name) != 0 && rename_dump_files(cd, name, matched) == 0) {
      final_name = matched;
    }
    klog_printf("[Redump] /data/%s.%s verified\n", final_name, ext);
    return;
  }
  if (strcmp(name, serial) != 0 && rename_dump_files(cd, name, serial) == 0) {
    final_name = serial;
  }
  klog_printf("[Redump] /data/%s.%s not in Redump dat\n", final_name, ext);
}

static void dump_log_reset(struct dump_log *log, const char *label) {
  log->label = label;
  log->last_pct = 0;
  log->last_mark = 0;
  log->done = 0;
  log->start_ms = mono_ms();
  log->sample_ms = log->start_ms;
  log->sample_done = 0;
}

void dump_loaded(enum disc_type type, const char *serial) {
  char image[512];
  char stem[512];
  const char *name = serial;
  struct dump_log log;
  struct disc_hash hash;
  struct dumped_name hit;
  uint64_t image_bytes = 0;
  int cd = type == DISC_PS1_CD_ROM || type == DISC_PS2_CD_ROM;
  int ps3 = type == DISC_PS3_BD_ROM || type == DISC_PS3_DVD_ROM;
  int rc;

  if (!cd && type != DISC_PS2_DVD_ROM && !ps3) {
    return;
  }
  if (!serial_filename(serial)) {
    return;
  }
  // GENERIC_* is not a Redump serial. The dump starts under that name, and a checksum match can rename it.
  if (!generic_serial(serial)) {
    redump_name(serial, stem, sizeof(stem));
    if (dump_name_ok(stem, sizeof(image))) {
      name = stem;
    }
  }
  if (data_file(image, sizeof(image), name, cd ? ".bin" : ".iso") != 0) {
    return;
  }

  // The drive size tells a finished image from a partial one, and one pressing
  // of a shared serial from another. An unknown size still protects a file
  // this dump would truncate.
  if (disc_image_bytes(type, &image_bytes) != 0) {
    image_bytes = 0;
  }
  if (stem_dumped(name, cd, image_bytes)) {
    log_already(cd, name);
    return;
  }
  if (strcmp(name, serial) != 0 && stem_dumped(serial, cd, image_bytes)) {
    log_already(cd, serial);
    return;
  }
  hit.cd = cd;
  hit.size = image_bytes;
  hit.stem[0] = '\0';
  if (image_bytes > 0 && !generic_serial(serial) && redump_each_name(serial, visit_dumped, &hit)) {
    log_already(cd, hit.stem);
    return;
  }
  if (image_bytes > 0 && generic_serial(serial)) {
    char sole[512];

    redump_name_for_size(image_bytes, cd ? 0 : 1, sole, sizeof(sole));
    if (stem_dumped(sole, cd, image_bytes)) {
      log_already(cd, sole);
      return;
    }
  }

  memset(&hash, '\0', sizeof(hash));
  dump_log_reset(&log, image);
  if (cd) {
    klog_printf("[Dump] /data/%s.bin /data/%s.cue\n", name, name);
    rc = dump_disc(type, image, &hash, on_dump_progress, &log);
  } else if (!ps3) {
    klog_printf("[Dump] %s\n", image);
    rc = dump_disc(type, image, &hash, on_dump_progress, &log);
  } else {
    klog_printf("[Dump] %s\n", image);
    // IRD creation is commented out until the disc key can be read.
    // The drive has no disc-key ioctl, so Data1 and Data2 would stay zero.
    // struct disc_ird *ird = NULL;
    // char ird_path[512];
    // int n;
    // rc = dump_ps3(image, &ird, &hash, on_dump_progress, &log);
    rc = dump_ps3(image, NULL, &hash, on_dump_progress, &log);
    log_dump_result(&log, rc == 0);
    if (rc == 0) {
      log_dat_result(cd, serial, name, image, &hash);
    }
    // if (rc == 0 && ird) {
    //   n = snprintf(ird_path, sizeof(ird_path), "/data/%s.ird", name);
    //   if (n >= 0 && (size_t)n < sizeof(ird_path)) {
    //     klog_printf("[Dump] %s disc key unavailable\n", serial);
    //     klog_printf("[Dump] %s\n", ird_path);
    //     if (disc_ird_write(ird, ird_path, on_dump_progress, &log) != 0) {
    //       unlink(ird_path);
    //     }
    //   }
    //   disc_ird_free(ird);
    // }
    return;
  }
  log_dump_result(&log, rc == 0);
  if (rc == 0) {
    log_dat_result(cd, serial, name, image, &hash);
  }
}
