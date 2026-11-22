"""Stage the VE880 library and its portable headers, excluding the BCM SoC code."""
from pathlib import Path
import sys
import tarfile

archive, output = Path(sys.argv[1]), Path(sys.argv[2])
root = "bcm63xx-phone-a49d39ee17c6b083a48ed583c3644f154127b593/bcm63xx-phone/src/extern/zarlink/"
with tarfile.open(archive) as source:
    for entry in source:
        if entry.isfile() and entry.name.endswith("/phone/zarlink/profiles/profiles_common.c"):
            # Use the pinned VoicePath package for the call-progress coefficients.
            data = source.extractfile(entry).read()
            tones = []
            for name in (b"TONE_DIAL", b"TONE_BUSY", b"TONE_RINGBACK"):
                start = data.index(b"const VpProfileDataType " + name + b"[]")
                end = data.index(b"};", start) + 2
                tones.append(b"static " + data[start:end])
            (output / "line-tones.h").write_bytes(b"\n".join(tones) + b"\n")
        if not entry.isfile() or not entry.name.startswith(root):
            continue
        path = Path(entry.name[len(root):])
        if path.parent.as_posix() == "arch/bcm63xx" and path.suffix == ".h":
            dest = output / "port" / path.name
        elif (path.parent.as_posix() in ("2.24.0.lite/api_lib/includes",
              "2.24.0.lite/api_lib/common", "2.24.0.lite/api_lib/vp880_api")
              and path.suffix in (".h", ".c")):
            dest = output / path.parent.name / path.name
        else:
            continue
        data = source.extractfile(entry).read()
        if path.name == "vp_api_cfg.h":
            data = data.replace(b"#define VP880_INCLUDE_LINE_TEST_PACKAGE VP880_LINE_TEST_PROFESSIONAL",
                                b"#undef VP880_INCLUDE_LINE_TEST_PACKAGE")
        dest.parent.mkdir(exist_ok=True)
        dest.write_bytes(data)
