#ifndef DISC_IRD_H_
#define DISC_IRD_H_

#include <stdint.h>

#include "disc_dump.h"

struct disc_ird;

// Parse a PS3 region disc. media_bytes is the ISO size. mount is the decrypted
// filesystem used for file bytes, PARAM.SFO, and PS3UPDAT.PUP.
// Returns NULL when the image has no PS3 region table, or when allocation fails.
// A non-NULL result can still fail later in disc_ird_write. The ISO stays either way.
struct disc_ird *disc_ird_prepare(int fd, uint64_t media_bytes, const char *mount);

// Next contiguous ISO bytes. offset is the position of data in the image.
void disc_ird_feed(struct disc_ird *ird, uint64_t offset, const void *data, size_t n);

// Replace the generated PIC. pic is 115 bytes.
void disc_ird_set_pic(struct disc_ird *ird, const uint8_t pic[115]);

// Hash the mounted files and write a gzip IRD v9. Returns 0 on success.
// A failure removes path. progress may be NULL; done/total are plaintext file bytes.
int disc_ird_write(struct disc_ird *ird, const char *path, disc_dump_progress_fn progress, void *user);

void disc_ird_free(struct disc_ird *ird);

#endif
