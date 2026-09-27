"""Read-only inspection of the running FalloutNV.exe memory (development aid).

  python tools/nvpeek.py u32 0x11E0E80 16        dump dwords
  python tools/nvpeek.py rtti 0x12345678          class name of the object at an address
  python tools/nvpeek.py highactors               ProcessLists' high-process actors
"""
import ctypes
import struct
import sys
from ctypes import wintypes

k32 = ctypes.WinDLL("kernel32", use_last_error=True)


def find_pid(name="falloutnv.exe"):
    class PROCESSENTRY32W(ctypes.Structure):
        _fields_ = [("dwSize", wintypes.DWORD), ("cntUsage", wintypes.DWORD), ("th32ProcessID", wintypes.DWORD),
                    ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wintypes.DWORD), ("cntThreads", wintypes.DWORD),
                    ("th32ParentProcessID", wintypes.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wintypes.DWORD),
                    ("szExeFile", ctypes.c_wchar * 260)]
    snap = k32.CreateToolhelp32Snapshot(0x2, 0)
    entry = PROCESSENTRY32W()
    entry.dwSize = ctypes.sizeof(entry)
    ok = k32.Process32FirstW(snap, ctypes.byref(entry))
    pid = None
    while ok:
        if entry.szExeFile.lower() == name:
            pid = entry.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(entry))
    k32.CloseHandle(snap)
    return pid


class Process:
    def __init__(self):
        pid = find_pid()
        if pid is None:
            raise SystemExit("FalloutNV.exe is not running")
        self.handle = k32.OpenProcess(0x0010 | 0x0400, False, pid)

    def read(self, address, size):
        buf = ctypes.create_string_buffer(size)
        got = ctypes.c_size_t()
        if not k32.ReadProcessMemory(self.handle, ctypes.c_void_p(address), buf, size, ctypes.byref(got)):
            return None
        return buf.raw

    def u32(self, address):
        b = self.read(address, 4)
        return struct.unpack("<I", b)[0] if b else None

    def rtti(self, obj):
        vt = self.u32(obj)
        col = self.u32(vt - 4) if vt else None
        td = self.u32(col + 12) if col else None
        name = self.read(td + 8, 64) if td else None
        return name.split(b"\0")[0].decode(errors="replace") if name else "?"


def main():
    p = Process()
    cmd = sys.argv[1]
    if cmd == "u32":
        a = int(sys.argv[2], 16)
        n = int(sys.argv[3]) if len(sys.argv) > 3 else 16
        for i in range(n):
            v = p.u32(a + 4 * i)
            print(f"+{4 * i:03X} {v:08X}" if v is not None else f"+{4 * i:03X} ????????")
    elif cmd == "rtti":
        a = int(sys.argv[2], 16)
        print(p.rtti(a))
    elif cmd == "highactors":
        base = 0x11E0E80
        items = p.u32(base + 8)
        heads = [p.u32(base + 4 + 0x10 + 4 * i) for i in range(4)]
        tails = [p.u32(base + 4 + 0x20 + 4 * i) for i in range(4)]
        print("items", hex(items), "heads", heads, "tails", tails)
        for i in range(heads[0], min(tails[0], heads[0] + 60)):
            obj = p.u32(items + 4 * i)
            pos = struct.unpack("<3f", p.read(obj + 0x30, 12)) if obj else None
            print(i, hex(obj or 0), p.rtti(obj) if obj else "", pos)


if __name__ == "__main__":
    main()
