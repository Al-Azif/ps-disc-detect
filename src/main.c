#include <string.h>
#include <unistd.h>

#include "detect_disc.h"
#include "disc_mount.h"
#include "disc_serial.h"
#include "disc_title.h"
#include "dump_loaded.h"
#include "redump.h"

#if defined(__ORBIS__) && defined(__PROSPERO__)
#error "Define only one of __ORBIS__ or __PROSPERO__"
#elif defined(__ORBIS__)
#include <ps4/klog.h>
#elif defined(__PROSPERO__)
#include <ps5/klog.h>
#else
#error "Build for PS4 (__ORBIS__) or PS5 (__PROSPERO__)"
#endif

#define POLL_US 1500000

static void log_disc(enum disc_type type, const char *serial) {
  char name[256];

  redump_name(serial, name, sizeof(name));
  if (name[0] != '\0') {
    klog_printf("[Drive Status Change] %s (%#x) title: %s (%s)\n", disc_type_name(type), (unsigned)type, serial, name);
  } else {
    klog_printf("[Drive Status Change] %s (%#x) title: %s\n", disc_type_name(type), (unsigned)type, serial);
  }
}

int main(void) {
  enum disc_type last = DISC_NOT_PRESENT;
  int waiting_title = 0;
  char shown[32];

  shown[0] = '\0';
  klog_puts("Optical Drive Monitor Started... Ready for disc insertion.");

  while (1) {
    enum disc_type type = detect_disc();

    // Spin-up and ioctl failures are "not ready": keep the last disc and try again.
    if (type == DISC_TIMEOUT || type == DISC_ERROR) {
      usleep(POLL_US);
      continue;
    }

    if (type != last) {
      waiting_title = 0;
      if (type == DISC_NOT_PRESENT || type == DISC_NO_PROFILE) {
        // An empty tray is profile 0 on this drive. Sense 02/3A is the other
        // empty report. Forget the serial either way so the same disc is
        // checked again after it is put back in.
        if (type == DISC_NOT_PRESENT) {
          klog_puts("[Drive Status Change] Disc was ejected.");
        } else {
          klog_printf("[Drive Status Change] %s (%#x)\n", disc_type_name(type), (unsigned)type);
        }
        shown[0] = '\0';
      } else {
        char serial[32];
        int got = 0;

        if (disc_has_title(type)) {
          got = disc_title_id(type, serial, sizeof(serial));
        }
        if (got == 1) {
          log_disc(type, serial);
          set_serial(shown, sizeof(shown), serial);
          dump_loaded(type, serial);
        } else {
          klog_printf("[Drive Status Change] %s (%#x)\n", disc_type_name(type), (unsigned)type);
          if (got < 0) {
            waiting_title = 1;
          }
        }
      }
      last = type;
    } else if (waiting_title) {
      char serial[32];
      int got = disc_title_id(type, serial, sizeof(serial));

      if (got == 1) {
        waiting_title = 0;
        if (strcmp(serial, shown) != 0) {
          log_disc(type, serial);
          set_serial(shown, sizeof(shown), serial);
          dump_loaded(type, serial);
        }
      } else if (got == 0) {
        waiting_title = 0;
      }
    } else if (disc_has_title(type) && !mount_has_files()) {
      // Same profile, new disc: the mount drops before the title ID changes.
      waiting_title = 1;
    }

    usleep(POLL_US);
  }

  return 0;
}
