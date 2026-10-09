#ifndef DISC_MOUNT_H_
#define DISC_MOUNT_H_

#include <stddef.h>
#include <sys/types.h>

const char *disc_mount_root(void);
void disc_path(char *out, size_t out_len, const char *rel);
int mount_has_files(void);
int scan_mount(int *has_files, int *has_psx_exe, char *cnf_path, size_t cnf_path_len);
int load_disc_file(const char *rel, off_t min_size, off_t max_size, int regular, unsigned char **out, size_t *out_n);

#endif
