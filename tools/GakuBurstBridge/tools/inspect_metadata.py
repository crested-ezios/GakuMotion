#!/usr/bin/env python3
"""Summarize local GakuRecorder JSON metadata/probe evidence without game access."""
from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path

TARGETS = {
    "UnityEngine.Animations.AnimationStream": "stream_fields_unboxed",
    "UnityEngine.Animations.TransformStreamHandle": "handle_fields_unboxed",
    "ActorAnimation.CampusActorAnimationJob": "job_fields_unboxed",
    "ActorAnimation.ActorAnimationSwingJobSkeleton": "swing_fields_unboxed",
    "ActorAnimation.CampusActorAnimationRig": None,
    "UnityEngine.Animations.AnimatorJobExtensions": None,
}


def walk(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from walk(child)
    elif isinstance(value, list):
        for child in value:
            yield from walk(child)


def typename(row):
    name = row.get("full_name", row.get("fullName"))
    if name:
        return name
    if row.get("namespace") and row.get("name"):
        return row["namespace"] + "." + row["name"]
    return row.get("name", "")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--probe", type=Path, help="Recorder actor_pipeline_report.json or expanded pipeline report")
    parser.add_argument("--index", type=Path, help="Recorder il2cpp_index.json; names/method shapes only")
    parser.add_argument("--abi-json", type=Path, help="Explicit runtime observations; see docs/MAINTENANCE_SOP.md")
    parser.add_argument("--profile", type=Path, default=Path(__file__).resolve().parents[1] / "profiles/supported-build-20261008.json")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not any((args.probe, args.index, args.abi_json)):
        parser.error("Provide --probe, --index or --abi-json")
    try:
        profile = json.loads(args.profile.read_text(encoding="utf-8-sig"))
        report = dict(tool="inspect_metadata", profile_id=profile["id"], sources=[], classes=[], checks=[],
                      warning="Names, fields and methods are evidence only. Missing runtime value sizes are unknown, not confirmed. Never subtract a boxed header from reference-type instance fields.")
        if args.probe:
            raw = args.probe.read_bytes()
            probe = json.loads(raw.decode("utf-8-sig"))
            report["sources"].append(dict(kind="probe", path=str(args.probe.resolve()), sha256=hashlib.sha256(raw).hexdigest()))
            for row in walk(probe):
                name = typename(row)
                if name not in TARGETS:
                    continue
                selected = dict(full_name=name, assembly=row.get("assembly", row.get("image")),
                                base_class=row.get("base_class"), fields=row.get("fields", []),
                                methods=row.get("methods", []))
                report["classes"].append(selected)
                group = TARGETS[name]
                if not group:
                    continue
                fields = {f.get("name"): f for f in selected["fields"]}
                for field, expected in profile["abi"][group].items():
                    actual = fields.get(field, {}).get("offset")
                    unboxed = actual - profile["abi"]["boxed_header_size"] if isinstance(actual, int) else None
                    report["checks"].append(dict(name=f"{name}.{field}", expected_unboxed=expected,
                                                  observed_boxed=actual, observed_unboxed=unboxed,
                                                  status="unknown" if actual is None else "match" if unboxed == expected else "mismatch"))
        if args.index:
            raw = args.index.read_bytes()
            index = json.loads(raw.decode("utf-8-sig"))
            report["sources"].append(dict(kind="index", path=str(args.index.resolve()), sha256=hashlib.sha256(raw).hexdigest()))
            types = index.get("types", [])
            for row in types:
                name = typename(row)
                if name not in TARGETS:
                    continue
                index_number = row.get("index")
                methods = [dict(name=m.get("name"), parameter_count=m.get("parameterCount", m.get("parameter_count")))
                           for m in index.get("methods", []) if m.get("declaringType") == index_number]
                report.setdefault("index_types", []).append(dict(full_name=name, image=row.get("image"), methods=methods))
        observed = {}
        if args.abi_json:
            raw = args.abi_json.read_bytes()
            document = json.loads(raw.decode("utf-8-sig"))
            observed = document.get("observed", {})
            report["sources"].append(dict(kind="runtime_abi", path=str(args.abi_json.resolve()),
                                           sha256=hashlib.sha256(raw).hexdigest(), observation_source=document.get("observation_source")))
        for key in ("pointer_size", "boxed_header_size", "stream_value_size", "handle_value_size"):
            actual = observed.get(key)
            expected = profile["abi"][key]
            report["checks"].append(dict(name=key, actual=actual, expected=expected,
                                          status="unknown" if actual is None else "match" if actual == expected else "mismatch"))
        for group in ("stream_fields_unboxed", "handle_fields_unboxed", "job_fields_unboxed", "swing_fields_unboxed"):
            if group not in observed:
                continue
            for field, expected in profile["abi"][group].items():
                actual = observed[group].get(field)
                report["checks"].append(dict(name=f"runtime.{group}.{field}", actual=actual, expected=expected,
                                              status="unknown" if actual is None else "match" if actual == expected else "mismatch"))
        report["mismatch_count"] = sum(c["status"] == "mismatch" for c in report["checks"])
        report["unknown_count"] = sum(c["status"] == "unknown" for c in report["checks"])
        report["write_approval"] = False
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"{args.output}: mismatches={report['mismatch_count']}; unknown={report['unknown_count']}; writing is never approved by this tool")
        return 2 if report["mismatch_count"] else 0
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Inspection failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
