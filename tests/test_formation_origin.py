"""Read-only NativeAOT formation origin ABI checks using synthetic VMD data."""
import ctypes
import hashlib
import json
import pathlib
import struct
import sys
import tempfile

library = ctypes.CDLL(str(pathlib.Path(sys.argv[1]).resolve()))
read = library.GkVmdFormation_ReadOrigin
read.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_float)]
read.restype = ctypes.c_int

def bone(name, position):
    return (name.encode("cp932").ljust(15, b"\0") +
            struct.pack("<I3f4f", 0, *position, 0, 0, 0, 1) + bytes(64))

def source(directory, parent_x):
    header = b"Vocaloid Motion Data 0002".ljust(30, b"\0") + b"Test".ljust(20, b"\0")
    data = (header + struct.pack("<I", 2) + bone("全ての親", (parent_x, 0, 0)) +
            bone("センター", (1, 0, 2)) + bytes(4 * 5))
    (directory / "source.vmd").write_bytes(data)
    (directory / "info.json").write_text(json.dumps({"source_vmd_sha256": hashlib.sha256(data).hexdigest()}))
    return data

def origin(directory):
    value = (ctypes.c_float * 3)()
    code = read(str(directory).encode("utf-8"), value)
    return code, tuple(value)

with tempfile.TemporaryDirectory() as temp:
    root = pathlib.Path(temp)
    first, second = root / "first", root / "second"
    first.mkdir(); second.mkdir()
    source(first, 0); data = source(second, 2)
    code_a, pos_a = origin(first); code_b, pos_b = origin(second)
    assert code_a == code_b == 0
    # The source evaluator converts MMD to Unity and applies its metre scale.
    assert abs(pos_a[0] + 0.07795) < 1e-4 and abs(pos_a[2] + 0.1559) < 1e-4
    assert abs(pos_b[0] - pos_a[0] + 2 * 0.07795) < 1e-4
    (second / "source.vmd").write_bytes(data + b"tampered")
    assert origin(second)[0] != 0
    (second / "info.json").unlink()
    assert origin(second)[0] != 0
    print("formation origin: source hierarchy, coordinate conversion and provenance passed")
