#include <dirent.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "disc_mount.h"
#include "disc_serial.h"
#include "disc_title.h"
#include "param_json.h"
#include "sfo.h"
#include "system_cnf.h"

/*
 * Title ID from the mounted game, once detect_disc() has named the generation.
 *
 * PS1: SYSTEM.CNF BOOT=, else the PVD volume ID. PSX.EXE in that line falls back to the PVD.
 * PS2: SYSTEM.CNF BOOT2=.
 * PS3: TITLE_ID in PS3_GAME/PARAM.SFO. Field max length is 16. Logged as XXXX-YYYYY.
 * PS4: TITLE_ID in bd/param.sfo, after its 0x800-byte header. Field max length is 12.
 *      Otherwise the 9-character directory name under app/. Logged as XXXX-YYYYY.
 * PS5: first PS5GD contentId in bd/param.json, the 9 bytes at [7, 16).
 *      Otherwise the 9-character directory name under app/. Logged as XXXX-YYYYY.
 * A disc whose title read fails yields GENERIC_PS1, GENERIC_PS2, GENERIC_PS3, GENERIC_PS4, or GENERIC_PS5.
 *
 * Returns:
 * 1 and writes dest
 * 0 if this profile has no title ID
 * -1 while the game files are still mounting
 */

#define SECTOR_SIZE 0x800
#define ISO_PVD_SECTOR 0x10

static const char DISC_DEV[] = "/dev/cd0";

static int serial_is_psx_exe(const char *serial) {
  return serial && (strcmp(serial, "PSXEXE") == 0 || strcmp(serial, "PSX-EXE") == 0);
}

static void read_pvd_serial(char *dest, size_t dest_len) {
  int fd;
  unsigned char buffer[SECTOR_SIZE];

  if (!dest || dest_len == 0) {
    return;
  }
  dest[0] = '\0';
  fd = open(DISC_DEV, O_RDONLY);
  if (fd < 0) {
    return;
  }
  if (pread(fd, buffer, SECTOR_SIZE, (off_t)ISO_PVD_SECTOR * SECTOR_SIZE) == SECTOR_SIZE) {
    char vol_id[33];
    memcpy(vol_id, buffer + 40, 32);
    vol_id[32] = '\0';
    sanitize_serial(vol_id, dest, dest_len);
  }
  close(fd);
}

static void use_pvd_serial(char *dest, size_t dest_len, const char *pvd_serial) {
  set_serial(dest, dest_len, (pvd_serial && pvd_serial[0]) ? pvd_serial : "GENERIC_PS1");
}

// 1: a 9-character title directory was copied.
// 0: app/ is present but has no title directory.
// -1: app/ is not there yet.
static int app_dir_title(char *dest, size_t dest_len) {
  char path[512];
  struct stat st;
  DIR *dir;
  struct dirent *entry;

  disc_path(path, sizeof(path), "app");
  if (stat(path, &st) != 0) {
    return -1;
  }
  if (!S_ISDIR(st.st_mode)) {
    return 0;
  }
  dir = opendir(path);
  if (!dir) {
    return 0;
  }
  while ((entry = readdir(dir)) != NULL) {
    const char *name = entry->d_name;
    size_t i;

    if (entry->d_fileno == 0 || entry->d_type != DT_DIR || strlen(name) != 9) {
      continue;
    }
    for (i = 0; i < 9; i++) {
      unsigned char c = (unsigned char)name[i];
      if (!((unsigned)(c - 'A') < 26u || (unsigned)(c - '0') <= 9u)) {
        break;
      }
    }
    if (i != 9) {
      continue;
    }
    if (dest_len <= 10) {
      closedir(dir);
      return 0;
    }
    memcpy(dest, name, 9);
    dest[9] = '\0';
    hyphenate_serial(dest, dest_len);
    closedir(dir);
    return dest[0] != '\0';
  }
  closedir(dir);
  return 0;
}

static int title_ps1(char *dest, size_t dest_len) {
  int has_files = 0;
  int has_psx = 0;
  char cnf[512];
  char pvd[32];

  if (scan_mount(&has_files, &has_psx, cnf, sizeof(cnf)) != 0 || !has_files) {
    return -1;
  }
  // Wait for the boot file. Any earlier directory entry would publish the volume ID too soon.
  if (cnf[0] == '\0' && !has_psx) {
    return -1;
  }
  read_pvd_serial(pvd, sizeof(pvd));
  if (cnf[0] != '\0' && cnf_boot_serial(cnf, "BOOT", dest, dest_len, "GENERIC_PS1")) {
    if (serial_is_psx_exe(dest)) {
      use_pvd_serial(dest, dest_len, pvd);
    }
    return 1;
  }
  if (!has_psx) {
    return -1;
  }
  use_pvd_serial(dest, dest_len, pvd);
  return 1;
}

static int title_ps2(char *dest, size_t dest_len) {
  int has_files = 0;
  char cnf[512];

  if (scan_mount(&has_files, NULL, cnf, sizeof(cnf)) != 0 || !has_files) {
    return -1;
  }
  if (cnf[0] == '\0' || !cnf_boot_serial(cnf, "BOOT2", dest, dest_len, "GENERIC_PS2")) {
    return -1;
  }
  return 1;
}

static int title_ps3(char *dest, size_t dest_len) {
  unsigned char *buf = NULL;
  size_t n = 0;
  int loaded = load_disc_file("PS3_GAME/PARAM.SFO", 1, SFO_MAX_SIZE, 1, &buf, &n);
  int ok = 0;

  if (loaded == 1 && sfo_layout_ok(buf, n)) {
    ok = sfo_copy_title(buf, n, 16, 9, dest, dest_len);
  }
  free(buf);
  if (ok) {
    return 1;
  }
  if (loaded < 0 && !mount_has_files()) {
    return -1;
  }
  set_serial(dest, dest_len, "GENERIC_PS3");
  return 1;
}

// 1: title written.
// -1: the mount is still empty.
static int title_or_dir(int loaded, int ok, char *dest, size_t dest_len, const char *generic) {
  int from_dir;

  if (ok) {
    return 1;
  }
  from_dir = app_dir_title(dest, dest_len);
  if (from_dir == 1) {
    return 1;
  }
  if (loaded < 0 && from_dir < 0 && !mount_has_files()) {
    return -1;
  }
  set_serial(dest, dest_len, generic);
  return 1;
}

static int title_ps4(char *dest, size_t dest_len) {
  unsigned char *buf = NULL;
  size_t n = 0;
  int loaded = load_disc_file("bd/param.sfo", SFO_ROOT_HDR, SFO_MAX_SIZE, 1, &buf, &n);
  int ok = 0;

  if (loaded == 1 && n > SFO_ROOT_HDR && sfo_layout_ok(buf + SFO_ROOT_HDR, n - SFO_ROOT_HDR)) {
    ok = sfo_copy_title(buf + SFO_ROOT_HDR, n - SFO_ROOT_HDR, 12, 11, dest, dest_len);
  }
  free(buf);
  return title_or_dir(loaded, ok, dest, dest_len, "GENERIC_PS4");
}

static int title_ps5(char *dest, size_t dest_len) {
  unsigned char *buf = NULL;
  size_t n = 0;
  int loaded = load_disc_file("bd/param.json", JSON_FILE_MIN, JSON_FILE_MAX, 1, &buf, &n);
  int ok = 0;

  // sceSblRootParamVerifyPprRootParam (ioctl 0xC0305202 on /dev/rootparam) is not called. The JSON is the file after the 0x800-byte header.
  // The parser accepts 1..0x20000 bytes of that JSON.
  if (loaded == 1 && n > JSON_FILE_MIN) {
    size_t body = n - JSON_FILE_MIN;
    if (body <= JSON_BODY_MAX) {
      ok = json_ps5_title(buf + JSON_FILE_MIN, body, dest, dest_len);
    }
  }
  free(buf);
  return title_or_dir(loaded, ok, dest, dest_len, "GENERIC_PS5");
}

int disc_has_title(enum disc_type type) {
  switch (type) {
  case DISC_PS1_CD_ROM:
  case DISC_PS2_CD_ROM:
  case DISC_PS2_DVD_ROM:
  case DISC_PS3_DVD_ROM:
  case DISC_PS3_BD_ROM:
  case DISC_PS4_BD_ROM:
  case DISC_PPR_BD_ROM:
    return 1;
  default:
    return 0;
  }
}

int disc_title_id(enum disc_type type, char *dest, size_t dest_len) {
  if (!dest || dest_len == 0) {
    return 0;
  }
  dest[0] = '\0';
  switch (type) {
  case DISC_PS1_CD_ROM:
    return title_ps1(dest, dest_len);
  case DISC_PS2_CD_ROM:
  case DISC_PS2_DVD_ROM:
    return title_ps2(dest, dest_len);
  case DISC_PS3_DVD_ROM:
  case DISC_PS3_BD_ROM:
    return title_ps3(dest, dest_len);
  case DISC_PS4_BD_ROM:
    return title_ps4(dest, dest_len);
  case DISC_PPR_BD_ROM:
    return title_ps5(dest, dest_len);
  default:
    return 0;
  }
}
