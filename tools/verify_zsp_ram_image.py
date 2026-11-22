"""Check the pinned payload and manual-only voice DSP entry point in the image."""

from hashlib import sha256
from pathlib import Path
import argparse
import stat

from verify_spi_uart_image import inventory, kernel

STOCK_SHA = "82e90129a04c4f829a5e9d7b1d6dee33c5eecc8024f8133559d655e964a9e815"
MOD = "hg8245_zsp_stage"
LAB = "hg8245-zsp-lab"


def verify_zsp(image, require_bootinfo=False, require_codec=False,
               require_version=False, require_tdm=False, require_pcm=False):
    entries = inventory(kernel(image))
    firmware = entries.get("lib/firmware/hg8245/zsp_min.out")
    mod = entries.get(f"lib/modules/6.6.144/{MOD}.ko")
    command = entries.get(f"usr/sbin/{LAB}")
    if not firmware or not mod or not command or not command[0] & stat.S_IXUSR:
        raise ValueError("firmware, module or manual lab command missing")
    if len(firmware[1]) != 322212 or sha256(firmware[1]).hexdigest() != STOCK_SHA:
        raise ValueError("embedded ZSP firmware does not match stock")
    if any(mark not in mod[1] for mark in (
            b"parmtype=boot:bool", b"ZSP_BOOT_COMPLETE", b"ZSP_BOOT_TIMEOUT", b"ZSP stopped")):
        raise ValueError("module lacks one-shot boot markers")
    if require_bootinfo and any(mark not in mod[1] for mark in (
            b"stock bootinfo verified", b"timeout diagnostics: raw IRQ20=")):
        raise ValueError("module lacks the updated bootinfo initialization or timeout evidence")
    if b"tel-unplugged" not in command[1] or b'boot=1' not in command[1]:
        raise ValueError("manual command is not explicitly gated")
    if require_codec and (
            b"codec=1" not in command[1] or
            b"parmtype=codec:bool" not in mod[1] or
            b"ZSP ready; reading LE88226 ID" not in mod[1] or
            b"codec ID probe result=" not in mod[1] or
            b"stock diagnostic expects 04 b3" not in mod[1] or
            b"parmtype=tel_unplugged:bool" in mod[1]):
        raise ValueError("module lacks the guarded, linked codec check")
    if require_version and (
            b"version=1" not in command[1] or
            b"parmtype=version:bool" not in mod[1] or
            b"HIF version query 0x54 published" not in mod[1] or
            b"ZSP_VERSION year=" not in mod[1] or
            b"ZSP_VERSION_TIMEOUT" not in mod[1]):
        raise ValueError("module lacks the explicit HIF version operation")
    if require_tdm and (
            b"boot=1 codec=1 tdm=1" not in command[1] or
            b"parmtype=tdm:bool" not in mod[1] or
            b"HIF stock TDM command 0x1b published" not in mod[1] or
            b"ZSP_TDM_MODE_ACK mode=0 status=0" not in mod[1] or
            b"ZSP_TDM_TIMEOUT" not in mod[1]):
        raise ValueError("module lacks the guarded stock TDM operation")
    if require_pcm and (
            b"boot=1 codec=1 tdm=1 pcm=1" not in command[1] or
            b"parmtype=pcm:bool" not in mod[1] or
            b"PCM host fields verified" not in mod[1] or
            b"PCM/GPIO masked fields restored" not in mod[1]):
        raise ValueError("module lacks the guarded PCM preparation and rollback")
    for path, (_, data) in entries.items():
        if path.startswith(("etc/rc.d/", "etc/init.d/", "etc/modules.d/", "etc/modules-boot.d/")):
            if any(mark.encode() in data for mark in (MOD, LAB)):
                raise ValueError(f"unexpected ZSP automatic boot hook: {path}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--require-bootinfo", action="store_true")
    parser.add_argument("--require-codec", action="store_true")
    parser.add_argument("--require-version", action="store_true")
    parser.add_argument("--require-tdm", action="store_true")
    parser.add_argument("--require-pcm", action="store_true")
    args = parser.parse_args()
    try:
        verify_zsp(args.image.read_bytes(), args.require_bootinfo,
                   args.require_codec, args.require_version, args.require_tdm,
                   args.require_pcm)
    except (OSError, ValueError) as error:
        parser.exit(1, f"{args.image}: {error}\n")
    print("PASS: pinned stock DSP image, module and manual-only boot command")
