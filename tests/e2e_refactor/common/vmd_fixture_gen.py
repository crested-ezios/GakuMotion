"""Deterministic VMD, WAV, .gkclip, .gkcamera, and info.json fixture generator.

Produces byte-for-byte identical binary outputs to vmd_fixture_gen.hpp.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
from enum import Enum
import json
import math
from pathlib import Path
import struct
from typing import Iterable, Sequence

K_LINEAR_BONE_BEZIER_64 = bytes(
    [
        20, 0, 0, 0, 20, 0, 0, 0, 107, 0, 0, 0, 107, 0, 0, 0,
        20, 0, 0, 0, 20, 0, 0, 0, 107, 0, 0, 0, 107, 0, 0, 0,
        20, 0, 0, 0, 20, 0, 0, 0, 107, 0, 0, 0, 107, 0, 0, 0,
        20, 0, 0, 0, 20, 0, 0, 0, 107, 0, 0, 0, 107, 0, 0, 0,
    ]
)

K_LINEAR_CAM_BEZIER_24 = bytes(
    [
        20, 107, 20, 107,
        20, 107, 20, 107,
        20, 107, 20, 107,
        20, 107, 20, 107,
        20, 107, 20, 107,
        20, 107, 20, 107,
    ]
)


class VmdVersion(Enum):
    V2 = "v2"
    V1 = "v1"


class NegativeVmdKind(Enum):
    BAD_MAGIC = "BadMagic"
    TRUNCATED_HEADER = "TruncatedHeader"
    TRUNCATED_MODEL = "TruncatedModel"
    TRUNCATED_BONE_RECORD = "TruncatedBoneRecord"
    INFLATED_BONE_COUNT = "InflatedBoneCount"
    TRUNCATED_MORPH_RECORD = "TruncatedMorphRecord"
    TRUNCATED_CAMERA_RECORD = "TruncatedCameraRecord"
    ZERO_QUATERNION_BONE = "ZeroQuaternionBone"
    ZERO_FOV_CAMERA = "ZeroFovCamera"
    INVALID_CAMERA_CURVE = "InvalidCameraCurve"


class NegativeWavKind(Enum):
    TRUNCATED_RIFF = "TruncatedRiff"
    MISSING_DATA_CHUNK = "MissingDataChunk"
    UNSUPPORTED_CODEC = "UnsupportedCodec"
    ZERO_DATA_BYTES = "ZeroDataBytes"


@dataclass
class BoneKey:
    name: str
    frame: int = 0
    pos: tuple[float, float, float] = (0.0, 0.0, 0.0)
    rot: tuple[float, float, float, float] = (0.0, 0.0, 0.0, 1.0)
    bezier: bytes = K_LINEAR_BONE_BEZIER_64


@dataclass
class MorphKey:
    name: str
    frame: int = 0
    weight: float = 1.0


@dataclass
class CameraKey:
    frame: int = 0
    distance: float = -4.0
    target: tuple[float, float, float] = (0.0, 1.0, 0.0)
    rot_rad: tuple[float, float, float] = (0.0, 0.0, 0.0)
    curves: bytes = K_LINEAR_CAM_BEZIER_24
    fov: int = 45
    perspective: int = 0


@dataclass
class IkKey:
    frame: int = 0
    visible: bool = True
    ik_switches: list[tuple[str, bool]] = field(default_factory=list)


@dataclass
class VmdSpec:
    version: VmdVersion = VmdVersion.V2
    model_name: str = "SyntheticModel"
    bones: list[BoneKey] = field(default_factory=list)
    morphs: list[MorphKey] = field(default_factory=list)
    cameras: list[CameraKey] = field(default_factory=list)
    iks: list[IkKey] = field(default_factory=list)


@dataclass
class ClipBoneSpec:
    hid: int = 0
    name: str = "Hips"
    flags: int = 1


@dataclass
class PositionSlotSpec:
    slot: int = 1
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    yaw_degrees: float = 0.0
    has_motion: bool = True
    has_facial: bool = False
    source_vmd: str = "source.vmd"
    source_morph_vmd: str = ""
    source_vmd_sha256: str = ""
    source_fps: float = 30.0
    frames: int = 3
    bones: int = 1


@dataclass
class TrackInfoSpec:
    title: str = "Synthetic Track"
    subtitle: str = "Synthetic Author"
    duration_seconds: float = 0.5
    total_frames: int = 15
    bone_count: int = 1
    fps: float = 30.0
    source_vmd: str = "source.vmd"
    source_fps: float = 30.0
    source_vmd_sha256: str = "0" * 64
    has_facial: bool = False
    has_camera: bool = False
    source_morph_vmd: str = ""
    source_camera_vmd: str = ""
    audio_file: str = ""
    audio_duration: float = 0.0
    has_cover: bool = False
    cover_file: str = ""
    is_group: bool = False
    position_count: int = 1
    positions: list[PositionSlotSpec] = field(default_factory=list)
    import_backend: str = "csharp"
    import_candidate_b_tree: str = "0" * 64


_SJIS_TABLE: dict[str, bytes] = {
    "全ての親": b"\x91\x53\x82\xC4\x82\xCC\x90\x65",
    "センター": b"\x83\x5A\x83\x93\x83\x5E\x81\x5B",
    "グルーブ": b"\x83\x4F\x83\x8B\x81\x5B\x83\x75",
    "下半身": b"\x89\xBA\x94\xBC\x90\x67",
    "上半身": b"\x8F\xE3\x94\xBC\x90\x67",
    "首": b"\x8E\xF1",
    "頭": b"\x93\xAA",
    "左腕": b"\x8D\xB6\x98\x72",
    "右腕": b"\x89\x45\x98\x72",
    "左足ＩＫ": b"\x8D\xB6\x91\xAB\x82\x68\x82\x6A",
    "右足ＩＫ": b"\x89\x45\x91\xAB\x82\x68\x82\x6A",
    "両目": b"\x97\xBC\x96\xDA",
    "左目": b"\x8D\xB6\x96\xDA",
    "右目": b"\x89\x45\x96\xDA",
    "目戻": b"\x96\xDA\x96\xDF",
    "目": b"\x96\xDA",
    "あ": b"\x82\xA0",
    "い": b"\x82\xA2",
    "う": b"\x82\xA4",
    "え": b"\x82\xA6",
    "お": b"\x82\xA8",
    "まばたき": b"\x82\xDC\x82\xCE\x82\xBD\x82\xAB",
    "笑い": b"\x8F\xCE\x82\xA2",
    "にやり": b"\x82\xC9\x82\xE2\x82\xE8",
    "困る": b"\x8D\xA2\x82\xE9",
}


def _encode_sjis(name: str) -> bytes:
    if name in _SJIS_TABLE:
        return _SJIS_TABLE[name]
    return name.encode("cp932")


def _fixed_bytes(raw: bytes, width: int) -> bytes:
    if len(raw) >= width:
        return raw[:width]
    return raw + b"\x00" * (width - len(raw))


def build_vmd_bytes(spec: VmdSpec) -> bytes:
    buf = bytearray()
    model_width = 20 if spec.version == VmdVersion.V2 else 10
    magic = b"Vocaloid Motion Data 0002" if spec.version == VmdVersion.V2 else b"Vocaloid Motion Data file"
    buf.extend(_fixed_bytes(magic, 30))
    buf.extend(_fixed_bytes(_encode_sjis(spec.model_name), model_width))

    buf.extend(struct.pack("<I", len(spec.bones)))
    for b in spec.bones:
        buf.extend(_fixed_bytes(_encode_sjis(b.name), 15))
        buf.extend(struct.pack("<I7f", b.frame, b.pos[0], b.pos[1], b.pos[2], b.rot[0], b.rot[1], b.rot[2], b.rot[3]))
        if len(b.bezier) != 64:
            raise ValueError(f"Bone bezier must be 64 bytes, got {len(b.bezier)}")
        buf.extend(b.bezier)

    buf.extend(struct.pack("<I", len(spec.morphs)))
    for m in spec.morphs:
        buf.extend(_fixed_bytes(_encode_sjis(m.name), 15))
        buf.extend(struct.pack("<If", m.frame, m.weight))

    buf.extend(struct.pack("<I", len(spec.cameras)))
    for c in spec.cameras:
        buf.extend(
            struct.pack(
                "<If6f",
                c.frame,
                c.distance,
                c.target[0],
                c.target[1],
                c.target[2],
                c.rot_rad[0],
                c.rot_rad[1],
                c.rot_rad[2],
            )
        )
        if len(c.curves) != 24:
            raise ValueError(f"Camera curves must be 24 bytes, got {len(c.curves)}")
        buf.extend(c.curves)
        buf.extend(struct.pack("<IB", c.fov, c.perspective))

    buf.extend(struct.pack("<II", 0, 0))  # lightCount=0, shadowCount=0

    buf.extend(struct.pack("<I", len(spec.iks)))
    for ik in spec.iks:
        buf.extend(struct.pack("<IBI", ik.frame, 1 if ik.visible else 0, len(ik.ik_switches)))
        for sw_name, enabled in ik.ik_switches:
            buf.extend(_fixed_bytes(_encode_sjis(sw_name), 20))
            buf.extend(struct.pack("<B", 1 if enabled else 0))

    return bytes(buf)


def make_body_vmd(last_frame: int = 2, include_morphs: bool = False, seed: int = 0) -> bytes:
    frames = [0]
    if last_frame > 0:
        frames.append(last_frame)

    bones: list[BoneKey] = []
    for f in frames:
        bones.append(BoneKey(name="全ての親", frame=f))
        if f > 0:
            if seed == 0:
                pos = (0.0, 0.25, 0.0)
                rot = (0.0, 0.0, 0.0, 1.0)
            else:
                dx = float((seed % 7) - 3) * 0.25
                dz = float((seed % 5) - 2) * 0.125
                t = float((seed % 15) + 1) / 32.0
                qw = math.sqrt(1.0 - t * t)
                pos = (dx, 0.25, dz)
                rot = (t, 0.0, 0.0, qw)
            bones.append(BoneKey(name="センター", frame=f, pos=pos, rot=rot))
        else:
            bones.append(BoneKey(name="センター", frame=f))
        bones.append(BoneKey(name="上半身", frame=f))

    morphs: list[MorphKey] = []
    if include_morphs:
        for f in frames:
            morphs.append(MorphKey(name="あ", frame=f, weight=0.0 if f == 0 else 1.0))
            morphs.append(MorphKey(name="まばたき", frame=f, weight=0.0 if f == 0 else 0.75))

    return build_vmd_bytes(VmdSpec(bones=bones, morphs=morphs))


def make_morph_vmd(last_frame: int = 2) -> bytes:
    frames = [0]
    if last_frame > 0:
        frames.append(last_frame)
    morphs: list[MorphKey] = []
    for f in frames:
        w = 0.0 if f == 0 else 1.0
        morphs.append(MorphKey(name="あ", frame=f, weight=w))
        morphs.append(MorphKey(name="まばたき", frame=f, weight=w))
    return build_vmd_bytes(VmdSpec(morphs=morphs))


def make_eye_only_vmd(last_frame: int = 2) -> bytes:
    frames = [0]
    if last_frame > 0:
        frames.append(last_frame)
    bones = [BoneKey(name="両目", frame=f) for f in frames]
    return build_vmd_bytes(VmdSpec(bones=bones))


def make_camera_vmd(last_frame: int = 2, fov: int = 45) -> bytes:
    frames = [0]
    if last_frame > 0:
        frames.append(last_frame)
    cameras = [CameraKey(frame=f, fov=fov) for f in frames]
    return build_vmd_bytes(VmdSpec(cameras=cameras))


def make_empty_vmd() -> bytes:
    return build_vmd_bytes(VmdSpec())


def make_negative_vmd(kind: NegativeVmdKind) -> bytes:
    if kind == NegativeVmdKind.BAD_MAGIC:
        return _fixed_bytes(b"Not A Valid VMD Header", 50)
    if kind == NegativeVmdKind.TRUNCATED_HEADER:
        return b"Vocaloid Motion "
    if kind == NegativeVmdKind.TRUNCATED_MODEL:
        return _fixed_bytes(b"Vocaloid Motion Data 0002", 30) + _fixed_bytes(b"Synthetic", 10)
    if kind == NegativeVmdKind.TRUNCATED_BONE_RECORD:
        return (
            _fixed_bytes(b"Vocaloid Motion Data 0002", 30)
            + _fixed_bytes(b"SyntheticModel", 20)
            + struct.pack("<I", 1)
            + b"\x00" * 50
        )
    if kind == NegativeVmdKind.INFLATED_BONE_COUNT:
        return (
            _fixed_bytes(b"Vocaloid Motion Data 0002", 30)
            + _fixed_bytes(b"SyntheticModel", 20)
            + struct.pack("<I", 1000)
        )
    if kind == NegativeVmdKind.TRUNCATED_MORPH_RECORD:
        return (
            _fixed_bytes(b"Vocaloid Motion Data 0002", 30)
            + _fixed_bytes(b"SyntheticModel", 20)
            + struct.pack("<II", 0, 1)
            + b"\x00" * 10
        )
    if kind == NegativeVmdKind.TRUNCATED_CAMERA_RECORD:
        return (
            _fixed_bytes(b"Vocaloid Motion Data 0002", 30)
            + _fixed_bytes(b"SyntheticModel", 20)
            + struct.pack("<III", 0, 0, 1)
            + b"\x00" * 30
        )
    if kind == NegativeVmdKind.ZERO_QUATERNION_BONE:
        return build_vmd_bytes(VmdSpec(bones=[BoneKey(name="センター", frame=0, rot=(0.0, 0.0, 0.0, 0.0))]))
    if kind == NegativeVmdKind.ZERO_FOV_CAMERA:
        return make_camera_vmd(2, fov=0)
    if kind == NegativeVmdKind.INVALID_CAMERA_CURVE:
        bad_curves = bytearray(K_LINEAR_CAM_BEZIER_24)
        bad_curves[0] = 200
        return build_vmd_bytes(VmdSpec(cameras=[CameraKey(frame=0, curves=bytes(bad_curves))]))
    raise ValueError(f"Unsupported NegativeVmdKind: {kind}")


def make_pcm_wav_bytes(
    duration_seconds: float = 0.5,
    sample_rate: int = 8000,
    channels: int = 1,
    bits_per_sample: int = 16,
    data_first: bool = False,
    odd_junk_chunk: bool = False,
) -> bytes:
    bytes_per_sample = bits_per_sample // 8
    block_align = channels * bytes_per_sample
    byte_rate = sample_rate * block_align
    sample_count = 0
    if duration_seconds > 0.0:
        # Match C++ std::llround behavior cleanly for typical non-halfway test durations
        computed = int(math.floor(duration_seconds * sample_rate + 0.5))
        sample_count = max(1, computed)
    data_bytes = sample_count * block_align

    fmt_chunk = b"fmt " + struct.pack("<IHHIIHH", 16, 1, channels, sample_rate, byte_rate, block_align, bits_per_sample)
    junk_chunk = b""
    if odd_junk_chunk:
        junk_chunk = b"JUNK" + struct.pack("<I", 3) + b"ABC\x00"
    data_chunk = b"data" + struct.pack("<I", data_bytes) + (b"\x00" * data_bytes)

    riff_size = 4 + len(fmt_chunk) + len(junk_chunk) + len(data_chunk)
    header = b"RIFF" + struct.pack("<I", riff_size) + b"WAVE"
    if data_first:
        return header + data_chunk + junk_chunk + fmt_chunk
    return header + fmt_chunk + junk_chunk + data_chunk


def make_negative_wav(kind: NegativeWavKind) -> bytes:
    if kind == NegativeWavKind.TRUNCATED_RIFF:
        return b"RIFF"
    if kind == NegativeWavKind.MISSING_DATA_CHUNK:
        return (
            b"RIFF"
            + struct.pack("<I", 28)
            + b"WAVE"
            + b"fmt "
            + struct.pack("<IHHIIHH", 16, 1, 1, 8000, 16000, 2, 16)
        )
    if kind == NegativeWavKind.UNSUPPORTED_CODEC:
        raw = bytearray(make_pcm_wav_bytes(0.1, 8000, 1, 16, False, False))
        raw[20:22] = struct.pack("<H", 7)
        return bytes(raw)
    if kind == NegativeWavKind.ZERO_DATA_BYTES:
        return make_pcm_wav_bytes(0.0, 8000, 1, 16, False, False)
    raise ValueError(f"Unsupported NegativeWavKind: {kind}")


def write_pcm_wav(
    path: Path | str,
    duration_seconds: float = 0.5,
    sample_rate: int = 8000,
    channels: int = 1,
    bits_per_sample: int = 16,
) -> Path:
    dest = Path(path)
    dest.parent.mkdir(parents=True, exist_ok=True)
    dest.write_bytes(make_pcm_wav_bytes(duration_seconds, sample_rate, channels, bits_per_sample))
    return dest


def make_minimal_gkclip_bytes(
    fps: float = 30.0,
    frames: int = 3,
    bones: Sequence[ClipBoneSpec] = (ClipBoneSpec(0, "Hips", 1),),
    blendshapes: Sequence[str] = (),
) -> bytes:
    buf = bytearray()
    bone_count = len(bones)
    duration_seconds = float(frames - 1) / float(fps) if frames > 1 else 0.0
    root_scale = 0.07795

    buf.extend(b"GKCLIPCL")
    buf.extend(struct.pack("<IfIfIf", 4, fps, frames, duration_seconds, bone_count, root_scale))

    for b in bones:
        name_bytes = b.name.encode("utf-8")[:255]
        buf.extend(struct.pack("<HBB", b.hid, len(name_bytes), b.flags))
        buf.extend(name_bytes)

    for _ in range(frames):
        buf.extend(struct.pack("<3f", 0.0, 0.0, 0.0))
        for _ in range(bone_count):
            buf.extend(struct.pack("<4f", 0.0, 0.0, 0.0, 1.0))

    buf.extend(struct.pack("<B", 0))  # hasCamera = 0
    buf.extend(struct.pack("<I", len(blendshapes)))
    for name in blendshapes:
        name_bytes = name.encode("utf-8")[:65535]
        buf.extend(struct.pack("<H", len(name_bytes)))
        buf.extend(name_bytes)
    total_weights = frames * len(blendshapes)
    if total_weights > 0:
        buf.extend(struct.pack(f"<{total_weights}f", *([0.0] * total_weights)))

    return bytes(buf)


def make_minimal_gkcamera_bytes(fps: float = 60.0, count: int = 3) -> bytes:
    buf = bytearray()
    buf.extend(b"GKCAM\x00\x00\x00")
    buf.extend(struct.pack("<IfI", 1, fps, count))
    for _ in range(count):
        buf.extend(struct.pack("<9fI", 0.0, 1.0, -4.0, 0.0, 0.0, 0.0, 1.0, 45.0, 2.0, 0))
    return bytes(buf)


def make_info_json(spec: TrackInfoSpec) -> str:
    payload: dict[str, object] = {
        "title": spec.title,
        "subtitle": spec.subtitle,
        "duration_seconds": spec.duration_seconds,
        "total_frames": spec.total_frames,
        "bone_count": spec.bone_count,
        "fps": spec.fps,
        "source_vmd": spec.source_vmd,
        "source_fps": spec.source_fps,
        "source_vmd_sha256": spec.source_vmd_sha256,
        "has_facial": spec.has_facial,
        "has_camera": spec.has_camera,
        "source_morph_vmd": spec.source_morph_vmd,
        "source_camera_vmd": spec.source_camera_vmd,
    }
    if spec.audio_file:
        payload["audio_file"] = spec.audio_file
        payload["audio_duration"] = spec.audio_duration
    if spec.has_cover:
        payload["has_cover"] = True
        payload["cover_file"] = spec.cover_file
    payload["is_group"] = spec.is_group
    payload["position_count"] = spec.position_count
    payload["positions"] = [
        {
            "slot": p.slot,
            "x": p.x,
            "y": p.y,
            "z": p.z,
            "yaw_degrees": p.yaw_degrees,
            "has_motion": p.has_motion,
            "has_facial": p.has_facial,
            "source_vmd": p.source_vmd,
            "source_morph_vmd": p.source_morph_vmd,
            "source_vmd_sha256": p.source_vmd_sha256,
            "source_fps": p.source_fps,
            "frames": p.frames,
            "bones": p.bones,
        }
        for p in spec.positions
    ]
    payload["track_timing"] = {
        "motion": {"speed": 1.0, "offset_seconds": 0.0},
        "facial": {"speed": 1.0, "offset_seconds": 0.0},
        "camera": {"speed": 1.0, "offset_seconds": 0.0},
        "audio": {"speed": 1.0, "offset_seconds": 0.0},
    }
    payload["import_backend"] = spec.import_backend
    payload["import_candidate_b_tree"] = spec.import_candidate_b_tree
    return json.dumps(payload, indent=2, ensure_ascii=False) + "\n"


def write_synthetic_track_dir(
    dir_path: Path | str,
    spec: TrackInfoSpec,
    write_camera: bool = False,
    write_audio: bool = False,
) -> Path:
    root = Path(dir_path)
    root.mkdir(parents=True, exist_ok=True)
    frames = max(1, spec.total_frames)
    clip_bytes = make_minimal_gkclip_bytes(spec.fps, frames)
    (root / "motion.gkclip").write_bytes(clip_bytes)
    (root / "source.vmd").write_bytes(make_body_vmd(2, spec.has_facial, 0))
    if write_camera or spec.has_camera:
        (root / "camera.gkcamera").write_bytes(make_minimal_gkcamera_bytes(60.0, frames))
        (root / "source_camera.vmd").write_bytes(make_camera_vmd(2, 45))
    if write_audio or spec.audio_file:
        audio_name = spec.audio_file or "song.wav"
        dur = spec.audio_duration if spec.audio_duration > 0.0 else 0.5
        write_pcm_wav(root / audio_name, dur, 8000, 1, 16)
    if spec.is_group and spec.position_count >= 2:
        for k in range(2, spec.position_count + 1):
            slot_dir = root / "positions" / f"slot_{k}"
            slot_dir.mkdir(parents=True, exist_ok=True)
            (slot_dir / "motion.gkclip").write_bytes(clip_bytes)
            (slot_dir / "source.vmd").write_bytes(make_body_vmd(2, spec.has_facial, k))
    (root / "info.json").write_text(make_info_json(spec), encoding="utf-8")
    return root


def write_synthetic_group_track(
    dir_path: Path | str,
    slot_count: int = 3,
    title: str = "Group Track",
    subtitle: str = "Synthetic Author",
    write_camera: bool = True,
    write_audio: bool = True,
) -> Path:
    count = max(2, slot_count)
    spec = TrackInfoSpec(
        title=title,
        subtitle=subtitle,
        total_frames=3,
        fps=30.0,
        duration_seconds=2.0 / 30.0,
        bone_count=1,
        has_facial=False,
        has_camera=write_camera,
        source_camera_vmd="source_camera.vmd" if write_camera else "",
        audio_file="song.wav" if write_audio else "",
        audio_duration=0.5 if write_audio else 0.0,
        is_group=True,
        position_count=count,
        positions=[
            PositionSlotSpec(
                slot=k,
                x=0.0,
                y=0.0,
                z=0.0,
                yaw_degrees=0.0,
                has_motion=True,
                has_facial=False,
                source_vmd=f"slot_{k}.vmd",
                source_fps=30.0,
                frames=3,
                bones=1,
            )
            for k in range(1, count + 1)
        ],
    )
    return write_synthetic_track_dir(dir_path, spec, write_camera=write_camera, write_audio=write_audio)


def emit_parity_bundle(out_dir: Path | str) -> list[Path]:
    root = Path(out_dir)
    root.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    files: dict[str, bytes] = {
        "body_default.vmd": make_body_vmd(2, False, 0),
        "body_morph_seed3.vmd": make_body_vmd(10, True, 3),
        "morph.vmd": make_morph_vmd(2),
        "eye_only.vmd": make_eye_only_vmd(2),
        "camera.vmd": make_camera_vmd(2, 45),
        "empty.vmd": make_empty_vmd(),
        "neg_bad_magic.vmd": make_negative_vmd(NegativeVmdKind.BAD_MAGIC),
        "neg_trunc_header.vmd": make_negative_vmd(NegativeVmdKind.TRUNCATED_HEADER),
        "neg_trunc_model.vmd": make_negative_vmd(NegativeVmdKind.TRUNCATED_MODEL),
        "neg_trunc_bone.vmd": make_negative_vmd(NegativeVmdKind.TRUNCATED_BONE_RECORD),
        "neg_inflated_bone.vmd": make_negative_vmd(NegativeVmdKind.INFLATED_BONE_COUNT),
        "neg_trunc_morph.vmd": make_negative_vmd(NegativeVmdKind.TRUNCATED_MORPH_RECORD),
        "neg_trunc_camera.vmd": make_negative_vmd(NegativeVmdKind.TRUNCATED_CAMERA_RECORD),
        "neg_zero_quat.vmd": make_negative_vmd(NegativeVmdKind.ZERO_QUATERNION_BONE),
        "neg_zero_fov.vmd": make_negative_vmd(NegativeVmdKind.ZERO_FOV_CAMERA),
        "neg_bad_cam_curve.vmd": make_negative_vmd(NegativeVmdKind.INVALID_CAMERA_CURVE),
        "audio_default.wav": make_pcm_wav_bytes(0.5, 8000, 1, 16, False, False),
        "audio_data_first_junk.wav": make_pcm_wav_bytes(0.25, 44100, 2, 16, True, True),
        "neg_trunc_riff.wav": make_negative_wav(NegativeWavKind.TRUNCATED_RIFF),
        "neg_missing_data.wav": make_negative_wav(NegativeWavKind.MISSING_DATA_CHUNK),
        "neg_bad_codec.wav": make_negative_wav(NegativeWavKind.UNSUPPORTED_CODEC),
        "neg_zero_data.wav": make_negative_wav(NegativeWavKind.ZERO_DATA_BYTES),
        "clip_minimal.gkclip": make_minimal_gkclip_bytes(30.0, 3),
        "clip_blendshapes.gkclip": make_minimal_gkclip_bytes(
            60.0,
            4,
            [ClipBoneSpec(0, "Hips", 1), ClipBoneSpec(7, "Spine", 1)],
            ["a", "blink"],
        ),
        "camera_minimal.gkcamera": make_minimal_gkcamera_bytes(60.0, 3),
    }
    for name, data in files.items():
        p = root / name
        p.write_bytes(data)
        written.append(p)
    write_synthetic_group_track(root / "group_track_3", slot_count=3)
    return written


def _cli(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="cmd", required=True)

    p_parity = sub.add_parser("emit-parity", help="Emit canonical binary parity fixture bundle")
    p_parity.add_argument("--out-dir", type=Path, required=True)

    p_group = sub.add_parser("emit-group-track", help="Emit synthetic group track directory")
    p_group.add_argument("--out-dir", type=Path, required=True)
    p_group.add_argument("--slots", type=int, default=3)

    args = parser.parse_args(argv)
    if args.cmd == "emit-parity":
        paths = emit_parity_bundle(args.out_dir)
        print(f"Emitted {len(paths)} parity files to {args.out_dir}")
        return 0
    if args.cmd == "emit-group-track":
        out = write_synthetic_group_track(args.out_dir, slot_count=args.slots)
        print(f"Emitted synthetic group track ({args.slots} slots) to {out}")
        return 0
    return 1


if __name__ == "__main__":
    raise SystemExit(_cli())
