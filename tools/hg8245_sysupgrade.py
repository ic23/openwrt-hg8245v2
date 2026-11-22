"""Package erase-aligned HG8245V2 system components for OpenWrt sysupgrade."""
import argparse
import io
import json
import struct
import subprocess
import tarfile
from hashlib import sha256
from pathlib import Path

from hg8245_factory import END, ERASE, KERNEL, SECOND, SQUASH, legacy_payload, require, vendor_payload


def pack(region, release, output, fwtool):
    require(len(region) == END - KERNEL, "incorrect system span")
    pieces = {}
    for name, start, end, kind in (("entry", KERNEL, SECOND, 1), ("kernel", SECOND, SQUASH, 2)):
        offset = start - KERNEL
        image = vendor_payload(region, offset, end - KERNEL, kind)
        legacy_payload(image, kind + 1)
        size = (84 + len(image) + 4095) // 4096 * 4096
        pieces[name] = region[offset:offset + size]
    root = region[SQUASH - KERNEL:]
    used, = struct.unpack_from("<Q", root, 40)
    size = (used + ERASE - 1) // ERASE * ERASE
    require(root.startswith(b"hsqs") and 96 <= used and size <= len(root) - 5 * ERASE,
            "need five free JFFS2 eraseblocks")
    pieces["root"] = root[:size]
    control = ["BOARD=huawei,hg8245", "LAYOUT=b176-nor-v1"]
    for name, data in pieces.items():
        control.append(f"{name} {len(data)} {sha256(data).hexdigest()}")
    pieces["CONTROL"] = ("\n".join(control) + "\n").encode()
    with tarfile.open(output, "w", format=tarfile.USTAR_FORMAT) as tar:
        for name, data in pieces.items():
            member = tarfile.TarInfo(f"sysupgrade-hg8245v2/{name}")
            member.size = len(data)
            tar.addfile(member, io.BytesIO(data))
    meta = {"metadata_version": "1.1", "compat_version": "1.0",
            "supported_devices": ["huawei,hg8245"], "version": {
                "dist": "OpenWrt", "version": "24.10.8", "revision": release,
                "target": "sd5113/generic", "board": "huawei,hg8245"}}
    subprocess.run([fwtool, "-I", "-", output], input=json.dumps(meta).encode(), check=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for param in ("region", "release", "output", "fwtool"):
        parser.add_argument(param)
    args = parser.parse_args()
    pack(Path(args.region).read_bytes(), args.release, args.output, args.fwtool)
