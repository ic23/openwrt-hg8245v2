"""Check the HG8245 RAM-only image before taking it to U-Boot."""

import argparse
from pathlib import Path
import struct
import zlib


HEADER = struct.Struct(">7I4B32s")
UIMAGE_MAGIC = 0x27051956
ZIMAGE_MAGIC = 0x016F2818
FDT_MAGIC = 0xD00DFEED
LOAD = 0x91000000
STAGE = 0x92000000
RAM_END = 0x93200000
MAX_SIZE = 16 * 1024 * 1024


def verify(image):
    if not HEADER.size + 48 + 40 <= len(image) <= MAX_SIZE:
        raise ValueError("invalid image size (maximum 16 MiB)")

    magic, hcrc, _, size, load, entry, dcrc, os, arch, kind, comp, _ = (
        HEADER.unpack_from(image)
    )
    if magic != UIMAGE_MAGIC or (os, arch, kind, comp) != (5, 2, 2, 0):
        raise ValueError("expected a legacy uncompressed ARM Linux kernel uImage")
    if (load, entry) != (LOAD, LOAD):
        raise ValueError(f"load and entry must both be {LOAD:#x}")
    if STAGE + len(image) > RAM_END or LOAD + size > STAGE:
        raise ValueError("staged file or copied payload overlaps reserved RAM")
    header = image[:4] + b"\0" * 4 + image[8:HEADER.size]
    if zlib.crc32(header) != hcrc:
        raise ValueError("uImage header CRC mismatch")

    payload = image[HEADER.size:]
    if len(payload) != size or zlib.crc32(payload) != dcrc:
        raise ValueError("uImage payload length or CRC mismatch")
    magic, start, end = struct.unpack_from("<III", payload, 0x24)
    if magic != ZIMAGE_MAGIC:
        raise ValueError("payload is not a little-endian ARM zImage")
    dtb_start = end - start
    if not 48 <= dtb_start <= len(payload) - 40:
        raise ValueError("zImage has an invalid length or no appended DTB")
    magic, total = struct.unpack_from(">II", payload, dtb_start)
    if magic != FDT_MAGIC or total < 40 or total != len(payload) - dtb_start:
        raise ValueError("expected one complete DTB immediately after zImage")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    args = parser.parse_args()
    try:
        with args.image.open("rb") as file:
            data = file.read(MAX_SIZE + 1)
        verify(data)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{args.image}: {error}\n")
    print(f"PASS: {len(data)} bytes; uImage/ARM zImage/DTB framing and both CRCs")
    print(f"RAM staging address {STAGE:#x}; kernel load/entry {LOAD:#x}")


if __name__ == "__main__":
    main()
