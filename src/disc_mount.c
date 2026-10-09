#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "disc_mount.h"

static const char DISC_MNT[] = "/mnt/disc";

const char *disc_mount_root(void) {
  return DISC_MNT;
}

void disc_path(char *out, size_t out_len, const char *rel) {
  snprintf(out, out_len, "%s/%s", DISC_MNT, rel);
}

static int iso_name_is(const char *name, const char *wanted) {
  size_t i = 0;

  for (; wanted[i]; i++) {
    if (toupper((unsigned char)name[i]) != toupper((unsigned char)wanted[i])) {
      return 0;
    }
  }
  return name[i] == '\0' || name[i] == ';';
}

int mount_has_files(void) {
  DIR *dir = opendir(DISC_MNT);
  struct dirent *entry;
  int has = 0;

  if (!dir) {
    return 0;
  }
  while ((entry = readdir(dir)) != NULL) {
    const char *name = entry->d_name;
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
      continue;
    }
    has = 1;
    break;
  }
  closedir(dir);
  return has;
}

int scan_mount(int *has_files, int *has_psx_exe, char *cnf_path, size_t cnf_path_len) {
  DIR *dir;
  struct dirent *entry;

  if (has_files) {
    *has_files = 0;
  }
  if (has_psx_exe) {
    *has_psx_exe = 0;
  }
  if (cnf_path && cnf_path_len) {
    cnf_path[0] = '\0';
  }

  dir = opendir(DISC_MNT);
  if (!dir) {
    return -1;
  }
  while ((entry = readdir(dir)) != NULL) {
    const char *name = entry->d_name;
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) {
      continue;
    }
    if (has_files) {
      *has_files = 1;
    }
    if (has_psx_exe && iso_name_is(name, "PSX.EXE")) {
      *has_psx_exe = 1;
    }
    if (cnf_path && cnf_path_len && cnf_path[0] == '\0' && iso_name_is(name, "SYSTEM.CNF")) {
      snprintf(cnf_path, cnf_path_len, "%s/%s", DISC_MNT, name);
    }
  }
  closedir(dir);
  return 0;
}

// 1: file loaded.
// 0: path exists but was rejected.
// -1: path is not there yet.
int load_disc_file(const char *rel, off_t min_size, off_t max_size, int regular, unsigned char **out, size_t *out_n) {
  char path[512];
  struct stat st;
  unsigned char *buf;
  int fd;
  size_t got = 0;

  *out = NULL;
  *out_n = 0;
  disc_path(path, sizeof(path), rel);
  if (stat(path, &st) != 0) {
    return -1;
  }
  if ((regular && !S_ISREG(st.st_mode)) || st.st_size < min_size || st.st_size > max_size) {
    return 0;
  }
  buf = malloc((size_t)st.st_size);
  if (!buf) {
    return 0;
  }
  fd = open(path, O_RDONLY);
  if (fd < 0) {
    free(buf);
    return 0;
  }
  while (got < (size_t)st.st_size) {
    ssize_t n = read(fd, buf + got, (size_t)st.st_size - got);
    if (n < 0) {
      close(fd);
      free(buf);
      return 0;
    }
    if (n == 0) {
      break;
    }
    got += (size_t)n;
  }
  close(fd);
  if (got != (size_t)st.st_size) {
    free(buf);
    return 0;
  }
  *out = buf;
  *out_n = got;
  return 1;
}
