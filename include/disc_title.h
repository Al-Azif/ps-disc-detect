#ifndef DISC_TITLE_H_
#define DISC_TITLE_H_

#include <stddef.h>

#include "disc_type.h"

int disc_has_title(enum disc_type type);
int disc_title_id(enum disc_type type, char *dest, size_t dest_len);

#endif
