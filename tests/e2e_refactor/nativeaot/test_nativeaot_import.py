"""Opaque-box E2E tests for NativeAOT Group-Song Import (F2) and Selective/Zero-Rebake Edit (F3)."""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import sys
from typing import Any, Callable

_HERE = Path(__file__).resolve().parent
if str(_HERE) not in sys.path:
    sys.path.insert(0, str(_HERE))

import nativeaot_fixtures as nf  # noqa: E402
import vmd_fixture_gen as vfg  # noqa: E402


class TestFailure(AssertionError):
    pass


def check(cond: bool, msg: str) -> None:
    if not cond:
        raise TestFailure(msg)


def check_eq(actual: Any, expected: Any, label: str) -> None:
    if actual != expected:
        raise TestFailure(f"{label}: expected {expected!r}, got {actual!r}")


def check_near(actual: float, expected: float, eps: float, label: str) -> None:
    if not (math.isfinite(actual) and math.isfinite(expected) and abs(actual - expected) <= eps):
        raise TestFailure(f"{label}: expected {expected!r} +- {eps}, got {actual!r}")


class Capabilities:
    def __init__(self, dll_path: Path, work_dir: Path) -> None:
        self.dll_path = dll_path
        self.client: nf.NativeAotClient | None = None
        self.abi_version: int = 0
        self.cap_f2: bool = False
        self.cap_f3: bool = False
        self.reason: str = ""
        self._probe(work_dir)

    def _probe(self, work_dir: Path) -> None:
        if not self.dll_path.is_file():
            self.reason = f"Missing NativeAOT DLL: {self.dll_path}"
            return
        try:
            self.client = nf.NativeAotClient(self.dll_path)
            self.abi_version = self.client.abi_version()
        except Exception as exc:
            self.reason = f"Failed to load NativeAOT DLL: {exc}"
            return

        if self.abi_version < 4:
            self.reason = f"NativeAOT ABI version {self.abi_version} < 4"
            return

        probe_root = nf.reset_dir(work_dir / "_cap_probe")
        v1 = nf.write_vmd(probe_root / "in" / "s1.vmd", nf.make_distinct_body_vmd(4, seed=1))
        v2 = nf.write_vmd(probe_root / "in" / "s2.vmd", nf.make_distinct_body_vmd(6, seed=2))
        lib_dir = probe_root / "lib"
        lib_dir.mkdir(parents=True, exist_ok=True)

        rc, resp, raw = self.client.run_import(
            {
                "motions_root": str(lib_dir),
                "title": "CapProbeTrack",
                "positions": [
                    {"slot": 1, "motion_vmd": str(v1), "disable_morph": True},
                    {"slot": 2, "motion_vmd": str(v2), "disable_morph": True, "x": 1.5, "yaw_degrees": 30.0},
                ],
            }
        )
        if rc != 0 or not resp or not resp.get("ok"):
            self.reason = f"Pre-M1 NativeAOT DLL does not support group positions[] (rc={rc}, raw={raw})"
            return

        track_dir = Path(resp["dir"])
        info_path = track_dir / "info.json"
        slot2_clip = track_dir / "positions" / "slot_2" / "motion.gkclip"
        if not info_path.is_file() or not slot2_clip.is_file():
            self.reason = "Pre-M1 NativeAOT DLL did not emit positions/slot_2/motion.gkclip and info.json"
            return

        info = nf.read_json(info_path)
        if not (
            info.get("is_group") is True
            and info.get("position_count") == 2
            and isinstance(info.get("positions"), list)
            and len(info["positions"]) == 2
        ):
            self.reason = "Pre-M1 NativeAOT info.json missing M1 is_group/position_count/positions[]"
            return
        self.cap_f2 = True

        sha1_before = nf.sha256_file(track_dir / "motion.gkclip")
        sha2_before = nf.sha256_file(slot2_clip)
        rc_edit, resp_edit, raw_edit = self.client.run_import(
            {
                "edit_dir": str(track_dir),
                "positions": [
                    {"slot": 1, "source_slot": 1, "preserve_existing": True},
                    {"slot": 2, "source_slot": 2, "preserve_existing": True, "x": 2.5, "yaw_degrees": 45.0},
                ],
            }
        )
        if rc_edit != 0 or not resp_edit or not resp_edit.get("ok"):
            self.reason = f"Pre-M1 NativeAOT DLL does not support group edit (rc={rc_edit}, raw={raw_edit})"
            return
        info2 = nf.read_json(info_path)
        sha1_after = nf.sha256_file(track_dir / "motion.gkclip")
        sha2_after = nf.sha256_file(slot2_clip)
        if (
            sha1_before == sha1_after
            and sha2_before == sha2_after
            and len(info2.get("positions", [])) == 2
            and abs(float(info2["positions"][1].get("x", 0.0)) - 2.5) < 1e-4
        ):
            self.cap_f3 = True
        else:
            self.reason = "Pre-M1 NativeAOT DLL failed zero-rebake placement edit probe"


# =============================================================================
# F2 Tier 1 Tests (6 tests >= 5)
# =============================================================================

def test_t1_f2_01_two_slot_layout(cap: Capabilities, work: Path) -> str:
    v1 = nf.write_vmd(work / "in" / "p1.vmd", nf.make_distinct_body_vmd(4, seed=1))
    v2 = nf.write_vmd(work / "in" / "p2.vmd", nf.make_distinct_body_vmd(6, seed=2))
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)

    rc, resp, raw = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "TwoSlotGroup",
            "positions": [
                {"slot": 1, "motion_vmd": str(v1), "disable_morph": True},
                {"slot": 2, "motion_vmd": str(v2), "disable_morph": True, "x": -1.25, "z": 0.5, "yaw_degrees": 15.0},
            ],
        }
    )
    check_eq(rc, 0, f"rc ({raw})")
    check(resp is not None and resp.get("ok") is True, f"ok ({raw})")
    track = Path(resp["dir"])
    check((track / "motion.gkclip").is_file(), "slot 1 root motion.gkclip")
    check((track / "source.vmd").is_file(), "slot 1 root source.vmd")
    check((track / "motion.morph_report.json").is_file(), "slot 1 morph_report")
    s2 = track / "positions" / "slot_2"
    check((s2 / "motion.gkclip").is_file(), "slot 2 motion.gkclip")
    check((s2 / "source.vmd").is_file(), "slot 2 source.vmd")
    check((s2 / "motion.morph_report.json").is_file(), "slot 2 morph_report")
    check(not (track / "positions" / "slot_1").exists(), "slot_1 dir must not exist")

    info = nf.read_json(track / "info.json")
    check_eq(info.get("is_group"), True, "is_group")
    check_eq(info.get("position_count"), 2, "position_count")
    check_eq(info.get("import_backend"), "csharp", "import_backend")
    pos = info.get("positions", [])
    check_eq(len(pos), 2, "len(positions)")
    check_eq(pos[0]["slot"], 1, "pos[0].slot")
    check_eq(pos[1]["slot"], 2, "pos[1].slot")
    check_eq(pos[0]["source_vmd_sha256"], nf.sha256_file(v1), "slot 1 sha256")
    check_eq(pos[1]["source_vmd_sha256"], nf.sha256_file(v2), "slot 2 sha256")
    return "2-slot group layout and info.json verified"


def test_t1_f2_02_five_slot_shared_camera_audio(cap: Capabilities, work: Path) -> str:
    in_dir = work / "in"
    cam = nf.write_vmd(in_dir / "shared_cam.vmd", vfg.make_camera_vmd(last_frame=10, fov=40))
    wav = vfg.write_pcm_wav(work / "audio" / "bgm.wav", duration_seconds=0.8)
    morph = nf.write_vmd(work / "morph" / "face.vmd", vfg.make_morph_vmd(last_frame=8))
    positions = []
    vmds = []
    for k in range(1, 6):
        v = nf.write_vmd(in_dir / f"idol_{k}.vmd", nf.make_distinct_body_vmd(4 + k * 4, seed=k, include_morphs=(k == 1)))
        vmds.append(v)
        entry: dict[str, Any] = {
            "slot": k,
            "motion_vmd": str(v),
            "x": float(k - 1) * 0.75,
            "yaw_degrees": float(k - 1) * 10.0,
        }
        if k == 3:
            entry["morph_vmd"] = str(morph)
        elif k in (2, 4):
            entry["disable_morph"] = True
        positions.append(entry)

    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)
    rc, resp, raw = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "FiveIdolConcert",
            "camera_vmd": str(cam),
            "audio": str(wav),
            "positions": positions,
        }
    )
    check_eq(rc, 0, f"rc ({raw})")
    check(resp is not None and resp.get("ok") is True, f"ok ({raw})")
    track = Path(resp["dir"])
    check((track / "camera.gkcamera").is_file(), "root camera.gkcamera")
    check((track / "source_camera.vmd").is_file(), "root source_camera.vmd")
    check((track / "song.wav").is_file(), "root song.wav")

    for k in range(2, 6):
        sk = track / "positions" / f"slot_{k}"
        check((sk / "motion.gkclip").is_file(), f"slot_{k}/motion.gkclip")
        check(not (sk / "camera.gkcamera").exists(), f"no camera.gkcamera in slot_{k}")
        check(not (sk / "source_camera.vmd").exists(), f"no source_camera.vmd in slot_{k}")
        check(not (sk / "song.wav").exists(), f"no song.wav in slot_{k}")

    info = nf.read_json(track / "info.json")
    check_eq(info.get("is_group"), True, "is_group")
    check_eq(info.get("position_count"), 5, "position_count")
    check_eq(info.get("has_camera"), True, "has_camera")
    check_eq(info.get("has_facial"), True, "has_facial")
    check_eq(info.get("audio_file"), "song.wav", "audio_file")
    pos = info["positions"]
    check_eq(len(pos), 5, "len(positions)")
    check_eq(pos[0]["has_facial"], True, "slot 1 embedded facial")
    check_eq(pos[1]["has_facial"], False, "slot 2 disabled facial")
    check_eq(pos[2]["has_facial"], True, "slot 3 standalone morph facial")
    check_eq(pos[3]["has_facial"], False, "slot 4 disabled facial")
    check(info["total_frames"] == max(p["frames"] for p in pos), "total_frames == max(slot frames)")
    check(pos[4]["frames"] > pos[0]["frames"], "slot 5 has more frames than slot 1")
    return "5-slot layout, shared root camera/audio, mixed morphs verified"


def test_t1_f2_03_filename_sort_and_slot_ordering(cap: Capabilities, work: Path) -> str:
    in_dir = work / "in"
    unsorted_files = [
        nf.write_vmd(in_dir / "03_kotone.vmd", nf.make_distinct_body_vmd(16, seed=3)),
        nf.write_vmd(in_dir / "01_saki.vmd", nf.make_distinct_body_vmd(4, seed=1)),
        nf.write_vmd(in_dir / "02_temari.vmd", nf.make_distinct_body_vmd(10, seed=2)),
    ]
    sorted_files = sorted(unsorted_files, key=lambda p: p.name.lower())
    check_eq([p.name for p in sorted_files], ["01_saki.vmd", "02_temari.vmd", "03_kotone.vmd"], "filename sort")

    # Submit out-of-order slot indices [3, 1, 2] to verify ImportPipeline sorts by slot integer
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)
    rc, resp, raw = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "SortedSlots",
            "positions": [
                {"slot": 3, "motion_vmd": str(sorted_files[2]), "disable_morph": True, "x": 2.0},
                {"slot": 1, "motion_vmd": str(sorted_files[0]), "disable_morph": True},
                {"slot": 2, "motion_vmd": str(sorted_files[1]), "disable_morph": True, "x": 1.0},
            ],
        }
    )
    check_eq(rc, 0, f"rc ({raw})")
    check(resp is not None and resp.get("ok") is True, f"ok ({raw})")
    track = Path(resp["dir"])
    info = nf.read_json(track / "info.json")
    pos = info["positions"]
    check_eq([p["slot"] for p in pos], [1, 2, 3], "sorted slot indices")
    for idx, p in enumerate(pos):
        check_eq(p["source_vmd_sha256"], nf.sha256_file(sorted_files[idx]), f"slot {idx+1} sha256")
    check(pos[0]["frames"] < pos[1]["frames"] < pos[2]["frames"], "frame counts follow assigned VMDs")
    return "filename sort + slot-index sorting verified"


def test_t1_f2_04_slot1_origin_clamp_and_slot2_coords(cap: Capabilities, work: Path) -> str:
    in_dir = work / "in"
    v1 = nf.write_vmd(in_dir / "a.vmd", nf.make_distinct_body_vmd(4, seed=1))
    v2 = nf.write_vmd(in_dir / "b.vmd", nf.make_distinct_body_vmd(4, seed=2))
    v3 = nf.write_vmd(in_dir / "c.vmd", nf.make_distinct_body_vmd(4, seed=3))
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)
    rc, resp, raw = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "PlacementClamp",
            "positions": [
                {"slot": 1, "motion_vmd": str(v1), "disable_morph": True, "x": 9.5, "y": -2.0, "z": 3.25, "yaw_degrees": 45.0},
                {"slot": 2, "motion_vmd": str(v2), "disable_morph": True, "x": -0.0, "y": 0.25, "z": -1.5, "yaw_degrees": -90.5},
                {"slot": 3, "motion_vmd": str(v3), "disable_morph": True, "x": 1.75, "y": 0.0, "z": 2.25, "yaw_degrees": 180.0},
            ],
        }
    )
    check_eq(rc, 0, f"rc ({raw})")
    check(resp is not None and resp.get("ok") is True, f"ok ({raw})")
    track = Path(resp["dir"])
    info_raw = (track / "info.json").read_text(encoding="utf-8")
    check("-0," not in info_raw and "-0\n" not in info_raw, "negative zero cleaned")
    pos = json.loads(info_raw)["positions"]
    check_near(pos[0]["x"], 0.0, 1e-6, "slot1 x clamped")
    check_near(pos[0]["y"], 0.0, 1e-6, "slot1 y clamped")
    check_near(pos[0]["z"], 0.0, 1e-6, "slot1 z clamped")
    check_near(pos[0]["yaw_degrees"], 0.0, 1e-6, "slot1 yaw clamped")
    check_near(pos[1]["x"], 0.0, 1e-6, "slot2 x")
    check_near(pos[1]["y"], 0.25, 1e-6, "slot2 y")
    check_near(pos[1]["z"], -1.5, 1e-6, "slot2 z")
    check_near(pos[1]["yaw_degrees"], -90.5, 1e-6, "slot2 yaw")
    check_near(pos[2]["x"], 1.75, 1e-6, "slot3 x")
    check_near(pos[2]["z"], 2.25, 1e-6, "slot3 z")
    check_near(pos[2]["yaw_degrees"], 180.0, 1e-6, "slot3 yaw")
    return "slot 1 clamped to origin; slot 2+ coords preserved"


def test_t1_f2_05_per_slot_source_fps_and_slot1_inheritance(cap: Capabilities, work: Path) -> str:
    in_dir = work / "in"
    v1 = nf.write_vmd(in_dir / "s1.vmd", nf.make_distinct_body_vmd(6, seed=1))
    v2 = nf.write_vmd(in_dir / "s2.vmd", nf.make_distinct_body_vmd(12, seed=2))
    v3 = nf.write_vmd(in_dir / "s3.vmd", nf.make_distinct_body_vmd(10, seed=3))
    m1 = nf.write_vmd(work / "morph" / "m1.vmd", vfg.make_morph_vmd(last_frame=6))
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)

    # Omit slot 1 in positions[] so it is synthesized from top-level motion_vmd + morph_vmd
    rc, resp, raw = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "PerSlotFpsInherit",
            "motion_vmd": str(v1),
            "morph_vmd": str(m1),
            "source_fps": 25.0,
            "positions": [
                {"slot": 2, "motion_vmd": str(v2), "source_fps": 60.0, "disable_morph": True, "x": 1.0},
                {"slot": 3, "motion_vmd": str(v3), "source_fps": 30.0, "disable_morph": True, "x": 2.0},
            ],
        }
    )
    check_eq(rc, 0, f"rc ({raw})")
    check(resp is not None and resp.get("ok") is True, f"ok ({raw})")
    track = Path(resp["dir"])
    info = nf.read_json(track / "info.json")
    pos = info["positions"]
    check_eq(len(pos), 3, "len(positions)")
    check_near(pos[0]["source_fps"], 25.0, 1e-4, "slot 1 inherited source_fps=25")
    check_eq(pos[0]["has_facial"], True, "slot 1 inherited morph_vmd")
    check_near(pos[1]["source_fps"], 60.0, 1e-4, "slot 2 source_fps=60")
    check_eq(pos[1]["has_facial"], False, "slot 2 disable_morph")
    check_near(pos[2]["source_fps"], 30.0, 1e-4, "slot 3 source_fps=30")
    return "per-slot source_fps (25/60/30) and top-level slot 1 inheritance verified"


def test_t1_f2_06_solo_backward_compatibility(cap: Capabilities, work: Path) -> str:
    v1 = nf.write_vmd(work / "in" / "solo.vmd", nf.make_distinct_body_vmd(8, seed=7, include_morphs=True))
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)
    rc, resp, raw = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "SoloCompat",
            "motion_vmd": str(v1),
        }
    )
    check_eq(rc, 0, f"rc ({raw})")
    check(resp is not None and resp.get("ok") is True, f"ok ({raw})")
    track = Path(resp["dir"])
    check((track / "motion.gkclip").is_file(), "root motion.gkclip")
    check(not (track / "positions").exists(), "solo must not create positions/ dir")
    info = nf.read_json(track / "info.json")
    check_eq(info.get("is_group"), False, "is_group=false")
    check_eq(info.get("position_count"), 1, "position_count=1")
    pos = info.get("positions", [])
    check_eq(len(pos), 1, "positions length 1")
    check_eq(pos[0]["slot"], 1, "slot=1")
    check_near(pos[0]["x"], 0.0, 1e-6, "x=0")
    return "solo backward compatibility verified"


# =============================================================================
# F2 Tier 2 Tests (6 tests >= 5)
# =============================================================================

def test_t2_f2_01_c_abi_bounds_and_candidate_b(cap: Capabilities, work: Path) -> str:
    check_eq(cap.abi_version, 4, "GkVmdImporter_AbiVersion")

    # InvalidArgument (2): null request, null response, capacity <= 0
    rc_null_req, _ = cap.client.run_raw(None, 256, resp_is_null=False)
    check_eq(rc_null_req, 2, "null request -> InvalidArgument (2)")
    rc_null_resp, _ = cap.client.run_raw(b"{}", 256, resp_is_null=True)
    check_eq(rc_null_resp, 2, "null response -> InvalidArgument (2)")
    rc_zero_cap, _ = cap.client.run_raw(b"{}", 0, resp_is_null=False)
    check_eq(rc_zero_cap, 2, "zero capacity -> InvalidArgument (2)")

    # BufferTooSmall (3): tiny capacity 16
    v1 = nf.write_vmd(work / "in" / "abi.vmd", nf.make_distinct_body_vmd(4, seed=1))
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)
    rc_small, _, _ = cap.client.run_import(
        {"motions_root": str(lib), "title": "AbiSmallBuf", "motion_vmd": str(v1)},
        resp_cap=16,
    )
    check_eq(rc_small, 3, "tiny responseCapacity -> BufferTooSmall (3)")

    # Verify candidate_b_tree on normal import
    rc_ok, resp_ok, raw = cap.client.run_import(
        {"motions_root": str(lib), "title": "AbiOk", "motion_vmd": str(v1)}
    )
    check_eq(rc_ok, 0, f"normal rc ({raw})")
    tree = str(resp_ok.get("candidate_b_tree", ""))
    check_eq(len(tree), 64, "candidate_b_tree 64-hex length")
    check(all(c in "0123456789abcdef" for c in tree.lower()), "candidate_b_tree hex")
    return "ABI v4, InvalidArgument(2), BufferTooSmall(3), candidate_b_tree verified"


def test_t2_f2_02_invalid_positions_structure(cap: Capabilities, work: Path) -> str:
    in_dir = work / "in"
    v1 = nf.write_vmd(in_dir / "v1.vmd", nf.make_distinct_body_vmd(4, seed=1))
    v2 = nf.write_vmd(in_dir / "v2.vmd", nf.make_distinct_body_vmd(4, seed=2))
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)

    bad_requests = [
        ("empty_positions", {"motions_root": str(lib), "title": "BadEmpty", "motion_vmd": str(v1), "positions": []}),
        ("dup_slot", {"motions_root": str(lib), "title": "BadDup", "positions": [{"slot": 1, "motion_vmd": str(v1)}, {"slot": 1, "motion_vmd": str(v2)}]}),
        ("non_contiguous", {"motions_root": str(lib), "title": "BadGap", "positions": [{"slot": 1, "motion_vmd": str(v1)}, {"slot": 3, "motion_vmd": str(v2)}]}),
        ("slot_zero", {"motions_root": str(lib), "title": "BadZero", "positions": [{"slot": 0, "motion_vmd": str(v1)}, {"slot": 2, "motion_vmd": str(v2)}]}),
        ("missing_slot1", {"motions_root": str(lib), "title": "BadNoSlot1", "positions": [{"slot": 2, "motion_vmd": str(v2)}]}),
    ]
    for label, req in bad_requests:
        rc, resp, raw = cap.client.run_import(req)
        check_eq(rc, 1, f"{label} rc ({raw})")
        check(resp is not None and resp.get("ok") is False, f"{label} ok=false ({raw})")
    check_eq(list(lib.iterdir()), [], "no partial directories created in motions_root")
    return "empty/duplicate/non-contiguous/slot<1/missing-slot1 all rejected atomically"


def test_t2_f2_03_invalid_fps_and_non_finite_coords(cap: Capabilities, work: Path) -> str:
    in_dir = work / "in"
    v1 = nf.write_vmd(in_dir / "v1.vmd", nf.make_distinct_body_vmd(4, seed=1))
    v2 = nf.write_vmd(in_dir / "v2.vmd", nf.make_distinct_body_vmd(4, seed=2))
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)

    # Invalid source_fps = 42 on slot 2
    rc_fps, resp_fps, raw_fps = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "BadSlotFps",
            "positions": [
                {"slot": 1, "motion_vmd": str(v1)},
                {"slot": 2, "motion_vmd": str(v2), "source_fps": 42.0},
            ],
        }
    )
    check_eq(rc_fps, 1, f"bad slot fps rc ({raw_fps})")
    check(resp_fps is not None and resp_fps.get("code") == "invalid_source_fps", f"code=invalid_source_fps ({raw_fps})")

    # Non-finite / non-numeric x, non-array positions, negative source_fps
    bad_raw_payloads = [
        {"motions_root": str(lib), "title": "BadStrX", "positions": [{"slot": 1, "motion_vmd": str(v1)}, {"slot": 2, "motion_vmd": str(v2), "x": "NaN"}]},
        {"motions_root": str(lib), "title": "BadPosType", "motion_vmd": str(v1), "positions": "not-an-array"},
        {"motions_root": str(lib), "title": "NegFps", "positions": [{"slot": 1, "motion_vmd": str(v1), "source_fps": -30.0}]},
    ]
    for idx, payload in enumerate(bad_raw_payloads):
        rc, resp, raw = cap.client.run_import(payload)
        check_eq(rc, 1, f"bad payload {idx} rc ({raw})")
        check(resp is not None and resp.get("ok") is False, f"bad payload {idx} ok=false")

    # Literal NaN token (non-standard JSON)
    nan_bytes = json.dumps({"motions_root": str(lib), "title": "LitNaN", "positions": [{"slot": 1, "motion_vmd": str(v1), "x": 0}]}).replace('"x": 0', '"x": NaN').encode("utf-8")
    rc_nan, resp_nan, raw_nan = cap.client.run_json_bytes(nan_bytes)
    check_eq(rc_nan, 1, f"literal NaN rc ({raw_nan})")
    check(resp_nan is not None and resp_nan.get("ok") is False, "literal NaN ok=false")
    check_eq(list(lib.iterdir()), [], "motions_root remained empty")
    return "invalid_source_fps and non-finite/bad-type coords rejected atomically"


def test_t2_f2_04_corrupt_slot2_vmd_atomic_failure(cap: Capabilities, work: Path) -> str:
    in_dir = work / "in"
    v1 = nf.write_vmd(in_dir / "good1.vmd", nf.make_distinct_body_vmd(4, seed=1))
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)

    bad_vmds = {
        "corrupt_magic": vfg.make_negative_vmd(vfg.NegativeVmdKind.BAD_MAGIC),
        "zero_quat": vfg.make_negative_vmd(vfg.NegativeVmdKind.ZERO_QUATERNION_BONE),
        "eye_only": vfg.make_eye_only_vmd(2),
        "empty_vmd": vfg.make_empty_vmd(),
    }
    for label, raw_vmd in bad_vmds.items():
        v2 = nf.write_vmd(in_dir / f"{label}.vmd", raw_vmd)
        rc, resp, raw = cap.client.run_import(
            {
                "motions_root": str(lib),
                "title": f"Fail_{label}",
                "positions": [
                    {"slot": 1, "motion_vmd": str(v1), "disable_morph": True},
                    {"slot": 2, "motion_vmd": str(v2), "disable_morph": True},
                ],
            }
        )
        check_eq(rc, 1, f"{label} rc ({raw})")
        check(resp is not None and resp.get("ok") is False, f"{label} ok=false")
        check(not (lib / f"Fail_{label}").exists(), f"{label} did not leave partial target")
    return "corrupt/zero-quat/eye-only/empty slot 2 VMDs rejected atomically"


def test_t2_f2_05_name_collision_and_target_leg_geometry(cap: Capabilities, work: Path) -> str:
    in_dir = work / "in"
    v1 = nf.write_vmd(in_dir / "slot1" / "v1.vmd", nf.make_distinct_body_vmd(4, seed=1))
    v2 = nf.write_vmd(in_dir / "slot2" / "v2.vmd", nf.make_distinct_body_vmd(4, seed=2))
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)

    # First import Col_Track
    rc0, resp0, raw0 = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "Col_Track",
            "positions": [{"slot": 1, "motion_vmd": str(v1)}, {"slot": 2, "motion_vmd": str(v2)}],
        }
    )
    check_eq(rc0, 0, f"initial import ({raw0})")
    existing_dir = Path(resp0["dir"])
    snap_before = nf.snapshot_tree(existing_dir)

    # Attempt normalized collision: "Col:Track" -> SafeTrackName == "Col_Track"
    rc_col, resp_col, raw_col = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "Col:Track",
            "positions": [{"slot": 1, "motion_vmd": str(v1)}, {"slot": 2, "motion_vmd": str(v2)}],
        }
    )
    check_eq(rc_col, 1, f"collision rc ({raw_col})")
    check(resp_col is not None and resp_col.get("ok") is False, "collision ok=false")
    check_eq(nf.snapshot_tree(existing_dir), snap_before, "existing track untouched on collision")

    # Bind target_leg_geometry.json to slot 2 VMD
    nf.write_target_leg_geometry(v2, bind_to_vmd=True)
    rc_geom, resp_geom, raw_geom = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "GeomBlockedTrack",
            "positions": [{"slot": 1, "motion_vmd": str(v1)}, {"slot": 2, "motion_vmd": str(v2)}],
        }
    )
    check_eq(rc_geom, 1, f"geom rc ({raw_geom})")
    check(resp_geom is not None and resp_geom.get("code") == "unsupported_target_geometry", f"code=unsupported_target_geometry ({raw_geom})")
    check(not (lib / "GeomBlockedTrack").exists(), "geom failure did not create target dir")
    return "SafeTrackName collision and slot-2 target_leg_geometry.json rejection verified"


def test_t2_f2_06_unicode_cjk_utf8_roundtrip(cap: Capabilities, work: Path) -> str:
    cjk_in = work / "入力_咲季"
    v1 = nf.write_vmd(cjk_in / "01_咲季.vmd", nf.make_custom_cp932_body_vmd(last_frame=6))
    v2 = nf.write_vmd(cjk_in / "02_手毬.vmd", nf.make_distinct_body_vmd(8, seed=5))
    lib = work / "曲庫_テスト"
    lib.mkdir(parents=True, exist_ok=True)
    title = "团曲_咲季_テスト"
    rc, resp, raw = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": title,
            "subtitle": "初星学園",
            "positions": [
                {"slot": 1, "motion_vmd": str(v1), "disable_morph": True},
                {"slot": 2, "motion_vmd": str(v2), "disable_morph": True, "x": 1.25},
            ],
        }
    )
    check_eq(rc, 0, f"cjk rc ({raw})")
    check(resp is not None and resp.get("ok") is True, f"cjk ok ({raw})")
    track = Path(resp["dir"])
    check_eq(track.name, nf.expected_safe_track_name(title), "SafeTrackName ASCII+SHA8")
    check((track / "motion.gkclip").is_file(), "root clip exists")
    check((track / "positions" / "slot_2" / "motion.gkclip").is_file(), "slot 2 clip exists")
    info = nf.read_json(track / "info.json")
    check_eq(info.get("title"), title, "UTF-8 title round-trip")
    check_eq(info.get("subtitle"), "初星学園", "UTF-8 subtitle round-trip")
    return "UTF-8 CJK title/paths + SafeTrackName round-trip verified"


# =============================================================================
# F3 Tier 1 Tests (6 tests >= 5)
# =============================================================================

def _create_base_group_track(cap: Capabilities, work: Path, slot_count: int = 3, with_camera_audio: bool = True) -> Path:
    in_dir = work / "init_in"
    positions = []
    for k in range(1, slot_count + 1):
        v = nf.write_vmd(in_dir / f"s{k}.vmd", nf.make_distinct_body_vmd(4 + k * 2, seed=k, include_morphs=(k == 1)))
        positions.append(
            {
                "slot": k,
                "motion_vmd": str(v),
                "x": float(k - 1) * 1.0,
                "y": 0.0,
                "z": float(k - 1) * -0.5,
                "yaw_degrees": float(k - 1) * 15.0,
            }
        )
    req: dict[str, Any] = {
        "motions_root": str(work / "lib"),
        "title": "BaseGroupTrack",
        "positions": positions,
    }
    (work / "lib").mkdir(parents=True, exist_ok=True)
    if with_camera_audio:
        req["camera_vmd"] = str(nf.write_vmd(in_dir / "cam.vmd", vfg.make_camera_vmd(last_frame=10, fov=45)))
        req["audio"] = str(vfg.write_pcm_wav(work / "init_audio" / "song.wav", duration_seconds=0.6))
    rc, resp, raw = cap.client.run_import(req)
    check_eq(rc, 0, f"base import rc ({raw})")
    check(resp is not None and resp.get("ok") is True, f"base import ok ({raw})")
    return Path(resp["dir"])


def test_t1_f3_01_zero_rebake_placement_and_metadata_edit(cap: Capabilities, work: Path) -> str:
    track = _create_base_group_track(cap, work, slot_count=3, with_camera_audio=True)

    # Delete source.vmd across ALL slots beforehand to prove evaluator does NOT run!
    for k in range(1, 4):
        src = nf.slot_dir(track, k) / "source.vmd"
        if src.is_file():
            src.unlink()

    before_hashes = {
        "c1": nf.sha256_file(track / "motion.gkclip"),
        "r1": nf.sha256_file(track / "motion.morph_report.json"),
        "c2": nf.sha256_file(track / "positions" / "slot_2" / "motion.gkclip"),
        "r2": nf.sha256_file(track / "positions" / "slot_2" / "motion.morph_report.json"),
        "c3": nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip"),
        "r3": nf.sha256_file(track / "positions" / "slot_3" / "motion.morph_report.json"),
        "cam": nf.sha256_file(track / "camera.gkcamera"),
        "wav": nf.sha256_file(track / "song.wav"),
    }

    rc, resp, raw = cap.client.run_import(
        {
            "edit_dir": str(track),
            "title": "EditedTitleZeroRebake",
            "subtitle": "UpdatedSubtitle",
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "preserve_existing": True, "x": 3.5, "y": 0.25, "z": -1.75, "yaw_degrees": 60.0},
                {"slot": 3, "source_slot": 3, "preserve_existing": True, "x": -2.0, "y": 0.0, "z": 1.0, "yaw_degrees": -45.0},
            ],
        }
    )
    check_eq(rc, 0, f"zero-rebake edit rc ({raw})")
    check(resp is not None and resp.get("ok") is True, f"zero-rebake edit ok ({raw})")

    check_eq(nf.sha256_file(track / "motion.gkclip"), before_hashes["c1"], "slot 1 clip unchanged")
    check_eq(nf.sha256_file(track / "motion.morph_report.json"), before_hashes["r1"], "slot 1 report unchanged")
    check_eq(nf.sha256_file(track / "positions" / "slot_2" / "motion.gkclip"), before_hashes["c2"], "slot 2 clip unchanged")
    check_eq(nf.sha256_file(track / "positions" / "slot_2" / "motion.morph_report.json"), before_hashes["r2"], "slot 2 report unchanged")
    check_eq(nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip"), before_hashes["c3"], "slot 3 clip unchanged")
    check_eq(nf.sha256_file(track / "positions" / "slot_3" / "motion.morph_report.json"), before_hashes["r3"], "slot 3 report unchanged")
    check_eq(nf.sha256_file(track / "camera.gkcamera"), before_hashes["cam"], "camera unchanged")
    check_eq(nf.sha256_file(track / "song.wav"), before_hashes["wav"], "audio unchanged")

    info = nf.read_json(track / "info.json")
    check_eq(info.get("title"), "EditedTitleZeroRebake", "title updated")
    check_eq(info.get("subtitle"), "UpdatedSubtitle", "subtitle updated")
    check_near(info["positions"][1]["x"], 3.5, 1e-6, "slot 2 x updated")
    check_near(info["positions"][1]["yaw_degrees"], 60.0, 1e-6, "slot 2 yaw updated")
    check_near(info["positions"][2]["x"], -2.0, 1e-6, "slot 3 x updated")
    check_near(info["positions"][2]["yaw_degrees"], -45.0, 1e-6, "slot 3 yaw updated")
    return "zero-rebake placement + metadata edit succeeded with deleted source.vmd and identical clip hashes"


def test_t1_f3_02_selective_slot2_vmd_swap_preserves_others_and_sidecars(cap: Capabilities, work: Path) -> str:
    track = _create_base_group_track(cap, work, slot_count=3, with_camera_audio=False)
    sidecars = {k: nf.write_slot_sidecars(nf.slot_dir(track, k), f"slot_{k}_orig") for k in range(1, 4)}

    c1_before = nf.sha256_file(track / "motion.gkclip")
    s1_before = nf.sha256_file(track / "source.vmd")
    r1_before = nf.sha256_file(track / "motion.morph_report.json")
    c2_before = nf.sha256_file(track / "positions" / "slot_2" / "motion.gkclip")
    c3_before = nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip")
    s3_before = nf.sha256_file(track / "positions" / "slot_3" / "source.vmd")
    r3_before = nf.sha256_file(track / "positions" / "slot_3" / "motion.morph_report.json")

    new_v2 = nf.write_vmd(work / "swap_in" / "new_s2.vmd", nf.make_distinct_body_vmd(36, seed=9))
    rc, resp, raw = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "motion_vmd": str(new_v2), "x": 1.0, "z": -0.5, "yaw_degrees": 15.0},
                {"slot": 3, "source_slot": 3, "preserve_existing": True, "x": 2.0, "z": -1.0, "yaw_degrees": 30.0},
            ],
        }
    )
    check_eq(rc, 0, f"selective swap rc ({raw})")
    check(resp is not None and resp.get("ok") is True, f"selective swap ok ({raw})")

    check_eq(nf.sha256_file(track / "motion.gkclip"), c1_before, "slot 1 clip preserved")
    check_eq(nf.sha256_file(track / "source.vmd"), s1_before, "slot 1 source.vmd preserved")
    check_eq(nf.sha256_file(track / "motion.morph_report.json"), r1_before, "slot 1 morph report preserved")
    check_eq(nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip"), c3_before, "slot 3 clip preserved")
    check_eq(nf.sha256_file(track / "positions" / "slot_3" / "source.vmd"), s3_before, "slot 3 source.vmd preserved")
    check_eq(nf.sha256_file(track / "positions" / "slot_3" / "motion.morph_report.json"), r3_before, "slot 3 morph report preserved")

    check(nf.sha256_file(track / "positions" / "slot_2" / "motion.gkclip") != c2_before, "slot 2 clip changed")
    for k in range(1, 4):
        check_eq(nf.read_slot_sidecars(nf.slot_dir(track, k)), sidecars[k], f"slot {k} sidecars preserved")

    info = nf.read_json(track / "info.json")
    check_eq(info["positions"][1]["source_vmd_sha256"], nf.sha256_file(new_v2), "slot 2 sha256 updated")
    check_eq(info["total_frames"], info["positions"][1]["frames"], "top-level total_frames reflects longer slot 2")
    return "selective slot-2 VMD swap preserved slot 1 & 3 clips and all slot sidecars"


def test_t1_f3_03_selective_morph_edit(cap: Capabilities, work: Path) -> str:
    track = _create_base_group_track(cap, work, slot_count=3, with_camera_audio=False)
    c1_before = nf.sha256_file(track / "motion.gkclip")
    c3_before = nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip")

    morph2 = nf.write_vmd(work / "morph_in" / "m2.vmd", vfg.make_morph_vmd(last_frame=6))
    rc1, resp1, raw1 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "morph_vmd": str(morph2), "x": 1.0, "z": -0.5, "yaw_degrees": 15.0},
                {"slot": 3, "source_slot": 3, "preserve_existing": True, "x": 2.0, "z": -1.0, "yaw_degrees": 30.0},
            ],
        }
    )
    check_eq(rc1, 0, f"add morph rc ({raw1})")
    check_eq(nf.sha256_file(track / "motion.gkclip"), c1_before, "slot 1 untouched")
    check_eq(nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip"), c3_before, "slot 3 untouched")
    check((track / "positions" / "slot_2" / "source_morph.vmd").is_file(), "slot 2 source_morph.vmd created")
    info1 = nf.read_json(track / "info.json")
    check_eq(info1["positions"][1]["has_facial"], True, "slot 2 has_facial=true")

    # Clear standalone morph on slot 2
    rc2, resp2, raw2 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "clear_morph": True, "x": 1.0, "z": -0.5, "yaw_degrees": 15.0},
                {"slot": 3, "source_slot": 3, "preserve_existing": True, "x": 2.0, "z": -1.0, "yaw_degrees": 30.0},
            ],
        }
    )
    check_eq(rc2, 0, f"clear morph rc ({raw2})")
    check(not (track / "positions" / "slot_2" / "source_morph.vmd").exists(), "slot 2 source_morph.vmd removed")
    info2 = nf.read_json(track / "info.json")
    check_eq(info2["positions"][1]["has_facial"], False, "slot 2 has_facial=false after clear_morph")

    # Re-add morph then disable_morph on slot 2
    rc3, _, _ = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "morph_vmd": str(morph2), "x": 1.0, "z": -0.5, "yaw_degrees": 15.0},
                {"slot": 3, "source_slot": 3, "preserve_existing": True, "x": 2.0, "z": -1.0, "yaw_degrees": 30.0},
            ],
        }
    )
    check_eq(rc3, 0, "re-add morph rc")
    rc4, resp4, raw4 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "disable_morph": True, "x": 1.0, "z": -0.5, "yaw_degrees": 15.0},
                {"slot": 3, "source_slot": 3, "preserve_existing": True, "x": 2.0, "z": -1.0, "yaw_degrees": 30.0},
            ],
        }
    )
    check_eq(rc4, 0, f"disable morph rc ({raw4})")
    check_eq(nf.sha256_file(track / "motion.gkclip"), c1_before, "slot 1 still untouched")
    check_eq(nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip"), c3_before, "slot 3 still untouched")
    check(not (track / "positions" / "slot_2" / "source_morph.vmd").exists(), "slot 2 source_morph.vmd gone after disable")
    info4 = nf.read_json(track / "info.json")
    check_eq(info4["positions"][1]["has_facial"], False, "slot 2 has_facial=false after disable_morph")
    return "selective slot-2 morph_vmd / clear_morph / disable_morph verified"


def test_t1_f3_04_delete_middle_slot_and_renumber_sidecars(cap: Capabilities, work: Path) -> str:
    track = _create_base_group_track(cap, work, slot_count=4, with_camera_audio=False)
    sidecars = {k: nf.write_slot_sidecars(nf.slot_dir(track, k), f"orig_slot_{k}") for k in range(1, 5)}
    clip_hashes = {k: nf.sha256_file(nf.slot_dir(track, k) / "motion.gkclip") for k in range(1, 5)}

    # Delete slot 2 -> keep source_slot 1, 3, 4 mapped to slot 1, 2, 3
    rc, resp, raw = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 3, "preserve_existing": True, "x": 2.0, "z": -1.0, "yaw_degrees": 30.0},
                {"slot": 3, "source_slot": 4, "preserve_existing": True, "x": 3.0, "z": -1.5, "yaw_degrees": 45.0},
            ],
        }
    )
    check_eq(rc, 0, f"delete slot 2 rc ({raw})")
    check(resp is not None and resp.get("ok") is True, f"delete slot 2 ok ({raw})")
    check(not (track / "positions" / "slot_4").exists(), "positions/slot_4 removed")

    check_eq(nf.sha256_file(track / "motion.gkclip"), clip_hashes[1], "slot 1 clip preserved")
    check_eq(nf.sha256_file(track / "positions" / "slot_2" / "motion.gkclip"), clip_hashes[3], "old slot 3 clip moved to slot 2")
    check_eq(nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip"), clip_hashes[4], "old slot 4 clip moved to slot 3")
    check_eq(nf.read_slot_sidecars(track), sidecars[1], "slot 1 sidecars preserved")
    check_eq(nf.read_slot_sidecars(track / "positions" / "slot_2"), sidecars[3], "old slot 3 sidecars migrated to slot 2")
    check_eq(nf.read_slot_sidecars(track / "positions" / "slot_3"), sidecars[4], "old slot 4 sidecars migrated to slot 3")

    info = nf.read_json(track / "info.json")
    check_eq(info.get("is_group"), True, "is_group")
    check_eq(info.get("position_count"), 3, "position_count=3")
    check_eq([p["slot"] for p in info["positions"]], [1, 2, 3], "renumbered slots [1,2,3]")
    return "middle slot deletion + slot renumbering + per-slot sidecar migration verified"


def test_t1_f3_05_solo_to_group_and_group_to_solo(cap: Capabilities, work: Path) -> str:
    in_dir = work / "in"
    v1 = nf.write_vmd(in_dir / "solo.vmd", nf.make_distinct_body_vmd(6, seed=1))
    v2 = nf.write_vmd(in_dir / "append.vmd", nf.make_distinct_body_vmd(10, seed=2))
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)

    rc0, resp0, raw0 = cap.client.run_import({"motions_root": str(lib), "title": "ExpandShrink", "motion_vmd": str(v1)})
    check_eq(rc0, 0, f"solo import ({raw0})")
    track = Path(resp0["dir"])
    clip1_sha = nf.sha256_file(track / "motion.gkclip")
    # Delete root source.vmd to prove appending slot 2 does NOT rebake slot 1
    (track / "source.vmd").unlink()

    rc1, resp1, raw1 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 0, "motion_vmd": str(v2), "disable_morph": True, "x": 1.5, "yaw_degrees": -20.0},
            ],
        }
    )
    check_eq(rc1, 0, f"expand rc ({raw1})")
    check_eq(nf.sha256_file(track / "motion.gkclip"), clip1_sha, "slot 1 clip untouched on expand")
    check((track / "positions" / "slot_2" / "motion.gkclip").is_file(), "slot 2 created")
    info1 = nf.read_json(track / "info.json")
    check_eq(info1.get("is_group"), True, "is_group=true after expand")
    check_eq(info1.get("position_count"), 2, "position_count=2 after expand")

    # Shrink back to solo (1 slot)
    rc2, resp2, raw2 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [{"slot": 1, "source_slot": 1, "preserve_existing": True}],
        }
    )
    check_eq(rc2, 0, f"shrink rc ({raw2})")
    check_eq(nf.sha256_file(track / "motion.gkclip"), clip1_sha, "slot 1 clip untouched on shrink")
    check(not (track / "positions").exists(), "positions/ removed after shrink to solo")
    info2 = nf.read_json(track / "info.json")
    check_eq(info2.get("is_group"), False, "is_group=false after shrink")
    check_eq(info2.get("position_count"), 1, "position_count=1 after shrink")
    return "solo->group expansion and group->solo shrink verified"


def test_t1_f3_06_delete_slot1_promotes_slot2(cap: Capabilities, work: Path) -> str:
    track = _create_base_group_track(cap, work, slot_count=3, with_camera_audio=False)
    sidecars = {k: nf.write_slot_sidecars(nf.slot_dir(track, k), f"promote_{k}") for k in range(1, 4)}
    clip2_sha = nf.sha256_file(track / "positions" / "slot_2" / "motion.gkclip")
    clip3_sha = nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip")

    # Delete slot 1 -> old slot 2 becomes slot 1 (root), old slot 3 becomes slot 2
    rc, resp, raw = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 2, "preserve_existing": True, "x": 9.0, "yaw_degrees": 77.0},
                {"slot": 2, "source_slot": 3, "preserve_existing": True, "x": 2.0, "z": -1.0, "yaw_degrees": 30.0},
            ],
        }
    )
    check_eq(rc, 0, f"delete slot 1 rc ({raw})")
    check(not (track / "positions" / "slot_3").exists(), "positions/slot_3 removed")
    check_eq(nf.sha256_file(track / "motion.gkclip"), clip2_sha, "old slot 2 clip promoted to root")
    check_eq(nf.sha256_file(track / "positions" / "slot_2" / "motion.gkclip"), clip3_sha, "old slot 3 clip moved to slot 2")
    check_eq(nf.read_slot_sidecars(track), sidecars[2], "old slot 2 sidecars promoted to root")
    check_eq(nf.read_slot_sidecars(track / "positions" / "slot_2"), sidecars[3], "old slot 3 sidecars moved to slot 2")

    info = nf.read_json(track / "info.json")
    check_eq(info.get("position_count"), 2, "position_count=2")
    check_near(info["positions"][0]["x"], 0.0, 1e-6, "promoted slot 1 x normalized to 0")
    check_near(info["positions"][0]["yaw_degrees"], 0.0, 1e-6, "promoted slot 1 yaw normalized to 0")
    check_near(info["positions"][1]["x"], 2.0, 1e-6, "slot 2 x preserved")
    return "slot 1 deletion promoted slot 2 to root with origin normalization"


# =============================================================================
# F3 Tier 2 Tests (6 tests >= 5)
# =============================================================================

def test_t2_f3_01_duplicate_source_slot_atomic(cap: Capabilities, work: Path) -> str:
    track = _create_base_group_track(cap, work, slot_count=3, with_camera_audio=False)
    snap = nf.snapshot_tree(track)
    rc, resp, raw = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 1, "preserve_existing": True},
            ],
        }
    )
    check_eq(rc, 1, f"dup source_slot rc ({raw})")
    check(resp is not None and resp.get("ok") is False, "dup source_slot ok=false")
    check_eq(nf.snapshot_tree(track), snap, "track tree completely unchanged")
    return "duplicate source_slot rejected atomically"


def test_t2_f3_02_nonexistent_source_slot_atomic(cap: Capabilities, work: Path) -> str:
    track = _create_base_group_track(cap, work, slot_count=2, with_camera_audio=False)
    snap = nf.snapshot_tree(track)
    rc, resp, raw = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 99, "preserve_existing": True},
            ],
        }
    )
    check_eq(rc, 1, f"nonexistent source_slot rc ({raw})")
    check(resp is not None and resp.get("ok") is False, "nonexistent source_slot ok=false")
    check_eq(nf.snapshot_tree(track), snap, "track tree completely unchanged")
    return "nonexistent source_slot (99) rejected atomically"


def test_t2_f3_03_invalid_edit_positions_and_fps(cap: Capabilities, work: Path) -> str:
    track = _create_base_group_track(cap, work, slot_count=2, with_camera_audio=False)
    snap = nf.snapshot_tree(track)

    bad_edits = [
        {"edit_dir": str(track), "positions": []},
        {"edit_dir": str(track), "positions": [{"slot": 2, "source_slot": 1}, {"slot": 2, "source_slot": 2}]},
        {"edit_dir": str(track), "positions": [{"slot": 1, "source_slot": 1, "preserve_existing": True}, {"slot": 2, "source_slot": 2, "source_fps": 42.0}]},
    ]
    for idx, req in enumerate(bad_edits):
        rc, resp, raw = cap.client.run_import(req)
        check_eq(rc, 1, f"bad edit {idx} rc ({raw})")
        check(resp is not None and resp.get("ok") is False, f"bad edit {idx} ok=false")
        check_eq(nf.snapshot_tree(track), snap, f"track unchanged after bad edit {idx}")
    return "empty positions / duplicate slot / invalid source_fps on edit rejected atomically"


def test_t2_f3_04_missing_source_vmd_on_slot2_rebake(cap: Capabilities, work: Path) -> str:
    track = _create_base_group_track(cap, work, slot_count=2, with_camera_audio=False)
    (track / "positions" / "slot_2" / "source.vmd").unlink()
    snap = nf.snapshot_tree(track)

    rc, resp, raw = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "rebake_output": True},
            ],
        }
    )
    check_eq(rc, 1, f"missing source.vmd rebake rc ({raw})")
    check(resp is not None and resp.get("ok") is False, "missing source.vmd ok=false")
    check_eq(nf.snapshot_tree(track), snap, "track unchanged")
    return "missing slot 2 source.vmd during rebake rejected atomically"


def test_t2_f3_05_slot_target_leg_geometry_blocks_rebake_allows_placement(cap: Capabilities, work: Path) -> str:
    track = _create_base_group_track(cap, work, slot_count=2, with_camera_audio=False)
    slot2 = track / "positions" / "slot_2"
    geom_path = nf.write_target_leg_geometry(slot2 / "source.vmd", bind_to_vmd=True, dest_dir=slot2)
    snap_before = nf.snapshot_tree(track)

    # Attempting to rebake slot 2 must fail with unsupported_target_geometry
    rc_fail, resp_fail, raw_fail = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "disable_morph": True},
            ],
        }
    )
    check_eq(rc_fail, 1, f"slot2 geom rebake rc ({raw_fail})")
    check(resp_fail is not None and resp_fail.get("code") == "unsupported_target_geometry", f"code=unsupported_target_geometry ({raw_fail})")
    check_eq(nf.snapshot_tree(track), snap_before, "track unchanged after blocked rebake")

    # Placement-only edit (zero-rebake) must succeed and preserve target_leg_geometry.json
    clip2_before = nf.sha256_file(slot2 / "motion.gkclip")
    geom_before = nf.sha256_file(geom_path)
    rc_ok, resp_ok, raw_ok = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "preserve_existing": True, "x": 4.25, "yaw_degrees": -30.0},
            ],
        }
    )
    check_eq(rc_ok, 0, f"placement edit rc ({raw_ok})")
    check(resp_ok is not None and resp_ok.get("ok") is True, "placement edit ok=true")
    check(geom_path.is_file(), "target_leg_geometry.json preserved")
    check_eq(nf.sha256_file(geom_path), geom_before, "target_leg_geometry.json hash preserved")
    check_eq(nf.sha256_file(slot2 / "motion.gkclip"), clip2_before, "slot 2 clip hash preserved")
    check_near(nf.read_json(track / "info.json")["positions"][1]["x"], 4.25, 1e-6, "slot 2 x updated")
    return "slot 2 target_leg_geometry.json blocked rebake while allowing placement-only edit"


def test_t2_f3_06_corrupt_replacement_and_tx_root_inside_library(cap: Capabilities, work: Path) -> str:
    track = _create_base_group_track(cap, work, slot_count=2, with_camera_audio=False)
    snap = nf.snapshot_tree(track)
    bad_vmd = nf.write_vmd(work / "bad" / "corrupt.vmd", vfg.make_negative_vmd(vfg.NegativeVmdKind.TRUNCATED_BONE_RECORD))

    # 1. Corrupt replacement VMD on slot 2
    rc1, resp1, raw1 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "motion_vmd": str(bad_vmd)},
            ],
        }
    )
    check_eq(rc1, 1, f"corrupt swap rc ({raw1})")
    check(resp1 is not None and resp1.get("ok") is False, "corrupt swap ok=false")
    check_eq(nf.snapshot_tree(track), snap, "track unchanged after corrupt swap")

    # 2. transaction_root inside motions_root
    tx_inside = work / "lib" / "nested_tx"
    rc2, resp2, raw2 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "transaction_root": str(tx_inside),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "preserve_existing": True, "x": 2.5},
            ],
        }
    )
    check_eq(rc2, 1, f"tx_root inside motions_root rc ({raw2})")
    check(resp2 is not None and resp2.get("ok") is False, "tx_root inside motions_root ok=false")
    check_eq(nf.snapshot_tree(track), snap, "track unchanged after invalid transaction_root")
    return "corrupt replacement VMD and transaction_root inside motions_root rejected atomically"


# =============================================================================
# Tier 3 Cross-Feature Tests (2 Python tests)
# =============================================================================

def test_t3_f2f3_01_mixed_import_then_selective_edit_and_placement(cap: Capabilities, work: Path) -> str:
    in_dir = work / "in"
    v1 = nf.write_vmd(in_dir / "a.vmd", nf.make_distinct_body_vmd(6, seed=1, include_morphs=True))
    v2 = nf.write_vmd(in_dir / "b.vmd", nf.make_distinct_body_vmd(10, seed=2))
    v3 = nf.write_vmd(in_dir / "c.vmd", nf.make_distinct_body_vmd(14, seed=3))
    cam = nf.write_vmd(in_dir / "cam.vmd", vfg.make_camera_vmd(last_frame=14, fov=42))
    wav = vfg.write_pcm_wav(work / "audio" / "bgm.wav", duration_seconds=0.7)
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)

    rc0, resp0, raw0 = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "F2F3Cross01",
            "camera_vmd": str(cam),
            "audio": str(wav),
            "positions": [
                {"slot": 1, "motion_vmd": str(v1), "source_fps": 25.0},
                {"slot": 2, "motion_vmd": str(v2), "source_fps": 30.0, "disable_morph": True, "x": 1.0},
                {"slot": 3, "motion_vmd": str(v3), "source_fps": 60.0, "disable_morph": True, "x": 2.0},
            ],
        }
    )
    check_eq(rc0, 0, f"F2 import ({raw0})")
    track = Path(resp0["dir"])

    c1_sha = nf.sha256_file(track / "motion.gkclip")
    c3_sha = nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip")
    cam_sha = nf.sha256_file(track / "camera.gkcamera")
    wav_sha = nf.sha256_file(track / "song.wav")

    # Selective F3 edit: replace slot 2 VMD with longer 40-frame VMD + zero-rebake placement on slot 3
    v2_new = nf.write_vmd(work / "in2" / "b_new.vmd", nf.make_distinct_body_vmd(40, seed=11))
    rc1, resp1, raw1 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "motion_vmd": str(v2_new), "source_fps": 30.0, "x": 1.5, "yaw_degrees": -15.0},
                {"slot": 3, "source_slot": 3, "preserve_existing": True, "x": -1.5, "z": 0.75, "yaw_degrees": 25.0},
            ],
        }
    )
    check_eq(rc1, 0, f"F3 edit ({raw1})")
    check_eq(nf.sha256_file(track / "motion.gkclip"), c1_sha, "slot 1 preserved")
    check_eq(nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip"), c3_sha, "slot 3 preserved")
    check_eq(nf.sha256_file(track / "camera.gkcamera"), cam_sha, "shared camera preserved")
    check_eq(nf.sha256_file(track / "song.wav"), wav_sha, "shared audio preserved")

    info = nf.read_json(track / "info.json")
    check_eq(info["total_frames"], info["positions"][1]["frames"], "total_frames recomputed from new slot 2")
    check_near(info["positions"][0]["source_fps"], 25.0, 1e-4, "slot 1 fps kept 25")
    check_near(info["positions"][2]["source_fps"], 60.0, 1e-4, "slot 3 fps kept 60")
    check_near(info["positions"][2]["x"], -1.5, 1e-6, "slot 3 placement updated")
    return "F2 mixed import -> F3 selective slot 2 rebake + slot 3 zero-rebake placement verified"


def test_t3_f2f3_02_solo_promote_reorder_shrink_cycle(cap: Capabilities, work: Path) -> str:
    in_dir = work / "in"
    v1 = nf.write_vmd(in_dir / "s1.vmd", nf.make_distinct_body_vmd(4, seed=1))
    v2 = nf.write_vmd(in_dir / "s2.vmd", nf.make_distinct_body_vmd(8, seed=2))
    v3 = nf.write_vmd(in_dir / "s3.vmd", nf.make_distinct_body_vmd(12, seed=3))
    lib = work / "lib"
    lib.mkdir(parents=True, exist_ok=True)

    # 1. Solo F2 import
    rc0, resp0, raw0 = cap.client.run_import({"motions_root": str(lib), "title": "LifecycleCycle", "motion_vmd": str(v1)})
    check_eq(rc0, 0, f"solo ({raw0})")
    track = Path(resp0["dir"])
    c1 = nf.sha256_file(track / "motion.gkclip")

    # 2. Promote solo -> 3-slot group
    rc1, _, raw1 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 0, "motion_vmd": str(v2), "disable_morph": True, "x": 1.0},
                {"slot": 3, "source_slot": 0, "motion_vmd": str(v3), "disable_morph": True, "x": -1.0},
            ],
        }
    )
    check_eq(rc1, 0, f"promote ({raw1})")
    c2 = nf.sha256_file(track / "positions" / "slot_2" / "motion.gkclip")
    c3 = nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip")
    check_eq(nf.sha256_file(track / "motion.gkclip"), c1, "slot 1 preserved")

    # 3. Reorder [3, 1, 2] -> [1, 2, 3] (slot 3 promoted to root)
    rc2, _, raw2 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 3, "preserve_existing": True},
                {"slot": 2, "source_slot": 1, "preserve_existing": True, "x": 0.75},
                {"slot": 3, "source_slot": 2, "preserve_existing": True, "x": -0.75},
            ],
        }
    )
    check_eq(rc2, 0, f"reorder ({raw2})")
    check_eq(nf.sha256_file(track / "motion.gkclip"), c3, "old slot 3 promoted to root")
    check_eq(nf.sha256_file(track / "positions" / "slot_2" / "motion.gkclip"), c1, "old slot 1 moved to slot 2")
    check_eq(nf.sha256_file(track / "positions" / "slot_3" / "motion.gkclip"), c2, "old slot 2 moved to slot 3")

    # 4. Shrink back to 1-slot solo keeping root (old slot 3)
    rc3, _, raw3 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [{"slot": 1, "source_slot": 1, "preserve_existing": True}],
        }
    )
    check_eq(rc3, 0, f"shrink ({raw3})")
    check(not (track / "positions").exists(), "positions/ removed")
    info3 = nf.read_json(track / "info.json")
    check_eq(info3["is_group"], False, "is_group=false")
    check_eq(info3["position_count"], 1, "position_count=1")
    check_eq(nf.sha256_file(track / "motion.gkclip"), c3, "root clip preserved on shrink")
    return "solo -> 3-slot group -> reorder -> solo shrink cycle verified"


# =============================================================================
# Tier 4 Real-World Application Scenario (1 Python scenario)
# =============================================================================

def test_t4_na_py_01_five_idol_lifecycle(cap: Capabilities, work: Path) -> str:
    """TEST_INFRA Scenario 1 (NativeAOT half): 5-idol filename-sorted import -> swap -> zero-rebake -> delete/renumber."""
    in_dir = work / "vmds"
    idol_names = ["05_chinatsu.vmd", "02_temari.vmd", "04_mao.vmd", "01_saki.vmd", "03_kotone.vmd"]
    for idx, fname in enumerate(idol_names):
        nf.write_vmd(in_dir / fname, nf.make_distinct_body_vmd(6 + idx * 4, seed=idx + 1, include_morphs=(fname.startswith("01_"))))
    sorted_vmds = sorted(in_dir.glob("*.vmd"), key=lambda p: p.name.lower())
    check_eq(len(sorted_vmds), 5, "5 sorted VMDs")

    cam = nf.write_vmd(work / "cam" / "stage_cam.vmd", vfg.make_camera_vmd(last_frame=20, fov=45))
    wav = vfg.write_pcm_wav(work / "audio" / "live_song.wav", duration_seconds=1.2)
    lib = work / "library"
    lib.mkdir(parents=True, exist_ok=True)

    rc0, resp0, raw0 = cap.client.run_import(
        {
            "motions_root": str(lib),
            "title": "CampusLive5Idol",
            "subtitle": "Hatsuboshi",
            "camera_vmd": str(cam),
            "audio": str(wav),
            "positions": [
                {
                    "slot": k + 1,
                    "motion_vmd": str(vpath),
                    "x": float(k - 2) * 0.8 if k > 0 else 0.0,
                    "z": -0.25 * float(k) if k > 0 else 0.0,
                    "yaw_degrees": float(k) * 5.0 if k > 0 else 0.0,
                }
                for k, vpath in enumerate(sorted_vmds)
            ],
        }
    )
    check_eq(rc0, 0, f"5-idol import ({raw0})")
    track = Path(resp0["dir"])
    sidecars = {k: nf.write_slot_sidecars(nf.slot_dir(track, k), f"idol_{k}") for k in range(1, 6)}
    orig_clips = {k: nf.sha256_file(nf.slot_dir(track, k) / "motion.gkclip") for k in range(1, 6)}
    cam_sha = nf.sha256_file(track / "camera.gkcamera")
    wav_sha = nf.sha256_file(track / "song.wav")

    # Step 2: Selective slot 3 VMD swap
    swap3_vmd = nf.write_vmd(work / "swap3" / "03_kotone_v2.vmd", nf.make_distinct_body_vmd(32, seed=42))
    rc1, _, raw1 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "preserve_existing": True, "x": -1.2, "z": -0.25, "yaw_degrees": 5.0},
                {"slot": 3, "source_slot": 3, "motion_vmd": str(swap3_vmd), "x": 0.0, "z": -0.5, "yaw_degrees": 10.0},
                {"slot": 4, "source_slot": 4, "preserve_existing": True, "x": 0.8, "z": -0.75, "yaw_degrees": 15.0},
                {"slot": 5, "source_slot": 5, "preserve_existing": True, "x": 1.6, "z": -1.0, "yaw_degrees": 20.0},
            ],
        }
    )
    check_eq(rc1, 0, f"slot 3 swap ({raw1})")
    new_clip3_sha = nf.sha256_file(nf.slot_dir(track, 3) / "motion.gkclip")
    check(new_clip3_sha != orig_clips[3], "slot 3 clip updated")
    for k in (1, 2, 4, 5):
        check_eq(nf.sha256_file(nf.slot_dir(track, k) / "motion.gkclip"), orig_clips[k], f"slot {k} clip preserved")

    # Step 3: Zero-rebake slot 2 placement edit (delete slot 2 source.vmd to prove no rebake)
    (nf.slot_dir(track, 2) / "source.vmd").unlink()
    rc2, _, raw2 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "preserve_existing": True, "x": -2.5, "y": 0.1, "z": 0.5, "yaw_degrees": -25.0},
                {"slot": 3, "source_slot": 3, "preserve_existing": True, "x": 0.0, "z": -0.5, "yaw_degrees": 10.0},
                {"slot": 4, "source_slot": 4, "preserve_existing": True, "x": 0.8, "z": -0.75, "yaw_degrees": 15.0},
                {"slot": 5, "source_slot": 5, "preserve_existing": True, "x": 1.6, "z": -1.0, "yaw_degrees": 20.0},
            ],
        }
    )
    check_eq(rc2, 0, f"slot 2 placement edit ({raw2})")
    check_eq(nf.sha256_file(nf.slot_dir(track, 2) / "motion.gkclip"), orig_clips[2], "slot 2 clip preserved on placement edit")

    # Step 4: Delete slot 4 -> slot 5 renumbers to slot 4
    rc3, _, raw3 = cap.client.run_import(
        {
            "edit_dir": str(track),
            "positions": [
                {"slot": 1, "source_slot": 1, "preserve_existing": True},
                {"slot": 2, "source_slot": 2, "preserve_existing": True, "x": -2.5, "y": 0.1, "z": 0.5, "yaw_degrees": -25.0},
                {"slot": 3, "source_slot": 3, "preserve_existing": True, "x": 0.0, "z": -0.5, "yaw_degrees": 10.0},
                {"slot": 4, "source_slot": 5, "preserve_existing": True, "x": 1.6, "z": -1.0, "yaw_degrees": 20.0},
            ],
        }
    )
    check_eq(rc3, 0, f"delete slot 4 ({raw3})")
    check(not (track / "positions" / "slot_5").exists(), "slot_5 removed")
    check_eq(nf.sha256_file(nf.slot_dir(track, 1) / "motion.gkclip"), orig_clips[1], "final slot 1 clip")
    check_eq(nf.sha256_file(nf.slot_dir(track, 2) / "motion.gkclip"), orig_clips[2], "final slot 2 clip")
    check_eq(nf.sha256_file(nf.slot_dir(track, 3) / "motion.gkclip"), new_clip3_sha, "final slot 3 clip")
    check_eq(nf.sha256_file(nf.slot_dir(track, 4) / "motion.gkclip"), orig_clips[5], "final slot 4 clip (former slot 5)")
    check_eq(nf.read_slot_sidecars(nf.slot_dir(track, 4)), sidecars[5], "slot 5 sidecars migrated to slot 4")
    check_eq(nf.sha256_file(track / "camera.gkcamera"), cam_sha, "camera intact")
    check_eq(nf.sha256_file(track / "song.wav"), wav_sha, "audio intact")
    info_final = nf.read_json(track / "info.json")
    check_eq(info_final.get("position_count"), 4, "final position_count=4")
    return "5-idol lifecycle (sorted import -> slot3 swap -> zero-rebake slot2 -> slot4 delete/renumber) verified"


TestFn = Callable[[Capabilities, Path], str]

TESTS: list[tuple[str, int, str, str, str, TestFn]] = [
    # F2 Tier 1
    ("T1-F2-01", 1, "F2", "2-slot group disk layout and info.json", "f2", test_t1_f2_01_two_slot_layout),
    ("T1-F2-02", 1, "F2", "5-slot group with shared camera/audio and mixed morphs", "f2", test_t1_f2_02_five_slot_shared_camera_audio),
    ("T1-F2-03", 1, "F2", "Filename-sorted VMDs and out-of-order slot normalization", "f2", test_t1_f2_03_filename_sort_and_slot_ordering),
    ("T1-F2-04", 1, "F2", "Slot 1 origin clamp and slot 2+ relative coordinates", "f2", test_t1_f2_04_slot1_origin_clamp_and_slot2_coords),
    ("T1-F2-05", 1, "F2", "Per-slot source_fps and top-level slot 1 inheritance", "f2", test_t1_f2_05_per_slot_source_fps_and_slot1_inheritance),
    ("T1-F2-06", 1, "F2", "Solo song backward compatibility", "f2", test_t1_f2_06_solo_backward_compatibility),
    # F2 Tier 2
    ("T2-F2-01", 2, "F2", "C ABI InvalidArgument, BufferTooSmall, and candidate_b_tree", "f2", test_t2_f2_01_c_abi_bounds_and_candidate_b),
    ("T2-F2-02", 2, "F2", "Reject empty, duplicate, non-contiguous, <1, missing-slot1 positions", "f2", test_t2_f2_02_invalid_positions_structure),
    ("T2-F2-03", 2, "F2", "Reject invalid source_fps and non-finite/wrong-type coords", "f2", test_t2_f2_03_invalid_fps_and_non_finite_coords),
    ("T2-F2-04", 2, "F2", "Atomic rejection of corrupt/eye-only/empty VMD in slot 2", "f2", test_t2_f2_04_corrupt_slot2_vmd_atomic_failure),
    ("T2-F2-05", 2, "F2", "SafeTrackName collision and slot-2 target_leg_geometry rejection", "f2", test_t2_f2_05_name_collision_and_target_leg_geometry),
    ("T2-F2-06", 2, "F2", "Unicode/CJK UTF-8 title and VMD path round-trip", "f2", test_t2_f2_06_unicode_cjk_utf8_roundtrip),
    # F3 Tier 1
    ("T1-F3-01", 1, "F3", "Zero-rebake initial_placement and metadata edit with deleted source.vmd", "f3", test_t1_f3_01_zero_rebake_placement_and_metadata_edit),
    ("T1-F3-02", 1, "F3", "Selective slot-2 VMD replacement preserves unmodified slots and sidecars", "f3", test_t1_f3_02_selective_slot2_vmd_swap_preserves_others_and_sidecars),
    ("T1-F3-03", 1, "F3", "Selective slot-2 morph_vmd, clear_morph, and disable_morph", "f3", test_t1_f3_03_selective_morph_edit),
    ("T1-F3-04", 1, "F3", "Delete middle slot with positions/slot_k renumbering and sidecar migration", "f3", test_t1_f3_04_delete_middle_slot_and_renumber_sidecars),
    ("T1-F3-05", 1, "F3", "Solo->group slot append and group->solo shrink", "f3", test_t1_f3_05_solo_to_group_and_group_to_solo),
    ("T1-F3-06", 1, "F3", "Delete slot 1 promotes slot 2 to root and normalizes origin", "f3", test_t1_f3_06_delete_slot1_promotes_slot2),
    # F3 Tier 2
    ("T2-F3-01", 2, "F3", "Duplicate source_slot rejected atomically", "f3", test_t2_f3_01_duplicate_source_slot_atomic),
    ("T2-F3-02", 2, "F3", "Nonexistent source_slot rejected atomically", "f3", test_t2_f3_02_nonexistent_source_slot_atomic),
    ("T2-F3-03", 2, "F3", "Invalid positions/slot/source_fps on edit rejected atomically", "f3", test_t2_f3_03_invalid_edit_positions_and_fps),
    ("T2-F3-04", 2, "F3", "Missing source.vmd on slot-2 rebake rejected atomically", "f3", test_t2_f3_04_missing_source_vmd_on_slot2_rebake),
    ("T2-F3-05", 2, "F3", "Slot-2 target_leg_geometry blocks rebake while allowing placement edit", "f3", test_t2_f3_05_slot_target_leg_geometry_blocks_rebake_allows_placement),
    ("T2-F3-06", 2, "F3", "Corrupt replacement VMD and transaction_root inside library rejected atomically", "f3", test_t2_f3_06_corrupt_replacement_and_tx_root_inside_library),
    # Tier 3
    ("T3-F2F3-01", 3, "F2", "F2 mixed multi-slot import -> F3 selective rebake and placement edit", "f3", test_t3_f2f3_01_mixed_import_then_selective_edit_and_placement),
    ("T3-F2F3-02", 3, "F3", "Solo F2 import -> F3 expand -> reorder -> shrink cycle", "f3", test_t3_f2f3_02_solo_promote_reorder_shrink_cycle),
    # Tier 4
    ("T4-NA-PY-01", 4, "F3", "5-idol group song import -> slot swap -> zero-rebake placement -> delete/renumber", "f3", test_t4_na_py_01_five_idol_lifecycle),
]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--tier", choices=("1", "2", "3", "4", "All"), default="All")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--work-dir", type=Path, required=True)
    args = parser.parse_args(argv)

    args.work_dir.mkdir(parents=True, exist_ok=True)
    cap = Capabilities(args.dll, args.work_dir)
    tier_filter = 0 if args.tier == "All" else int(args.tier)

    results: list[dict[str, Any]] = []
    fail_count = 0
    for tid, tier, feature, name, cap_req, fn in TESTS:
        if tier_filter != 0 and tier != tier_filter:
            continue

        needs_f2 = cap_req in ("f2", "f3")
        needs_f3 = cap_req == "f3"
        if (needs_f2 and not cap.cap_f2) or (needs_f3 and not cap.cap_f3):
            results.append(
                {
                    "id": tid,
                    "tier": tier,
                    "feature": feature,
                    "name": name,
                    "status": "PENDING_IMPL",
                    "detail": cap.reason or "NativeAOT M1 capability unavailable",
                }
            )
            continue

        case_work = nf.reset_dir(args.work_dir / tid)
        try:
            detail = fn(cap, case_work)
            results.append(
                {
                    "id": tid,
                    "tier": tier,
                    "feature": feature,
                    "name": name,
                    "status": "PASS",
                    "detail": detail,
                }
            )
        except Exception as exc:
            fail_count += 1
            results.append(
                {
                    "id": tid,
                    "tier": tier,
                    "feature": feature,
                    "name": name,
                    "status": "FAIL",
                    "detail": str(exc),
                }
            )

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(results, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return 1 if fail_count > 0 else 0


if __name__ == "__main__":
    raise SystemExit(main())
