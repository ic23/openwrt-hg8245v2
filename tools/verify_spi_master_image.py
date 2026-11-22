"""Verify the manual SD5113 master or the separate LE88226 ID lab image."""

import argparse
import struct
from pathlib import Path

from verify_ram_image import HEADER
from verify_spi_uart_image import inventory_with_start, kernel
from verify_zsp_ram_image import verify_zsp


def dt_nodes(image):
    payload = image[HEADER.size:]
    start, end = struct.unpack_from("<II", payload, 0x28)
    return fdt_nodes(payload[end - start:])


def fdt_nodes(dtb):
    _, total, off, strings, _, _, _, _, string_size, struct_size = struct.unpack_from(
        ">10I", dtb)
    limit = off + struct_size
    if total != len(dtb) or max(limit, strings + string_size) > total:
        raise ValueError("invalid DTB block bounds")
    names = dtb[strings:strings + string_size]
    stack, nodes = [], {}
    while off + 4 <= limit:
        token = struct.unpack_from(">I", dtb, off)[0]
        off += 4
        if token == 1:
            nul = dtb.index(0, off, limit)
            stack.append(dtb[off:nul].decode("ascii"))
            nodes["/".join(stack) or "/"] = {}
            off = (nul + 4) & ~3
        elif token == 2 and stack:
            stack.pop()
        elif token == 3 and stack and off + 8 <= limit:
            size, name_off = struct.unpack_from(">II", dtb, off)
            off += 8
            if off + size > limit:
                break
            name = names[name_off:names.index(0, name_off)].decode("ascii")
            nodes["/".join(stack) or "/"][name] = dtb[off:off + size]
            off = (off + size + 3) & ~3
        elif token == 4:
            continue
        elif token == 9 and not stack:
            return nodes
        else:
            break
    raise ValueError("invalid DTB structure")


def verify_master(image, le88226_id=False, voice_chain=False, gpio_func=False,
                  require_cs0_pads_only=False):
    le88226_id |= voice_chain
    raw_kernel = kernel(image)
    entries, initramfs_start = inventory_with_start(raw_kernel)
    module = entries.get("lib/modules/6.6.144/spi-sd5113.ko")
    if not module:
        raise ValueError("SD5113 master module is not packaged")
    code = module[1]
    if (not code.startswith(b"\x7fELF\x01\x01") or
            struct.unpack_from("<HH", code, 16) != (1, 40)):
        raise ValueError("expected ARM relocatable kernel module")
    shared_funcs = b"sd5113_gpio_set_funcs" in code
    if (not (shared_funcs or b"sd5113_gpio_spi_dir" in code) or
            any(x not in code for x in (b"devm_spi_register_controller", b"SPI master registered"))):
        raise ValueError("missing controller registration or pad ownership code")
    if gpio_func and (not shared_funcs or b"SPI pad route lost" not in code):
        raise ValueError("missing shared GPIO FUNC and pre-transfer SPI route verification")
    if gpio_func and b"sd5113_gpio_set_funcs\0" not in raw_kernel[:initramfs_start]:
        raise ValueError("kernel must export the shared GPIO FUNC helper")
    read_bank_symbol = b"sd5113_gpio_read_bank_funcs\0"
    cs0_pads_only = read_bank_symbol in code
    if require_cs0_pads_only and not cs0_pads_only:
        raise ValueError("required SPI fix: CS0 must not own GPIO4")
    if cs0_pads_only and (read_bank_symbol not in raw_kernel[:initramfs_start] or
                          b"GPIO4 FUNC[4] is set" not in code):
        raise ValueError("missing read-only GPIO4 conflict guard")
    client = entries.get("lib/modules/6.6.144/hg8245_le88226_id.ko")
    if bool(client) != le88226_id:
        raise ValueError("incorrect inclusion of LE88226 ID module")
    if client:
        code = client[1]
        if (len(code) < 52 or not code.startswith(b"\x7fELF\x01\x01") or
                struct.unpack_from("<HH", code, 16) != (1, 40)):
            raise ValueError("expected ARM relocatable LE88226 ID module")
        if any(mark not in code for mark in (b"spi_sync_locked", b"spi_bus_lock",
                                               b"spi_bus_unlock", b"LE88226 SPI core ID=",
                                               b"parmtype=tel_unplugged:bool")):
            raise ValueError("missing guarded SPI core-only ID client")
        if b"__ioremap" in code or b"regmap_" in code:
            raise ValueError("ID client must not access controller registers directly")
        if voice_chain and any(mark not in code for mark in (b"hg8245_le88226_run_id",
                                                               b"SPI ID armed; no transfer")):
            raise ValueError("client is missing the deferred ID entry")
    zsp = entries.get("lib/modules/6.6.144/hg8245_zsp_stage.ko")
    if bool(zsp) != voice_chain:
        raise ValueError("unexpected inclusion or absence of the ZSP module")
    if voice_chain:
        verify_zsp(image, require_bootinfo=True, require_codec=True,
                   require_tdm=True, require_pcm=True)
        if any(mark not in zsp[1] for mark in (b"parmtype=spi_core:bool",
                                                b"using the SPI core ID client",
                                                b"HIGHWAY CRG+0x38[27] clear/set verified")):
            raise ValueError("missing the integrated HIGHWAY/SPI-core ZSP path")
        if gpio_func and (b"sd5113_gpio_set_funcs" not in zsp[1] or b"GPIO2_FUNC" not in zsp[1]):
            raise ValueError("HIGHWAY still uses the old GPIO path")
    for path, (_, data) in entries.items():
        if path.endswith(("/hg8245_slicprobe.ko", "/spidev.ko")):
            raise ValueError(f"unexpected concurrent SPI client: {path}")
        if path.startswith(("etc/rc.d/", "etc/init.d/", "etc/modules.d/", "etc/modules-boot.d/")):
            if any(mark in data for mark in (b"spi-sd5113", b"spi_sd5113",
                                               b"hg8245_le88226_id", b"hg8245-le88226-id")):
                raise ValueError(f"unexpected automatic SPI load: {path}")
    nodes = dt_nodes(image)
    if ("huawei,voice-chain-lab" in nodes["/"]) != voice_chain:
        raise ValueError("wrong voice-chain root marker")
    spi_nodes = [(path, props) for path, props in nodes.items()
                 if b"hisilicon,sd5113-spi" in props.get("compatible", b"").split(b"\0")]
    if len(spi_nodes) != 1:
        raise ValueError("expected one SD5113 SPI node")
    path, props = spi_nodes[0]
    if (props.get("status") != b"okay\0" or props.get("clock-names") != b"ahb\0" or
            props.get("reg") != struct.pack(">II", 0x16150000, 0x64)):
        raise ValueError("wrong SPI node status, clock or resource")
    children = {p for p in nodes if p.startswith(path + "/")}
    if le88226_id:
        codec_path = path + "/codec@0"
        if children != {codec_path}:
            raise ValueError("ID lab requires only the LE88226 child")
        codec = nodes[codec_path]
        if (codec.get("compatible") != b"huawei,hg8245-le88226-id\0" or
                codec.get("reg") != struct.pack(">I", 0) or
                codec.get("spi-max-frequency") != struct.pack(">I", 8000000) or
                codec.get("spi-cpha") != b"" or codec.get("spi-cpol") != b""):
            raise ValueError("wrong LE88226 CS, speed or stock frame mode")
        if ("huawei,wait-zsp" in codec) != voice_chain:
            raise ValueError("wrong LE88226 deferred bind state")
    elif children:
        raise ValueError("SPI master lab DT must not contain any SPI clients")
    gpio = [v for v in nodes.values() if b"hisilicon,sd5113-gpio" in v.get("compatible", b"")]
    if len(gpio) != 1 or not gpio[0].get("phandle"):
        raise ValueError("SD5113 GPIO provider phandle missing")
    handle = struct.unpack(">I", gpio[0]["phandle"])[0]
    num_pads = 4 if cs0_pads_only else 5
    expected = b"".join(struct.pack(">III", handle, pin, 0) for pin in range(num_pads))
    if props.get("spi-pads-gpios") != expected:
        raise ValueError(f"SPI pad map does not match GPIO0[{num_pads - 1}:0]")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("image", type=Path)
    p.add_argument("--le88226-id", action="store_true")
    p.add_argument("--voice-chain", action="store_true")
    p.add_argument("--gpio-func", action="store_true")
    p.add_argument("--cs0-pads-only", action="store_true")
    args = p.parse_args()
    try:
        verify_master(args.image.read_bytes(), args.le88226_id, args.voice_chain, args.gpio_func,
                      args.cs0_pads_only)
    except (OSError, ValueError, struct.error) as error:
        p.exit(1, f"{args.image}: {error}\n")
    print("PASS: ARM SPI master, correct pads, no autoload; " +
          ("guarded HIGHWAY/ZSP and deferred LE88226 ID" if args.voice_chain else
           "one guarded LE88226 ID client" if args.le88226_id else "no SPI child"))
