"""Offline PE inspection helpers for game-update validation (Python stdlib only)."""
import struct
from pathlib import Path


class PE:
    def __init__(self, path):
        self.data = Path(path).read_bytes()
        nt = self.u32(0x3c)
        assert self.data[nt:nt + 4] == b'PE\0\0'
        self.base = self.u64(nt + 24 + 24)
        table = nt + 24 + self.u16(nt + 20)
        self.sections = []
        for i in range(self.u16(nt + 6)):
            p = table + i * 40
            self.sections.append((self.data[p:p+8].rstrip(b'\0').decode(),
                                  *struct.unpack_from('<IIII', self.data, p + 8)))

    def u16(self, p): return struct.unpack_from('<H', self.data, p)[0]
    def u32(self, p): return struct.unpack_from('<I', self.data, p)[0]
    def u64(self, p): return struct.unpack_from('<Q', self.data, p)[0]

    def offset(self, rva):
        for _, _, start, size, raw in self.sections:
            if start <= rva < start + size: return raw + rva - start
        raise ValueError(f'RVA {rva:x} is not file-backed')

    def rva(self, offset):
        for _, _, start, size, raw in self.sections:
            if raw <= offset < raw + size: return start + offset - raw
        raise ValueError(f'Offset {offset:x} is not mapped')

    def find(self, pattern):
        p = self.data.find(pattern)
        while p >= 0:
            yield p
            p = self.data.find(pattern, p + 1)

    def vtables(self, name):
        # MSVC x64 RTTI: type descriptor -> complete object locator -> vftable.
        for p in self.find(name.encode() + b'\0'):
            td = self.rva(p - 16)
            for ref in self.find(struct.pack('<I', td)):
                col = ref - 12
                if self.u32(col) != 1: continue
                try: col_rva = self.rva(col)
                except ValueError: continue
                if self.u32(col + 20) != col_rva: continue
                for slot in self.find(struct.pack('<Q', self.base + col_rva)):
                    yield self.rva(slot + 8), self.u32(col + 4)


if __name__ == '__main__':
    import sys
    pe = PE(sys.argv[1])
    for name in sys.argv[2:]:
        for vt, displacement in pe.vtables(name):
            values = [hex(pe.u64(pe.offset(vt) + i * 8) - pe.base) for i in range(12)]
            print(name, hex(vt), 'this+', hex(displacement), values)
