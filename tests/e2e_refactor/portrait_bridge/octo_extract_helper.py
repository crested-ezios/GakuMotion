#!/usr/bin/env python3
"""Offline read-only Octo portrait extractor for E2E-3 (F5/F6/F13).

Strictly accesses <game-root> read-only ('rb'); NEVER launches the game process.
Decrypts octo/pdb/400/705100/octocacheevai (AES-128-CBC + MD5 integrity check),
de-obfuscates the first 256 bytes of each bundle with the verified interleaved XOR
mask, extracts RGBA Texture2D PNGs via UnityPy, and writes them to --out-dir.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import struct
import sys
from pathlib import Path
from typing import Dict, List, Tuple

from cryptography.hazmat.primitives import padding
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
import UnityPy


OCTO_KEY = hashlib.md5(b"1nuv9td1bw1udefk").digest()
OCTO_IV = hashlib.md5(b"LvAUtf+tnz").digest()

# Target extraction set exercising Tier 1 bust-up, Tier 2 audition fallback,
# Tier 3 speaker fallback, and Tier 4 thumb-circle fallback.
TARGETS: List[Tuple[str, str, str]] = [
    ("hski", "img_chr_hski_00-bustup", "tier1_bustup"),
    ("ttmr", "img_chr_ttmr_00-bustup", "tier1_bustup"),
    ("fktn", "img_chr_fktn_00-bustup", "tier1_bustup"),
    ("nasr", "img_chr_nasr_00-bustup", "tier1_bustup"),
    ("hkne", "img_chr_hkne_00-audition", "tier2_audition"),
    ("krnh", "img_adv_speaker_krnh_000-000", "tier3_speaker"),
    ("amao_thumb", "img_chr_amao_00-thumb-circle", "tier4_thumb_circle"),
]


def _read_varint(buf: bytes, pos: int) -> Tuple[int, int]:
    val = 0
    shift = 0
    while True:
        if pos >= len(buf):
            raise ValueError("Truncated protobuf varint")
        b = buf[pos]
        pos += 1
        val |= (b & 0x7F) << shift
        if not (b & 0x80):
            return val, pos
        shift += 7


def _parse_proto_fields(buf: bytes) -> List[Tuple[int, int, object]]:
    pos = 0
    n = len(buf)
    fields: List[Tuple[int, int, object]] = []
    while pos < n:
        tag, pos = _read_varint(buf, pos)
        fn = tag >> 3
        wt = tag & 7
        if wt == 0:
            val, pos = _read_varint(buf, pos)
            fields.append((fn, wt, val))
        elif wt == 2:
            ln, pos = _read_varint(buf, pos)
            if pos + ln > n:
                raise ValueError("Truncated length-delimited field")
            fields.append((fn, wt, buf[pos : pos + ln]))
            pos += ln
        elif wt == 5:
            if pos + 4 > n:
                raise ValueError("Truncated fixed32 field")
            fields.append((fn, wt, buf[pos : pos + 4]))
            pos += 4
        elif wt == 1:
            if pos + 8 > n:
                raise ValueError("Truncated fixed64 field")
            fields.append((fn, wt, buf[pos : pos + 8]))
            pos += 8
        else:
            raise ValueError(f"Unsupported protobuf wire type: {wt}")
    return fields


def decrypt_octo_manifest(manifest_path: Path) -> bytes:
    raw = manifest_path.read_bytes()
    if len(raw) < 17 or raw[0] != 0x01:
        raise ValueError(f"Invalid octocacheevai header: size={len(raw)}")
    cipher = Cipher(algorithms.AES(OCTO_KEY), modes.CBC(OCTO_IV))
    decryptor = cipher.decryptor()
    padded = decryptor.update(raw[1:]) + decryptor.finalize()
    unpadder = padding.PKCS7(128).unpadder()
    plaintext = unpadder.update(padded) + unpadder.finalize()
    if len(plaintext) < 16:
        raise ValueError("Decrypted manifest shorter than MD5 trailer")
    digest_expected = plaintext[:16]
    payload = plaintext[16:]
    if hashlib.md5(payload).digest() != digest_expected:
        raise ValueError("Manifest MD5 integrity check failed")
    return payload


def parse_target_bundles(pb_bytes: bytes, target_names: set[str]) -> Dict[str, dict]:
    found: Dict[str, dict] = {}
    for fn, wt, val in _parse_proto_fields(pb_bytes):
        if fn != 2 or wt != 2:
            continue
        info: dict = {}
        for sfn, swt, sval in _parse_proto_fields(val):  # type: ignore[arg-type]
            if sfn == 1 and swt == 0:
                info["id"] = int(sval)  # type: ignore[arg-type]
            elif sfn == 3 and swt == 2:
                info["name"] = sval.decode("utf-8", "replace")  # type: ignore[union-attr]
            elif sfn == 4 and swt == 0:
                info["size"] = int(sval)  # type: ignore[arg-type]
            elif sfn == 10 and swt == 2:
                info["md5"] = sval.decode("utf-8", "replace")  # type: ignore[union-attr]
        name = info.get("name")
        if name in target_names:
            found[name] = info
            if len(found) == len(target_names):
                break
    return found


def deobfuscate_bundle(raw_bytes: bytes, bundle_name: str) -> bytes:
    buf = bytearray(raw_bytes)
    if len(buf) < 24:
        raise ValueError(f"Bundle {bundle_name} too short: {len(buf)} bytes")
    nb = bundle_name.encode("ascii")
    L = len(nb)
    c0 = buf[0] ^ ord("U") ^ nb[0]
    limit = min(256, len(buf))
    for j in range(limit):
        i = j // 2
        if j % 2 == 0:
            k = nb[i % L] ^ c0
        else:
            k = nb[L - 1 - (i % L)] ^ (c0 ^ 0xFF)
        buf[j] ^= k
    if buf[:7] != b"UnityFS":
        raise ValueError(f"Bundle {bundle_name} failed UnityFS header check after XOR")
    # UnityFS 64-bit big-endian totalSize sits right after version strings at offset 0x18? Wait:
    # Let's parse the variable-length null-terminated strings in UnityFS header:
    # magic("UnityFS\0"), format(u32 BE), unity_ver(cstr), unity_rev(cstr), size(i64 BE).
    p = 8 + 4  # skip "UnityFS\0" + uint32 format
    for _ in range(2):
        nul = buf.find(0, p)
        if nul < 0:
            raise ValueError(f"Malformed UnityFS version strings in {bundle_name}")
        p = nul + 1
    total_size = struct.unpack(">Q", buf[p : p + 8])[0]
    if total_size != len(buf):
        raise ValueError(
            f"UnityFS totalSize mismatch for {bundle_name}: header={total_size}, actual={len(buf)}"
        )
    return bytes(buf)


def extract_portraits(game_root: Path, out_dir: Path) -> dict:
    manifest_path = game_root / "octo" / "pdb" / "400" / "705100" / "octocacheevai"
    if not manifest_path.is_file():
        raise FileNotFoundError(f"Octo manifest not found: {manifest_path}")

    UnityPy.config.FALLBACK_UNITY_VERSION = "2022.3.21f1"
    out_dir.mkdir(parents=True, exist_ok=True)

    pb_bytes = decrypt_octo_manifest(manifest_path)
    target_names = {bundle for _, bundle, _ in TARGETS}
    bundles = parse_target_bundles(pb_bytes, target_names)

    extracted = []
    for code, bundle_name, tier in TARGETS:
        if bundle_name not in bundles:
            raise KeyError(f"Target bundle '{bundle_name}' missing from Octo manifest")
        info = bundles[bundle_name]
        bid = info["id"]
        md5_hex = info["md5"]
        expected_size = info["size"]
        folder = ("A" + str(bid)).encode("ascii").hex().upper()
        bundle_path = game_root / "octo" / "v1" / "400" / str(bid % 10) / folder / md5_hex
        if not bundle_path.is_file():
            raise FileNotFoundError(f"Cached bundle file missing: {bundle_path}")

        raw = bundle_path.read_bytes()
        if len(raw) != expected_size:
            raise ValueError(f"Size mismatch for {bundle_name}: {len(raw)} != {expected_size}")
        if hashlib.md5(raw).hexdigest() != md5_hex:
            raise ValueError(f"MD5 mismatch for {bundle_name}")

        clean_bundle = deobfuscate_bundle(raw, bundle_name)
        env = UnityPy.load(clean_bundle)
        tex = None
        for obj in env.objects:
            if obj.type.name == "Texture2D":
                tex = obj.read()
                break
        if tex is None:
            raise RuntimeError(f"No Texture2D found in bundle {bundle_name}")

        img = tex.image
        if img.mode != "RGBA":
            img = img.convert("RGBA")

        out_png = out_dir / f"{code}.png"
        img.save(out_png, format="PNG")
        extracted.append(
            {
                "code": code,
                "bundle": bundle_name,
                "tier": tier,
                "id": bid,
                "width": img.width,
                "height": img.height,
                "mode": img.mode,
                "pngPath": str(out_png),
                "bytes": out_png.stat().st_size,
            }
        )

    meta = {
        "gameRoot": str(game_root),
        "manifestPath": str(manifest_path),
        "extracted": extracted,
    }
    (out_dir / "extracted_meta.json").write_text(
        json.dumps(meta, indent=2, ensure_ascii=False), encoding="utf-8"
    )
    return meta


def main() -> int:
    parser = argparse.ArgumentParser(description="Read-only Octo portrait extractor for E2E-3")
    parser.add_argument(
        "--game-root",
        default=os.environ.get("GAKUMOTION_GAME_ROOT", r"f:\Games\gakumas"),
        help="Read-only Gakumas installation root",
    )
    parser.add_argument(
        "--out-dir",
        required=True,
        help="Destination directory under build/e2e_refactor/portrait_bridge/",
    )
    args = parser.parse_args()

    meta = extract_portraits(Path(args.game_root), Path(args.out_dir))
    print(f"[octo_extract_helper] Extracted {len(meta['extracted'])} portraits to {args.out_dir}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
