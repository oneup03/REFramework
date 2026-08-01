# Offline RE toolkit for MHWilds (pattern/offset re-derivation after game updates)

Static-analysis helpers used to derive every Wilds-specific hook, pattern, and struct offset the
Flat3D feature relies on. When a game update breaks a scan or offset, this is the fastest path to
re-deriving it — no debugger attach, no IDA session; everything works from the exe on disk.

Companion documents:
- `docs/FLAT3D_WILDS_RE.md` — every game-version-dependent VALUE, its fragility tier, and its
  per-constant re-derivation recipe.
- The auto-memory `re-derivation-methodology` (Claude sessions) — the METHODS themselves, with
  worked examples of which technique cracked which problem.

## Setup

```
pip install pefile capstone numpy
```

Point `EXE` in `re_wilds.py` at the game binary (default `B:/SteamLibrary/.../MonsterHunterWilds.exe`).

## re_wilds.py — image loader + search primitives

- `Img(EXE)` — maps PE sections (the protector renames them: `.shared`/`.ecode` are the executable
  ones), `va_to_off`/`read_va` for reading at virtual addresses.
- `img.find_string_vas("SomeDebugString")` — locate anchor strings.
- `xrefs_multi(img, [va, ...])` — numpy-vectorized rel32 reference scan over all executable
  sections: finds every `lea/mov rip-rel`, `call`, `jmp` whose displacement resolves to the target
  VA. This is the workhorse: string VA -> code references, or function VA -> call sites.
- `disasm_window(img, va)` / `find_calls_near(img, ref_va)` — capstone disassembly around a hit
  (the "walk back N instructions to the E8 call" idiom used by the runtime scans, reproduced
  offline so you can validate a pattern BEFORE shipping it).

## funcs.py — function-span index

Parses the `.pdata` RUNTIME_FUNCTION table (~426k entries) into sorted starts/spans:
`F.func_of(va)` = enclosing function start. Offline equivalent of
`utility::find_function_start_unwind`.

## dispatch.py — worked example

Dumps the engine command-executor's typeid jump table (the `[cmd]&0x3f` switch) and disassembles a
case handler. Template for "find the handler for engine command X" problems (copy/clear/target
binds), which is where the T3 state-tracker offsets in `prime_resource_state` came from.

## The playbook (short form)

1. **Prefer reflection/TDB** (`sdk::find_type_definition`, typeinfo bruteforce-scan of an object) —
   survives every update. See `get_d3d12_resource_container` for the pattern.
2. **String anchor** next: `find_string_vas` -> `xrefs_multi` -> disasm window -> identify the call
   -> turn the SITE into a byte pattern for the runtime scan. Verify uniqueness offline first.
   Beware: Wilds REMOVED many classic anchors (e.g. `CircularDOF_SceneMipTexture`) when target
   creation moved behind a name-hash registry — an absent string is a dead scan, not a bug.
3. **Structure/shape scan** when strings fail: express what the function DOES as a capstone
   predicate (e.g. "reads dword [rdx+0x10], indexes [reg+reg*8]") and sweep candidate functions.
4. **Runtime learn-and-hijack** when static identification is impossible: observe live behavior at
   a point you control and learn the mapping (e.g. unknown RTV handle bound inside the GUI-draw
   window == the overlay target).
5. **Avoid engine internals entirely** when the D3D12 layer suffices: detour driver vtable entries
   from your own device/command-list objects (same implementation class as the engine's) and scope
   with in-stream markers you already record. Zero patterns, zero offsets, survives every update.
