#include <ctype.h>
#include <string.h>

#include "disc_serial.h"

void set_serial(char *dest, size_t dest_len, const char *src) {
  size_t n = 0;

  if (!dest || dest_len == 0) {
    return;
  }
  if (src) {
    while (src[n] && n + 1 < dest_len) {
      dest[n] = src[n];
      n++;
    }
  }
  dest[n] = '\0';
}

void sanitize_serial(const char *src, char *dest, size_t dest_len) {
  const char *start;
  const char *last_sep = NULL;
  const char *colon = NULL;
  const char *p;
  size_t i = 0;

  if (!dest || dest_len == 0) {
    return;
  }
  dest[0] = '\0';
  if (!src) {
    return;
  }

  start = src;
  while (*start == ' ' || *start == '\t') {
    start++;
  }
  for (p = start; *p && *p != ';' && *p != '\r' && *p != '\n'; p++) {
    if (*p == '\\' || *p == '/') {
      last_sep = p;
    } else if (*p == ':' && !colon) {
      colon = p;
    }
  }
  if (last_sep) {
    start = last_sep + 1;
  } else if (colon) {
    start = colon + 1;
  }
  while (*start == ' ' || *start == '\t' || *start == '\\' || *start == '/') {
    start++;
  }

  while (*start && i < dest_len - 1) {
    if (*start == ';' || *start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
      break;
    }
    if (isalnum((unsigned char)*start)) {
      dest[i++] = (char)toupper((unsigned char)*start);
    }
    start++;
  }
  dest[i] = '\0';

  if (i > 4 && isalpha((unsigned char)dest[0])) {
    int num_letters = 0;
    while (num_letters < (int)i && isalpha((unsigned char)dest[num_letters])) {
      num_letters++;
    }
    if (num_letters > 0 && num_letters < (int)i && i + 1 < dest_len) {
      memmove(dest + num_letters + 1, dest + num_letters, i - (size_t)num_letters + 1);
      dest[num_letters] = '-';
    }
  }
}

void hyphenate_serial(char *dest, size_t dest_len) {
  char raw[32];

  if (!dest || dest[0] == '\0') {
    return;
  }
  set_serial(raw, sizeof(raw), dest);
  sanitize_serial(raw, dest, dest_len);
}
