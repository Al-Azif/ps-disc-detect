#ifndef DISC_TYPE_H_
#define DISC_TYPE_H_

enum disc_type {
  DISC_NO_PROFILE = 0x0000,   // "No Current Profile"
  DISC_DVD_ROM = 0x0010,
  DISC_DVD_R = 0x0011,
  DISC_DVD_RAM = 0x0012,
  DISC_DVD_RW = 0x0013,
  DISC_DVD_PLUS_RW = 0x001A,
  DISC_DVD_PLUS_R = 0x001B,
  DISC_DVD_PLUS_RW_DL = 0x002A,
  DISC_DVD_PLUS_R_DL = 0x002B,
  DISC_BD_ROM = 0x0040,
  DISC_BD_R = 0x0041,         // also profile 0x42, both print "BD_R"
  DISC_BD_R_RANDOM = 0x0042,
  DISC_BD_RE = 0x0043,
  DISC_PS1_CD_ROM = 0xFF50,   // -176
  DISC_PS2_CD_ROM = 0xFF60,   // -160
  DISC_PS2_DVD_ROM = 0xFF61,  // -159
  DISC_PS3_DVD_ROM = 0xFF70,  // -144
  DISC_PS3_BD_ROM = 0xFF71,   // -143
  DISC_PS4_BD_ROM = 0xFF80,   // -128
  DISC_PPR_BD_ROM = 0xFF90,   // -112
  DISC_INCOMPATIBLE = 0xFFFF, // sense 02/30, ShellCore stores 0xFFFF
  DISC_NOT_PRESENT = -1,      // sense 02/3A; ShellCore stores 0 and an error
  DISC_TIMEOUT = -2,
  DISC_ERROR = -3
};

const char *disc_type_name(int type);

#endif
