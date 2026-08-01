"""Parse .pdata RUNTIME_FUNCTION table -> function boundaries, with binary search."""
import bisect, struct
import re_wilds as R

def load_pdata(img):
    import pefile
    pe = pefile.PE(R.EXE, fast_load=True)
    pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
    starts = []
    spans = {}  # start_va -> end_va
    try:
        for e in pe.DIRECTORY_ENTRY_EXCEPTION:
            s = img.base + e.struct.BeginAddress
            en = img.base + e.struct.EndAddress
            starts.append(s)
            spans[s] = en
    except AttributeError:
        pass
    pe.close()
    starts = sorted(set(starts))
    return starts, spans

class Funcs:
    def __init__(self, img):
        self.img = img
        self.starts, self.spans = load_pdata(img)
        print(f"loaded {len(self.starts)} RUNTIME_FUNCTIONs")

    def func_of(self, va):
        """Return (start, end) of the function containing va, or None."""
        i = bisect.bisect_right(self.starts, va) - 1
        if i < 0:
            return None
        s = self.starts[i]
        e = self.spans.get(s, s)
        # chained entries: end may be exact; if va within [s,e) accept
        if s <= va < e:
            return (s, e)
        # sometimes pdata splits a func; accept if within next start
        if i + 1 < len(self.starts) and s <= va < self.starts[i+1]:
            return (s, self.starts[i+1])
        return None

if __name__ == "__main__":
    img = R.Img(R.EXE)
    f = Funcs(img)
    for va in [0x14a0a2664, 0x14a0ab69e, 0x14a0a262f, 0x14d0e7e50, 0x14486f8d0]:
        fn = f.func_of(va)
        if fn:
            print(f"0x{va:x} -> func [0x{fn[0]:x} .. 0x{fn[1]:x}] size={fn[1]-fn[0]:#x}")
        else:
            print(f"0x{va:x} -> NO func entry")
