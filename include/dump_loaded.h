#ifndef DUMP_LOADED_H_
#define DUMP_LOADED_H_

#include "disc_type.h"

// Dump a PS1, PS2, or PS3 disc once its serial is known. An image of this
// disc that is already on /data is left in place. PS4 and PS5 return immediately.
void dump_loaded(enum disc_type type, const char *serial);

#endif
