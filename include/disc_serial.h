#ifndef DISC_SERIAL_H_
#define DISC_SERIAL_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void set_serial(char *dest, size_t dest_len, const char *src);
void sanitize_serial(const char *src, char *dest, size_t dest_len);
void hyphenate_serial(char *dest, size_t dest_len);

#ifdef __cplusplus
}
#endif

#endif
