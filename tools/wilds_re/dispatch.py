import re_wilds as R
from funcs import Funcs
from capstone import Cs, CS_ARCH_X86, CS_MODE_64
import struct

img = R.Img(R.EXE)
F = Funcs(img)
md = Cs(CS_ARCH_X86, CS_MODE_64); md.detail = True

def strlea(ins):
    if ins.mnemonic in ("lea","mov") and "rip" in ins.op_str:
        try:
            disp = int(ins.op_str.split("rip +")[1].split("]")[0].strip(), 16)
            tgt = ins.address + ins.size + disp
            sb = img.read_va(tgt, 48)
            if sb:
                ss = sb.split(b"\x00")[0]
                if 2 <= len(ss) <= 46 and all(32 <= b < 127 for b in ss):
                    return f'   ; "{ss.decode("latin1")}"'
        except Exception: pass
    return ""

# Dump dispatch prologue around 0x14a9995f0..0x14a999650
print("=== dispatch region ===")
data = img.read_va(0x14a9995f0, 0x60)
tbl = None
for ins in md.disasm(data, 0x14a9995f0):
    tag = "  [CALL]" if ins.mnemonic=="call" else ""
    print(f"  0x{ins.address:x}: {ins.mnemonic:8} {ins.op_str}{tag}")
    if ins.mnemonic in ("lea",) and "rip" in ins.op_str:
        disp = int(ins.op_str.split("rip +")[1].split("]")[0].strip(), 16)
        tbl = ins.address + ins.size + disp

print(f"\njump table base guess: 0x{tbl:x}" if tbl else "no table lea found")
if tbl:
    print("table entries (int32 rel to table base):")
    raw = img.read_va(tbl, 4*0x22)
    for k in range(0x22):
        off = struct.unpack_from("<i", raw, k*4)[0]
        case_va = tbl + off
        mark = "  <== CopyTexture (typeid 1)" if k==1 else ("  <== Clear (typeid 0)" if k==0 else "")
        print(f"  [{k:#04x}] -> 0x{case_va:x}{mark}")

# Dump the typeid=1 case
if tbl:
    raw = img.read_va(tbl, 4*2)
    case1 = tbl + struct.unpack_from("<i", raw, 4)[0]
    print(f"\n=== CopyTexture case @ 0x{case1:x} (dump 0x120) ===")
    d = img.read_va(case1, 0x120)
    for ins in md.disasm(d, case1):
        tag = "  [CALL]" if ins.mnemonic=="call" else ""
        print(f"  0x{ins.address:x}: {ins.mnemonic:8} {ins.op_str}{strlea(ins)}{tag}")
        if ins.mnemonic in ("ret","jmp") and ins.address > case1+0x20:
            break
