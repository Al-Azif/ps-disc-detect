#ifndef DETECT_DISC_H_
#define DETECT_DISC_H_

#include <stdint.h>

#include "disc_type.h"

struct disc_pass {
  int fd;
  uint32_t path;
  uint32_t target;
  uint32_t lun;
};

enum disc_type detect_disc(void);

int disc_pass_open(struct disc_pass *pass);
void disc_pass_close(struct disc_pass *pass);
int disc_pass_scsi(struct disc_pass *pass, const uint8_t *cdb, uint8_t cdb_len, void *data, uint32_t data_len, uint32_t timeout_ms, uint32_t *sense_out);
void disc_request_read_speed(uint32_t end_lba);

#endif
