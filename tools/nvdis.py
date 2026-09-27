"""Read-only FalloutNV.exe inspection for hook development.

  python tools/nvdis.py dis 0x873200 [count]   disassemble at a virtual address
  python tools/nvdis.py calls 0x873200         list `call rel32` sites targeting an address
  python tools/nvdis.py vtable 0x1234567 [n]   print vtable entries
  python tools/nvdis.py rtti .?AVhkpBoxShape@@ find vtables whose RTTI name matches
"""
import os
import struct
import sys

import capstone
import pefile

EXE = os.environ.get("FALLOUTNV_EXE", r"C:\games\Steam\steamapps\common\Fallout New Vegas\FalloutNV.exe")


CACHE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "build", "FalloutNV.image.bin")


def dump_live(base, size):
    """The Steam executable is encrypted on disk; read the decrypted image from a running game."""
    import ctypes
    from ctypes import wintypes
    k32 = ctypes.WinDLL("kernel32", use_last_error=True)
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)

    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                    ("szExeFile", ctypes.c_wchar * 260)]
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    pid = None
    ok = k32.Process32FirstW(snap, ctypes.byref(entry))
    while ok:
        if entry.szExeFile.lower() == "falloutnv.exe":
            pid = entry.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    if pid is None:
        return None
    process = k32.OpenProcess(0x0010 | 0x0400, False, pid)  # VM_READ | QUERY_INFORMATION
    out = bytearray(size)
    page = 0x1000
    buf = ctypes.create_string_buffer(page)
    got = ctypes.c_size_t()
    for off in range(0, size, page):
        if k32.ReadProcessMemory(process, ctypes.c_void_p(base + off), buf, page, ctypes.byref(got)):
            out[off:off + page] = buf.raw
    k32.CloseHandle(process)
    return bytes(out)


class Image:
    def __init__(self, path):
        self.pe = pefile.PE(path, fast_load=True)
        self.base = self.pe.OPTIONAL_HEADER.ImageBase
        size = self.pe.OPTIONAL_HEADER.SizeOfImage
        if os.path.exists(CACHE) and os.path.getsize(CACHE) == size and "--refresh" not in sys.argv:
            self.data = open(CACHE, "rb").read()
            return
        live = dump_live(self.base, size)
        if live is None:
            raise SystemExit("FalloutNV.exe is encrypted on disk; start the game once so its image can be read")
        os.makedirs(os.path.dirname(CACHE), exist_ok=True)
        open(CACHE, "wb").write(live)
        self.data = live

    def read(self, va, size):
        off = va - self.base
        return self.data[off:off + size]

    def u32(self, va):
        return struct.unpack_from("<I", self.data, va - self.base)[0]

    def text(self):
        for s in self.pe.sections:
            if s.Name.rstrip(b"\0") == b".text":
                return self.base + s.VirtualAddress, s.Misc_VirtualSize
        raise RuntimeError("no .text")


def main():
    img = Image(EXE)
    cmd, arg = sys.argv[1], sys.argv[2]
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    if cmd == "dis":
        va = int(arg, 16)
        count = int(sys.argv[3]) if len(sys.argv) > 3 else 24
        for i, ins in enumerate(md.disasm(img.read(va, count * 16), va)):
            if i >= count:
                break
            print(f"{ins.address:08X}  {ins.bytes.hex():<20} {ins.mnemonic} {ins.op_str}")
    elif cmd == "calls":
        target = int(arg, 16)
        start, size = img.text()
        code = img.read(start, size)
        for i in range(len(code) - 5):
            if code[i] == 0xE8:
                rel = struct.unpack_from("<i", code, i + 1)[0]
                if start + i + 5 + rel == target:
                    print(f"{start + i:08X}")
    elif cmd == "vtable":
        va = int(arg, 16)
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
        for k in range(n):
            print(f"[{k:2}] {img.u32(va + 4 * k):08X}")
    elif cmd == "rtti":
        name = arg.encode()
        idx = img.data.find(name)
        while idx >= 0:
            td = img.base + idx - 8  # TypeDescriptor: vftable ptr, spare, name
            print(f"TypeDescriptor {td:08X}")
            for col in range(0, len(img.data) - 20, 4):
                if struct.unpack_from("<I", img.data, col + 12)[0] == td and struct.unpack_from("<I", img.data, col)[0] == 0:
                    col_va = img.base + col
                    packed = struct.pack("<I", col_va)
                    j = img.data.find(packed)
                    while j >= 0:
                        print(f"  COL {col_va:08X} offset {struct.unpack_from('<I', img.data, col + 4)[0]:X} vtable {img.base + j + 4:08X}")
                        j = img.data.find(packed, j + 4)
            idx = img.data.find(name, idx + 1)


if __name__ == "__main__":
    main()
