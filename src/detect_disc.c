#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/cam_compat.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#include "detect_disc.h"

/*
 * PS4 headers report CAM_VERSION 0x17, but that kernel still encodes 0x16
 * and the 0x4A8-byte CCB, so the ioctl is CAMIOCOMMAND_0x16.
 * PS5 headers report CAM_VERSION 0x19 and the 0x4E0-byte CCB, and
 * CAMIOCOMMAND in scsi_pass.h already encodes that.
 * A PS4 disc's Sony inquiry rewrites BD profiles 0x40-0x43 to 0xFF80.
 * PS5 ShellCore keeps the GET CONFIGURATION profile.
 */

#if defined(__ORBIS__)
#define DISC_CAMIOCOMMAND CAMIOCOMMAND_0x16
#define DISC_CAMGETPASSTHRU CAMGETPASSTHRU_0x16
#define DISC_CCB_SIZE 0x4A8
_Static_assert(CAMIOCOMMAND_0x16 == 0xC4A81602u, "CAMIOCOMMAND");
_Static_assert(CAMGETPASSTHRU_0x16 == 0xC4A81603u, "CAMGETPASSTHRU");
#elif defined(__PROSPERO__)
#define DISC_CAMIOCOMMAND CAMIOCOMMAND
#define DISC_CAMGETPASSTHRU CAMGETPASSTHRU
#define DISC_CCB_SIZE 0x4E0
_Static_assert(CAMIOCOMMAND == 0xC4E01902u, "CAMIOCOMMAND");
_Static_assert(CAMGETPASSTHRU == 0xC4E01903u, "CAMGETPASSTHRU");
#endif

_Static_assert(sizeof(union ccb) == DISC_CCB_SIZE, "ccb size");

// Fixed sense (0x70) or descriptor sense (0x72)
static uint32_t decode_sense(const uint8_t *s) {
  unsigned key;
  unsigned asc;
  unsigned ascq;

  if ((s[0] & 0x7E) == 0x72) {
    key = s[1] & 0x0F;
    asc = s[2];
    ascq = s[3];
  } else {
    key = s[2] & 0x0F;
    if (s[7] <= 4) {
      asc = 0;
      ascq = 0;
    } else {
      asc = s[12];
      ascq = (s[7] == 5) ? 0 : s[13];
    }
  }
  return (key << 16) | (asc << 8) | ascq;
}

// Commands are not nested, so one CCB is reused instead of allocating one per read.
static union ccb cam_ccb;

static union ccb *cam_ccb_get(void) {
  memset(&cam_ccb, '\0', sizeof(cam_ccb));
  return &cam_ccb;
}

static int scsi_cmd(int fd, uint32_t path, uint32_t target, uint32_t lun, const uint8_t *cdb, uint8_t cdb_len, void *data, uint32_t data_len, uint32_t dir, uint32_t timeout_ms, uint32_t *sense_out) {
  union ccb *ccb = cam_ccb_get();
  int rc;

  ccb->csio.ccb_h.retry_count = 1;
  ccb->csio.ccb_h.func_code = XPT_SCSI_IO;
  ccb->csio.ccb_h.path_id = path;
  ccb->csio.ccb_h.target_id = target;
  ccb->csio.ccb_h.target_lun = lun;
  ccb->csio.ccb_h.flags = dir;
  ccb->csio.ccb_h.timeout = timeout_ms;
  ccb->csio.data_ptr = data;
  ccb->csio.dxfer_len = data_len;
  ccb->csio.sense_len = SSD_FULL_SIZE;
  ccb->csio.cdb_len = cdb_len;
  ccb->csio.tag_action = MSG_SIMPLE_Q_TAG;
  memcpy(ccb->csio.cdb_io.cdb_bytes, cdb, cdb_len);

  rc = ioctl(fd, DISC_CAMIOCOMMAND, ccb);
  if (rc < 0) {
    if (sense_out) {
      *sense_out = 0;
    }
    return -1;
  }

  if ((ccb->csio.ccb_h.status & CAM_STATUS_MASK) == CAM_REQ_CMP) {
    return 0;
  }

  if (sense_out) {
    uint32_t status = ccb->csio.ccb_h.status;
    *sense_out = 0;
    if ((status & ~CAM_DEV_QFRZN) == (CAM_AUTOSNS_VALID | CAM_SCSI_STATUS_ERROR) && ccb->csio.scsi_status == 2) {
      *sense_out = decode_sense((const uint8_t *)&ccb->csio.sense_data);
    }
  }
  return -1;
}

static int get_passthru(int fd, const char *name, uint32_t unit, char *out_name, uint32_t *out_unit, uint32_t *path, uint32_t *target, uint32_t *lun) {
  union ccb *ccb = cam_ccb_get();
  unsigned i;

  ccb->cgdl.ccb_h.func_code = XPT_GDEVLIST;
  if (name) {
    for (i = 0; name[i] && i < sizeof(ccb->cgdl.periph_name) - 1; i++) {
      ccb->cgdl.periph_name[i] = name[i];
    }
    ccb->cgdl.periph_name[i] = '\0';
    ccb->cgdl.unit_number = unit;
  }
  if (ioctl(fd, DISC_CAMGETPASSTHRU, ccb) < 0 || ccb->cgdl.status == CAM_GDEVLIST_ERROR) {
    return -1;
  }
  if (out_name) {
    memcpy(out_name, ccb->cgdl.periph_name, sizeof(ccb->cgdl.periph_name));
    out_name[sizeof(ccb->cgdl.periph_name) - 1] = '\0';
  }
  if (out_unit) {
    *out_unit = ccb->cgdl.unit_number;
  }
  if (path) {
    *path = ccb->cgdl.ccb_h.path_id;
  }
  if (target) {
    *target = ccb->cgdl.ccb_h.target_id;
  }
  if (lun) {
    *lun = ccb->cgdl.ccb_h.target_lun;
  }
  return 0;
}

static int open_cd0(uint32_t *path, uint32_t *target, uint32_t *lun) {
  int xpt;
  int fd;
  char pass_name[16];
  char dev[64];
  uint32_t pass_unit = 0;

  xpt = open("/dev/xpt0", O_RDWR);
  if (xpt < 0) {
    return -1;
  }
  if (get_passthru(xpt, "cd", 0, pass_name, &pass_unit, NULL, NULL, NULL) != 0) {
    close(xpt);
    return -1;
  }
  close(xpt);

  snprintf(dev, sizeof(dev), "/dev/%s%u", pass_name, pass_unit);
  fd = open(dev, O_RDWR);
  if (fd < 0) {
    return -1;
  }
  if (get_passthru(fd, NULL, 0, NULL, NULL, path, target, lun) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

int disc_pass_open(struct disc_pass *pass) {
  int fd;
  uint32_t path = 0;
  uint32_t target = 0;
  uint32_t lun = 0;

  if (!pass) {
    return -1;
  }
  fd = open_cd0(&path, &target, &lun);
  if (fd < 0) {
    pass->fd = -1;
    return -1;
  }
  pass->fd = fd;
  pass->path = path;
  pass->target = target;
  pass->lun = lun;
  return 0;
}

void disc_pass_close(struct disc_pass *pass) {
  if (!pass || pass->fd < 0) {
    return;
  }
  close(pass->fd);
  pass->fd = -1;
}

int disc_pass_scsi(struct disc_pass *pass, const uint8_t *cdb, uint8_t cdb_len, void *data, uint32_t data_len, uint32_t timeout_ms, uint32_t *sense_out) {
  if (!pass || pass->fd < 0 || !cdb || cdb_len == 0 || cdb_len > 16) {
    if (sense_out) {
      *sense_out = 0;
    }
    return -1;
  }
  return scsi_cmd(pass->fd, pass->path, pass->target, pass->lun, cdb, cdb_len, data, data_len, CAM_DIR_IN, timeout_ms, sense_out);
}

static void put_be32(uint8_t *p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}

static int set_cd_speed_max(struct disc_pass *pass) {
  uint8_t cdb[12] = {0xBB, 0, 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};

  return scsi_cmd(pass->fd, pass->path, pass->target, pass->lun, cdb, 12, NULL, 0, CAM_DIR_NONE, 10000, NULL);
}

static int set_streaming_max(struct disc_pass *pass, uint32_t end_lba) {
  uint8_t cdb[12] = {0xB6, 0, 0, 0, 0, 0, 0, 0, 0, 0, 28, 0};
  uint8_t pd[28];

  memset(pd, '\0', sizeof(pd));
  put_be32(pd + 8, end_lba);
  // Exact is clear: the drive matches this as best it can and does not reject it.
  // 0x10000000 KB/s is above any real drive, so the drive selects its maximum.
  put_be32(pd + 12, 0x10000000u);
  put_be32(pd + 16, 1000);
  put_be32(pd + 20, 0x10000000u);
  put_be32(pd + 24, 1000);
  return scsi_cmd(pass->fd, pass->path, pass->target, pass->lun, cdb, 12, pd, sizeof(pd), CAM_DIR_OUT, 10000, NULL);
}

void disc_request_read_speed(uint32_t end_lba) {
  struct disc_pass pass;

  if (disc_pass_open(&pass) != 0) {
    return;
  }
  if (end_lba == 0 || set_streaming_max(&pass, end_lba) != 0) {
    set_cd_speed_max(&pass);
  }
  disc_pass_close(&pass);
}

#if defined(__ORBIS__)
// Vendor and product are 24 bytes. The revision follows them and is not part of the id.
static int inquiry_is_ps_system(const uint8_t *inq) {
  static const char ids[][25] = {
      "SONY    PS-SYSTEM   399R",
      "SONY    PS-SYSTEM   400R",
      "SONY    PS-SYSTEM   401R",
  };
  unsigned i;

  for (i = 0; i < 3; i++) {
    if (memcmp(inq + 8, ids[i], 24) == 0) {
      return 1;
    }
  }
  return 0;
}
#endif

static int read_profile(int fd, uint32_t path, uint32_t target, uint32_t lun, uint16_t *profile, uint32_t *sense_out) {
  // GET CONFIGURATION, RT=0, starting feature 0, allocation length 0x0804.
  static const uint8_t cdb[12] = {0x46, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x04, 0x00, 0x00, 0x00};
  uint8_t data[0x804];

  memset(data, '\0', sizeof(data));
  if (scsi_cmd(fd, path, target, lun, cdb, 12, data, sizeof(data), CAM_DIR_IN, 10000, sense_out) != 0) {
    return -1;
  }
  *profile = (uint16_t)((data[6] << 8) | data[7]);
  return 0;
}

static int wants_disc_structure(uint16_t profile) {
  switch (profile) {
  case 0x10:
  case 0x12:
  case 0x13:
  case 0x14:
  case 0x15:
  case 0x16:
  case 0x1A:
  case 0x1B:
  case 0x2A:
  case 0x2B:
    return 1;
  default:
    return 0;
  }
}

// DVD book type in the high nibble of physical format info.
static uint16_t apply_book_type(uint16_t profile, unsigned book) {
  switch (book) {
  case 1:
    return 0x12;
  case 2:
    return 0x11;
  case 3:
    return 0x13;
  case 9:
    return 0x1A;
  case 10:
    return 0x1B;
  case 13:
    return 0x2A;
  case 14:
    return 0x2B;
  default:
    return profile;
  }
}

static int read_book_type(int fd, uint32_t path, uint32_t target, uint32_t lun, unsigned *book, uint32_t *sense_out) {
  // READ DISC STRUCTURE, format 0, allocation length 0x0804.
  static const uint8_t cdb[12] = {0xAD, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x04, 0x00, 0x00};
  uint8_t data[0x804];

  memset(data, '\0', sizeof(data));
  if (scsi_cmd(fd, path, target, lun, cdb, 12, data, sizeof(data), CAM_DIR_IN, 10000, sense_out) != 0) {
    return -1;
  }
  *book = data[4] >> 4;
  return 0;
}

static enum disc_type classify_sense(uint32_t sense) {
  if ((sense & 0xFFFF00u) == 0x023000u) {
    return DISC_INCOMPATIBLE;
  }
  if ((sense & 0xFFFF00u) == 0x023A00u) {
    return DISC_NOT_PRESENT;
  }
  return DISC_ERROR;
}

enum disc_type detect_disc(void) {
  uint32_t path = 0;
  uint32_t target = 0;
  uint32_t lun = 0;
  uint32_t sense = 0;
  uint16_t profile = 0;
  int fd;
  int left;
#if defined(__ORBIS__)
  int sony = 0;
  uint8_t inq[256];
  static const uint8_t inquiry_cdb[6] = {0x12, 0x00, 0x00, 0x00, 36, 0x00};
#endif

  fd = open_cd0(&path, &target, &lun);
  if (fd < 0) {
    return DISC_ERROR;
  }

#if defined(__ORBIS__)
  memset(inq, '\0', sizeof(inq));
  if (scsi_cmd(fd, path, target, lun, inquiry_cdb, 6, inq, 36, CAM_DIR_IN, 5000, NULL) == 0) {
    sony = inquiry_is_ps_system(inq);
  }
#endif

  if (read_profile(fd, path, target, lun, &profile, &sense) != 0) {
    for (left = 119;;) {
      enum disc_type sensed = classify_sense(sense);
      if (sensed == DISC_INCOMPATIBLE || sensed == DISC_NOT_PRESENT) {
        close(fd);
        return sensed;
      }
      if (--left == -2) {
        break;
      }
      sleep(1);
      if (read_profile(fd, path, target, lun, &profile, &sense) == 0) {
        goto ready;
      }
    }
    close(fd);
    return DISC_TIMEOUT;
  }

ready:
#if defined(__ORBIS__)
  // Internal BD drive reports a standard BD profile for a PS4 disc.
  if (sony && (profile & 0xFFFC) == 0x40) {
    profile = DISC_PS4_BD_ROM;
  }
#endif

  if (wants_disc_structure(profile)) {
    unsigned book = 0;
    if (read_book_type(fd, path, target, lun, &book, &sense) != 0) {
      enum disc_type sensed = classify_sense(sense);
      close(fd);
      return sensed;
    }
    profile = apply_book_type(profile, book);
  }

  close(fd);
  return (enum disc_type)profile;
}
