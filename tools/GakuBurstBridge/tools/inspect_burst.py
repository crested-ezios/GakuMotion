#!/usr/bin/env python3
"""Read-only PE64/unwind/profile inspection. Does not load or execute the DLL."""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import sys
from pathlib import Path


class PEError(ValueError):
    pass


def number(value):
    return int(value, 0) if isinstance(value, str) else int(value)


class PE:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        if self.data[:2] != b"MZ":
            raise PEError("Missing MZ header")
        nt = self.unpack("<I", 0x3C)[0]
        if self.data[nt:nt + 4] != b"PE\0\0":
            raise PEError("Missing PE header")
        machine, count, timestamp, _, _, opt_size, characteristics = self.unpack("<HHIIIHH", nt + 4)
        opt = nt + 24
        if machine != 0x8664 or self.unpack("<H", opt)[0] != 0x20B:
            raise PEError("This inspector accepts AMD64 PE32+ only")
        self.image_base = self.unpack("<Q", opt + 24)[0]
        self.image_size = self.unpack("<I", opt + 56)[0]
        self.headers_size = self.unpack("<I", opt + 60)[0]
        self.timestamp, self.characteristics = timestamp, characteristics
        directories = self.unpack("<I", opt + 108)[0]
        self.directories = [self.unpack("<II", opt + 112 + 8 * i) for i in range(min(directories, 16))]
        self.sections = []
        for i in range(count):
            at = opt + opt_size + 40 * i
            name, virtual_size, rva, raw_size, raw, _, _, _, _, flags = self.unpack("<8sIIIIIIHHI", at)
            if raw + raw_size > len(self.data):
                raise PEError("Truncated section")
            self.sections.append(dict(name=name.rstrip(b"\0").decode("ascii", "replace"),
                                      rva=rva, virtual_size=virtual_size, raw_offset=raw,
                                      raw_size=raw_size, flags=flags))
        self.functions = self.runtime_functions()

    def unpack(self, fmt, offset):
        if offset < 0 or offset + struct.calcsize(fmt) > len(self.data):
            raise PEError(f"Out-of-range file offset {offset:#x}")
        return struct.unpack_from(fmt, self.data, offset)

    def offset(self, rva, size=1):
        if 0 <= rva and rva + size <= min(self.headers_size, len(self.data)):
            return rva
        for s in self.sections:
            delta = rva - s["rva"]
            if 0 <= delta and delta + size <= s["raw_size"]:
                return s["raw_offset"] + delta
        raise PEError(f"RVA {rva:#x} has no {size}-byte on-disk mapping")

    def read(self, rva, size):
        at = self.offset(rva, size)
        return self.data[at:at + size]

    def string(self, rva):
        at = self.offset(rva)
        end = self.data.find(b"\0", at, min(at + 4096, len(self.data)))
        if end < 0:
            raise PEError("Unterminated PE string")
        return self.data[at:end].decode("ascii", "replace")

    def runtime_functions(self):
        rva, size = self.directories[3] if len(self.directories) > 3 else (0, 0)
        if not rva or not size:
            return []
        if size % 12:
            raise PEError("Exception directory is not an AMD64 RUNTIME_FUNCTION array")
        result = []
        for begin, end, unwind in struct.iter_unpack("<III", self.read(rva, size)):
            if begin >= end:
                continue
            result.append(dict(begin_rva=begin, end_rva=end, unwind_rva=unwind))
        return result

    def containing(self, rva):
        return next((f for f in self.functions if f["begin_rva"] <= rva < f["end_rva"]), None)

    def exports(self):
        rva, size = self.directories[0] if self.directories else (0, 0)
        if not rva:
            return []
        fields = struct.unpack("<IIHHIIIIIII", self.read(rva, 40))
        _, _, _, _, _, ordinal_base, count, named, funcs, names, ordinals = fields
        if max(count, named) > 1000000:
            raise PEError("Unreasonable export count")
        result = []
        for i in range(named):
            name_rva = struct.unpack("<I", self.read(names + 4 * i, 4))[0]
            ordinal_index = struct.unpack("<H", self.read(ordinals + 2 * i, 2))[0]
            if ordinal_index >= count:
                raise PEError("Bad export ordinal index")
            target = struct.unpack("<I", self.read(funcs + 4 * ordinal_index, 4))[0]
            result.append(dict(name=self.string(name_rva), ordinal=ordinal_base + ordinal_index,
                               rva=target, forwarded=(rva <= target < rva + size)))
        return result

    def indirect_call(self, return_rva):
        code = self.read(return_rva - 6, 6)
        if code[:2] != b"\xff\x15":
            return dict(return_rva=return_rva, bytes=code.hex(" "), supported=False)
        displacement = struct.unpack("<i", code[2:])[0]
        slot = return_rva + displacement
        try:
            slot_value = struct.unpack("<Q", self.read(slot, 8))[0]
            slot_backed = True
        except PEError:
            # Runtime import/function slots may live in zero-initialized .bss.
            # There is no on-disk callee address to read or to infer.
            slot_value, slot_backed = None, False
        return dict(return_rva=return_rva, instruction_rva=return_rva - 6,
                    bytes=code.hex(" "), slot_rva=slot, supported=True,
                    slot_file_value=slot_value, slot_file_backed=slot_backed)

    def scan_slot(self, slot):
        result = []
        for s in self.sections:
            if not s["flags"] & 0x20000000:  # IMAGE_SCN_MEM_EXECUTE
                continue
            data = self.data[s["raw_offset"]:s["raw_offset"] + s["raw_size"]]
            at = data.find(b"\xff\x15")
            while at >= 0:
                if at + 6 <= len(data):
                    return_rva = s["rva"] + at + 6
                    displacement = struct.unpack_from("<i", data, at + 2)[0]
                    if return_rva + displacement == slot:
                        result.append(dict(call=self.indirect_call(return_rva),
                                           unwind=self.containing(return_rva - 1)))
                at = data.find(b"\xff\x15", at + 1)
        return result


def check_profile(pe, profile, sha):
    checks = [dict(name="sha256", ok=sha.lower() == profile["burst_sha256"].lower(),
                   actual=sha, expected=profile["burst_sha256"])]
    for variant in profile["variants"]:
        label, entry, caller = variant["name"], number(variant["entry_rva"]), number(variant["delta_return_rva"])
        prefix = bytes.fromhex(variant["entry_prefix"])
        try:
            actual = pe.read(entry, len(prefix))
            checks.append(dict(name=f"{label}.entry_prefix", ok=actual == prefix, actual=actual.hex(" "), expected=prefix.hex(" ")))
            unwind = pe.containing(caller - 1)
            expected_end = number(variant["end_rva"]) if "end_rva" in variant else None
            checks.append(dict(name=f"{label}.unwind_entry",
                               ok=bool(unwind and unwind["begin_rva"] == entry and
                                       (expected_end is None or unwind["end_rva"] == expected_end)),
                               actual=unwind, expected=dict(begin_rva=entry, end_rva=expected_end)))
            call = pe.indirect_call(caller)
            checks.append(dict(name=f"{label}.delta_slot", ok=call.get("slot_rva") == number(profile["delta_slot_rva"]), actual=call,
                               expected=number(profile["delta_slot_rva"])))
        except PEError as error:
            checks.append(dict(name=f"{label}.mapping", ok=False, actual=str(error), expected="mapped reference entry and call"))
    return checks


def traces(path, pe):
    result = []
    for line in Path(path).read_text(encoding="utf-8-sig", errors="replace").splitlines():
        match = re.search(r"caller=([0-9A-Fa-fx]+).*?image=([0-9A-Fa-fx]+).*?rva=(0x[0-9a-fA-F]+).*?begin=([0-9A-Fa-fx]+)", line)
        if not match:
            continue
        caller, image, rva, begin = (int(v, 16) for v in match.groups())
        record = dict(caller_rva=rva, traced_begin_rva=begin - image,
                      address_arithmetic_ok=(caller - image == rva), unwind=pe.containing(rva - 1))
        if record not in result:
            result.append(record)
    return result


def native_traces(path, pe):
    document = json.loads(Path(path).read_text(encoding="utf-8-sig"))
    if document.get("schema_version") != 1 or document.get("api_version") != 2:
        raise PEError("Unsupported native trace schema / API version")
    result = []
    for trace in document.get("traces", []):
        caller = number(trace["caller_rva"])
        begin, end = number(trace["function_begin_rva"]), number(trace["function_end_rva"])
        unwind = pe.containing(caller - 1)
        try:
            call = pe.indirect_call(caller)
        except PEError as error:
            call = dict(return_rva=caller, supported=False, error=str(error))
        result.append(dict(caller_rva=caller, traced_begin_rva=begin, traced_end_rva=end,
                           unwind=unwind,
                           unwind_matches=bool(unwind and unwind["begin_rva"] == begin and unwind["end_rva"] == end),
                           call=call, calls=trace.get("calls"), delta_min=trace.get("delta_min"), delta_max=trace.get("delta_max")))
    return dict(source_profile=document.get("profile"), source_state=document.get("state"),
                source_result=document.get("last_result"), trace_capacity=document.get("trace_capacity"),
                dropped_traces=document.get("dropped_traces"), delta_probe_calls=document.get("delta_probe_calls"),
                queries=result, warning="Live trace and disk DLL must come from the same client version. Any missing unwind or unsupported call shape requires manual review.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--profile", type=Path, default=Path(__file__).resolve().parents[1] / "profiles/supported-build-20261008.json")
    parser.add_argument("--caller-rva", action="append", type=number, default=[] , help="Return address RVA, not CALL instruction RVA")
    parser.add_argument("--scan-slot-rva", type=number, help="Read-only FF 15 scan; candidates are not automatically identified as Swing")
    parser.add_argument("--trace-log", type=Path)
    parser.add_argument("--trace-json", type=Path, help="GakuBurstBridge API v2 burst_trace.json, obtained with Recorder PROBE")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        pe = PE(args.dll)
        sha = hashlib.sha256(pe.data).hexdigest()
        profile = json.loads(args.profile.read_text(encoding="utf-8-sig"))
        checks = check_profile(pe, profile, sha)
        report = dict(tool="inspect_burst", input=str(args.dll.resolve()), sha256=sha,
                      file_size=len(pe.data), image_base=pe.image_base, image_size=pe.image_size,
                      timestamp=pe.timestamp, sections=pe.sections,
                      runtime_function_count=len(pe.functions), exports=pe.exports(),
                      profile_id=profile["id"], profile_checks=checks,
                      profile_matches=all(c["ok"] for c in checks),
                      caller_queries=[dict(call=pe.indirect_call(rva), unwind=pe.containing(rva - 1)) for rva in args.caller_rva],
                      warning="On-disk call slot contents do not prove the runtime callee. No DLL was loaded. No injection is authorized by this report.")
        if args.scan_slot_rva is not None:
            report["slot_scan"] = pe.scan_slot(args.scan_slot_rva)
        if args.trace_log:
            report["live_trace_queries"] = traces(args.trace_log, pe)
        if args.trace_json:
            report["native_trace_queries"] = native_traces(args.trace_json, pe)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print(f"{args.output}: profile_matches={report['profile_matches']}; exports={len(report['exports'])}; unwind={len(pe.functions)}")
        return 0 if report["profile_matches"] else 2
    except (OSError, ValueError, KeyError, struct.error) as error:
        print(f"Inspection failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
