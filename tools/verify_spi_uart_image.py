"""Verify the embedded diagnostic module and boot hook in an HG8245 RAM image."""

import argparse
import lzma
from pathlib import Path
import re
import stat
import struct

from verify_ram_image import HEADER, verify

LIMIT = 96 * 1024 * 1024
XZ = b"\xfd7zXZ\x00"
NEWC = re.compile(rb"07070[12][0-9a-fA-F]{104}")
AUDIT = "hg8245-spi-audit"


def kernel(image):
    verify(image)
    payload = image[HEADER.size:]
    start, end = struct.unpack_from("<II", payload, 0x28)
    compressed = payload[: end - start]
    for offset in range(min(len(compressed), 0x20000)):
        if not compressed.startswith(XZ, offset):
            continue
        try:
            decoder = lzma.LZMADecompressor(format=lzma.FORMAT_XZ, memlimit=LIMIT)
            raw = decoder.decompress(compressed[offset:], max_length=LIMIT)
        except lzma.LZMAError:
            continue
        if decoder.eof:
            return raw
    raise ValueError("could not find the zImage XZ kernel stream")


def newc_entries(data, offset):
    while (match := NEWC.match(data, offset)) is not None:
        mode = int(data[offset + 14 : offset + 22], 16)
        size = int(data[offset + 54 : offset + 62], 16)
        namesize = int(data[offset + 94 : offset + 102], 16)
        begin = match.end()
        end = begin + namesize
        if not 1 <= namesize <= 4096 or end > len(data) or data[end - 1]:
            break
        name = data[begin : end - 1].decode("ascii").removeprefix("./")
        offset = (end + 3) & ~3
        end = offset + size
        if end > len(data):
            break
        if name == "TRAILER!!!":
            return
        yield name, mode, data[offset:end]
        offset = (end + 3) & ~3
    raise ValueError("invalid initramfs newc archive")


def inventory_with_start(raw):
    for match in NEWC.finditer(raw):
        if match.start() % 4:
            continue
        try:
            entries = {name: (mode, data) for name, mode, data in newc_entries(raw, match.start())}
        except ValueError:
            continue
        if "sbin/init" in entries:
            return entries, match.start()
    raise ValueError("could not read the built-in uncompressed initramfs")


def inventory(raw):
    return inventory_with_start(raw)[0]


def verify_audit(image, require_gpio4=False, require_cold=False):
    entries = inventory(kernel(image))
    command = entries.get(f"usr/sbin/{AUDIT}")
    service = entries.get(f"etc/init.d/{AUDIT}")
    hook = entries.get(f"etc/rc.d/S90{AUDIT}")
    modules = [name for name in entries if name.endswith("/hg8245_slicprobe.ko")]
    if not command or not service or not hook or len(modules) != 1:
        raise ValueError("diagnostic module, command or automatic boot hook missing")
    if not all(stat.S_ISREG(mode) and mode & 0o111 for mode, _ in (command, service)):
        raise ValueError("diagnostic command and service must be executable")
    if not stat.S_ISLNK(hook[0]) or hook[1].rstrip(b"\0") != f"../init.d/{AUDIT}".encode():
        raise ValueError("boot hook does not point to the diagnostic service")
    if f"{AUDIT} snapshot".encode() not in service[1] or b"tel_unplugged=1" in service[1]:
        raise ValueError("automatic boot hook is not limited to the passive snapshot")
    if b"stock_ctrl_cycle=1" not in command[1] or b"tel_unplugged=1" not in command[1]:
        raise ValueError("missing guarded UART ID probe")
    module = entries[modules[0]][1]
    if (not module.startswith(b"\x7fELF\x01\x01") or len(module) < 52 or
            struct.unpack_from("<HH", module, 16) != (1, 40)):
        raise ValueError("embedded module is not an ARM relocatable ELF")
    for marker in (b"vermagic=", b"parmtype=stock_ctrl_cycle:bool", b"GPIO0_OUTMODE=",
                   b"GPIO0_DATA=", b"CRG34=", b"GPIO2_DIR=",
                   b"snapshot only; no MMIO writes or transfers"):
        if marker not in module:
            raise ValueError(f"embedded diagnostic module lacks {marker!r}")
    if require_gpio4:
        if (b"parmtype=gpio4_low_trial:bool" not in module or
                b"gpio4_low_trial=1" not in command[1] or
                b"id-gpio4-low" in service[1]):
            raise ValueError("GPIO4 trial missing or allowed to run from the boot hook")
    if require_cold:
        if (b"parmtype=stock_cold_cycle:bool" not in module or
                b"stock_cold_cycle=1" not in command[1] or
                b"id-cold" in service[1]):
            raise ValueError("cold SPI trial missing or allowed to run from the boot hook")
    return modules[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--require-gpio4", action="store_true")
    parser.add_argument("--require-cold", action="store_true")
    args = parser.parse_args()
    try:
        module = verify_audit(args.image.read_bytes(), args.require_gpio4, args.require_cold)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{args.image}: {error}\n")
    print(f"PASS: {module}, UART command, and passive boot hook are embedded")


if __name__ == "__main__":
    main()
