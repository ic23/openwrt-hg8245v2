"""Exercise the shipping baseline MIB and VEIP service without a PON link."""
import ctypes as c
import struct
import subprocess
import tempfile
from pathlib import Path

PKG = Path(__file__).resolve().parents[1] / "openwrt-target/package/network/sd5113-omcid"
SRC = PKG / "src"


def run(lib):
    class Integrity(c.Structure):
        _fields_ = [("seal", c.c_void_p), ("verify", c.c_void_p)]

    integrity = Integrity.in_dll(lib, "omci_passthrough")
    lib.omci_mib_new.restype = c.c_void_p
    m = c.c_void_p(lib.omci_mib_new())
    assert m and lib.omci_classes_consistent()
    err = c.create_string_buffer(256)
    baseline = (PKG / "files/board.mib").read_bytes()
    transaction = 0

    def request(action, cls, entity, payload=b""):
        nonlocal transaction
        transaction += 1
        hdr = struct.pack(">HBBHH", transaction, 0x40 | action, 10, cls, entity)
        raw = (hdr + payload).ljust(40, b"\0")
        response = c.create_string_buffer(48)
        assert lib.omci_handle_frame(m, raw, len(raw), None, c.byref(integrity), response)
        out = response.raw
        assert out[:8] == struct.pack(">HBBHH", transaction, 0x20 | action, 10, cls, entity)
        return out[8:40]

    class Service(c.Structure):
        _fields_ = [("paths", c.c_uint32 * 8), ("vid", c.c_int), ("pcp", c.c_int),
                    ("unmarked", c.c_int), ("dscp", c.c_ubyte * 24),
                    ("filters", (c.c_ubyte * 26) * 2)]

    def route(expected=None, result=0):
        svc = Service()
        rc = lib.sd5113_service(m, c.byref(svc))
        assert rc == result, rc
        if expected is not None:
            gem, alloc, vid = expected
            path = (gem << 12 | alloc) if gem >= 0 else 0xffffffff
            assert list(svc.paths) == [path] * 8 and svc.vid == vid
        return svc

    try:
        assert lib.omci_mib_parse(m, baseline, err, 256) == 0, err.value
        route([-1, 0, 0])
        assert request(15, 2, 0)[0] == 0
        count = int.from_bytes(request(13, 2, 0)[:2], "big")
        assert count > 0
        upload = [request(14, 2, 0, struct.pack(">H", i)) for i in range(count)]
        assert any(row[:4] == bytes.fromhex("01490101") for row in upload)
        assert request(9, 329, 257, b"\x80\x00")[:4] == b"\0\x80\0\0"
        # Provision with the same baseline transactions a single-service OLT sends.
        for cls, inst, payload in (
            (45, 50, b""),
            (47, 10, struct.pack(">HBBH", 50, 1, 11, 257)),
            (268, 801, struct.pack(">HHB", 1020, 0x8001, 3)),
            (266, 300, struct.pack(">HBH", 801, 1, 50)),
            (47, 11, struct.pack(">HBBH", 50, 2, 5, 300)),
        ):
            assert request(4, cls, inst, payload)[0] == 0, cls
        # Fixture for the subsequent ambiguous two-service profile.
        graph = b"\n45 50\n47 10 50 1 11 257\n47 11 50 2 5 300\n266 300 801 1 50\n268 801 1020 0x8001 3\n"
        route([-1, 0, 0])
        assert request(8, 262, 0x8001, b"\x80\x00\x01\x90")[0] == 0
        route([1020, 400, 0])
        # The OLT can install a port filter before the VEIP tag operation.
        vlan_list = struct.pack(">12H", 302, 500, *([0]*10))
        assert request(4, 84, 11, vlan_list + bytes([16, 2]))[0] == 0
        assert bytes(route([1020, 400, 0]).filters[1]) == bytes([16, 2]) + vlan_list
        # Apply actual baseline Create, Set and Delete transactions.
        assert request(4, 171, 32, b"\x0a\x01\x01\x00")[0] == 0
        assert request(9, 171, 32, b"\x70\x00")[:9] == bytes.fromhex("007000000181008100")
        row = struct.pack(">IIII", 15 << 28, 15 << 28, 15 << 16, 302 << 3 | 6)
        assert request(8, 171, 32, b"\x04\x00" + row)[0] == 0
        route([1020, 400, 302])
        delete_row = row[:8] + b"\xff"*8
        assert request(8, 171, 32, b"\x04\x00" + delete_row)[0] == 0
        route([1020, 400, 0])
        assert request(8, 256, 0, b"\x02\x00\x01")[0] == 0
        route([-1, 0, 0])
        assert request(8, 256, 0, b"\x02\x00\x00")[0] == 0
        route([1020, 400, 0])
        assert request(6, 171, 32)[0] == 0
        route([1020, 400, 0])
        assert request(15, 2, 0)[0] == 0
        route([-1, 0, 0])
        ambiguous = graph + b"47 12 50 3 5 301\n266 301 802 1 50\n268 802 1021 0x8001 3\n"
        assert lib.omci_mib_parse(m, baseline + ambiguous, err, 256) == 0, err.value
        assert request(8, 262, 0x8001, b"\x80\x00\x01\x90")[0] == 0
        route(None, 2)
        mapper = (b"\n45 50\n47 10 50 1 11 257\n47 11 50 2 3 80\n"
                  b"130 80 65535 300 300 65535 300 300 301 301 301 1 0 5 0\n"
                  b"266 300 801 5 80\n268 801 1020 0x8001 3\n"
                  b"266 301 802 5 80\n268 802 1021 0x8002 3\n")
        assert lib.omci_mib_parse(m, baseline + mapper, err, 256) == 0, err.value
        for slot, alloc in ((0x8001, 400), (0x8002, 401)):
            assert request(8, 262, slot, struct.pack(">HH", 0x8000, alloc))[0] == 0
        svc = route()
        lo, hi = (1020 << 12) | 400, (1021 << 12) | 401
        assert list(svc.paths) == [lo, lo, 0xffffffff, lo, lo, hi, hi, hi]
        assert svc.unmarked == 5
        packed = bytes(range(24))
        assert request(8, 130, 80, b"\x00\x60\x00" + packed)[0] == 0
        svc = route()
        assert svc.unmarked == 8 and bytes(svc.dscp) == packed
        assert request(4, 171, 32, b"\x0a\x01\x01\x00")[0] == 0
        row = struct.pack(">IIII", 15 << 28, 15 << 28, 15 << 16, 5 << 16 | 302 << 3 | 4)
        assert request(8, 171, 32, b"\x04\x00" + row)[0] == 0
        svc = route()
        assert svc.vid == 302 and svc.pcp == 5
    finally:
        lib.omci_mib_free(m)


if __name__ == "__main__":
    with tempfile.TemporaryDirectory() as tmp:
        binary = Path(tmp) / "libomci.so"
        units = ["codec", "crc", "mib", "swdl", "classes", "service", "apply"]
        subprocess.run(["cc", "-Werror", "-Wall", "-shared", "-fPIC", "-o", binary,
                        *(SRC / (unit + ".c") for unit in units)], check=True)
        run(c.CDLL(str(binary)))
    print("PASS baseline Reset/Upload/Get/Create/Set/Delete, T-CONT, GEM, symmetric VLAN, mapper with two T-CONTs, disabled PCP, DSCP map and VLAN PCP")
