"""Synthetic offline PE checks; no installed game or third-party packages needed."""

import contextlib
import importlib.util
import io
from pathlib import Path
import struct
import tempfile
import unittest


ROOT = Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("verify_game_build", ROOT / "scripts/verify-game-build.py")
verifier = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verifier)


def fixture():
    data = bytearray(0xe00)
    base = 0x140000000

    def put(offset, fmt, *values):
        struct.pack_into(fmt, data, offset, *values)

    data[:2] = b"MZ"
    put(0x3c, "<I", 0x80)
    data[0x80:0x84] = b"PE\0\0"
    put(0x84, "<HH", 0x8664, 3)
    put(0x94, "<H", 240)
    put(0x98, "<H", 0x20b)
    put(0xb0, "<Q", base)
    put(0x104, "<I", 16)
    put(0x110, "<II", 0x2200, 40)  # Import directory.
    put(0x120, "<II", 0x3000, 12)  # Exception directory.
    for index, (name, virtual, rva, size, raw, flags) in enumerate([
        (b".text", 0x200, 0x1000, 0x200, 0x400, 0x60000020),
        (b".rdata", 0x800, 0x2000, 0x600, 0x600, 0x40000040),
        (b".pdata", 0x200, 0x3000, 0x200, 0xc00, 0x40000040),
    ]):
        offset = 0x188 + index * 40
        data[offset:offset + 8] = name.ljust(8, b"\0")
        put(offset + 8, "<IIII", virtual, rva, size, raw)
        put(offset + 36, "<I", flags)
    data[0x400:0x600] = b"\xcc" * 0x200
    data[0x400:0x405] = b"\x48\x83\xec\x28\xc3"
    data[0x420:0x424] = b"\x48\x8b\xc2\xc3"
    put(0xc00, "<III", 0x1000, 0x1010, 0x21f0)
    # ZLobbyIdle vtable, COL, type descriptor, and one-element base hierarchy.
    put(0x600, "<QQ", base + 0x2040, base + 0x1000)
    put(0x640, "<6I", 1, 0, 0, 0x2080, 0x20c0, 0x2040)
    name = b".?AVZLobbyIdle@@\0"
    data[0x690:0x690 + len(name)] = name
    put(0x6c0, "<4I", 0, 0, 1, 0x20e0)
    put(0x6e0, "<I", 0x2100)
    put(0x700, "<I", 0x2080)
    # Import lookup table and IAT deliberately live at different RVAs.
    put(0x800, "<5I", 0x2240, 0, 0, 0x2280, 0x2260)
    put(0x840, "<QQ", 0x22a0, 0)
    put(0x860, "<QQ", 0x22a0, 0)
    dll = b"steam_api64.dll\0"
    data[0x880:0x880 + len(dll)] = dll
    symbol = b"SteamAPI_RunCallbacks\0"
    data[0x8a2:0x8a2 + len(symbol)] = symbol
    return data


class VerificationTests(unittest.TestCase):
    def image(self, data=None):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        path = Path(directory.name) / "synthetic.exe"
        path.write_bytes(fixture() if data is None else data)
        return verifier.Image(path)

    def test_function_start_and_leaf_boundary(self):
        image = self.image()
        self.assertEqual(image.code_start(0x1000), ".pdata function start")
        self.assertIn("generic leaf", image.code_start(0x1020))
        for address in (0x1004, 0x1023, 0x1030, 0x2000):
            with self.subTest(address=address), self.assertRaises(ValueError):
                image.code_start(address)

    def test_rakpeer_startup_requires_is_active_prologue(self):
        with self.assertRaisesRegex(ValueError, "IsActive"):
            verifier.check_rakpeer_startup(self.image(), 0x1000)
        data = fixture()
        data[0x400:0x40c] = b"\xff\x50\x78\x84\xc0\x74\x13\xb8\x01\x00\x00\x00"
        self.assertIn("IsActive prologue", verifier.check_rakpeer_startup(self.image(data), 0x1000))

    def test_rtti_class_and_subobject_are_both_required(self):
        image = self.image()
        self.assertIn("ZLobbyIdle", verifier.check_table(image, "kLobbyIdleVtable", 0x2008))
        with self.assertRaisesRegex(ValueError, "expected ZLobbyJoin"):
            verifier.check_table(image, "kLobbyJoinVtable", 0x2008)
        data = fixture()
        struct.pack_into("<I", data, 0x644, 8)
        with self.assertRaisesRegex(ValueError, "COL offset"):
            verifier.check_table(self.image(data), "kLobbyIdleVtable", 0x2008)

    def test_corrupt_col_self_reference_is_rejected(self):
        data = fixture()
        struct.pack_into("<I", data, 0x654, 0x2041)
        with self.assertRaisesRegex(ValueError, "complete object locator"):
            self.image(data).rtti(0x2008)

    def test_iat_resolves_name_but_lookup_table_is_not_an_iat_slot(self):
        image = self.image()
        self.assertEqual(image.import_at(0x2260), ("steam_api64.dll", "SteamAPI_RunCallbacks"))
        for address in (0x2240, 0x2261, 0x2268):
            with self.subTest(address=address), self.assertRaises(ValueError):
                image.import_at(address)

    def test_ordinal_import_is_rejected(self):
        data = fixture()
        struct.pack_into("<Q", data, 0x840, (1 << 63) | 42)
        with self.assertRaisesRegex(ValueError, "ordinal"):
            self.image(data).import_at(0x2260)

    def test_virtual_zero_fill_is_not_read_as_file_bytes(self):
        image = self.image()
        self.assertEqual(image.section(0x2600)[0], ".rdata")
        with self.assertRaisesRegex(ValueError, "not file-backed"):
            image.bytes(0x2600, 1)

    def test_current_source_parses_without_silently_dropping_hooks(self):
        for store, count in (("epic", 11), ("steam", 8)):
            text = verifier.source(ROOT / f"src/{store}/{store}_plugin.cpp")
            self.assertEqual(len(verifier.hooks(text)), count)
            with self.assertRaisesRegex(ValueError, "unsupported hook"):
                verifier.hooks(text.replace("rva::kLobbyIdleVtable,", "rva::kLobbyIdleVtable + 8,"))

    def test_backend_alternatives_are_read_from_cpp(self):
        text = verifier.source(ROOT / "src/backend/backend_patch.cpp")
        for store in ("epic", "steam"):
            plan = verifier.backend_plan(text, store)
            self.assertEqual(set(plan), {"domain", "auth1", "auth2", "pin", "format", "length", "dynamic"})
            self.assertIn(b"https://{0}\0", plan["format"][1])
            self.assertIn(b"http://{0}\0\0", plan["format"][1])
            self.assertEqual(plan["dynamic"][1], [b"\0", b"\1"])
        changed = text.replace("{0x75, 0x08}", "{0x75, 0x09}")
        self.assertEqual(verifier.backend_plan(changed, "epic")["auth1"][1][0], b"\x75\x09")

    def test_cli_requires_explicit_input_and_reports_missing_file(self):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as error:
            verifier.main([])
        self.assertEqual(error.exception.code, 2)
        with contextlib.redirect_stdout(io.StringIO()) as output:
            result = verifier.main(["--epic", str(ROOT / "tests/nonexistent-game.exe")])
        self.assertEqual(result, 1)
        self.assertIn("FAIL [epic]", output.getvalue())


if __name__ == "__main__":
    unittest.main()
