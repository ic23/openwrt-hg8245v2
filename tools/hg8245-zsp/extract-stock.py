"""Install the pinned DSP firmware from the repository or a supplied stock image."""

import argparse
from hashlib import sha256
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "firmware_current" / "tools"))
from extract_squashfs3_full import FS

STOCK_SHA = "82e90129a04c4f829a5e9d7b1d6dee33c5eecc8024f8133559d655e964a9e815"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path)
    parser.add_argument("--stock-nor", type=Path, help="use the target unit's original NOR backup")
    args = parser.parse_args()
    local_rootfs = ROOT / "firmware_current" / "extracted" / "rootfs.bin"
    if args.stock_nor or local_rootfs.is_file():
        if args.stock_nor:
            sys.path.insert(0, str(ROOT / "tools"))
            from hg8245_factory import check_stock, legacy_payload, vendor_payload, SECOND, END
            nor = args.stock_nor.read_bytes()
            check_stock(nor)
            fs = FS(legacy_payload(vendor_payload(nor, SECOND, END, 2), 3))
        else:
            fs = FS(local_rootfs)
        _, entries = fs.walk()
        match = [i for p, i, t, _ in entries if p == "usr/bin/zsp_min.out" and t == 2]
        if len(match) != 1:
            parser.exit(1, "expected one usr/bin/zsp_min.out in stock SquashFS\n")
        data = fs.read_file(fs.inodes[match[0]])
    else:
        bundled = ROOT / "openwrt-target/package/kernel/le88266/firmware/zsp_min.out"
        data = bundled.read_bytes()
    if len(data) != 322212 or sha256(data).hexdigest() != STOCK_SHA:
        parser.exit(1, "stock DSP image does not match the verified payload\n")
    args.output.write_bytes(data)
    print(f"Pinned stock DSP ELF: {args.output} ({len(data)} bytes)")


if __name__ == "__main__":
    main()
