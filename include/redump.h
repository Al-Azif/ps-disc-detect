#ifndef REDUMP_H_
#define REDUMP_H_

#include <stddef.h>

#include "disc_hash.h"

// Write the Redump name when serial has exactly one. dest is empty otherwise.
void redump_name(const char *serial, char *dest, size_t dest_len);

// Call visit for each Redump name stored for serial. GENERIC_* has none.
// A non-zero visit result stops the walk and is returned.
int redump_each_name(const char *serial, int (*visit)(const char *name, void *user), void *user);

// When exactly one Redump game has this image size, copy its name.
// iso is 1 for an ISO and 0 for a CD bin. dest is empty when none or several match.
void redump_name_for_size(uint64_t size, int iso, char *dest, size_t dest_len);

// Match whole against a Redump image. A CD bin with no whole-file match is sliced
// against the track checksums stored for serial. GENERIC_* has no Redump serial,
// so that bin is matched against track sets whose sizes sum to the file. On a
// match, name receives the Redump name. image_path is the .bin for a CD and the
// .iso otherwise. iso is 1 for an ISO. Returns 1 when the checksums match.
int redump_verify(const char *serial, const char *image_path, const struct disc_hash *whole, int iso, char *name, size_t name_len);

#endif
