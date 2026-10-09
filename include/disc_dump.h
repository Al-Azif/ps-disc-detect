#ifndef DISC_DUMP_H_
#define DISC_DUMP_H_

#include <stdint.h>

#include "disc_hash.h"
#include "disc_type.h"

// done and total are image bytes. total is 0 when the drive did not report a size.
// progress may be NULL.
typedef void (*disc_dump_progress_fn)(uint64_t done, uint64_t total, void *user);

// Bytes dump_disc would write for the disc in the drive. A CD bin counts
// 2352-byte sectors from the first track through the lead-out. DVD and BD
// use the drive's media size. Returns 0 and stores the length in *out, or
// -1 when the size cannot be read.
int disc_image_bytes(enum disc_type type, uint64_t *out);

struct disc_ird;

// DVD and BD are written to path. A CD is written as a .bin and a .cue from the same stem.
// progress is called as image bytes are written. hash, when set, receives the CRC-32,
// MD5, and SHA-1 of the ISO or the CD bin. Returns 0 when the image is complete, or -1.
// This does not write an IRD. PS3 images that need one go through dump_ps3.
int dump_disc(enum disc_type type, const char *path, struct disc_hash *hash, disc_dump_progress_fn progress, void *user);

// Write a PS3 ISO. On success, *ird_out is an IRD builder when the disc has a region
// table, otherwise NULL. The caller writes and frees it. On failure *ird_out is NULL.
// ird_out may be NULL to write the ISO only. hash, when set, receives the ISO checksums.
// Returns 0 when the ISO is complete, or -1.
int dump_ps3(const char *path, struct disc_ird **ird_out, struct disc_hash *hash, disc_dump_progress_fn progress, void *user);

#endif
