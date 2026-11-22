"""Compose the normal-system erase range for the HG8245V2 board-1 stock layout."""
import argparse
from hashlib import sha256
from pathlib import Path
import socket
import struct
import subprocess
from xml.etree import ElementTree
from zlib import crc32


KERNEL, SECOND, SQUASH, END = 0x60000, 0x1E0000, 0x4C0000, 0xAE0000
ERASE = 0x20000
FLASH_SIZE = 0x1000000
LAYOUT = {
    "uboot": (0, 0x40000), "flash_config": (0x40000, 0x1000),
    "system_param": (0x41000, 0x1F000), "kernel": (KERNEL, SECOND - KERNEL),
    "rootfs": (SECOND, END - SECOND), "minisys": (END, 0x380000),
    "file_system": (0xE60000, 0x180000), "reserved": (0xFE0000, 0x20000),
}
UBOOT_SHA = "87e2360895ccedce064672c651319ea595c8f651f00c595de7dce0e6b1307e14"
FLASH_CONFIG_SHA = "e9d03301dec771f48e09f50acbd28ff604f67e86ca54f2fef04ff2543755108a"
STOCK_FS_SHA = "6bd59e7838b58da67d1d64858da4887ae22eb859a313907ed2efe180d19979a7"


def require(condition, description):
    if not condition:
        raise ValueError(description)


def vendor_payload(flash, offset, limit, kind):
    tag, length, checksum = struct.unpack_from("<III", flash, offset + 72)
    require(flash[offset:offset + 4] == b"whwh" and tag == kind, "vendor header")
    require(64 <= length <= limit - offset - 84, "vendor payload outside partition")
    payload = flash[offset + 84:offset + 84 + length]
    require(checksum == crc32(payload), "vendor CRC")
    return payload


def legacy_payload(image, kind):
    magic, hcrc, _, size, _, _, dcrc, os_, arch, typ, comp = struct.unpack_from(">7I4B", image)
    require((magic, os_, arch, typ, comp) == (0x27051956, 5, 2, kind, 0), "legacy image type")
    require(size == len(image) - 64, "legacy image length")
    require(hcrc == crc32(image[:4] + bytes(4) + image[8:64]), "legacy header CRC")
    require(dcrc == crc32(image[64:]), "legacy data CRC")
    return image[64:]


def check_stock(stock):
    require(len(stock) == FLASH_SIZE and sha256(stock[:0x40000]).hexdigest() == UBOOT_SHA,
            "expected a full NOR backup with the supported U-Boot")
    xml = stock[0x40000:0x41000].split(b"</root>", 1)[0] + b"</root>"
    for name, span in LAYOUT.items():
        nodes = ElementTree.fromstring(xml).findall(name)
        require(len(nodes) == 1, "missing or duplicate stock partition " + name)
        node = nodes[0]
        require(node.get("rotate_flag") == "0" and
                tuple(int(node.get(key), 0) for key in ("address0", "length0")) == span,
                "incompatible stock layout: " + name)
    legacy_payload(vendor_payload(stock, KERNEL, SECOND, 1), 2)
    fs = legacy_payload(vendor_payload(stock, SECOND, END, 2), 3)
    require(sha256(fs).hexdigest() == STOCK_FS_SHA, "unsupported stock rootfs/DSP source")
    vendor_payload(stock, END, 0xE60000, 4)
    board = ElementTree.fromstring(stock[0x41000:0x42000].split(b"\0", 1)[0])
    sn = bytes.fromhex(board.find("sn").attrib["cur_sn"])
    require(len(sn) == 8 and any(sn) and board.find("LanMac") is not None, "missing BoardInfo")
    cal = stock[0xFE0000:0xFE0AE0]
    require(len(set(cal)) > 2, "missing wireless calibration")


def wrap(stock, offset, kind, legacy):
    if stock is None:
        # Huawei header: magic, 64-byte version field, stock build timestamp, image type.
        prefix = struct.pack("<4s64sII", b"whwh", b"V100R003C00SPC106B176", 0x4F6FEE90, kind)
    else:
        prefix = stock[offset:offset + 76]
    return prefix + struct.pack("<II", len(legacy), crc32(legacy)) + legacy


def installer_hashes(region, stock=None):
    return {
        "system": sha256(region).hexdigest(),
        "boot": UBOOT_SHA if stock is None else sha256(stock[:0x40000]).hexdigest(),
        "layout": FLASH_CONFIG_SHA if stock is None else sha256(stock[0x40000:0x41000]).hexdigest(),
    }


def compose(stock, first, second, squash):
    if stock is not None:
        check_stock(stock)
    legacy_payload(first, 2)
    kernel = legacy_payload(second, 3)
    require(kernel[36:40] == bytes.fromhex("18286f01"), "expected ARM zImage")
    require(squash[:4] == b"hsqs" and len(squash) >= 96, "expected SquashFS")
    used, = struct.unpack_from("<Q", squash, 40)
    require(96 <= used <= len(squash), "truncated SquashFS")
    overlay = SQUASH + (used + ERASE - 1) // ERASE * ERASE
    require(SQUASH + len(squash) <= overlay <= END - 3 * ERASE, "insufficient room for overlay")
    region = bytearray(b"\xff") * (END - KERNEL)
    for start, limit, data in ((KERNEL, SECOND, wrap(stock, KERNEL, 1, first)),
                               (SECOND, SQUASH, wrap(stock, SECOND, 2, second)),
                               (SQUASH, overlay, squash)):
        require(len(data) <= limit - start, "payload exceeds its partition")
        region[start - KERNEL:start - KERNEL + len(data)] = data
    # A valid NOR JFFS2 cleanmarker lets fstools mount the empty filesystem
    # on the first boot, including kernels without OpenWrt's EOF-marker patch.
    header = struct.pack("<HHI", 0x1985, 0x2003, 12)
    cleanmarker = header + struct.pack("<I", crc32(header, 0xffffffff) ^ 0xffffffff)
    for block in range(overlay - KERNEL, END - KERNEL, ERASE):
        region[block:block + len(cleanmarker)] = cleanmarker
    require(len(region) == END - KERNEL, "incorrect system region size")
    require(vendor_payload(region, 0, SECOND - KERNEL, 1) == first and
            vendor_payload(region, SECOND - KERNEL, SQUASH - KERNEL, 2) == second, "envelope round-trip")
    print(f"SquashFS {used} bytes; rootfs_data 0x{overlay:x}..0x{END:x} ({END-overlay} bytes)")
    return region


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("stock", type=Path, nargs="?", help="untouched full NOR backup from the unit")
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--receive-backup", metavar="PC_LAN_IP",
                         help="listen on this PC address (TCP 8245) and create a verified stock backup")
    actions.add_argument("--pack", nargs=4, type=Path, metavar=("ENTRY", "KERNEL", "SQUASHFS", "OUTPUT"))
    actions.add_argument("--bind-installer", nargs=2, type=Path, metavar=("DTB", "REGION"),
                         help="set /chosen hashes for the installer's pre-write checks")
    actions.add_argument("--programmer-image", nargs=2, type=Path, metavar=("REGION", "OUTPUT"),
                         help="prepare a private full-chip file offline from this unit's original backup")
    args = parser.parse_args()
    if args.stock is None and not (args.pack or args.bind_installer):
        parser.error("a full NOR backup path is required for this operation")
    if args.receive_backup:
        if args.stock.exists():
            parser.error("choose a new filename for the stock backup")
        with socket.create_server((args.receive_backup, 8245)) as listener:
            print(f"Waiting on {args.receive_backup}:8245", flush=True)
            conn, _ = listener.accept()
            with conn, conn.makefile("rb") as stream:
                conn.settimeout(90)
                stock = stream.read(FLASH_SIZE)
        check_stock(stock)
        with args.stock.open("xb") as target:
            target.write(stock)
        print(f"Verified {len(stock)} bytes, SHA-256 {sha256(stock).hexdigest()}")
        return
    stock = args.stock.read_bytes() if args.stock is not None else None
    if args.bind_installer:
        if stock is not None:
            check_stock(stock)
        dtb, region = args.bind_installer
        for part, digest in installer_hashes(region.read_bytes(), stock).items():
            subprocess.run(["fdtput", "-t", "s", dtb, "/chosen", f"openwrt,{part}-sha256", digest], check=True)
        return
    if args.pack:
        first, second, squash, output = args.pack
        region = compose(stock, first.read_bytes(), second.read_bytes(), squash.read_bytes())
        with output.open("xb") as target:
            target.write(region)
        return
    elif args.programmer_image:
        check_stock(stock)
        source, output = args.programmer_image
        region = source.read_bytes()
        checksums = (source.parent / "SHA256SUMS").read_text().splitlines()
        require(f"{sha256(region).hexdigest()}  {source.name}" in checksums, "release SHA-256 mismatch")
        require(len(region) == END - KERNEL, "wrong system region length")
        combined = stock[:KERNEL] + region + stock[END:]
        legacy_payload(vendor_payload(combined, KERNEL, SECOND, 1), 2)
        legacy_payload(vendor_payload(combined, SECOND, SQUASH, 2), 3)
        require(combined[SQUASH:SQUASH + 4] == b"hsqs", "missing OpenWrt rootfs")
        with output.open("xb") as target:
            target.write(combined)
    else:
        check_stock(stock)
    print("Verified stock NOR layout, container CRCs, BoardInfo and calibration region")


if __name__ == "__main__":
    main()
