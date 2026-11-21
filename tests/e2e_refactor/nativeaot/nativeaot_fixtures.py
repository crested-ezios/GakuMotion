"""Local ctypes bindings and fixture helpers for E2E-2 (tests/e2e_refactor/nativeaot/).

Imports shared generators from ../common/vmd_fixture_gen.py without modifying common/.
"""
from __future__ import annotations

import ctypes
import hashlib
import json
from pathlib import Path
import shutil
import sys
from typing import Any

_COMMON_DIR = Path(__file__).resolve().parent.parent / "common"
if str(_COMMON_DIR) not in sys.path:
    sys.path.insert(0, str(_COMMON_DIR))

import vmd_fixture_gen as vfg  # noqa: E402


PER_SLOT_SIDECAR_FILES = (
    "bone_overrides.json",
    "root_height_calibration.json",
    "aux_replay.json",
)


class NativeAotClient:
    """Thin ctypes wrapper around GakuMotion.VmdImporter.Native.dll."""

    def __init__(self, dll_path: Path | str) -> None:
        self.dll_path = Path(dll_path)
        if not self.dll_path.is_file():
            raise FileNotFoundError(f"NativeAOT DLL not found: {self.dll_path}")
        self._lib = ctypes.CDLL(str(self.dll_path))

        self._abi_version = self._lib.GkVmdImporter_AbiVersion
        self._abi_version.argtypes = []
        self._abi_version.restype = ctypes.c_int32

        self._run = self._lib.GkVmdImport_Run
        self._run.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_int32]
        self._run.restype = ctypes.c_int32

    def abi_version(self) -> int:
        return int(self._abi_version())

    def run_raw(self, req_bytes: bytes | None, resp_cap: int, resp_is_null: bool = False) -> tuple[int, str]:
        req_buf = ctypes.create_string_buffer(req_bytes) if req_bytes is not None else None
        req_ptr = ctypes.cast(req_buf, ctypes.c_void_p) if req_buf is not None else ctypes.c_void_p(0)

        if resp_is_null:
            rc = int(self._run(req_ptr, ctypes.c_void_p(0), ctypes.c_int32(resp_cap)))
            return rc, ""

        alloc_cap = max(1, int(resp_cap))
        resp_buf = ctypes.create_string_buffer(alloc_cap)
        rc = int(self._run(req_ptr, ctypes.cast(resp_buf, ctypes.c_void_p), ctypes.c_int32(resp_cap)))
        raw_text = resp_buf.value.decode("utf-8", errors="replace") if resp_cap > 0 else ""
        return rc, raw_text

    def run_json_bytes(self, raw_utf8: bytes, resp_cap: int = 65536) -> tuple[int, dict[str, Any] | None, str]:
        rc, raw_text = self.run_raw(raw_utf8, resp_cap=resp_cap, resp_is_null=False)
        parsed: dict[str, Any] | None = None
        if raw_text:
            try:
                obj = json.loads(raw_text)
                if isinstance(obj, dict):
                    parsed = obj
            except json.JSONDecodeError:
                parsed = None
        return rc, parsed, raw_text

    def run_import(self, request_obj: dict[str, Any], resp_cap: int = 65536) -> tuple[int, dict[str, Any] | None, str]:
        payload = json.dumps(request_obj, ensure_ascii=False).encode("utf-8")
        return self.run_json_bytes(payload, resp_cap=resp_cap)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest().lower()


def sha256_file(path: Path) -> str:
    return sha256_bytes(path.read_bytes())


def read_json(path: Path) -> dict[str, Any]:
    return json.loads(path.read_text(encoding="utf-8"))


def snapshot_tree(root: Path) -> dict[str, tuple[str, int, str]]:
    """Capture deterministic directory & file content SHA-256 snapshot."""
    snap: dict[str, tuple[str, int, str]] = {}
    if not root.exists():
        return snap
    for p in sorted(root.rglob("*"), key=lambda item: item.relative_to(root).as_posix()):
        rel = p.relative_to(root).as_posix()
        if p.is_dir():
            snap[rel] = ("dir", 0, "")
        else:
            data = p.read_bytes()
            snap[rel] = ("file", len(data), sha256_bytes(data))
    return snap


def reset_dir(path: Path) -> Path:
    if path.exists():
        shutil.rmtree(path)
    path.mkdir(parents=True, exist_ok=True)
    return path


def write_vmd(path: Path, data: bytes) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return path


def write_target_leg_geometry(vmd_path: Path, bind_to_vmd: bool = True, dest_dir: Path | None = None) -> Path:
    """Write target_leg_geometry.json bound (or unbound) to a VMD's SHA-256."""
    target_dir = dest_dir if dest_dir is not None else vmd_path.parent
    target_dir.mkdir(parents=True, exist_ok=True)
    geom_path = target_dir / "target_leg_geometry.json"
    digest = sha256_file(vmd_path) if bind_to_vmd else ("0" * 64)
    payload = {
        "schema": 1,
        "source_vmd_sha256": digest,
        "note": "synthetic e2e leg geometry binding",
    }
    geom_path.write_text(json.dumps(payload, indent=2), encoding="utf-8")
    return geom_path


def write_slot_sidecars(slot_dir: Path, tag: str) -> dict[str, bytes]:
    """Plant deterministic per-slot sidecar JSON files for preservation & migration checks."""
    slot_dir.mkdir(parents=True, exist_ok=True)
    files: dict[str, bytes] = {
        "bone_overrides.json": json.dumps(
            {"tag": tag, "overrides": {"LeftUpperArm": {"rx": 1.5, "ry": -2.0, "rz": 0.25}}},
            indent=2,
        ).encode("utf-8"),
        "root_height_calibration.json": json.dumps(
            {"tag": tag, "root_height_offset_m": 0.0125},
            indent=2,
        ).encode("utf-8"),
        "aux_replay.json": json.dumps(
            {"tag": tag, "aux_channels": ["eye_l", "eye_r"]},
            indent=2,
        ).encode("utf-8"),
    }
    for name, content in files.items():
        (slot_dir / name).write_bytes(content)
    return files


def read_slot_sidecars(slot_dir: Path) -> dict[str, bytes]:
    out: dict[str, bytes] = {}
    for name in PER_SLOT_SIDECAR_FILES:
        p = slot_dir / name
        if p.is_file():
            out[name] = p.read_bytes()
    return out


def make_distinct_body_vmd(last_frame: int, seed: int, include_morphs: bool = False) -> bytes:
    return vfg.make_body_vmd(last_frame=last_frame, include_morphs=include_morphs, seed=seed)


def make_custom_cp932_body_vmd(last_frame: int = 4) -> bytes:
    """Build a valid body VMD whose model_name uses CP932 characters outside _SJIS_TABLE."""
    spec = vfg.VmdSpec(
        model_name="咲季モデル",
        bones=[
            vfg.BoneKey(name="全ての親", frame=0),
            vfg.BoneKey(name="センター", frame=0),
            vfg.BoneKey(name="上半身", frame=0),
            vfg.BoneKey(name="全ての親", frame=last_frame),
            vfg.BoneKey(name="センター", frame=last_frame, pos=(0.1, 0.25, -0.05)),
            vfg.BoneKey(name="上半身", frame=last_frame),
        ],
    )
    return vfg.build_vmd_bytes(spec)


def slot_dir(track_dir: Path, slot_1_based: int) -> Path:
    return track_dir if slot_1_based <= 1 else (track_dir / "positions" / f"slot_{slot_1_based}")


def expected_safe_track_name(title: str) -> str:
    """Mirror ImportPipeline.SafeTrackName for non-ASCII title verification."""
    cleaned_chars = []
    for ch in title:
        if ch in '<>:"/\\|?*' or ord(ch) < 0x20:
            cleaned_chars.append("_")
        else:
            cleaned_chars.append(ch)
    name = "".join(cleaned_chars).strip(" .")
    if len(name) > 100:
        name = name[:100]
    if not name:
        name = "Imported motion"
    stem = name.split(".")[0].upper()
    if stem in {"CON", "PRN", "AUX", "NUL"} or (
        len(stem) == 4 and (stem.startswith("COM") or stem.startswith("LPT")) and "1" <= stem[3] <= "9"
    ):
        name = "_" + name
    if all(0x20 <= ord(c) <= 0x7E for c in name):
        return name
    ascii_part = "".join(c if 0x20 <= ord(c) <= 0x7E else "_" for c in name).strip(" _.")
    if not ascii_part:
        ascii_part = "Imported"
    digest = hashlib.sha256(title.encode("utf-8")).hexdigest().lower()[:8]
    return f"{ascii_part}-{digest}"
