"""
Reverse-engineering helper for MHWilds copy_texture.
Loads the exe, maps sections, provides string search + xref + disasm helpers.
"""
import sys, struct
import pefile
from capstone import Cs, CS_ARCH_X86, CS_MODE_64, CS_OP_MEM, CS_OP_IMM

EXE = r"B:/SteamLibrary/steamapps/common/MonsterHunterWilds/MonsterHunterWilds.exe"

class Img:
    def __init__(self, path):
        with open(path, "rb") as f:
            self.data = f.read()
        pe = pefile.PE(path, fast_load=True)
        self.base = pe.OPTIONAL_HEADER.ImageBase
        IMAGE_SCN_MEM_EXECUTE = 0x20000000
        self.sections = []
        for s in pe.sections:
            name = s.Name.rstrip(b"\x00").decode("latin1")
            self.sections.append({
                "name": name,
                "va": self.base + s.VirtualAddress,
                "vsize": s.Misc_VirtualSize,
                "raw": s.PointerToRawData,
                "rsize": s.SizeOfRawData,
                "exec": bool(s.Characteristics & IMAGE_SCN_MEM_EXECUTE),
            })
        pe.close()
        self.code_sections = [s["name"] for s in self.sections if s["exec"]]
        self.text = next((s for s in self.sections if s["exec"]), self.sections[0])
        print(f"ImageBase = 0x{self.base:x}")
        for s in self.sections:
            flag = "X" if s["exec"] else " "
            print(f"  [{flag}] {s['name']:8} va=0x{s['va']:x} vsize=0x{s['vsize']:x} raw=0x{s['raw']:x} rsize=0x{s['rsize']:x}")
        print(f"  code sections: {self.code_sections}")

    def va_to_off(self, va):
        for s in self.sections:
            if s["va"] <= va < s["va"] + max(s["vsize"], s["rsize"]):
                delta = va - s["va"]
                if delta < s["rsize"]:
                    return s["raw"] + delta
        return None

    def off_to_va(self, off):
        for s in self.sections:
            if s["raw"] <= off < s["raw"] + s["rsize"]:
                return s["va"] + (off - s["raw"])
        return None

    def section_of_va(self, va):
        for s in self.sections:
            if s["va"] <= va < s["va"] + max(s["vsize"], s["rsize"]):
                return s["name"]
        return None

    def read_va(self, va, n):
        off = self.va_to_off(va)
        if off is None:
            return None
        return self.data[off:off+n]

    def find_string_offsets(self, s):
        """Find all raw offsets of a null-terminated ascii string s (as bytes, with trailing NUL)."""
        needle = s.encode("latin1") + b"\x00"
        res = []
        start = 0
        while True:
            i = self.data.find(needle, start)
            if i < 0:
                break
            # require preceding byte to be NUL or non-ascii-ish (string boundary) — keep it simple: preceding is 00
            res.append(i)
            start = i + 1
        return res

    def find_string_vas(self, s):
        return [self.off_to_va(o) for o in self.find_string_offsets(s) if self.off_to_va(o) is not None]


def _u32(arr):
    import numpy as np
    return arr.astype(np.uint32)


def xrefs_multi(img, targets, window_sections=None, chunk=64 * 1024 * 1024):
    """Find rel32 references to each target VA. A rel32 field at position p (VA) satisfies
       (p + 4) + int32(bytes[p:p+4]) == target. Catches lea/mov rip-rel, call/jmp rel32.
       Returns {target_va: [operand_field_va, ...]}. Memory-bounded via chunking."""
    import numpy as np
    if window_sections is None:
        window_sections = [s["name"] for s in img.sections if s["exec"] and s["rsize"] > 0x10000]
    tset = list(targets)
    out = {t: [] for t in tset}
    for s in img.sections:
        if s["name"] not in window_sections:
            continue
        raw = img.data[s["raw"]:s["raw"] + s["rsize"]]
        base_va = s["va"]
        n = len(raw)
        pos = 0
        while pos < n - 4:
            end = min(pos + chunk, n)
            # positions [pos, end-4] fully contained; process this window with a 4-byte tail overlap
            seg = np.frombuffer(raw[pos:min(end + 4, n)], dtype=np.uint8)
            m = len(seg) - 4
            if m <= 0:
                break
            b0 = _u32(seg[0:m]); b1 = _u32(seg[1:m+1]); b2 = _u32(seg[2:m+2]); b3 = _u32(seg[3:m+3])
            rel = (b0 | (b1 << 8) | (b2 << 16) | (b3 << 24)).astype(np.int32).astype(np.int64)
            idx = np.arange(m, dtype=np.int64)
            dst = (base_va + pos) + idx + 4 + rel
            for t in tset:
                hit = np.nonzero(dst == t)[0]
                for hi in hit.tolist():
                    out[t].append(int(base_va + pos + hi))
            del b0, b1, b2, b3, rel, idx, dst, seg
            pos += chunk
    # dedup
    for t in out:
        out[t] = sorted(set(out[t]))
    return out


def disasm_window(img, center_va, before=48, after=48):
    """Disassemble a window [center_va-before, center_va+after], aligning by trying a few
       start offsets to get a sane linear stream that reaches center_va."""
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    md.detail = True
    start = center_va - before
    data = img.read_va(start, before + after)
    if data is None:
        return []
    insns = list(md.disasm(data, start))
    return insns


def find_calls_near(img, ref_va, back=64, fwd=8):
    """Emulate the legacy walk: from ref_va, find E8 call sites within [ref_va-back, ref_va].
       Returns list of (call_va, target_va)."""
    calls = []
    data = img.read_va(ref_va - back, back + fwd)
    if data is None:
        return calls
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    for ins in md.disasm(data, ref_va - back):
        if ins.mnemonic == "call" and ins.op_str.startswith("0x"):
            try:
                calls.append((ins.address, int(ins.op_str, 16)))
            except ValueError:
                pass
    return calls


if __name__ == "__main__":
    img = Img(EXE)
    print("\n=== Candidate copy-cluster strings ===")
    cands = [
        "CopyImage", "opyImage", "CopyTexture", "PostEffect Copy",
        "InterleaveNormalDepth", "InterleaveNormalDepthHalf",
        "InterleaveNormalDepthWithoutGBuffer", "InterleaveNormalDepthHalfWithoutGBuffer",
        "g_BilateralUpscaleDownscaledDepth", "ModifiedGBufferSRV", "PrevAODepth",
        "RE_POSTPROCESS_Color", "InputVelocity",
        "copy_texture", "CopyResource", "Blit", "copyTexture",
    ]
    for c in cands:
        vas = img.find_string_vas(c)
        if vas:
            print(f"  FOUND '{c}': {len(vas)} @ " + ", ".join(f"0x{v:x}" for v in vas[:6]))
        else:
            print(f"  ----- '{c}': not found")
