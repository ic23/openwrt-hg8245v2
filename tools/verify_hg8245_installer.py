"""Verify the installer and release region against the HG8245V2 NOR layout."""
import argparse
import json
import subprocess
import tarfile
import struct
from hashlib import sha256
from pathlib import Path

from hg8245_factory import END, ERASE, KERNEL, LAYOUT, SECOND, SQUASH, installer_hashes, legacy_payload, vendor_payload
from verify_fxs_image import inventory, kernel, wifi_initramfs
from verify_spi_master_image import dt_nodes, fdt_nodes

BASE = Path(__file__).resolve().parents[1] / "openwrt-target/target/linux/sd5113/base-files"


def verify(image, region, stock=None):
    nodes = dt_nodes(image)
    chosen = nodes["/chosen"]
    for name, digest in installer_hashes(region, stock).items():
        assert chosen["openwrt," + name + "-sha256"] == digest.encode() + b"\0"
    assert len(region) == END - KERNEL
    expected = {
        "boot": LAYOUT["uboot"], "flashcfg": LAYOUT["flash_config"],
        "syspara": LAYOUT["system_param"], "openwrt-system": (KERNEL, END - KERNEL),
        "minisystem": LAYOUT["minisys"], "jffs2cfg": LAYOUT["file_system"],
        "reserved": LAYOUT["reserved"],
    }
    parts = {}
    parent = "/soc/flash@20000000/partitions"
    for path, props in nodes.items():
        if path.rpartition("/")[0] != parent:
            continue
        label = props["label"].rstrip(b"\0").decode()
        assert label not in parts
        parts[label] = struct.unpack(">II", props["reg"])
        assert ("read-only" not in props) == (label == "openwrt-system")
    assert parts == expected
    archive = inventory(wifi_initramfs(kernel(image)))
    init = "lib/hg8245/installer-init"
    assert b"rdinit=/" + init.encode() in chosen["bootargs"].rstrip(b"\0").split()
    assert archive[init][0] & 0o111
    for name in (init, "lib/upgrade/platform.sh", "lib/hg8245/ram-init"):
        assert archive[name][1] == (BASE / name).read_bytes()
    for name in ("sbin/sysupgrade", "sbin/mtd", "usr/bin/sha256sum"):
        assert name in archive
    print("PASS installer payload hash, board geometry, protected regions and upgrade tools")


def verify_upgrade(path, region, fwtool):
    meta = json.loads(subprocess.check_output([fwtool, "-i", "-", path]))
    assert meta["supported_devices"] == ["huawei,hg8245"] and meta["compat_version"] == "1.0"
    with tarfile.open(path) as image:
        files = {name: image.extractfile("sysupgrade-hg8245v2/" + name).read()
                 for name in ("entry", "kernel", "root", "CONTROL")}
    manifest = files.pop("CONTROL").decode().splitlines()
    assert manifest[:2] == ["BOARD=huawei,hg8245", "LAYOUT=b176-nor-v1"]
    assert manifest[2:] == [f"{name} {len(data)} {sha256(data).hexdigest()}"
                            for name, data in files.items()]
    for name, start, limit in (("entry", KERNEL, SECOND), ("kernel", SECOND, SQUASH),
                               ("root", SQUASH, END)):
        data = files[name]
        assert 0 < len(data) <= limit - start and len(data) % 4096 == 0
        offset = start - KERNEL
        assert region[offset:offset + len(data)] == data
    used, = struct.unpack_from("<Q", files["root"], 40)
    assert len(files["root"]) == (used + ERASE - 1) // ERASE * ERASE
    assert SQUASH + len(files["root"]) <= END - 5 * ERASE
    payload = legacy_payload(vendor_payload(region, SECOND - KERNEL, SQUASH - KERNEL, 2), 3)
    start, end = struct.unpack_from("<II", payload, 0x28)
    dtb = payload[(end - start + 3) & ~3:]
    total, = struct.unpack_from(">I", dtb, 4)
    tree = fdt_nodes(dtb[:total])
    assert tree["/chosen"]["openwrt,upgrade-layout"] == b"b176-nor-v1\0"
    for node in ("ehci@40010000", "ohci@40020000"):
        assert tree["/soc/" + node]["status"] == b"okay\0"
    hog, = [node for node in tree.values() if node.get("line-name") == b"usb1-enable\0"]
    assert struct.unpack(">II", hog["gpios"]) == (58, 0) and "output-high" in hog
    writable = {p["label"].rstrip(b"\0").decode(): struct.unpack(">II", p["reg"])
                for name, p in tree.items()
                if name.rpartition("/")[0] == "/soc/flash@20000000/partitions"
                and "read-only" not in p}
    assert writable == {"krnl": (KERNEL, SECOND-KERNEL), "kernel": (SECOND, SQUASH-SECOND),
                        "rootfs": (SQUASH, END-SQUASH)}
    print("PASS LuCI metadata, payload SHA-256, installed DT, USB and writable NOR boundaries")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("installer", "region"):
        parser.add_argument(name, type=Path)
    parser.add_argument("stock", type=Path, nargs="?")
    parser.add_argument("--sysupgrade", type=Path)
    parser.add_argument("--fwtool", default="fwtool")
    args = parser.parse_args()
    verify(args.installer.read_bytes(), args.region.read_bytes(), args.stock.read_bytes() if args.stock else None)
    if args.sysupgrade:
        verify_upgrade(args.sysupgrade, args.region.read_bytes(), args.fwtool)
