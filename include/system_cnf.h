#ifndef SYSTEM_CNF_H_
#define SYSTEM_CNF_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

int cnf_boot_serial(const char *path, const char *key, char *dest, size_t dest_len, const char *generic);

#ifdef __cplusplus
}
#endif

#endif
