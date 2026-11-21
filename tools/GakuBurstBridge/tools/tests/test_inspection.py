import contextlib
import hashlib
import importlib.util
import io
import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]


def module(name):
    spec = importlib.util.spec_from_file_location(name, TOOLS / f"{name}.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


burst = module("inspect_burst")
metadata = module("inspect_metadata")


def fixture():
    data = bytearray(0x800)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHIIIHH", data, 0x84, 0x8664, 3, 0, 0, 0, 240, 0x2022)
    opt = 0x98
    struct.pack_into("<H", data, opt, 0x20B)
    struct.pack_into("<Q", data, opt + 24, 0x180000000)
    struct.pack_into("<I", data, opt + 56, 0x4000)
    struct.pack_into("<I", data, opt + 60, 0x200)
    struct.pack_into("<I", data, opt + 108, 16)
    struct.pack_into("<II", data, opt + 112 + 3 * 8, 0x2000, 12)
    table = opt + 240
    for i, section in enumerate(((b".text", 0x200, 0x1000, 0x200, 0x200, 0x60000020),
                                  (b".pdata", 0x200, 0x2000, 0x200, 0x400, 0x40000040),
                                  (b".bss", 0x200, 0x3000, 0, 0, 0xC0000080))):
        name, vs, rva, size, raw, flags = section
        struct.pack_into("<8sIIIIIIHHI", data, table + 40 * i, name, vs, rva, size, raw, 0, 0, 0, 0, flags)
    data[0x200:0x203] = b"\x55\x41\x57"
    data[0x210:0x212] = b"\xff\x15"
    struct.pack_into("<i", data, 0x212, 0x3000 - 0x1016)
    struct.pack_into("<III", data, 0x400, 0x1000, 0x1100, 0x2100)
    return data


class PEInspectionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.path = Path(self.temp.name) / "fixture.dll"
        self.path.write_bytes(fixture())
        self.pe = burst.PE(self.path)

    def tearDown(self):
        self.temp.cleanup()

    def test_rva_is_mapped_to_raw_offset(self):
        self.assertEqual(self.pe.offset(0x1010), 0x210)
        self.assertEqual(self.pe.read(0x1000, 3), b"\x55\x41\x57")

    def test_bss_slot_is_unknown_not_fake_callee(self):
        call = self.pe.indirect_call(0x1016)
        self.assertEqual(call["slot_rva"], 0x3000)
        self.assertIsNone(call["slot_file_value"])
        self.assertFalse(call["slot_file_backed"])

    def test_unwind_end_is_exclusive(self):
        self.assertEqual(self.pe.containing(0x1015)["begin_rva"], 0x1000)
        self.assertIsNone(self.pe.containing(0x1100))

    def test_scan_uses_signed_displacement_and_reports_candidate(self):
        self.assertEqual(len(self.pe.scan_slot(0x3000)), 1)
        self.assertEqual(self.pe.scan_slot(0x3000)[0]["call"]["return_rva"], 0x1016)
        self.assertEqual(self.pe.scan_slot(0x3001), [])

    def test_unsupported_call_opcode_is_rejected(self):
        self.assertFalse(self.pe.indirect_call(0x1020)["supported"])

    def test_truncated_pe_is_rejected(self):
        self.path.write_bytes(b"MZ")
        with self.assertRaises(burst.PEError):
            burst.PE(self.path)

    def test_profile_mismatch_is_data_not_bypassed(self):
        profile = dict(burst_sha256="wrong", delta_slot_rva="0x3000",
                       variants=[dict(name="sample", entry_rva="0x1000", delta_return_rva="0x1016", entry_prefix="55 41 57")])
        checks = burst.check_profile(self.pe, profile, "actual")
        self.assertFalse(checks[0]["ok"])
        self.assertTrue(all(c["ok"] for c in checks[1:]))

    def test_changed_mapping_is_profile_mismatch_not_read_failure(self):
        profile = dict(burst_sha256="actual", delta_slot_rva="0x3000",
                       variants=[dict(name="sample", entry_rva="0x9000", delta_return_rva="0x9016", entry_prefix="55")])
        checks = burst.check_profile(self.pe, profile, "actual")
        self.assertFalse(checks[-1]["ok"])

    def test_native_trace_requires_matching_unwind_and_reports_actual_slot(self):
        trace_file = Path(self.temp.name) / "trace.json"
        trace_file.write_text(json.dumps(dict(schema_version=1, api_version=2, traces=[dict(
            caller_rva="0x1016", function_begin_rva="0x1000", function_end_rva="0x1100", calls=10)])), encoding="utf-8")
        row = burst.native_traces(trace_file, self.pe)["queries"][0]
        self.assertTrue(row["unwind_matches"])
        self.assertEqual(row["call"]["slot_rva"], 0x3000)

    def test_native_trace_wrong_schema_is_rejected(self):
        trace_file = Path(self.temp.name) / "trace.json"
        trace_file.write_text('{"schema_version":2,"api_version":2}', encoding="utf-8")
        with self.assertRaises(burst.PEError):
            burst.native_traces(trace_file, self.pe)


class MetadataInspectionTests(unittest.TestCase):
    def execute(self, probe):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source, output = root / "probe.json", root / "report.json"
            source.write_text(json.dumps(probe), encoding="utf-8")
            with patch.object(sys, "argv", ["inspect_metadata", "--probe", str(source), "--output", str(output)]), contextlib.redirect_stdout(io.StringIO()):
                code = metadata.main()
            return code, json.loads(output.read_text(encoding="utf-8"))

    def test_missing_sizes_are_unknown_never_approved(self):
        code, report = self.execute({"discovered_classes": []})
        self.assertEqual(code, 0)
        self.assertEqual(report["unknown_count"], 4)
        self.assertFalse(report["write_approval"])

    def test_boxed_value_field_is_converted(self):
        code, report = self.execute({"discovered_classes": [{"full_name": "ActorAnimation.CampusActorAnimationJob",
             "fields": [{"name": "_quartzDriverHairJobBones", "offset": 96}]}]})
        check = next(c for c in report["checks"] if c["name"].endswith("._quartzDriverHairJobBones"))
        self.assertEqual(code, 0)
        self.assertEqual(check["observed_unboxed"], 80)
        self.assertEqual(check["status"], "match")

    def test_reference_type_field_is_not_unboxed(self):
        code, report = self.execute({"discovered_classes": [{"full_name": "ActorAnimation.CampusActorAnimationRig",
             "fields": [{"name": "_animator", "offset": 312}]}]})
        self.assertEqual(code, 0)
        self.assertEqual(report["classes"][0]["fields"][0]["offset"], 312)
        self.assertFalse(any("_animator" in c["name"] for c in report["checks"]))

    def test_changed_offset_is_explicit_mismatch(self):
        code, report = self.execute({"discovered_classes": [{"full_name": "ActorAnimation.CampusActorAnimationJob",
             "fields": [{"name": "_quartzDriverHairJobBones", "offset": 100}]}]})
        self.assertEqual(code, 2)
        self.assertEqual(report["mismatch_count"], 1)


if __name__ == "__main__":
    unittest.main()
