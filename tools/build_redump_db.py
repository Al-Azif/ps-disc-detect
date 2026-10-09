#!/usr/bin/env python3
"""Build the embedded disc database from Redump's serial DAT files.

With no DAT paths, the script downloads these zips and deletes them afterward:

    http://redump.org/datfile/psx/serial
    http://redump.org/datfile/ps2/serial
    http://redump.org/datfile/ps3/serial

Each game in those files has a <serial> child. The plain Redump DAT has
checksums and names, but no serial, and this script rejects it.

Redump's game name is the dump stem and the name shown for the disc. Whole
images (.iso, and a .bin that is not a track split) are stored with their
size, CRC-32, MD5, and SHA-1. Games that only have per-track bins store those
track checksums so a merged bin can be checked in slices. Cue sheets are not
stored: the payload writes its own cue, and that file will not match Redump's.
"""

from __future__ import annotations

import argparse
import re
import sqlite3
import tempfile
import urllib.error
import urllib.request
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

SCHEMA = """
CREATE TABLE serials (
  serial TEXT NOT NULL,
  name TEXT NOT NULL,
  PRIMARY KEY (serial, name)
) WITHOUT ROWID;
CREATE TABLE images (
  sha1 BLOB PRIMARY KEY,
  size INTEGER NOT NULL,
  crc INTEGER NOT NULL,
  md5 BLOB NOT NULL,
  name TEXT NOT NULL,
  kind TEXT NOT NULL
) WITHOUT ROWID;
CREATE TABLE tracks (
  name TEXT NOT NULL,
  track INTEGER NOT NULL,
  size INTEGER NOT NULL,
  crc INTEGER NOT NULL,
  md5 BLOB NOT NULL,
  sha1 BLOB NOT NULL,
  PRIMARY KEY (name, track)
) WITHOUT ROWID;
"""

# Redump serves these over HTTP. Port 443 refuses the connection.
SERIAL_DAT_URLS = (
    "http://redump.org/datfile/psx/serial",
    "http://redump.org/datfile/ps2/serial",
    "http://redump.org/datfile/ps3/serial",
)
# Redump answers 403 when the request has no User-Agent.
REDUMP_USER_AGENT = "Mozilla/5.0"

TRACK_RE = re.compile(r"\(Track\s+(\d+)\)", re.IGNORECASE)
RANGE_RE = re.compile(r"([A-Za-z]{3,5})\s*-?\s*(\d{3,6})\s*-\s*(\d{1,6})")


def sanitize(token: str) -> str | None:
    """Uppercase alnum, with a hyphen after the leading letters. Same shape as sanitize_serial."""
    alnum = "".join(ch for ch in token.upper() if ch.isalnum())
    if len(alnum) <= 4 or not alnum[0].isalpha():
        return None
    letters = 0
    while letters < len(alnum) and alnum[letters].isalpha():
        letters += 1
    if letters == 0 or letters >= len(alnum):
        return None
    return alnum[:letters] + "-" + alnum[letters:]


def expand_range(part: str) -> list[str]:
    match = RANGE_RE.fullmatch(part.strip())
    if not match:
        return []
    prefix = match.group(1).upper()
    start_text = match.group(2)
    end_text = match.group(3)
    width = len(end_text)
    if width > len(start_text):
        return []
    base = start_text[:-width]
    start = int(start_text[-width:])
    end = int(end_text)
    if end < start or end - start > 20:
        return []
    return [f"{prefix}-{base}{n:0{width}d}" for n in range(start, end + 1)]


def serial_keys(text: str) -> list[str]:
    keys: list[str] = []
    for part in re.split(r"[,;&/]+", text):
        part = part.strip()
        if not part:
            continue
        ranged = expand_range(part)
        if ranged:
            keys.extend(ranged)
            continue
        full = sanitize(part)
        if not full:
            continue
        keys.append(full)
        # Label notes such as SLUS-01041GH and SCES-50001-P. The console reads the digits.
        match = re.fullmatch(r"([A-Z]+)-(\d+)[A-Z].*", full)
        if match:
            keys.append(f"{match.group(1)}-{match.group(2)}")
    return keys


def name_ok(name: str) -> bool:
    if not name or name[0] == ".":
        return False
    for ch in name:
        if ord(ch) < 32 or ch in '/\\"':
            return False
    # "/data/" + name + ".cue" and the terminating NUL have to fit a 512-byte path.
    return len(name) + 11 <= 512


def rom_role(rom_name: str) -> tuple[str, int | None]:
    lower = rom_name.lower()
    if lower.endswith(".cue"):
        return "cue", None
    if lower.endswith(".iso"):
        return "iso", None
    if lower.endswith(".bin"):
        match = TRACK_RE.search(rom_name)
        if match:
            return "track", int(match.group(1))
        return "bin", None
    return "other", None


def parse_hash(value: str | None, nbytes: int, label: str) -> bytes:
    text = (value or "").strip()
    if len(text) != nbytes * 2 or any(ch not in "0123456789abcdefABCDEF" for ch in text):
        raise ValueError(f"{label} has a bad hash {text[:40]!r}")
    return bytes.fromhex(text)


def parse_rom(rom: ET.Element, label: str) -> dict[str, object]:
    rom_name = rom.get("name") or ""
    try:
        size = int(rom.get("size") or "")
        crc = int(rom.get("crc") or "", 16)
    except ValueError as exc:
        raise ValueError(f"{label} rom {rom_name!r} has a bad size or crc") from exc
    if size < 0 or crc < 0 or crc > 0xFFFFFFFF:
        raise ValueError(f"{label} rom {rom_name!r} has a bad size or crc")
    return {
        "name": rom_name,
        "size": size,
        "crc": crc,
        "md5": parse_hash(rom.get("md5"), 16, f"{label} rom {rom_name!r}"),
        "sha1": parse_hash(rom.get("sha1"), 20, f"{label} rom {rom_name!r}"),
    }


def add_image(db: sqlite3.Connection, rom: dict[str, object], name: str, kind: str, stats: dict[str, int]) -> None:
    row = db.execute("SELECT name FROM images WHERE sha1 = ?", (rom["sha1"],)).fetchone()
    if row is not None:
        if row[0] != name:
            stats["dup_sha1"] += 1
        return
    db.execute(
        "INSERT INTO images (sha1, size, crc, md5, name, kind) VALUES (?, ?, ?, ?, ?, ?)",
        (rom["sha1"], rom["size"], rom["crc"], rom["md5"], name, kind),
    )


def consume_game(db: sqlite3.Connection, elem: ET.Element, label: str, stats: dict[str, int]) -> bool:
    name = (elem.get("name") or "").strip()
    node = elem.find("serial")
    serial = (node.text or "").strip() if node is not None and node.text else ""
    stats["games"] += 1
    if not name or not name_ok(name):
        stats["bad_names"] += 1
        return bool(serial)

    wholes: list[tuple[dict[str, object], str]] = []
    tracks: list[tuple[dict[str, object], int]] = []
    for rom in elem.findall("rom"):
        parsed = parse_rom(rom, label)
        role, track_no = rom_role(str(parsed["name"]))
        if role in ("iso", "bin"):
            wholes.append((parsed, role))
        elif role == "track" and track_no is not None:
            tracks.append((parsed, track_no))
        elif role == "other":
            stats["other_roms"] += 1

    if wholes:
        for parsed, role in wholes:
            add_image(db, parsed, name, role, stats)
    elif tracks:
        for parsed, track_no in tracks:
            before = db.total_changes
            db.execute(
                "INSERT OR IGNORE INTO tracks (name, track, size, crc, md5, sha1) VALUES (?, ?, ?, ?, ?, ?)",
                (name, track_no, parsed["size"], parsed["crc"], parsed["md5"], parsed["sha1"]),
            )
            if db.total_changes == before:
                stats["dup_tracks"] += 1

    if serial:
        for key in set(serial_keys(serial)):
            db.execute("INSERT OR IGNORE INTO serials (serial, name) VALUES (?, ?)", (key, name))
    return bool(serial)


def download_serial_dats(dest: Path) -> list[Path]:
    dest.mkdir(parents=True, exist_ok=True)
    paths: list[Path] = []
    for url in SERIAL_DAT_URLS:
        system = url.rstrip("/").split("/")[-2]
        path = dest / f"{system}-serial.zip"
        print(f"Downloading {url}")
        request = urllib.request.Request(url, headers={"User-Agent": REDUMP_USER_AGENT})
        try:
            with urllib.request.urlopen(request, timeout=180) as response:
                data = response.read()
        except urllib.error.URLError as exc:
            raise RuntimeError(f"could not download {url}: {exc}") from exc
        if not data.startswith(b"PK"):
            raise RuntimeError(f"{url} did not return a zip")
        path.write_bytes(data)
        print(f"Downloaded {path.name} ({len(data)} bytes)")
        paths.append(path)
    return paths


def iter_dat_handles(path: Path):
    if path.suffix.lower() == ".zip":
        with zipfile.ZipFile(path) as bundle:
            dats = [name for name in bundle.namelist() if name.lower().endswith(".dat")]
            if len(dats) != 1:
                raise RuntimeError(f"{path} should contain one DAT, found {dats}")
            with bundle.open(dats[0]) as handle:
                yield handle
        return
    with path.open("rb") as handle:
        yield handle


def load_dat(db: sqlite3.Connection, path: Path, stats: dict[str, int]) -> None:
    saw_serial = False
    games_before = stats["games"]
    for handle in iter_dat_handles(path):
        for _event, elem in ET.iterparse(handle, events=("end",)):
            if elem.tag != "game":
                continue
            if consume_game(db, elem, str(path), stats):
                saw_serial = True
            elem.clear()
    if stats["games"] == games_before:
        raise RuntimeError(f"{path} has no games")
    if not saw_serial:
        raise RuntimeError(
            f"{path} has no <serial> elements. Download the serial DAT, for example "
            "http://redump.org/datfile/psx/serial"
        )


def build_db(dat_paths: list[Path], out_path: Path) -> None:
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.unlink(missing_ok=True)
    for suffix in ("-journal", "-wal", "-shm"):
        Path(str(out_path) + suffix).unlink(missing_ok=True)

    stats: dict[str, int] = {
        "games": 0,
        "bad_names": 0,
        "dup_sha1": 0,
        "dup_tracks": 0,
        "other_roms": 0,
    }
    db = sqlite3.connect(out_path)
    ok = False
    try:
        db.execute("PRAGMA page_size = 4096")
        db.executescript(SCHEMA)
        db.execute("BEGIN")
        for path in dat_paths:
            if not path.is_file():
                raise FileNotFoundError(path)
            load_dat(db, path, stats)
        db.commit()
        db.execute("VACUUM")
        integrity = db.execute("PRAGMA integrity_check").fetchone()[0]
        if integrity != "ok":
            raise RuntimeError(f"integrity_check failed: {integrity}")
        page = db.execute("PRAGMA page_size").fetchone()[0]
        if page != 4096:
            raise RuntimeError(f"page_size is {page}")
        serial_rows = db.execute("SELECT COUNT(*) FROM serials").fetchone()[0]
        unique = db.execute("SELECT COUNT(DISTINCT serial) FROM serials").fetchone()[0]
        ambiguous = db.execute(
            "SELECT COUNT(*) FROM (SELECT serial FROM serials GROUP BY serial HAVING COUNT(*) > 1)"
        ).fetchone()[0]
        images = db.execute("SELECT COUNT(*) FROM images").fetchone()[0]
        tracks = db.execute("SELECT COUNT(*) FROM tracks").fetchone()[0]
        longest = db.execute(
            "SELECT MAX(length(name)) FROM (SELECT name FROM serials UNION SELECT name FROM images UNION SELECT name FROM tracks)"
        ).fetchone()[0]
        ok = True
    finally:
        db.close()
        if not ok:
            out_path.unlink(missing_ok=True)

    print(
        f"Created {out_path} ({out_path.stat().st_size} bytes) "
        f"games={stats['games']} serials={serial_rows} unique={unique} ambiguous={ambiguous} "
        f"images={images} tracks={tracks} dup_sha1={stats['dup_sha1']} dup_tracks={stats['dup_tracks']} "
        f"bad_names={stats['bad_names']} other_roms={stats['other_roms']} longest_name={longest}"
    )


def write_asm(db_path: Path, asm_path: Path) -> None:
    resolved = db_path.resolve()
    if not resolved.is_file():
        raise FileNotFoundError(resolved)
    inc_path = resolved.as_posix().replace("\\", "\\\\").replace('"', '\\"')
    asm_path.parent.mkdir(parents=True, exist_ok=True)
    asm_path.write_text(
        "\n".join(
            [
                ".section .rodata",
                ".global redump_db",
                ".global redump_db_end",
                ".p2align 4",
                "redump_db:",
                f'.incbin "{inc_path}"',
                "redump_db_end:",
                '.section .note.GNU-stack,"",@progbits',
                "",
            ]
        ),
        encoding="utf-8",
    )
    print(f"Wrote {asm_path} for {resolved} ({resolved.stat().st_size} bytes)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--asm", type=Path, required=True)
    parser.add_argument("dats", type=Path, nargs="*", help="Local DAT or zip paths. Omit to download the serial DATs.")
    args = parser.parse_args()
    if args.dats:
        build_db(args.dats, args.out)
    else:
        with tempfile.TemporaryDirectory(prefix="redump-dats-") as tmp:
            downloaded = download_serial_dats(Path(tmp))
            build_db(downloaded, args.out)
    write_asm(args.out, args.asm)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
