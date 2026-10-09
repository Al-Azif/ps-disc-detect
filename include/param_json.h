#ifndef PARAM_JSON_H_
#define PARAM_JSON_H_

#include <stddef.h>

#define JSON_FILE_MIN 0x800
#define JSON_FILE_MAX 0x100000
#define JSON_BODY_MAX 0x20000

#ifdef __cplusplus
extern "C" {
#endif

int json_ps5_title(const unsigned char *buf, size_t n, char *dest, size_t dest_len);

#ifdef __cplusplus
}
#endif

#endif
