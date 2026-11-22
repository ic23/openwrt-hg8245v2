"""Compare actual ZSP staging callbacks against the stock HG8245 ELF."""

from ctypes import (
    CDLL, CFUNCTYPE, POINTER, Structure, byref, c_int, c_size_t, c_uint32, c_uint,
    c_void_p, create_string_buffer, string_at,
)
from hashlib import sha256
from pathlib import Path
from struct import pack, unpack_from
import os
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def stock_sections(data):
    sh_off = unpack_from("<I", data, 32)[0]
    n, str_idx = unpack_from("<HH", data, 48)
    headers = [unpack_from("<10I", data, sh_off + 40 * i) for i in range(n)]
    str_off = headers[str_idx][4]
    return {
        data[str_off + h[0]:].split(b"\0", 1)[0].decode(): (h[3], h[4], h[5])
        for h in headers
    }


def run(lib, data, fail_at=None):
    writes = []
    callback_type = CFUNCTYPE(c_int, c_void_p, c_int, c_uint32, c_void_p, c_size_t)

    @callback_type
    def writer(ctx, target, address, payload, length):
        writes.append((target, address, string_at(payload, length)))
        return -5 if len(writes) == fail_at else 0

    lib.zsp_stage.argtypes = [c_void_p, c_size_t, callback_type, c_void_p]
    lib.zsp_stage.restype = c_int
    buf = create_string_buffer(data)
    return lib.zsp_stage(buf, len(data), writer, None), writes


def verify(lib, data):
    assert sha256(data).hexdigest() == "82e90129a04c4f829a5e9d7b1d6dee33c5eecc8024f8133559d655e964a9e815"
    sections = stock_sections(data)
    ret, writes = run(lib, data)
    assert ret == 0 and len(writes) == 17, ret
    assert writes[0] == (0, 0x93700000, data)

    for index, (target, name) in enumerate([
        (0x50000000, ".exttext_5"), (0x50000000, ".text"),
        (0x50000000, ".InstBlock0"), (0x50000000, ".Interrupt"),
        (0x50000000, ".InstBlock1"), (0x50040000, ".data"),
        (0x50040000, ".DataBlock0"), (0x50040000, ".DataBlock1"),
        (0x50040000, ".T38Data"), (0x50040000, ".TdTable"),
        (0x50040000, ".DtmfTable"), (0x50040000, ".ANRTable"),
    ], 1):
        addr, off, size = sections[name]
        assert writes[index] == (1, target + addr, data[off:off + ((size + 3) & ~3)]), name

    slots = (
        {0: ".exttext_0", 1: ".exttext_2", 3: ".exttext_4", 4: ".exttext_1",
         5: ".exttext_3", 6: ".exttext_6", 7: ".exttext_7", 8: ".exttext_5",
         9: ".exttext_9", 10: ".exttext_10"},
        {0: ".extdata_0", 1: ".extdata_2", 3: ".extdata_4", 4: ".extdata_1"},
    )
    for t, (target, table) in enumerate(zip((0x50000000, 0x50040000), slots)):
        kind, ptr, payload = writes[13 + t]
        assert kind == 0 and ptr == 0x9374EAA4 + 0x90 * t and len(payload) == 0x90
        for i in range(12):
            rec = unpack_from("<3I", payload, 12 * i)
            if i in table:
                addr, off, size = sections[table[i]]
                assert rec == (0x93700000 + off, target + addr, (size + 7) // 8), table[i]
            else:
                assert rec == (0, 0, 0)
    assert writes[15] == (2, 0x50045024, pack("<II", 0x9374EAA4, 0x9374EB34))
    assert writes[16] == (2, 0x5004504C, pack("<I", 0x9358A000))

    truncated = data[:-400]
    wrong_arch = data[:18] + b"\x28\x00" + data[20:]
    bad_shoff = data[:32] + b"\xff" * 4 + data[36:]
    for invalid in (truncated, wrong_arch, bad_shoff):
        ret, aborted = run(lib, invalid)
        assert ret < 0 and not aborted
    ret, aborted = run(lib, data, fail_at=6)
    assert ret == -5 and len(aborted) == 6
    assert all(kind != 2 for kind, _, _ in aborted)
    print("PASS: actual stock ELF; 12 resident copies, 2 overlay tables and mailbox writes match")
    print("PASS: malformed headers do not emit writes; uploader errors stop before publishing")


def verify_hif(lib):
    writes = []
    callback_type = CFUNCTYPE(c_int, c_void_p, c_int, c_uint32, c_void_p, c_size_t)

    @callback_type
    def writer(ctx, target, address, payload, length):
        writes.append((target, address, string_at(payload, length)))
        return 0

    lib.zsp_hif_prepare_cursors.argtypes = [callback_type, c_void_p]
    lib.zsp_hif_prepare_cursors.restype = c_int
    assert lib.zsp_hif_prepare_cursors(writer, None) == 0
    assert writes == [(3, 0x9350080C, bytes(4)), (3, 0x9350400C, bytes(4))]
    writes.clear()
    lib.zsp_hif_prepare_bootinfo.argtypes = [callback_type, c_void_p]
    lib.zsp_hif_prepare_bootinfo.restype = c_int
    assert lib.zsp_hif_prepare_bootinfo(writer, None) == 0
    assert writes == [(4, 0x9358A000, pack(
        "<9I", 0x9358A024, 0x93599024, 0x93599124, 0x9359E1A4,
        0x935E7B24, 0x935EFB24, 0x93500000, 0x93502000, 0x93,
    ))]
    print("PASS: stock nine-word PSTK/HIF bootinfo block at 0x9358a000")

    lib.zsp_hif_up_slot.argtypes = [c_uint, c_uint, POINTER(c_uint32)]
    lib.zsp_hif_up_slot.restype = c_int
    address = c_uint32()
    assert lib.zsp_hif_up_slot(5, 5, byref(address)) == 0
    assert lib.zsp_hif_up_slot(0, 31, byref(address)) == 1 and address.value == 0x93503F00
    assert lib.zsp_hif_up_slot(32, 0, byref(address)) < 0

    lib.zsp_hif_is_boot_complete.argtypes = [c_void_p, c_size_t]
    lib.zsp_hif_is_boot_complete.restype = c_int
    fixtures = [
        (bytes.fromhex("ff 7f 00 00 04 01 01 01"), 1),
        (bytes.fromhex("ff 7f 00 00 04 00 08 03 04 01 01 01"), 1),
        (bytes.fromhex("ff 7f 00 00 04 01 08 03"), 0),
        (bytes.fromhex("00 7f 00 00 04 01 01 01"), 0),
        (bytes.fromhex("ff 7f 00 00 04 00 01 01"), -1),
        (bytes.fromhex("ff 7f 00 00 80 01 01 01"), -1),
        (bytes.fromhex("ff 00 00 00 04 01 01 01"), -1),
    ]
    for data, expected in fixtures:
        actual = lib.zsp_hif_is_boot_complete(create_string_buffer(data), len(data))
        assert (actual < 0 if expected < 0 else actual == expected), data.hex()
    print("PASS: HIF mailbox cursors, ring bounds and boot-complete classifier (synthetic slots)")


def verify_query(lib):
    class Version(Structure):
        _fields_ = [(name, c_uint) for name in ("status", "year", "date", "main", "sub")]

    addr, cursor = c_uint32(), c_uint()
    lib.zsp_hif_down_slot.argtypes = [c_uint, c_uint, POINTER(c_uint32), POINTER(c_uint)]
    lib.zsp_hif_down_slot.restype = c_int
    assert lib.zsp_hif_down_slot(7, 2, byref(addr), byref(cursor)) == 0
    assert addr.value == 0x93500700 and cursor.value == 0
    assert lib.zsp_hif_down_slot(7, 0, byref(addr), byref(cursor)) < 0
    assert lib.zsp_hif_down_slot(8, 0, byref(addr), byref(cursor)) < 0

    req = create_string_buffer(8)
    lib.zsp_hif_make_version_request.argtypes = [c_uint, c_void_p]
    lib.zsp_hif_make_version_request.restype = c_int
    assert lib.zsp_hif_make_version_request(1, req) == 0
    assert req.raw == bytes.fromhex("ff 7f 01 00 04 01 54 00")

    lib.zsp_hif_version_reply.argtypes = [c_void_p, c_size_t, c_uint, POINTER(Version)]
    lib.zsp_hif_version_reply.restype = c_int
    result = Version()

    def decode(data):
        return lib.zsp_hif_version_reply(create_string_buffer(data), len(data), 1, byref(result))

    data = bytes.fromhex("ff 7f 01 00 1a 01 54 12") + pack(
        "<11H", 0, 0x2010, 0x0413, 0x0403, 0x00F0, 0, 0, 0, 0, 0, 0)
    assert decode(data) == 1
    assert (result.status, result.year, result.date, result.main, result.sub) == (0, 0x2010, 0x0413, 0x0403, 0x00F0)
    assert decode(data[:2] + b"\x02" + data[3:]) == 0
    assert decode(data[:20]) < 0
    assert decode(bytes.fromhex("ff 7f 01 00 06 01 54 02 00 00")) == 0
    assert decode(bytes.fromhex("ff 7f 01 00 06 01 54 02 99 00")) < 0 and result.status == 0x99
    print("PASS: exact HIF version request, down-ring wrap/full and bounded reply decoder (synthetic)")


def verify_tdm(lib):
    req = create_string_buffer(16)
    lib.zsp_hif_make_stock_tdm_request.argtypes = [c_uint, c_void_p]
    lib.zsp_hif_make_stock_tdm_request.restype = c_int
    assert lib.zsp_hif_make_stock_tdm_request(0x15, req) == 0
    # From the user's stock md at 0x93500400, in ARM little-endian word order.
    assert req.raw == pack("<4I", 0x00157FFF, 0x001B010C, 0, 0)
    assert lib.zsp_hif_make_stock_tdm_request(2, req) == 0
    assert req.raw == pack("<4I", 0x00027FFF, 0x001B010C, 0, 0)
    lib.zsp_hif_tdm_reply.argtypes = [c_void_p, c_size_t, c_uint, POINTER(c_uint)]
    lib.zsp_hif_tdm_reply.restype = c_int
    status = c_uint()

    def decode(data):
        return lib.zsp_hif_tdm_reply(create_string_buffer(data), len(data), 2, byref(status))

    assert decode(bytes.fromhex("ff 7f 02 00 06 01 1b 02 00 00")) == 1
    assert decode(bytes.fromhex("ff 7f 02 00 06 01 1b 12 00 00")) == 1
    assert decode(bytes.fromhex("ff 7f 02 00 06 01 1b 02 18 00")) < 0 and status.value == 0x18
    assert decode(bytes.fromhex("ff 7f 01 00 06 01 1b 02 00 00")) == 0
    assert decode(bytes.fromhex("ff 7f 02 00 06 01 54 02 00 00")) == 0
    assert decode(bytes.fromhex("ff 7f 02 00 04 01 1b 02")) < 0
    print("PASS: captured stock TDM request payload, matched ACK and error replies (synthetic)")


def main():
    data = (ROOT / "openwrt-target/package/kernel/le88266/firmware/zsp_min.out").read_bytes()
    with tempfile.TemporaryDirectory() as directory:
        so = Path(directory) / "zsp-loader.so"
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", "-shared", "-fPIC", str(HERE / "zsp-loader.c"),
                        str(HERE / "zsp-hif.c"),
                        "-o", str(so)], check=True)
        lib = CDLL(str(so))
        verify(lib, data)
        verify_hif(lib)
        verify_query(lib)
        verify_tdm(lib)


if __name__ == "__main__":
    main()
