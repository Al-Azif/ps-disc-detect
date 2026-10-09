#include <ctype.h>
#include <string.h>

#include "disc_serial.h"
#include "system_cnf.h"

#include "ini.h"

struct cnf_find {
  const char *key;
  char *dest;
  size_t dest_len;
  const char *generic;
  int found;
};

// Exact key match, so BOOT does not consume BOOT2. Case does not matter.
static int same_key(const char *name, const char *key) {
  size_t i = 0;

  if (!name || !key) {
    return 0;
  }
  for (; key[i]; i++) {
    if (toupper((unsigned char)name[i]) != (unsigned char)key[i]) {
      return 0;
    }
  }
  return name[i] == '\0';
}

static int on_pair(void *user, const char *section, const char *name, const char *value) {
  struct cnf_find *find = user;

  (void)section;
  if (find->found || !same_key(name, find->key)) {
    return 1;
  }
  if (value) {
    sanitize_serial(value, find->dest, find->dest_len);
    if (find->dest[0] == '\0') {
      set_serial(find->dest, find->dest_len, find->generic);
    }
  } else {
    set_serial(find->dest, find->dest_len, find->generic);
  }
  find->found = 1;
  return 1;
}

int cnf_boot_serial(const char *path, const char *key, char *dest, size_t dest_len, const char *generic) {
  struct cnf_find find;
  int rc;

  if (!dest || dest_len == 0) {
    return 0;
  }
  dest[0] = '\0';
  find.key = key;
  find.dest = dest;
  find.dest_len = dest_len;
  find.generic = generic;
  find.found = 0;
  rc = ini_parse(path, on_pair, &find);
  if (rc == -1) {
    return 0;
  }
  return find.found;
}
