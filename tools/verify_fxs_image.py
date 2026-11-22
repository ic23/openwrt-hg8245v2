"""Check the shipping FXS/optics drivers, DT and boot bindings in a RAM image."""
import argparse
import struct
import zlib
from hashlib import sha256
from pathlib import Path

from verify_board_io_image import enabled, node_for, stock_gpio_map
from verify_spi_master_image import dt_nodes
from verify_spi_uart_image import inventory, kernel


def wifi_initramfs(raw):
    # Linux aligns the embedded archive. Restrict inflate to one bounded gzip member.
    magic = b"\x1f\x8b\x08"
    pos = raw.find(magic)
    while pos >= 0:
        if pos % 4 == 0:
            decoder = zlib.decompressobj(16 + zlib.MAX_WBITS)
            try:
                data = decoder.decompress(raw[pos:], 32 * 1024 * 1024)
            except zlib.error:
                data = b""
            if decoder.eof and data.startswith(b"070701"):
                return data
        pos = raw.find(magic, pos + 1)
    raise ValueError("missing gzip-compressed Wi-Fi initramfs")


def verify(image, lite=False, ethernet_panel=False, front_panel=False, panel_wifi=False, apps=(), pppoe=False,
           split_lan_forwarding=False, gpon_qos_iptv=False, flat_lan=False):
    raw = kernel(image)
    entries = inventory(wifi_initramfs(raw) if panel_wifi else raw)
    nodes = dt_nodes(image)
    switch = nodes["/soc/mdio@43b0a000/switch@0"]
    ar8xxx = switch["compatible"] == b"qca,ar8316\0"
    bootargs = nodes["/chosen"]["bootargs"].rstrip(b"\0").split()
    assert [arg for arg in bootargs if arg.startswith(b"rootfstype=")] == [b"rootfstype=ramfs"], (
        "FXS initramfs requires ramfs during unpack on the 50 MiB board"
    )
    init = "lib/hg8245/ram-init"
    assert [arg for arg in bootargs if arg.startswith(b"rdinit=")] == [f"rdinit=/{init}".encode()]
    mode, script = entries[init]
    source = Path(__file__).resolve().parents[1] / "openwrt-target/target/linux/sd5113/base-files" / init
    assert mode & 0o111 and script == source.read_bytes()
    memory = entries["etc/sysctl.d/90-hg8245-memory.conf"][1].splitlines()
    assert [line for line in memory if line and not line.startswith(b"#")] == [
        b"vm.watermark_boost_factor=0", b"vm.min_free_kbytes=2048",
    ]
    gpio_path, gpio = node_for(nodes, b"hisilicon,sd5113-gpio")
    pin = nodes[gpio_path + "/gpio32-high"]
    assert enabled(gpio) and enabled(pin) and pin.get("gpio-hog") == b""
    assert pin["gpios"] == struct.pack(">II", 32, 0) and pin.get("output-high") == b""
    assert "output-low" not in pin and "input" not in pin
    optic_compat = (b"huawei,hg8245v2-optics" if any(b"huawei,hg8245v2-optics" in v.get("compatible", b"") for v in nodes.values()) else b"huawei,hpsf2120")
    for compatible, bus, addr in ((b"zarlink,le88266", b"hisilicon,sd5113-spi", 0),
                                  (optic_compat, b"hisilicon,sd5113-i2c", 0x50)):
        path, props = node_for(nodes, compatible)
        parent, controller = node_for(nodes, bus)
        assert path.rpartition("/")[0] == parent and enabled(props) and enabled(controller)
        assert props["reg"] == struct.pack(">I", addr)
    modules = {Path(k).name: v for k, (_, v) in entries.items() if k.endswith(".ko")}
    _, slic = node_for(nodes, b"zarlink,le88266")
    assert slic.get("huawei,tel-ports-swapped") == b""
    assert b"huawei,tel-ports-swapped\0" in modules["le88266.ko"]
    for name, markers in (("le88266", [b"zarlink,le88266", b"le88266-pcm", b"service", b"tel2", b"chip_id", b"DSP reset release", b"ringback", b"incoming", b"intercom"]),
                          ("hpsf2120", [b"huawei,hpsf2120", b"HPSF2120"])):
        mod = modules[name + ".ko"]
        assert mod[:6] == b"\x7fELF\x01\x01" and struct.unpack_from("<HH", mod, 16) == (1, 40)
        assert all(marker in mod for marker in markers)
    assert not ({"hg8245_zsp_stage.ko", "hg8245_le88226_id.ko"} & modules.keys())
    assert b"upstream_overhead" in modules["sd5113_gpon_rx.ko"]
    assert b"TONE_DIAL" in modules["le88266.ko"] or b"phone\0" in modules["le88266.ko"]
    _, pci = node_for(nodes, b"hisilicon,sd5113-pci")
    assert enabled(pci)
    for name in ("ath9k.ko", "ath9k_pci_owl_loader.ko"):
        assert (name in modules) == (panel_wifi or (not lite and not ethernet_panel))
    for path in ("usr/sbin/uhttpd", "www/cgi-bin/luci"):
        assert (path in entries) == (not lite)
    if ethernet_panel:
        assert "usr/share/luci/menu.d/luci-app-hg8245.json" not in entries
        for pkg, page in (("luci-app-hg8245-telephony", "telephony"), ("luci-app-sd5113-gpon", "gpon")):
            rpc = f"usr/libexec/rpcd/hg8245.{page}"
            for path in (rpc, f"usr/share/rpcd/acl.d/{pkg}.json", f"usr/share/luci/menu.d/{pkg}.json",
                         f"www/luci-static/resources/view/hg8245/{page}.js"):
                assert (path in entries) == (pkg in apps), path
            if pkg in apps:
                assert entries[rpc][0] & 0o111
        for marker in (b"omci_frame", b"omci_tx", b"omci_capture"):
            assert marker in modules["sd5113_gpon_rx.ko"]
        assert all(k in entries for k in ("etc/config/hg8245", "etc/init.d/hg8245-sip"))
        assert b"tel2_audio" in modules["le88266.ko"]
    if panel_wifi:
        assert front_panel and ethernet_panel
        assert "gpio-button-hotplug.ko" in modules and "etc/rc.button/wps" in entries
        assert b"ieee80211_create_tpt_led_trigger" in modules["ath9k.ko"]
        key = nodes["/gpio-keys/button-wps"]
        assert key["linux,code"] == struct.pack(">I", 0x211)
        assert key["gpios"] == nodes["/stock-gpio-map"]["phandle"] + struct.pack(">II", 12, 1)
        assert b"wps_status" in entries["usr/sbin/hostapd"][1]
    if front_panel:
        led_dir, _ = node_for(nodes, b"gpio-leds")
        gpio_nexus, pins = stock_gpio_map(nodes, int.from_bytes(gpio["phandle"], "big"))
        expected = {"power": 37, "los": 61, "pon-0": 39, "pon-1": 60, "wlan": 4,
                    "usb": 43, "tel1": 47, "tel2": 48, "wps": 50}
        leds = {v["label"].rstrip(b"\0").decode(): v for k, v in nodes.items()
                if k.startswith(led_dir + "/") and "label" in v}
        assert set(leds) == set(expected)
        for name, pin in expected.items():
            assert leds[name]["gpios"] == struct.pack(">III", gpio_nexus, pin, 1) and pins[pin] == pin
        assert "linux,default-trigger" not in leds["wlan"]
        assert ar8xxx or "huawei,front-panel-leds" in switch
        assert "leds-gpio.ko" in modules
        files = Path(__file__).resolve().parents[1] / "openwrt-target/package/utils/hg8245-leds/files"
        for target, name in (("etc/init.d/hg8245-leds", "init"), ("lib/hg8245/front-panel", "front-panel")):
            mode, content = entries[target]
            assert mode & 0o111 and content == (files / name).read_bytes()
        assert "ledtrig-pattern.ko" in modules
        assert entries["etc/board.d/01_leds"][1] == (source.parents[2] / "etc/board.d/01_leds").read_bytes()
        for event in ("tel1", "tel2", "los", "pon", "usb", "wps", "bbu"):
            assert entries[f"usr/libexec/led-trigger/hg8245-{event}"][1].rstrip(b"\0") == b"/lib/hg8245/led-activate"
            plugin = f"www/luci-static/resources/view/system/led-trigger/hg8245-{event}.js"
            assert (plugin in entries) == ("luci-app-ledtrig-hg8245" in apps)
        assert entries["etc/rc.d/S18hg8245-leds"][1].rstrip(b"\0").endswith(b"/init.d/hg8245-leds")
    if lite or ethernet_panel:
        assert not {"usr/sbin/wpad", "usr/sbin/tcpdump"} & entries.keys()
        assert ("usr/sbin/pppd" in entries) == pppoe
        if pppoe:
            assert any(path.endswith("/pppoe.so") for path in entries)
    _, pon = node_for(nodes, b"hisilicon,sd5113-gpon-burst")
    _, lan = node_for(nodes, b"hisilicon,sd5113-eth")
    assert enabled(pon) and enabled(lan) and "hisilicon,restore-uboot-lan" in lan
    assert pon["interrupts"] == lan["interrupts"] == struct.pack(">I", 25)
    assert pon["hisilicon,dp-irq-regs"] == lan["phandle"]
    assert b"syscon" in lan["compatible"].split(b"\0")
    pads = list(struct.iter_unpack(">III", pon["pon-gpios"]))
    assert pads == [(int.from_bytes(gpio["phandle"], "big"), pin, 0) for pin in (22, 23)]
    pon_regs = list(struct.iter_unpack(">II", pon["reg"]))
    assert pon_regs[6:10] == [(0x43200038, 12), (0x43400028, 4), (0x4340003c, 4), (0x43400040, 20)]
    if ethernet_panel:
        assert pon_regs[10:14] == [(0x42900008, 4), (0x42980100, 256), (0x42900004, 4), (0x42980000, 44)]
    assert pon_regs[14:16] == [(0x43400000, 4), (0x4340002c, 8)]
    if optic_compat == b"huawei,hg8245v2-optics":
        assert pon_regs[16:21] == [(0x43380000, 0x4000), (0x42900000, 4), (0x43400004, 0x20),
                                 (0x43200048, 0x20), (0x43400024, 4)]
        if "gem-crypto" in pon["reg-names"].decode().split("\0"):
            assert pon_regs[21:] == [(0x43300008, 0x6c)]
            assert b"gem-crypto" in modules["sd5113_gpon_rx.ko"]
        else:
            assert len(pon_regs) == 21
        if "hisilicon,gem-wan" in lan:
            assert b"sd5113_eth_set_gem" in raw and b"gem_service" in modules["sd5113_gpon_rx.ko"]
            assert entries["usr/sbin/sd5113-omcid"][1][:4] == b"\x7fELF"
            assert entries["etc/rc.d/S81sd5113-omcid"][1].rstrip(b"\0").endswith(b"/init.d/sd5113-omcid")
            assert b"329 0x0101" in entries["usr/share/sd5113-omcid/board.mib"][1]
        assert b"omcc_port" in modules["sd5113_gpon_rx.ko"] and b"random-delay" in modules["sd5113_gpon_rx.ko"]
        assert all(word in modules["hpsf2120.ko"] for word in (b"RTXM167-407-C10", b"LTB3468L-BCG+"))
        assert b"snpassword" in modules["sd5113_gpon_rx.ko"] and b"alloc_ids" in modules["sd5113_gpon_rx.ko"]
        assert "usr/sbin/hg8245-optic" in entries
    split_ports = ar8xxx or "huawei,split-lan-ports" in switch
    if flat_lan:
        assert not split_ports and "hisilicon,lan-trunk-port" not in lan
        assert b"ports=eth0" in entries["etc/board.d/02_network"][1]
    if split_lan_forwarding or "hisilicon,lan-trunk-port" in lan:
        assert split_ports
        resources = dict(zip(lan["reg-names"].rstrip(b"\0").split(b"\0"),
                             struct.iter_unpack(">II", lan["reg"])))
        assert resources[b"fe-mode"] == (0x42c80000, 0x4000)
        assert resources[b"fe-broadcast"] == (0x43782000, 0x4800)
        assert lan["hisilicon,lan-trunk-port"] == struct.pack(">I", 0)
        assert b"LAN VLAN forwarding setup failed" in raw
    if gpon_qos_iptv:
        assert b"map %u %d %d %d " in entries["usr/sbin/sd5113-omcid"][1]
        assert b"p%d=%u/%u" in modules["sd5113_gpon_rx.ko"]
        assert entries["usr/sbin/igmpproxy"][0] & 0o111
        assert entries["etc/rc.d/S99igmpproxy"][1].rstrip(b"\0").endswith(b"/init.d/igmpproxy")
        assert "www/luci-static/resources/view/hg8245/iptv.js" in entries
        assert b"igmp_snooping=1" in entries["etc/uci-defaults/13_hg8245_iptv"][1]
    if split_ports:
        if ar8xxx:
            assert switch["qca,postrip"] == struct.pack(">I", 0x80061b65)
            assert switch["qca,led-control"] == struct.pack(">III", *[0xcf35cf35] * 3)
            assert b"qca,ar8316" in modules["ar8xxx.ko"] and "swconfig.ko" in modules
            assert entries["sbin/swconfig"][0] & 0o111 and "lib/network/switch.sh" in entries
            assert b"lan_port_vids" not in raw
        else:
            assert b"lan_port_vids" in raw
        assert b"lan1 lan2 lan3 lan4" in entries["etc/board.d/02_network"][1]
        assert b"8021q" in entries["etc/uci-defaults/12_hg8245_lan_ports"][1]
    assert pon["huawei,board-info"] == next(v["phandle"] for v in nodes.values() if v.get("label") == b"syspara\0")
    for la, ll in struct.iter_unpack(">II", lan["reg"]):
        assert all(la + ll <= pa or pa + pl <= la for pa, pl in pon_regs)
    assert sha256(entries["lib/firmware/hg8245/zsp_min.out"][1]).hexdigest() == (
        "82e90129a04c4f829a5e9d7b1d6dee33c5eecc8024f8133559d655e964a9e815")
    assert entries["usr/sbin/hpsf2120"][1].rstrip(b"\0") == b"hg8245-optic-id"
    autoload = [line for k, (_, v) in entries.items() if k.startswith("etc/modules.d/")
                for line in v.splitlines()]
    assert all(mod in autoload for mod in (b"le88266", b"hpsf2120", b"i2c-sd5113", b"spi-sd5113", b"sd5113_gpon_rx"))
    print("PASS: Le88266 ASoC/FXS and HPSF2120, DT addresses, modules and boot bindings")


def verify_single_feature(image, feature):
    raw = kernel(image)
    try:
        unpacked = wifi_initramfs(raw)
    except ValueError:
        unpacked = raw
    entries = inventory(unpacked)
    nodes = dt_nodes(image)
    modules = {Path(k).name for k in entries if k.endswith(".ko")}
    autoload = {line for k, (_, data) in entries.items() if k.startswith("etc/modules.d/")
                for line in data.splitlines()}
    for name, module, compatible in (("telephony", "le88266", b"zarlink,le88266"),
                                     ("gpon", "sd5113_gpon_rx", b"hisilicon,sd5113-gpon-burst")):
        assert enabled(node_for(nodes, compatible)[1])
        assert (module + ".ko" in modules) == (feature == name)
        assert (module.encode() in autoload) == (feature == name)
    assert ("lib/firmware/hg8245/zsp_min.out" in entries) == (feature == "telephony")
    print(f"PASS: independent {feature} hardware support and autoload")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image", type=Path)
    parser.add_argument("--flat-lan", action="store_true")
    parser.add_argument("--gpon-qos-iptv", action="store_true")
    parser.add_argument("--split-lan-forwarding", action="store_true")
    parser.add_argument("--pppoe", action="store_true")
    parser.add_argument("--lite", action="store_true")
    parser.add_argument("--ethernet-panel", action="store_true")
    parser.add_argument("--front-panel", action="store_true")
    parser.add_argument("--panel-wifi", action="store_true")
    parser.add_argument("--app", action="append", default=[])
    parser.add_argument("--single-feature", choices=("telephony", "gpon"))
    args = parser.parse_args()
    if args.single_feature:
        verify_single_feature(args.image.read_bytes(), args.single_feature)
        raise SystemExit(0)
    verify(args.image.read_bytes(), lite=args.lite, ethernet_panel=args.ethernet_panel, front_panel=args.front_panel,
           panel_wifi=args.panel_wifi, apps=args.app, pppoe=args.pppoe, split_lan_forwarding=args.split_lan_forwarding,
           gpon_qos_iptv=args.gpon_qos_iptv, flat_lan=args.flat_lan)
