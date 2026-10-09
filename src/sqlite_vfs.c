#include <stdint.h>

#include <sqlite3.h>

/*
 * The payload libc has no pread, mmap, or fdatasync, which the SQLite unix
 * VFS references. Redump lookup deserializes an in-memory database (the
 * embedded redump_db image, or a copy of /data/redump.db read with the C library),
 * so this VFS exists to satisfy sqlite3_os_init and never opens a file.
 */

static int payload_open(sqlite3_vfs *vfs, const char *name, sqlite3_file *file, int flags, int *out_flags) {
  (void)vfs;
  (void)name;
  (void)flags;
  (void)out_flags;
  file->pMethods = 0;
  return SQLITE_CANTOPEN;
}

static int payload_delete(sqlite3_vfs *vfs, const char *name, int sync_dir) {
  (void)vfs;
  (void)name;
  (void)sync_dir;
  return SQLITE_IOERR_DELETE;
}

static int payload_access(sqlite3_vfs *vfs, const char *name, int flags, int *result) {
  (void)vfs;
  (void)name;
  (void)flags;
  *result = 0;
  return SQLITE_OK;
}

static int payload_full_path(sqlite3_vfs *vfs, const char *in, int out_len, char *out) {
  int i = 0;

  (void)vfs;
  if (out_len <= 0) {
    return SQLITE_CANTOPEN;
  }
  if (!in) {
    in = "";
  }
  while (in[i] && i + 1 < out_len) {
    out[i] = in[i];
    i++;
  }
  out[i] = '\0';
  return SQLITE_OK;
}

static void *payload_dlopen(sqlite3_vfs *vfs, const char *path) {
  (void)vfs;
  (void)path;
  return 0;
}

static void payload_dlerror(sqlite3_vfs *vfs, int out_len, char *out) {
  (void)vfs;
  if (out_len > 0) {
    out[0] = '\0';
  }
}

static void (*payload_dlsym(sqlite3_vfs *vfs, void *handle, const char *symbol))(void) {
  (void)vfs;
  (void)handle;
  (void)symbol;
  return 0;
}

static void payload_dlclose(sqlite3_vfs *vfs, void *handle) {
  (void)vfs;
  (void)handle;
}

static int payload_random(sqlite3_vfs *vfs, int n, char *out) {
  static uint32_t state = 0x4E434454u;
  int i;

  (void)vfs;
  for (i = 0; i < n; i++) {
    state = state * 1664525u + 1013904223u;
    out[i] = (char)(state >> 24);
  }
  return n;
}

static int payload_sleep(sqlite3_vfs *vfs, int microseconds) {
  (void)vfs;
  (void)microseconds;
  return 0;
}

static int payload_last_error(sqlite3_vfs *vfs, int out_len, char *out) {
  (void)vfs;
  (void)out_len;
  (void)out;
  return 0;
}

static int payload_time(sqlite3_vfs *vfs, sqlite3_int64 *now) {
  (void)vfs;
  *now = 0;
  return SQLITE_OK;
}

static sqlite3_vfs payload_vfs = {2, sizeof(sqlite3_file), 512, 0, "payload", 0, payload_open, payload_delete, payload_access, payload_full_path, payload_dlopen, payload_dlerror, payload_dlsym, payload_dlclose, payload_random, payload_sleep, 0, payload_last_error, payload_time};

int sqlite3_os_init(void) {
  return sqlite3_vfs_register(&payload_vfs, 1);
}

int sqlite3_os_end(void) {
  return SQLITE_OK;
}
