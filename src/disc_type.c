#include "disc_type.h"

const char *disc_type_name(int type) {
  switch (type) {
  case DISC_NO_PROFILE:
    return "No Current Profile";
  case DISC_DVD_ROM:
    return "DVD-ROM";
  case DISC_DVD_R:
    return "DVD-R";
  case DISC_DVD_RAM:
    return "DVD-RAM";
  case DISC_DVD_RW:
    return "DVD-RW";
  case DISC_DVD_PLUS_RW:
    return "DVD+RW";
  case DISC_DVD_PLUS_R:
    return "DVD+R";
  case DISC_DVD_PLUS_RW_DL:
    return "DVD+RW DL";
  case DISC_DVD_PLUS_R_DL:
    return "DVD+R DL";
  case DISC_BD_ROM:
    return "BD-ROM";
  case DISC_BD_R:
  case DISC_BD_R_RANDOM:
    return "BD_R";
  case DISC_BD_RE:
    return "BD_RE";
  case DISC_PS1_CD_ROM:
    return "PS1 CD-ROM";
  case DISC_PS2_CD_ROM:
    return "PS2 CD-ROM";
  case DISC_PS2_DVD_ROM:
    return "PS2 DVD-ROM";
  case DISC_PS3_DVD_ROM:
    return "PS3 DVD-ROM";
  case DISC_PS3_BD_ROM:
    return "PS3 BD-ROM";
  case DISC_PS4_BD_ROM:
    return "PS4 BD-ROM";
  case DISC_PPR_BD_ROM:
    return "PPR BD-ROM";
  case DISC_INCOMPATIBLE:
    return "INCOMPATIBLE MEDIUM";
  case DISC_NOT_PRESENT:
    return "MEDIUM NOT PRESENT";
  case DISC_TIMEOUT:
    return "TIMEOUT";
  case DISC_ERROR:
    return "ERROR";
  default:
    return "Not Conforming to a Standard Profile";
  }
}
