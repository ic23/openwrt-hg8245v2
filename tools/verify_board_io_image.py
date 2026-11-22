"""Check the HG8245V2 diagnostic image's pin ownership and side-effect guards."""
import argparse
import struct
from pathlib import Path

from verify_spi_master_image import dt_nodes, verify_master
from verify_spi_uart_image import inventory_with_start, kernel


def enabled(props):
    return props.get("status", b"okay\0") == b"okay\0"


def node_for(nodes, compatible):
    matches = [(path, props) for path, props in nodes.items()
               if compatible in props.get("compatible", b"").split(b"\0")]
    if len(matches) != 1:
        raise ValueError(f"expected one {compatible!r} node")
    return matches[0]


def stock_gpio_map(nodes, handle):
    node = nodes.get("/stock-gpio-map")
    if node is None:
        return None, None
    if (node.get("#gpio-cells") != struct.pack(">I", 2)
            or node.get("gpio-map-mask") != struct.pack(">II", 0xffffffff, 0)
            or node.get("gpio-map-pass-thru") != struct.pack(">II", 0, 0xffffffff)
            or "gpio-controller" in node):
        raise ValueError("invalid board GPIO nexus")
    mapping = {}
    for logical, flags, controller, pin, mapped_flags in struct.iter_unpack(">IIIII", node["gpio-map"]):
        if flags or mapped_flags or controller != handle or logical in mapping:
            raise ValueError("invalid board GPIO mapping entry")
        mapping[logical] = pin
    if mapping != {pin: (43 if pin == 45 else pin) for pin in range(64)}:
        raise ValueError("expected stock GPIO IDs 0..63 with the 45 -> 43 alias")
    return struct.unpack(">I", node["phandle"])[0], mapping


def verify(image, legacy_no_spi=False, require_gpio_map=False, gpon_rx=False, gpon_queue=False):
    raw_kernel = kernel(image)
    entries, initramfs_start = inventory_with_start(raw_kernel)
    if b"sd5113_gpio_set_funcs\0" not in raw_kernel[:initramfs_start]:
        raise ValueError("kernel must export the shared GPIO FUNC helper")
    modules = {Path(n).name: data for n, (_, data) in entries.items()
               if n.startswith("lib/modules/") and n.endswith(".ko")}
    for name in ("gpio_keys_polled.ko", "evdev.ko", "leds-gpio.ko", "i2c-dev.ko", "i2c-sd5113.ko"):
        mod = modules.get(name, b"")
        if not mod.startswith(b"\x7fELF\x01\x01") or struct.unpack_from("<HH", mod, 16) != (1, 40):
            raise ValueError(f"missing ARM module: {name}")
    forbidden = ["gpio-button-hotplug.ko", "gpio_button_hotplug.ko", "gpio_keys.ko",
                 "spidev.ko", "hg8245_slicprobe.ko"]
    if legacy_no_spi:
        forbidden += ["spi-sd5113.ko", "hg8245_le88226_id.ko", "hg8245_zsp_stage.ko"]
    if any(x in modules for x in forbidden):
        raise ValueError("unexpected module in board I/O lab")
    for path, (_, data) in entries.items():
        if path.startswith(("etc/rc.d/", "etc/init.d/", "etc/modules.d/", "etc/modules-boot.d/")):
            if b"i2c-sd5113" in data or b"i2c_sd5113" in data:
                raise ValueError(f"unexpected automatic optical I2C module load: {path}")
            if b"hg8245-optic-id" in data:
                raise ValueError(f"unexpected automatic optical EEPROM read: {path}")
    util = entries.get("usr/sbin/hg8245-optic-id", (0, b""))[1]
    if (not util.startswith(b"\x7fELF\x01\x01") or struct.unpack_from("<H", util, 18)[0] != 40 or
            b"Optical PN:" not in util or b"huawei,board-io-lab" not in util):
        raise ValueError("missing guarded ARM optical PN client")
    nodes = dt_nodes(image)
    if gpon_rx or gpon_queue:
        if b"--rx-power" not in util or b"optical power=" not in util:
            raise ValueError("missing HPSF2120 RX power client")
        _, receiver = node_for(nodes, b"hisilicon,sd5113-gpon-rx-status")
        _, ethernet = node_for(nodes, b"hisilicon,sd5113-eth")
        _, codec = node_for(nodes, b"huawei,hg8245-le88226-id")
        regs = struct.pack(">8I", 0x420000a0, 4, 0x4320000c, 4, 0x43200044, 4, 0x43200080, 4)
        names = b"los\0lof\0ploam-rx\0ploam-crc-errors\0"
        queue = b"hisilicon,sd5113-gpon-rx-queue" in receiver["compatible"].split(b"\0")
        if gpon_queue and not queue:
            raise ValueError("missing GPON queue resources")
        if queue:
            regs += struct.pack(">4I", 0x43200034, 4, 0x43200084, 4)
            names += b"ploam-pending\0ploam-dropped\0"
        if (not enabled(receiver) or enabled(ethernet) or
                receiver.get("reg") != regs or receiver.get("reg-names") != names or
                "huawei,read-ec-after-id" not in codec):
            raise ValueError("wrong GPON status resources, conflicting GLB owner or missing EC readback")
        mod = modules.get("sd5113_gpon_rx.ko", b"")
        if queue and any(name not in mod for name in (b"ploam_pending\0", b"ploam_dropped\0")):
            raise ValueError("missing PLOAM queue attributes")
        if (not mod.startswith(b"\x7fELF\x01\x01") or struct.unpack_from("<HH", mod, 16) != (1, 40)
                or b"hisilicon,sd5113-gpon-rx-status" not in mod or
                b"ploam_crc_errors\0" not in mod or b"ploam_rx\0" not in mod or
                b"MPI EC readback=" not in modules.get("hg8245_le88226_id.ko", b"")):
            raise ValueError("missing GPON status module or SPI EC check")
        for path, (_, data) in entries.items():
            if path.startswith(("etc/modules.d/", "etc/modules-boot.d/", "etc/init.d/", "etc/rc.d/")):
                if b"sd5113_gpon_rx" in data or b"sd5113-gpon-rx" in data:
                    raise ValueError(f"unexpected automatic GPON module load: {path}")
    if ("huawei,board-io-lab" not in nodes["/"] or
            ("huawei,voice-chain-lab" in nodes["/"]) != (not legacy_no_spi)):
        raise ValueError("wrong board I/O DT marker")
    _, spi = node_for(nodes, b"hisilicon,sd5113-spi")
    if enabled(spi) == legacy_no_spi:
        raise ValueError("wrong SPI status for board I/O lab")
    _, gpio = node_for(nodes, b"hisilicon,sd5113-gpio")
    handle, = struct.unpack(">I", gpio["phandle"])
    nexus, mapping = stock_gpio_map(nodes, handle)
    if require_gpio_map and nexus is None:
        raise ValueError("missing stock GPIO map")
    if nexus is not None and (
            gpio.get("hisilicon,debug-gpio-banks") != b""
            or b"sd5113-gpio-banks\0" not in raw_kernel[:initramfs_start]):
        raise ValueError("missing read-only GPIO bank snapshot")
    used = set()

    def gpio_pins(data, n, panel=False):
        if len(data) != 12 * n:
            raise ValueError("bad GPIO descriptor count")
        out = []
        for bank, pin, flags in struct.iter_unpack(">III", data):
            if panel and nexus is not None:
                if bank != nexus or pin not in mapping:
                    raise ValueError("panel consumer bypasses the stock GPIO map")
                pin = mapping[pin]
            elif bank != handle:
                raise ValueError("peripheral GPIO must be a native physical pad")
            if pin in used or pin >= 64 or flags not in (0, 1):
                raise ValueError(f"bad or overlapping GPIO {pin}")
            used.add(pin)
            out.append((pin, flags))
        return out

    panels = {}
    for compatible in (b"gpio-keys-polled", b"gpio-leds"):
        path, props = node_for(nodes, compatible)
        if not enabled(props):
            raise ValueError(f"disabled panel device: {path}")
        present = {}
        for name, child in nodes.items():
            if name.rpartition("/")[0] != path or not enabled(child):
                continue
            (pin, flags), = gpio_pins(child.get("gpios", b""), 1, panel=True)
            present[pin] = flags
            if compatible == b"gpio-keys-polled":
                expected = {34: 183, 35: 184, 12: 185}
                if (pin not in expected or child.get("linux,code") != struct.pack(">I", expected[pin])
                        or "wakeup-source" in child):
                    raise ValueError(f"unexpected GPIO key actions: {name}")
            elif child.get("linux,default-trigger") or child.get("default-state") not in (b"keep\0", b"off\0"):
                raise ValueError(f"unexpected LED trigger/default: {name}")
        panels[compatible] = present
    if panels[b"gpio-keys-polled"] != {12: 1, 34: 1, 35: 1}:
        raise ValueError("panel key map differs from verified active-low GPIOs")
    if panels[b"gpio-leds"] != dict.fromkeys((4, 37, 39, 43, 47, 48, 50, 60, 61), 1):
        raise ValueError("panel LED map differs from verified active-low GPIOs")

    path, i2c = node_for(nodes, b"hisilicon,sd5113-i2c")
    if not enabled(i2c) or i2c.get("reg") != struct.pack(">II", 0x16151000, 0x100):
        raise ValueError("wrong SD5113 I2C node")
    if gpio_pins(i2c.get("i2c-gpios", b""), 2) != [(5, 0), (6, 0)]:
        raise ValueError("I2C pad route must own exactly GPIO0[6:5]")
    if any(name.startswith(path + "/") and enabled(props) for name, props in nodes.items()):
        raise ValueError("unexpected automatic I2C child")
    if b"sd5113_gpio_set_funcs" not in modules["i2c-sd5113.ko"]:
        raise ValueError("I2C does not use shared GPIO FUNC ownership")
    if not legacy_no_spi:
        if gpio_pins(spi.get("spi-pads-gpios", b""), 4) != [(pin, 0) for pin in range(4)]:
            raise ValueError("CS0 may own GPIO0[3:0] only; GPIO4 belongs to WLAN LED")
        verify_master(image, voice_chain=True, gpio_func=True, require_cs0_pads_only=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--legacy-no-spi", action="store_true")
    parser.add_argument("--require-gpio-map", action="store_true")
    parser.add_argument("--gpon-rx", action="store_true")
    parser.add_argument("--gpon-queue", action="store_true")
    args = parser.parse_args()
    try:
        verify(args.image.read_bytes(), args.legacy_no_spi, args.require_gpio_map, args.gpon_rx,
               args.gpon_queue)
    except (OSError, ValueError, KeyError, struct.error) as error:
        parser.exit(1, f"{args.image}: {error}\n")
    print("PASS: board I/O lab, disjoint GPIO keys/LEDs/I2C, "
          + ("no SPI" if args.legacy_no_spi else "CS0 GPIO0[3:0] only, manual SPI/SLIC")
          + ", optical probe manual"
          + (", stock GPIO map and read-only snapshot" if args.require_gpio_map else "")
          + (", manual GPON LOS/LOF and MPI EC readback" if args.gpon_rx or args.gpon_queue else "")
          + (", PLOAM queue status and drops" if args.gpon_queue else ""))
