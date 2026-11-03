#!/usr/bin/env python3
"""Verify source RVAs against explicitly supplied, offline x64 game PE files.

Usage: python scripts/verify-game-build.py --epic PATH --steam PATH
Either storefront may be omitted. Nothing is executed, patched, or discovered
automatically. A successful code-start check establishes structural plausibility,
not the semantic identity of an unhooked helper. Runtime-only object fields and
initialized backend configuration cannot be verified from an installed image.
"""

import argparse
import ast
import bisect
import hashlib
import importlib.util
from pathlib import Path
import re
import struct
import sys


ROOT = Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("inspect_pe", Path(__file__).with_name("inspect-pe.py"))
_inspect_pe = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_inspect_pe)


def require(condition, message):
    if not condition:
        raise ValueError(message)


def source(path):
    # Preserve strings (notably URLs), while removing C++ comments.
    return re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*.*?\*/',
                  lambda m: m[0] if m[0].startswith('"') else " ",
                  path.read_text(encoding="utf-8"), flags=re.S)


def number(text):
    text = text.strip()
    require(re.fullmatch(r"(?:0[xX][0-9a-fA-F]+|[0-9]+)", text),
            f"unsupported integer expression: {text!r}")
    return int(text, 16 if text.lower().startswith("0x") else 10)


def offsets(text):
    declarations = re.findall(r"constexpr\s+uintptr_t\s+(\w+)\s*=\s*([^;]+);", text)
    require(declarations, "no constexpr uintptr_t offsets found")
    result = {name: number(value) for name, value in declarations}
    require(len(result) == len(declarations), "duplicate offset declarations")
    return result


def hooks(text):
    arrays = re.findall(r"const\s+Spec\s+specs\s*\[\s*\]\s*=\s*\{(.*?)\};", text, re.S)
    require(len(arrays) == 1, "expected exactly one hook Spec array")
    rows = re.findall(r"\{([^{}]+)\}", arrays[0])
    require(rows, "empty hook Spec array")
    residue = re.sub(r"\{[^{}]+\}", "", arrays[0]).replace(",", "").strip()
    require(not residue, "unparsed content in hook Spec array")
    result = []
    for row in rows:
        match = re.fullmatch(
            r"\s*g_hooks\.(\w+)\s*,\s*rva::(\w+)\s*,\s*(\d+)\s*,"
            r"\s*rva::(\w+)\s*,\s*&\w+\s*", row)
        require(match, f"unsupported hook specification: {row.strip()}")
        name, table, slot, expected = match.groups()
        result.append((name, table, int(slot), expected))
    return result


class Image(_inspect_pe.PE):
    def __init__(self, path):
        super().__init__(path)
        require(self.data[:2] == b"MZ", "missing DOS signature")
        nt = self.u32(0x3c)
        require(self.u16(nt + 4) == 0x8664, "expected AMD64 PE")
        self.optional = nt + 24
        require(self.u16(self.optional) == 0x20b, "expected PE32+")
        require(self.u16(nt + 20) >= 112, "truncated optional header")
        self.directory_count = self.u32(self.optional + 108)
        require(112 + self.directory_count * 8 <= self.u16(nt + 20), "truncated data directories")
        table = self.optional + self.u16(nt + 20)
        self.flags = [self.u32(table + i * 40 + 36) for i in range(len(self.sections))]
        for _, _, _, size, raw in self.sections:
            require(raw + size <= len(self.data), "section extends beyond file")
        self.functions = []
        rva, size = self.directory(3)
        require(rva and size and size % 12 == 0, "missing or malformed exception directory")
        require(self.section(rva)[0] == ".pdata", "exception directory is not in .pdata")
        for begin, end, unwind in struct.iter_unpack("<III", self.bytes(rva, size)):
            if not (begin or end or unwind):
                continue
            require(begin < end and unwind, "invalid RUNTIME_FUNCTION entry")
            self.functions.append((begin, end))
        require(self.functions == sorted(self.functions), "unsorted .pdata functions")
        self.starts = [begin for begin, _ in self.functions]

    def directory(self, index):
        if index >= self.directory_count:
            return 0, 0
        return struct.unpack_from("<II", self.data, self.optional + 112 + index * 8)

    def section(self, rva, size=1):
        for section in self.sections:
            _, virtual, start, raw_size, _ = section
            if start <= rva and rva + size <= start + max(virtual, raw_size):
                return section
        raise ValueError(f"RVA 0x{rva:x} (+{size}) is outside mapped sections")

    def bytes(self, rva, size):
        _, _, start, raw_size, raw = self.section(rva, size)
        require(rva + size <= start + raw_size, f"RVA 0x{rva:x} (+{size}) is not file-backed")
        offset = raw + rva - start
        return self.data[offset:offset + size]

    def initial_data(self, rva, size):
        """File bytes plus loader zero-fill, never an observation of runtime data."""
        _, virtual, start, raw_size, _ = self.section(rva, size)
        require(not self.executable(rva), "expected non-executable data")
        require(rva + size <= start + virtual, "data extends beyond section VirtualSize")
        backed = min(size, max(0, start + raw_size - rva))
        value = self.bytes(rva, backed) if backed else b""
        return value + bytes(size - backed), backed != size

    def code_range(self, start, end):
        require(start < end, "empty/reversed code range")
        name, virtual, section_start, _, _ = self.section(start, end - start)
        require(name == ".text" and self.executable(start) and start == section_start
                and end == section_start + virtual,
                "code range must equal .text VirtualSize, excluding raw padding")
        return f"0x{start:x}..0x{end:x} (.text VirtualSize)"

    def unpack(self, fmt, rva):
        return struct.unpack(fmt, self.bytes(rva, struct.calcsize(fmt)))

    def cstring(self, rva, limit=1024):
        chars = bytearray()
        for i in range(limit):
            byte = self.bytes(rva + i, 1)[0]
            if not byte:
                return chars.decode("ascii")
            chars.append(byte)
        raise ValueError(f"unterminated string at RVA 0x{rva:x}")

    def executable(self, rva):
        return bool(self.flags[self.sections.index(self.section(rva))] & 0x20000000)

    def code_boundary(self, rva):
        require(self.executable(rva), f"RVA 0x{rva:x} is not executable")
        index = bisect.bisect_right(self.starts, rva) - 1
        if index >= 0:
            begin, end = self.functions[index]
            if begin == rva:
                return ".pdata function start"
            require(rva >= end, f"RVA 0x{rva:x} is inside function 0x{begin:x}..0x{end:x}")
        require(rva % 16 == 0 and self.bytes(rva - 1, 1) == b"\xcc",
                f"RVA 0x{rva:x} lacks .pdata entry or aligned/padded leaf boundary")
        return None

    def code_start(self, rva):
        boundary = self.code_boundary(rva)
        if boundary:
            return boundary
        # Deliberately narrow, complete instruction sequences for generic leaves.
        # Arbitrary executable bytes or an interior RET are not accepted.
        leaves = {
            b"\xc3": "ret",
            b"\x32\xc0\xc3": "xor al,al; ret",
            b"\x33\xc0\xc3": "xor eax,eax; ret",
            b"\x31\xc0\xc3": "xor eax,eax; ret",
            b"\xb0\x01\xc3": "mov al,1; ret",
            b"\xb8\x01\x00\x00\x00\xc3": "mov eax,1; ret",
            b"\x48\x8b\xc1\xc3": "mov rax,rcx; ret",
            b"\x48\x8b\xc2\xc3": "mov rax,rdx; ret",
            b"\x48\x89\xd0\xc3": "mov rax,rdx; ret",
        }
        for pattern, description in leaves.items():
            if self.bytes(rva, len(pattern)) == pattern:
                return f"generic leaf ({description})"
        raise ValueError(f"RVA 0x{rva:x} is not a recognized generic leaf: {self.bytes(rva, 12).hex(' ')}")

    def rtti(self, table):
        require(not self.executable(table), "vtable is in an executable section")
        col = self.unpack("<Q", table - 8)[0] - self.base
        signature, displacement, _, descriptor, hierarchy, self_rva = self.unpack("<6I", col)
        require(signature == 1 and self_rva == col, "invalid x64 complete object locator")
        name = self.cstring(descriptor + 16)
        require(name.startswith((".?AV", ".?AU")), f"invalid RTTI type name {name!r}")
        _, _, count, array = self.unpack("<4I", hierarchy)
        require(0 < count < 1024, "invalid RTTI base count")
        bases = []
        for i in range(count):
            base_descriptor = self.unpack("<I", array + i * 4)[0]
            type_descriptor = self.unpack("<I", base_descriptor)[0]
            bases.append(self.cstring(type_descriptor + 16))
        require(name in bases, "RTTI hierarchy omits complete type")
        return name, displacement, bases

    def import_at(self, target):
        rva, size = self.directory(1)
        require(rva and size >= 20, "missing import directory")
        for delta in range(0, size - 19, 20):
            lookup, timestamp, forwarder, name_rva, iat = self.unpack("<5I", rva + delta)
            if not any((lookup, timestamp, forwarder, name_rva, iat)):
                break
            dll = self.cstring(name_rva)
            # Bound imports without the original lookup table cannot establish names.
            require(lookup or not timestamp, f"{dll}: bound import has no lookup table")
            index = 0
            while True:
                thunk = self.unpack("<Q", (lookup or iat) + index * 8)[0]
                if not thunk:
                    break
                if iat + index * 8 == target:
                    self.bytes(target, 8)
                    require(not thunk & (1 << 63), "target IAT slot is imported by ordinal")
                    return dll, self.cstring(thunk + 2)
                index += 1
        raise ValueError(f"RVA 0x{target:x} is not an import address table slot")


# Exact unqualified RTTI class names. Namespace decoration is retained in output.
# Interface identities are checked through the RTTI hierarchy for multiple-
# inheritance tables; the COL offset must select the intended interface subobject.
TABLES = {
    "kLobbyIdleVtable": ("ZLobbyIdle", 0, False),
    "kLocalHostVtable": ("ZLobbyCreateLocalHost", 0, False),
    "kConnectedVtable": ("ZLobbyConnected", 0, False),
    "kLobbyConnectedVtable": ("ZLobbyConnected", 0, False),
    "kLobbyJoinVtable": ("ZLobbyJoin", 0, False),
    "kMultiplayerIdleVtable": ("ZMultiplayerIdle", 0, False),
    "kNetworkConnectingVtable": ("ZNetworkState_Connecting", 0, False),
    "kNetworkConnectedVtable": ("ZNetworkState_Connected", 0, False),
    "kRakPeerVtable": ("RakPeer", 0, False),
    "kRakNetSocketVtable": ("RNS2_Windows", 0, False),
    # Observed live in 3.290.1: RNS2_Windows +0x80 holds the primary (COL offset 0) pointer.
    "kEpicSocketOverrideVtable": ("SocketLayerOverride", 0, True),
    "kReplicaManagerVtable": ("ReplicaManager3", 0, True),
    "kSteamLobbyManagerVtable": ("ZGameLobbyManagerSteam", 0, False),
}
DATA = {"kNetworkStatePointer", "kUIStateMachineRoot", "kInviteDelegates", "kNetworkManager",
        "kNetworkContextPointer"}
FIELDS = {"kUIStateMachineOffset", "kUIContextOffset"}
SPECIAL = {"kCodeStart", "kCodeEnd", "kSteamApiRunCallbacksImport"}


def check_table(pe, name, rva):
    require(name in TABLES, f"no RTTI identity rule for {name}")
    expected, offset, inherited = TABLES[name]
    actual, displacement, bases = pe.rtti(rva)
    candidates = bases if inherited else [actual]
    require(any(re.match(r"\.\?A[UV]" + re.escape(expected) + r"@", candidate)
                for candidate in candidates),
            f"expected {'base ' if inherited else ''}{expected}; got {actual}, bases={bases}")
    require(displacement == offset, f"{actual}: expected COL offset {offset}, got {displacement}")
    return f"{actual}, COL offset {displacement}" + (f", base {expected}" if inherited else "")


# Reviewed Epic 3.290.1 leaves, deliberately independent of game_build.h: a new
# supported hash must not silently inherit exceptions reviewed for this image.
# Idle/LocalHost pseudocode is retained in build/game-update-3.290.1/epic-evidence.json:
# ev_264073a597a60a15800c313166143e4649b093b81f2e41625c53dab77e29902b
# ev_c5a3333463eaa88638820ee70ba1f599ffcf13588bc6ab722754e3942d84ca73
# The reviewed Update slot is 10, including LocalHost's unhooked helper. These
# checks establish hash-bound, RTTI-identified vtable entry boundaries; they do
# not infer complete bodies from padding or assert execution of the functions.
# 2026-11-03: Epic patch a63b6782... has the same size, PE timestamp, section
# layout and bytes at all three leaves (slot 10 unchanged); NAT body digest still matches.
REVIEWED_EPIC_SHA256 = "a63b678253965d037b497a53858a74cc2196184df5661c5385fc9bf9fc45ee82"
REVIEWED_EPIC_VTABLE_LEAVES = {
    "kLobbyIdleUpdate": (0x10077c0, "kLobbyIdleVtable", 10),
    "kLocalHostUpdate": (0x10140b0, "kLocalHostVtable", 10),
    "kConnectedUpdate": (0x1139a0, "kConnectedVtable", 10),
}
# NAT body extent [0xc53d80, 0xc53dd3) was supplied from reviewed assembly;
# pseudocode: ev_b0cc5cd2fbf4213dd772abb4201aec0ddb6d4adabccbd897f1de5d2d5883ae8f.
# The full 83-byte shape masks ONLY the five RIP-relative data displacements.
# All instructions, short branches and both returns are fixed. The complete body
# also has an exact digest, so even a masked operand change fails verification.
NAT_BODY_SHA256 = "c7cd2af8dedf08e81f4157d63181324f8b2793a5fb88df31bce4a6caf5745fd1"
NAT_BODY_SHAPE = (
    "48 8b 0d ?? ?? ?? ?? 4c 8b 05 ?? ?? ?? ?? 48 8b c1 48 c1 e8 3e a8 01 "
    "74 1d 0f b6 c1 48 8d 15 ?? ?? ?? ?? 48 8d 0c c2 48 2b ca 48 c1 f9 03 "
    "41 3b 48 60 0f 93 c0 c3 48 8b 0d ?? ?? ?? ?? 48 8b 15 ?? ?? ?? ?? "
    "48 2b ca 48 c1 f9 03 41 3b 48 60 0f 93 c0 c3"
)


def check_nat_body(pe, rva):
    require(rva == 0xc53d80, "NAT leaf RVA differs from reviewed boundary 0xc53d80")
    body = pe.bytes(rva, 83)
    require(hashlib.sha256(body).hexdigest() == NAT_BODY_SHA256,
            "NAT leaf complete 83-byte body digest mismatch")
    require(pe.bytes(rva + 83, 13) == b"\xcc" * 13, "NAT leaf trailing padding mismatch")
    pattern = b"".join(b"." if token == "??" else re.escape(bytes.fromhex(token))
                       for token in NAT_BODY_SHAPE.split())
    matches = []
    for section, flags in zip(pe.sections, pe.flags):
        if flags & 0x20000000:
            _, virtual, start, raw_size, _ = section
            data = pe.bytes(start, min(virtual, raw_size))
            matches.extend(start + match.start() for match in re.finditer(b"(?=" + pattern + b")", data, re.S))
    require(matches == [rva], f"NAT masked body must uniquely match reviewed RVA; got {matches}")
    return "reviewed 83-byte leaf; exact body SHA256 and unique masked instruction shape"


# RakPeer::Startup opens with `if (IsActive()) return RAKNET_ALREADY_STARTED;`:
# a virtual IsActive call (vtable +0x78), then `mov eax,1` on the taken branch.
# 3.290.1 moved Startup from slot 6 to slot 1; a slot-only check cannot see that.
RAKPEER_STARTUP_SHAPE = re.compile(rb"\xff\x50\x78\x84\xc0\x74.\xb8\x01\x00\x00\x00", re.S)


def check_rakpeer_startup(pe, rva):
    boundary = pe.code_start(rva)
    require(RAKPEER_STARTUP_SHAPE.search(pe.bytes(rva, 0x40)),
            "missing RakPeer::Startup IsActive/RAKNET_ALREADY_STARTED prologue")
    return f"{boundary}; RakPeer::Startup IsActive prologue"


def check_code(pe, store, name, addresses, digest):
    rva = addresses[name]
    if name == "kRakPeerStartup":
        return check_rakpeer_startup(pe, rva)
    if store != "epic" or name not in {*REVIEWED_EPIC_VTABLE_LEAVES, "kHostNatCheck"}:
        return pe.code_start(rva)
    require(digest == REVIEWED_EPIC_SHA256, "reviewed leaf requires the reviewed Epic 3.290.1 image SHA256")
    pe.code_boundary(rva)  # Still reject non-code, .pdata interiors and unpadded leaves.
    if name == "kHostNatCheck":
        return check_nat_body(pe, rva)
    reviewed_rva, table, slot = REVIEWED_EPIC_VTABLE_LEAVES[name]
    require(rva == reviewed_rva, f"leaf RVA differs from reviewed boundary 0x{reviewed_rva:x}")
    check_table(pe, table, addresses[table])
    actual = pe.unpack("<Q", addresses[table] + slot * 8)[0] - pe.base
    require(actual == rva, f"reviewed leaf must be {table}[{slot}], got RVA 0x{actual:x}")
    return f"reviewed hash-bound leaf entry via RTTI-verified {table}[{slot}]"


def check_backend_site(pe, field, rva, expected):
    if expected is None:
        section = pe.section(rva, 256)
        require(not pe.executable(rva), "backend domain lies in code")
        return f"256-byte buffer in {section[0]}; initialization is runtime-only"
    for alternative in expected:
        if field == "dynamic":
            actual, zero_fill = pe.initial_data(rva, len(alternative))
        else:
            actual, zero_fill = pe.bytes(rva, len(alternative)), False
        if actual == alternative:
            detail = alternative.hex(" ")
            if zero_fill:
                detail += " (loader-initial zero-fill; not observed runtime value)"
            return detail
    choices = " or ".join(value.hex(" ") for value in expected)
    raise ValueError(f"expected {choices}, got {actual.hex(' ')}")


def split_arguments(text):
    result, start, depth, quoted, escaped = [], 0, 0, False, False
    for i, char in enumerate(text):
        if quoted:
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif char == '"':
                quoted = False
        elif char == '"':
            quoted = True
        elif char in "({[":
            depth += 1
        elif char in ")}]":
            depth -= 1
        elif char == "," and depth == 0:
            result.append(text[start:i].strip())
            start = i + 1
    require(not quoted and depth == 0, "unbalanced C++ initializer")
    result.append(text[start:].strip())
    return result


def backend_plan(text, store):
    fields_match = re.search(r"struct\s+BackendLayout\s*\{(.*?)\};", text, re.S)
    require(fields_match, "missing BackendLayout")
    fields = re.findall(r"uintptr_t\s+(\w+)\s*;", fields_match[1])
    layout = re.search(r"constexpr\s+BackendLayout\s+k" + store.title() + r"Layout\s*\{([^}]+)\};", text)
    require(layout, f"missing {store} backend layout")
    values = [number(value) for value in layout[1].split(",")]
    require(len(fields) == len(values), "backend layout field/value count mismatch")
    layout = dict(zip(fields, values))
    # Mirror the accepted byte alternatives from the patcher. SHA256 separately
    # enforces exact installed-image identity (including absence of disk patches).
    variables = {}
    for name, literal in re.findall(r'Bytes\s+(\w+)\s*=\s*CString\(("(?:\\.|[^"\\])*")\)\s*;', text):
        value = ast.literal_eval(literal).encode("ascii") + b"\0"
        resize = re.search(r"\b" + re.escape(name) + r"\.resize\((\d+)\)\s*;", text)
        if resize:
            size = number(resize[1])
            value = value[:size].ljust(size, b"\0")
        variables[name] = value
    calls = re.findall(r"\badd\(layout\.(\w+)\s*,(.*?)\);", text, re.S)
    require(len(calls) == len(fields), "backend patch calls do not cover layout exactly")
    result = {}
    for field, arguments in calls:
        args = split_arguments(arguments)
        require(len(args) == 2 and args[1].startswith("{") and args[1].endswith("}"),
                f"unsupported accepted bytes for backend {field}")
        accepted = args[1][1:-1].strip()
        if not accepted:
            require(field == "domain", f"backend {field} has no signature")
            result[field] = (layout[field], None)
            continue
        expected = []
        for expression in split_arguments(accepted):
            if expression.startswith("{") and expression.endswith("}"):
                value = bytes(number(value) for value in expression[1:-1].split(","))
            elif expression in variables:
                value = variables[expression]
            else:
                string = re.fullmatch(r'CString\(("(?:\\.|[^"\\])*")\)', expression)
                require(string, f"unsupported backend {field} byte expression: {expression}")
                value = ast.literal_eval(string[1]).encode("ascii") + b"\0"
            expected.append(value)
        require(field not in result, f"duplicate backend patch {field}")
        result[field] = (layout[field], expected)
    require(set(result) == set(fields), "backend layout/signature mismatch")
    return result


class Report:
    def __init__(self, store):
        self.store = store
        self.failed = 0
        self.passed = 0

    def check(self, label, operation):
        try:
            detail = operation()
        except (ValueError, KeyError, IndexError, struct.error, AssertionError, OSError) as error:
            self.failed += 1
            print(f"FAIL [{self.store}] {label}: {error or type(error).__name__}")
        else:
            self.passed += 1
            print(f"PASS [{self.store}] {label}: {detail or 'OK'}")


def verify(store, path):
    report = Report(store)
    print(f"\n[{store}] {path}")
    try:
        pe = Image(path)
        build = source(ROOT / "src/game_build.h")
        digest_match = re.search(r"\bk" + store.title() + r'Sha256\s*=\s*"([0-9a-f]{64})"\s*;', build)
        require(digest_match, f"missing/invalid k{store.title()}Sha256 in src/game_build.h")
        digest = hashlib.sha256(pe.data).hexdigest()
        def identity():
            require(digest == digest_match[1], f"expected {digest_match[1]}, got {digest}")
            return digest
        report.check("SHA256", identity)
        addresses = offsets(source(ROOT / f"src/{store}/{store}_offsets.h"))
        specs = hooks(source(ROOT / f"src/{store}/{store}_plugin.cpp"))
        plan = backend_plan(source(ROOT / "src/backend/backend_patch.cpp"), store)

        for hook, table, slot, expected in specs:
            def hook_slot(table=table, slot=slot, expected=expected):
                actual = pe.unpack("<Q", addresses[table] + slot * 8)[0] - pe.base
                require(actual == addresses[expected],
                        f"{table}[{slot}]: expected {expected}=0x{addresses[expected]:x}, got RVA 0x{actual:x}")
                return f"{table}[{slot}] -> {expected} (0x{actual:x})"
            report.check(f"hook {hook}", hook_slot)

        for name, rva in addresses.items():
            if name.endswith("Vtable"):
                report.check(f"RTTI {name} 0x{rva:x}", lambda n=name, r=rva: check_table(pe, n, r))
            elif name in DATA:
                def data_address(rva=rva):
                    section = pe.section(rva, 8)
                    require(not pe.executable(rva), "data pointer is in executable section")
                    return f"mapped data in {section[0]} (runtime contents not checked)"
                report.check(f"data {name} 0x{rva:x}", data_address)
            elif name in FIELDS:
                print(f"INFO [{store}] {name}=0x{rva:x}: runtime object field; no offline identity check")
            elif name not in SPECIAL:
                report.check(f"code {name} 0x{rva:x}",
                             lambda n=name: check_code(pe, store, n, addresses, digest))

        if store == "steam":
            def steam_import():
                dll, symbol = pe.import_at(addresses["kSteamApiRunCallbacksImport"])
                require(dll.lower() == "steam_api64.dll" and symbol == "SteamAPI_RunCallbacks",
                        f"expected steam_api64.dll!SteamAPI_RunCallbacks, got {dll}!{symbol}")
                return f"{dll}!{symbol}"
            report.check("Steam callback IAT", steam_import)
            report.check("Steam code bounds",
                         lambda: pe.code_range(addresses["kCodeStart"], addresses["kCodeEnd"]))

        for field, (rva, expected) in plan.items():
            report.check(f"backend {field} 0x{rva:x}",
                         lambda f=field, r=rva, e=expected: check_backend_site(pe, f, r, e))
    except (ValueError, KeyError, IndexError, struct.error, AssertionError, OSError) as error:
        report.failed += 1
        print(f"FAIL [{store}] input/source parsing: {error or type(error).__name__}")
    print(f"[{store}] {report.passed} passed, {report.failed} failed")
    return report.failed == 0


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--epic", type=Path, help="explicit installed Epic HITMAN3.exe path")
    parser.add_argument("--steam", type=Path, help="explicit installed Steam HITMAN3.exe path")
    args = parser.parse_args(argv)
    if args.epic is None and args.steam is None:
        parser.error("supply at least one of --epic or --steam")
    results = [verify(store, path) for store, path in vars(args).items() if path is not None]
    print("\nOffline structural verification " + ("PASSED" if all(results) else "FAILED"))
    print("Unhooked helper semantics and runtime object layouts are outside this check.")
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
