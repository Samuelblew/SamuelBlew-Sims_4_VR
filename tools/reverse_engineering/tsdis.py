import sys, struct, re
from capstone import Cs, CS_ARCH_X86, CS_MODE_64

path = sys.argv[1]
data = open(path, "rb").read()
pe = struct.unpack_from("<I", data, 0x3C)[0]
ns = struct.unpack_from("<H", data, pe + 6)[0]
sh = pe + 24 + struct.unpack_from("<H", data, pe + 20)[0]
BASE = 0x140000000
secs = []
for i in range(ns):
    o = sh + i * 40
    secs.append((data[o:o + 8].rstrip(b"\0").decode(), *struct.unpack_from("<IIII", data, o + 8)))


def off(va):
    r = va - BASE
    for n, vs, sva, rs, rp in secs:
        if sva <= r < sva + rs:
            return r - sva + rp
    return None


def sec(va):
    r = va - BASE
    for n, vs, sva, rs, rp in secs:
        if sva <= r < sva + max(vs, rs):
            return n
    return None


def string_at(va):
    o = off(va)
    if o is None:
        return None
    e = o
    while e < len(data) and data[e] and e - o < 200:
        e += 1
    s = data[o:e]
    if len(s) >= 3 and all(32 <= c < 127 for c in s):
        return s.decode()
    w = data[o:o + 200]
    m = re.match(rb"((?:[ -~]\x00){3,})", w)
    if m:
        return "L" + m.group(1).decode("utf-16le")
    return None


md = Cs(CS_ARCH_X86, CS_MODE_64)
md.detail = False


def dis(va, maxn=80, stop_ret=True):
    o = off(va)
    out = []
    for ins in md.disasm(data[o:o + maxn * 15], va):
        line = f"{ins.address:x}  {ins.mnemonic:6} {ins.op_str}"
        m = re.search(r"\[rip ([+-]) 0x([0-9a-f]+)\]", ins.op_str)
        if m:
            t = ins.address + ins.size + (int(m.group(2), 16) * (1 if m.group(1) == "+" else -1))
            s = string_at(t)
            line += f"    ; -> {t:x} {sec(t)}" + (f' "{s}"' if s else "")
        elif ins.mnemonic in ("call", "jmp") and ins.op_str.startswith("0x"):
            line += ""
        out.append(line)
        maxn -= 1
        if maxn <= 0 or (stop_ret and ins.mnemonic in ("ret",) ):
            break
        if stop_ret and ins.mnemonic == "jmp" and maxn < 70 and not ins.op_str.startswith("0x14"):
            break
    return "\n".join(out)


if __name__ == "__main__":
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    for a in sys.argv[2].split(","):
        print(f"==== {a}")
        print(dis(int(a, 16), n))
