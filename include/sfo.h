#ifndef SFO_H_
#define SFO_H_

#include <stddef.h>
#include <stdint.h>

#define SFO_ROOT_HDR 0x800
#define SFO_MAX_SIZE 0x8000

int sfo_layout_ok(const unsigned char *buf, size_t n);
int sfo_copy_title(const unsigned char *buf, size_t n, uint32_t max_len, size_t copy_n, char *dest, size_t dest_len);

#endif
