# disc detect

A PlayStation 4 and PlayStation 5 payload that watches the optical drive. It reports the disc the same way ShellCore does, logs the profile and title ID, and dumps PlayStation, PlayStation 2, and PlayStation 3 discs to `/data`.

Status lines go to the Klog on both consoles. The payload polls every 1.5 seconds and prints only when the drive state changes.

## Build

The PS4 and PS5 payload SDKs must be installed, and the machine needs Python 3 plus a network path to [redump.org](http://redump.org) so the serial DAT files can be downloaded.

```sh
./build.sh
./build.sh Debug
```

`Release` is the default. The script looks for the SDKs at `/opt/ps4-payload-sdk` and `/opt/ps5-payload-sdk`. Set `PS4_PAYLOAD_SDK` and `PS5_PAYLOAD_SDK` if they live somewhere else.

Finished payloads:

| Platform      | ELF                      |
|---------------|--------------------------|
| PlayStation 4 | `build/bin/disc-ps4.elf` |
| PlayStation 5 | `build/bin/disc-ps5.elf` |

Caches, `redump.db`, and the embedded database object stay in `build/ps4` and `build/ps5`. Configure each platform on its own. `orbis-cmake` and `prospero-cmake` cannot be mixed in one tree.

```sh
export PS4_PAYLOAD_SDK=/opt/ps4-payload-sdk
export PS5_PAYLOAD_SDK=/opt/ps5-payload-sdk

"$PS4_PAYLOAD_SDK/bin/orbis-cmake" -S . -B build/ps4 -DCMAKE_BUILD_TYPE=Release
cmake --build build/ps4

"$PS5_PAYLOAD_SDK/bin/prospero-cmake" -S . -B build/ps5 -DCMAKE_BUILD_TYPE=Release
cmake --build build/ps5
```

The first build downloads the PlayStation, PlayStation 2, and PlayStation 3 serial DATs, builds a SQLite database, and links it into the ELF. The download runs again only when `tools/build_redump_db.py` changes or the database object is missing. To build that database from files you already have:

```sh
python3 tools/build_redump_db.py --out build/ps4/redump.db --asm build/ps4/redump_db.S path/to/psx.zip path/to/ps2.zip path/to/ps3.zip
```

Those files must be the serial DATs (each game has a `<serial>` child), or zips of them. The plain Redump DAT has no serials and is rejected.

## What it logs

On start:

```text
Optical Drive Monitor Started... Ready for disc insertion.
```

An empty tray is profile 0. A medium-not-present sense is reported as an eject:

```text
[Drive Status Change] No Current Profile (0)
[Drive Status Change] Disc was ejected.
```

A recognized disc logs its profile. Once `/mnt/disc` is readable, the title line follows. If that serial has exactly one Redump name, the name is included:

```text
[Drive Status Change] PS2 CD-ROM (0xff60) title: SCES-02066 (Ace Combat 3 - Electrosphere (Europe) (En,Fr,De,Es,It))
[Drive Status Change] PS2 DVD-ROM (0xff61) title: SLUS-20946
[Drive Status Change] PS4 BD-ROM (0xff80) title: CUSA-00001
```

A serial that maps to more than one Redump game stays as the serial until a dump checksum picks a name. Timeouts and ioctl errors keep the last disc and are not logged as ejects. The same disc is handled again only after the tray is empty and the disc is inserted again.

## Title IDs

Title IDs are uppercase and hyphenated (`CUSA00001` is logged as `CUSA-00001`). `GENERIC_PS1` and the other fallback names stay as they are.

| Disc                    | Title source                                                                                                                                                                                                                           |
|-------------------------|----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------|
| PS1 CD-ROM              | `SYSTEM.CNF` `BOOT=`. `PSX.EXE`, or a missing boot path, falls back to the PVD volume ID, then `GENERIC_PS1`.                                                                                                                          |
| PS2 CD-ROM, PS2 DVD-ROM | `SYSTEM.CNF` `BOOT2=`. A missing file becomes `GENERIC_PS2`.                                                                                                                                                                           |
| PS3 DVD-ROM, PS3 BD-ROM | `TITLE_ID` in `PS3_GAME/PARAM.SFO`. A failed read becomes `GENERIC_PS3`.                                                                                                                                                               |
| PS4 BD-ROM              | `TITLE_ID` in `bd/param.sfo`, after the 0x800-byte header. Otherwise the first 9-character directory under `app/`. A failed read becomes `GENERIC_PS4`.                                                                                |
| PPR BD-ROM              | The first `disc[].contents[]` entry in `bd/param.json` whose `contentType` starts with `PS5GD`. The id is `contentId` bytes 7 through 15. Otherwise the first 9-character directory under `app/`. A failed read becomes `GENERIC_PS5`. |

Profiles with no title ID are only logged by name. On a PlayStation 4, a Sony PS-System Blu-ray (revisions 399R, 400R, and 401R) is reported as PS4 BD-ROM. A PlayStation 5 keeps the drive's GET CONFIGURATION profile, so a PlayStation 5 game stays PPR BD-ROM.

## Dumps

PlayStation, PlayStation 2, and PlayStation 3 discs are written to `/data` after a serial is known. PlayStation 4 and PlayStation 5 discs are identified and not dumped. The dump blocks the poll loop until it finishes.

| Disc                                 | Files                                                         |
|--------------------------------------|---------------------------------------------------------------|
| PS1 CD-ROM, PS2 CD-ROM               | `/data/<name>.bin` and `/data/<name>.cue` (2352-byte sectors) |
| PS2 DVD-ROM, PS3 DVD-ROM, PS3 BD-ROM | `/data/<name>.iso`                                            |

Newer PS4s (Slims and Pros) and the PS5 lack the laser required to read CDs, so a PS1 disc or a PS2 CD is not seen on those consoles via their built in disc drive. DVD and Blu-ray still work.

The drive is asked for its maximum read speed before the copy. CD dumps use `SET CD SPEED`. DVD and Blu-ray dumps use `SET STREAMING`, and fall back to `SET CD SPEED` if the drive rejects it.

The file stem is chosen in this order:

1. The Redump name, when the serial has exactly one.
2. The serial (`SLUS-20946`, `SCES-02066`).
3. `GENERIC_PS1`, `GENERIC_PS2`, or `GENERIC_PS3` when the title ID could not be read.

A serial with several Redump names, or none, is written under the serial and renamed after the checksums match. A unique serial is written under the Redump name immediately. If verification fails, that file is renamed back to the serial. A checksum match can also replace a `GENERIC_*` name.

While a dump runs, progress is the percent when the size is known, or every 64 MiB when it is not. Each line shows elapsed time and speed since the previous line. The closing line is the image size, total time, and average speed:

```text
[Dump] /data/SLUS-20946.bin /data/SCUS-94491.cue
...
[Dump] /data/SCUS-94491.bin 45% 1:23 8.4 MiB/s
...
[Dump] /data/SCUS-94491.bin done 484.9 MiB in 2:11, average 3.7 MiB/s
[Redump] /data/Legend of Dragoon, The (USA, Canada) (Disc 1).bin verified
```

A miss keeps the serial (or `GENERIC_*`) name:

```text
[Redump] /data/FAKE-00000.bin not in Redump dat
```

A failed dump removes the partial file it created. A CD cue names the bin, and a rename rewrites that `FILE` line before the bin and cue are renamed.

### Already on `/data`

A finished image is not dumped again. The check looks at the stem that would be written, the serial name when that differs, and every Redump name for the serial. Size comes from the CD table of contents (2352-byte sectors from the first track through the lead-out) or from the drive's reported media size for DVD and Blu-ray.

A CD counts as dumped when the `.bin` is that size and the `.cue` is a non-empty file. DVD and Blu-ray check the `.iso` size. A shorter file is partial and is dumped again. When the drive does not report a size, a non-empty file at the stem or serial path is left alone. For `GENERIC_*`, the dump is skipped when that generic file matches the disc size, or when exactly one Redump game has that size and its file is already present. If several games share the size, the disc is still dumped.

```text
[Dump] /data/Legend of Dragoon, The (USA, Canada) (Disc 1).bin already dumped
```

### IRD

PlayStation 3 dumps stop at the ISO. IRD output is present in the tree and turned off: the drive does not expose the disc key, so `Data1` and `Data2` would be zeros. `src/dump_loaded.c` calls `dump_ps3` with a null IRD builder. The commented block beside that call is the path that would write `/data/<name>.ird`.

## Redump database

Names and checksums come from an embedded SQLite database built from Redump's PlayStation, PlayStation 2, and PlayStation 3 serial DATs. The first lookup of a run prefers `/data/redump.db` when that path is a regular SQLite file of at most 64 MiB and its `serials`, `images`, and `tracks` statements prepare. That file then replaces the embedded database for the rest of the run.

```text
[Redump] /data/redump.db
```

A missing file is silent and the embedded database is used. A file that is present but cannot be read or prepared is logged, and the embedded database is used:

```text
[Redump] /data/redump.db unusable
```

`redump_name` returns a name only when the serial has exactly one. Verification compares CRC-32, MD5, and SHA-1 of the whole image. A CD bin that does not match a whole-file row is sliced against track checksums whose sizes sum to the bin. The cue the payload writes is not compared to Redump. `GENERIC_*` is not looked up as a serial; that bin is matched by image size and checksums only.

Put a newer database on the console as `/data/redump.db`. Build it with `tools/build_redump_db.py` and copy the `.db` it writes. Do not copy `redump_db.S`.

## Layout

```text
external/                 sqlite, inih, nlohmann/json, miniz
include/                  public headers
src/                      payload sources
  main.c                  poll loop
  detect_disc.c           drive profile
  disc_title.c            per-generation title ID
  dump_loaded.c           dump, skip, and Redump rename
  disc_dump.c             ISO, bin, and cue writer
  redump.c                database lookup
tools/build_redump_db.py
build.sh
```

`src/sqlite_vfs.c` does not open files. `/data/redump.db` is read with `open` and `read`, then deserialized. The console libc has no `pread`, `mmap`, or `fdatasync` for SQLite's own VFS.

## Third-party code

| Tree              | Use                                                    | License       |
|-------------------|--------------------------------------------------------|---------------|
| `external/sqlite` | Embedded Redump database                               | Public domain |
| `external/inih`   | `SYSTEM.CNF`                                           | BSD           |
| `external/json`   | PlayStation 5 `param.json`                             | MIT           |
| `external/miniz`  | IRD gzip (linked, not written while IRD output is off) | MIT           |

Each vendor directory has its own `LICENSE`.
