#!/usr/bin/env python3
"""
Focused extractor for the exact HG8245 rootfs used in this investigation.

CONFIRMED by the current rootfs.bin:
  - SquashFS 3.4 / SQUASHFS_MAGIC_LZMA (raw bytes: 73 68 73 71)
  - legacy jro LZMA-alone streams (13-byte .lzma header)
  - 128 KiB filesystem blocks (block_log=17)
  - inode metadata stream: 0x8c7575 .. 0x8c9d53
  - directory metadata stream: 0x8c9d53 .. 0x8ccc55
  - fragment metadata: 137 x 16-byte entries at the metadata block
    referenced by the fragment-table index at 0x8ccf51

This is intentionally NOT a general-purpose SquashFS implementation. It is a
reproducible extractor for the module directory in this exact firmware image.
The first-file CalibrateMode.sh inode variant remains unresolved elsewhere in
the investigation and is deliberately outside this tool's scope.
"""

from __future__ import annotations

import argparse
import hashlib
import lzma
import struct
from pathlib import Path

MAGIC_LZMA = b"shsq"  # 0x71736873 little-endian
BLOCK_SIZE = 1 << 17

INODE_TABLE_START = 0x8C7575
DIRECTORY_TABLE_START = 0x8C9D53
DIRECTORY_STREAM_END = 0x8CCC55
FRAGMENT_TABLE_START = 0x8CCF51
MODULE_DIR_STREAM_OFFSET = 19040

EXPECTED_MODULES = [
    "hi_dma.ko", "hi_dp.ko", "hi_eth.ko", "hi_ext_int.ko", "hi_gpio.ko",
    "hi_gpio_int.ko", "hi_hw.ko", "hi_i2c.ko", "hi_ioctl_dp.ko",
    "hi_ioctl_sysctl.ko", "hi_mdio.ko", "hi_spi.ko", "hi_sysctl.ko",
    "hi_timer.ko", "hi_uart.ko",
]


def u16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<H", buf, off)[0]


def u32(buf: bytes, off: int) -> int:
    return struct.unpack_from("<I", buf, off)[0]


def u64(buf: bytes, off: int) -> int:
    return struct.unpack_from("<Q", buf, off)[0]


def s16(buf: bytes, off: int) -> int:
    return struct.unpack_from("<h", buf, off)[0]


def metadata_stream(image: bytes, start: int, end: int) -> tuple[bytes, list[tuple[int, int, int, int]]]:
    """Decode SquashFS3 metadata blocks used by this jro image."""
    out = bytearray()
    blocks: list[tuple[int, int, int, int]] = []
    pos = start
    while pos < end:
        if pos + 2 > end:
            raise ValueError(f"truncated metadata header at 0x{pos:x}")
        hdr = u16(image, pos)
        length = hdr & 0x7FFF
        nxt = pos + 2 + length
        if nxt > end:
            raise ValueError(f"metadata block overruns boundary at 0x{pos:x}")
        payload = image[pos + 2:nxt]
        if hdr & 0x8000:
            decoded = payload
        else:
            # CONFIRMED: this firmware's compressed metadata payloads carry
            # a complete legacy LZMA-alone header beginning 5d 00 00 01 00.
            if payload[:5] != b"\x5d\x00\x00\x01\x00":
                raise ValueError(
                    f"unexpected metadata compressor/header at 0x{pos:x}: "
                    f"{payload[:5].hex()}"
                )
            decoded = lzma.decompress(payload, format=lzma.FORMAT_ALONE)
        blocks.append((pos, hdr, length, len(decoded)))
        out.extend(decoded)
        pos = nxt
    if pos != end:
        raise AssertionError("metadata stream ended at unexpected offset")
    return bytes(out), blocks


def parse_module_directory(directory_stream: bytes) -> list[tuple[str, int, int, int]]:
    """Parse the exact module directory group and return name/inode/type/offset."""
    p = MODULE_DIR_STREAM_OFFSET
    count_field = directory_stream[p]
    # CONFIRMED: SquashFS3 directory count field is N-1, so 0x0e => 15 entries.
    count = count_field + 1
    start_block = int.from_bytes(directory_stream[p + 1:p + 4], "little")
    base_inode_raw = int.from_bytes(directory_stream[p + 4:p + 7], "little")
    base_inode = base_inode_raw >> 8
    reserved = u16(directory_stream, p + 7)
    if (count_field, start_block, base_inode, reserved) != (0x0E, 0x1C8E, 733, 0):
        raise ValueError(
            "module directory anchor does not match exact current image: "
            f"count_field={count_field:#x} start_block={start_block} "
            f"base_inode={base_inode} reserved={reserved}"
        )

    p += 9
    out = []
    for _ in range(count):
        type_and_offset = u16(directory_stream, p)
        entry_type = type_and_offset >> 13
        local_offset = type_and_offset & 0x1FFF
        name_size = directory_stream[p + 2] + 1
        inode_delta = s16(directory_stream, p + 3)
        name = directory_stream[p + 5:p + 5 + name_size].decode("ascii")
        out.append((name, base_inode + inode_delta, entry_type, local_offset))
        p += 5 + name_size
    names = [x[0] for x in out]
    if names != EXPECTED_MODULES:
        raise ValueError(f"unexpected module directory names: {names!r}")
    return out


def find_regular_inode(inode_stream: bytes, inode_number: int) -> tuple[int, int, int, int, int, int]:
    """Find the exact 32-byte regular-file inode header used by this image.

    The exact legacy encoding has the regular-file/mode word at +0 and the
    inode number at +8, followed by start_block +12, fragment +20,
    fragment offset +24 and file_size +28.
    """
    hits = []
    for off in range(0, len(inode_stream) - 32, 2):
        raw = inode_stream[off:off + 32]
        first_word = u16(raw, 0)
        ino = u32(raw, 8)
        fragment = u32(raw, 20)
        size = u32(raw, 28)
        # CONFIRMED for the module regular inodes: low nibble == 2.
        if ino == inode_number and (first_word & 0xF) == 2:
            hits.append((
                off,
                u32(raw, 12),
                fragment,
                u32(raw, 24),
                size,
                first_word,
            ))
    if len(hits) != 1:
        raise ValueError(f"inode {inode_number} has {len(hits)} candidate regular records: {hits}")
    return hits[0]


def fragment_entries(image: bytes) -> list[tuple[int, int]]:
    """Decode the exact image's 137-entry fragment table."""
    pointer = u64(image, FRAGMENT_TABLE_START)
    if pointer != DIRECTORY_STREAM_END:
        raise ValueError(
            f"fragment table index at 0x{FRAGMENT_TABLE_START:x} points to "
            f"0x{pointer:x}, expected 0x{DIRECTORY_STREAM_END:x}"
        )
    raw, _ = metadata_stream(image, pointer, FRAGMENT_TABLE_START)
    if len(raw) != 137 * 16:
        raise ValueError(f"fragment metadata length {len(raw)} != 2192")
    return [(u64(raw, i * 16), u32(raw, i * 16 + 8)) for i in range(137)]


def read_fragment_file(image: bytes, fragments: list[tuple[int, int]], fragment: int, offset: int, size: int) -> bytes:
    if fragment >= len(fragments):
        raise ValueError(f"fragment index {fragment} out of range")
    start, compressed_size = fragments[fragment]
    stream = image[start:start + compressed_size]
    decoded = lzma.decompress(stream, format=lzma.FORMAT_ALONE)
    if offset + size > len(decoded):
        raise ValueError(
            f"fragment slice out of range: frag={fragment} offset={offset} size={size} decoded={len(decoded)}"
        )
    return decoded[offset:offset + size]


def read_block_file(image: bytes, inode_stream: bytes, inode_off: int, start_block: int, size: int) -> bytes:
    """Read a regular file with normal SquashFS3 data blocks."""
    blocks = (size + BLOCK_SIZE - 1) // BLOCK_SIZE
    list_pos = inode_off + 32
    out = bytearray()
    data_pos = start_block
    for i in range(blocks):
        encoded_len = u32(inode_stream, list_pos + 4 * i)
        if encoded_len == 0:
            decoded_len = min(BLOCK_SIZE, size - len(out))
            out.extend(b"\0" * decoded_len)
            continue
        uncompressed = bool(encoded_len & 0x80000000)
        compressed_len = encoded_len & 0x7FFFFFFF
        payload = image[data_pos:data_pos + compressed_len]
        if len(payload) != compressed_len:
            raise ValueError(f"short data block at 0x{data_pos:x}")
        if uncompressed:
            decoded = payload
        else:
            if payload[:5] != b"\x5d\x00\x00\x01\x00":
                raise ValueError(
                    f"unexpected data compressor/header at 0x{data_pos:x}: {payload[:5].hex()}"
                )
            decoded = lzma.decompress(payload, format=lzma.FORMAT_ALONE)
        out.extend(decoded)
        data_pos += compressed_len
    return bytes(out[:size])


def extract(rootfs: Path, outdir: Path) -> None:
    image = rootfs.read_bytes()
    if image[:4] != MAGIC_LZMA:
        raise ValueError(f"rootfs magic {image[:4].hex()} != 73687371 (SQUASHFS_MAGIC_LZMA)")

    inode_stream, inode_blocks = metadata_stream(image, INODE_TABLE_START, DIRECTORY_TABLE_START)
    directory_stream, directory_blocks = metadata_stream(image, DIRECTORY_TABLE_START, DIRECTORY_STREAM_END)
    entries = parse_module_directory(directory_stream)
    fragments = fragment_entries(image)

    outdir.mkdir(parents=True, exist_ok=True)
    manifest = []
    print(f"rootfs: {rootfs}")
    print(f"size: {len(image):#x} bytes")
    print(f"inode metadata: {len(inode_stream)} bytes in {len(inode_blocks)} blocks")
    print(f"directory metadata: {len(directory_stream)} bytes in {len(directory_blocks)} blocks")
    print(f"fragments: {len(fragments)}")

    for name, inode_no, entry_type, local_offset in entries:
        inode_off, start_block, fragment, fragment_offset, size, first_word = find_regular_inode(
            inode_stream, inode_no
        )
        if entry_type != 2:
            raise ValueError(f"{name}: directory entry type {entry_type}, expected regular file 2")
        if fragment != 0xFFFFFFFF:
            data = read_fragment_file(image, fragments, fragment, fragment_offset, size)
        else:
            data = read_block_file(image, inode_stream, inode_off, start_block, size)
        if data[:4] != b"\x7fELF":
            raise ValueError(f"{name}: extracted payload does not start with ELF magic")

        dst = outdir / name
        dst.write_bytes(data)
        sha = hashlib.sha256(data).hexdigest()
        manifest.append(
            f"{name}\tinode={inode_no}\tinode_stream=0x{inode_off:x}\t"
            f"start=0x{start_block:x}\tfragment={fragment}\tfragment_offset={fragment_offset}\t"
            f"size={size}\tsha256={sha}"
        )
        print(manifest[-1])

    (outdir / "MODULES_MANIFEST.tsv").write_text("\n".join(manifest) + "\n", encoding="utf-8")
    print(f"wrote {len(manifest)} modules to {outdir}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("rootfs", type=Path)
    parser.add_argument("outdir", type=Path)
    args = parser.parse_args()
    extract(args.rootfs, args.outdir)


if __name__ == "__main__":
    main()
