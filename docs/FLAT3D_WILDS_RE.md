# Flat3D on Monster Hunter Wilds — reverse-engineering reference

This documents every **game-version-dependent** value the Flat3D per-eye harvest relies on for
Monster Hunter Wilds (TDB 81, D3D12), how each was derived, and **how to re-derive it when a game
update breaks it**. Read this first if Flat3D goes black or crashes after a Wilds patch.

By design there is **no runtime guard/degrade** — if a value goes stale the feature is expected to
fail (black or crash) rather than silently disable. This doc is the recovery manual.

Addresses below (`0x14…`) are from **one specific game build** and are illustrative only — the code
never hardcodes an absolute address; it resolves everything by pattern/string/typeinfo at load. What
*can* go stale is (a) the byte/string **patterns** and (b) the reverse-engineered **struct offsets**.

---

## 0. The core insight (survives offset churn)

**Wilds `create_texture` returns a RESERVED (tiled) D3D12 resource with no tiles mapped.** It accepts
SRV/RTV creation and reports a valid desc, but has *zero physical backing* until tiles are mapped, so
the engine's in-stream `copy_texture` into our clone lands on nothing and reads back all-zero. The fix
(`D3D12Component::back_reserved_texture`) maps each clone's tiles to a dedicated heap
(`GetResourceTiling` → `CreateHeap` → `UpdateTileMappings`). This is **pure D3D12** and has no
version-dependent constant — it is guarded by a layout check, so if a future version hands back
*committed* resources instead, it becomes a no-op automatically. The knowledge is the durable asset;
the offsets below are the fragile part.

The harvest pipeline otherwise reuses the exact `clone()` + `copy_texture` path the TemporalUpscaler
uses on other RE Engine titles — Wilds just needed the tiles mapped and the plumbing below resolved.

---

## 1. Fragility tiers

| Tier | Meaning | Recompile-safe? |
|---|---|---|
| **T1** | Pure D3D12 / reflection (type/method by name, or typeinfo bruteforce-scan) | Yes |
| **T2** | Code scan (byte pattern / debug-string anchor) — survives ASLR, breaks on recompile of that fn | No |
| **T3** | Hardcoded reverse-engineered **struct offset** — no signature to scan; breaks on struct relayout | No |

Only **T2** and **T3** need attention after a patch. **T3 is the real hazard** — a wrong offset
dereferences garbage and crashes.

---

## 2. Constant-by-constant

### 2.1 `RenderResource` runtime size — **T3 (version-gated)**
- **Where:** `shared/sdk/renderer/RenderResource.hpp` → `get_runtime_size()` (returns `0x20` for TDB≥81).
- **Why it matters:** every `TargetState`/`Texture`/`DirectXResource` member offset is relative to this
  base size. Wrong by 8 → `num_rtv` and all native-resource pointers read one field early and come back
  `0`/garbage. This single bug is what made the harvest look "impossible" for weeks (it was read at
  `>= 82`; Wilds is TDB 81 but *also* has the extra 8-byte `tdb82_padding`).
- **Re-derive:** raw-dump the memory of a bound `via.render.TargetState`. Its `Desc`
  (`rtvs` ptr / `num_rtv`==1 / rect == backbuffer WxH) must sit at `base + get_runtime_size()`. If the
  accessors return `num_rtv==0` / null natives, bump/drop the padding in `get_runtime_size()` until the
  Desc lands.
- **Sanity check:** `layer->get_main_target_state()->get_rtv_count()` returns ≥1 on a live frame.
- **#2 (reflection):** partly — it's version-gated, not hardcoded absolute. Could be auto-detected by
  scanning the object for the Desc signature, but the version-gate is standard SDK practice.

### 2.2 `create_texture` factory — **T2 (string-anchored)**
- **Where:** `shared/sdk/Renderer.cpp` → `create_texture()`, `is_mhwilds()` branch (+ `resolve_texture_memory_device`).
- **Anchor:** `scan_string(L"width=%u,height=%u,depth=%u,mip=%u,array=%u,format=%u,usage=%u,bind=%u")`
  → `scan_displacement_reference` → `find_function_start_unwind`. On Wilds this string is the factory's
  **own debug name** (used *inside* it), so the function containing the string-ref **is** the factory —
  unlike other RE games where the string sits at a log call next to a `call create_texture`.
- **Calling convention (T3-ish, hardcoded):** 4-arg output-struct, NOT the SDK's 2-arg `(device, desc)`:
  `factory(void* out /*rcx*/, void* device /*rdx*/, Texture::Desc* /*r8*/, uint32_t dim /*r9d*/)`;
  `out = {bool ok@0; Texture* tex@8; void* aux@0x10}` and is a **full ≥0x70-byte struct** (a smaller
  buffer smashes the stack); `dim = (desc->arr >= 2) ? 5 : 4`.
- **Re-derive:** if the debug string still exists, `find_function_start_unwind` from its ref gives the
  factory. Then **verify the 4-arg convention still holds** by disassembling: it reads `desc->mip@+0xc` /
  `format@+0x14`, writes `out[0]=ok` and `out[8]=tex`. If Capcom reverts to the 2-arg shape, drop the
  Wilds branch.
- **Sanity check:** the one-shot `[Flat3D] create_texture(Wilds): ok=1 … native=<nonzero>` log, and
  `back_reserved_texture` maps ~510 tiles for a 4K fmt-24 clone.

### 2.3 `resolve_texture_memory_device` — **T2 pattern + T3 offset**
- **Where:** `shared/sdk/Renderer.cpp` → `resolve_texture_memory_device()`.
- **What:** the *device* (arg1) `create_texture` needs — the **memory/heap** device, distinct from the
  RTV-pool device (§2.7).
- **Pattern:** `"B9 50 59 04 00 E8 ? ? ? ? 48 89 05 ? ? ? ?"` (`mov ecx,0x45950; call alloc;
  mov [global],rax`) → `calculate_absolute(match+13)` = the singleton global → device is
  `*(*global + 0x18)` (**T3 offset `0x18`**).
- **Re-derive:** `0x45950` is the render-system singleton's allocation size — it's the immediate in the
  pattern, so if the struct grows, that constant changes (find the new size at the singleton's lazy-init
  `operator new`). The device offset `0x18` = trace the engine's own `create_texture` **wrapper** and see
  which field of the singleton it passes as arg1.
- **Sanity check:** `create_texture(Wilds)` log shows `device=<nonzero>` and `ok=1`.
- **#2:** not reflection-derivable (raw memory-manager singleton, no reflection handle). Pattern is best available.

### 2.4 `copy_texture` engine builder — **T2 pattern (typeid is reflection)**
- **Where:** `shared/sdk/Renderer.cpp` → `copy_texture()` → `copy_engine` lambda.
- **What:** the engine's own CopyTexture **command builder** (Wilds has no resolvable standalone
  `copy_texture`; it's built through the command allocator). Preferred over the hand-rolled `copy_alloc`.
- **Pattern:** `"41 56 56 57 53 48 83 EC 28 4C 89 CE 4C 89 C3 48 89 D7 49 89 CE BA 01 00 00 00 41 B8 30 00 00 00 E8"`
  — prologue + `mov edx,1` (**typeid CopyTexture==1**) + `mov r8d,0x30` (**command size**) + `call alloc`.
  Signature: `f(RenderContext* /*rcx*/, RenderResource* dst /*rdx*/, RenderResource* src /*r8*/,
  Tracking* /*r9*/)` where `Tracking = {int32 id=-2; int32 val=0; int64 bitmap=0}` (id −2 = "immediate/no fence").
- **Re-derive:** `via.render.command.TypeId::CopyTexture` is **reflection-registered == 1**, and the command
  is `0x30` bytes. Find a function whose body is `alloc(this, /*edx*/1, /*r8d*/0x30)` then fills
  `src`/`dst` — there are ~58 callers in the exe. Confirm the arg order via the `mov` register shuffle in
  the prologue.
- **Sanity check:** `[Flat3D] overlay-copy eye=N … SRC=<x> fmt=24 3840x2160 | DST=<y> fmt=24 …`, then the
  clone reads non-black after tile-mapping.
- **#2:** the typeid/size are reflection-derived; the builder *function* is raw code → pattern needed.

### 2.5 `copy_alloc` command layout — **T3, DORMANT**
- **Where:** `shared/sdk/Renderer.cpp` → `copy_texture()` → `copy_alloc` lambda (fallback only).
- **Layout:** `src@0x10 / dst@0x18 / sync-token@0x20 == {id:-2, val:0} / src_subres@0x28 / dst_subres@0x2c`.
  (The SDK's `CopyBase` model — fence@0x20, subres@0x30/0x34 — is **wrong** for Wilds.)
- **Note:** only exercised if §2.4's `copy_engine` pattern fails to resolve. If you're editing this, prefer
  fixing §2.4 instead. Re-derive by disassembling the CopyTexture handler in the executor (§2.6) and
  reading which command offsets it consumes.

### 2.6 `prime_copy_dest_state` offsets — **T3, HIGHEST RISK** ⚠️
- **Where:** `shared/sdk/Renderer.cpp` → `prime_copy_dest_state()`.
- **What:** primes the engine command-executor's per-resource **state tracker** so the copy's dst-resolve
  takes a no-transition / direct-native fast path — our externally-created clone isn't in the executor's
  alias registry, so without this the resolve's `find()` walks off the end of the registry chain and
  **crashes**.
- **Offsets:**
  - state-tracker base pointer @ **`tex+0x100`**; per-subresource entries **`0x30`** bytes each.
  - resting state @ **`entry+0`**, current state @ **`entry+8`** — set BOTH to **`0x400`** (D3D12
    `COPY_DEST`) so the state-check returns "no transition" and `find()` never walks the registry.
  - resolve gate flag @ **`tex+0x158`** — set to **`0`** so the copy-execution resolve reads the native
    handle directly instead of walking the registry (nonzero => walk => crash for our unregistered clone).
- **Re-derive:** this is the deepest RE. Find the D3D12-backend command executor's CopyTexture handler
  (via the command dispatch table — `[cmd]&0x3f` typeid switch — or via the crash site if it faults).
  Trace how it resolves the **dst** `RenderResource`:
  1. a **state check** reads `*(dst+0x100) + subresource*0x30`, compares `[+8]` against the desired state
     (`0x400` for a copy destination). Match => no transition recorded.
  2. a **resource resolve** branches on the byte at `dst+0x158`: nonzero => walk the alias registry keyed
     on the native handle (crashes for our clone); zero => read the native directly.
  Set the tracker state to `COPY_DEST` and clear the gate, per subresource (`mip*arr` entries).
- **Sanity check:** with priming, the engine copy lands (clone non-black) and no crash; removing it
  reproduces the executor `find()` access-violation.
- **#2:** **NOT reflection-derivable** — pure engine-internal tracker, no reflection handle. If any
  constant needs re-RE after a patch, expect it to be this one. Budget disassembly time here.

### 2.7 native-resource offset — **T1 now (was T3), reflection-derived** ✅
- **Where:** `shared/sdk/Renderer.cpp` → `get_engine_native_resource_d3d12()`.
- **Was:** hardcoded `*(tex+0xf0)+0x20`. **Now:** routes through `Texture::get_d3d12_resource_container()`,
  which for TDB≥71 **bruteforce-scans the object for the container's `via.render.RenderResource`
  typeinfo** (Renderer.cpp `get_d3d12_resource_container`, ~L1943) and reads the native at
  `+get_runtime_size()`. Verified equal to the old `0xf0` path (the "0xE0-NATIVE same=true" probe), so the
  swap is a free resilience win — this offset no longer goes stale.
- **Sanity check:** the container scan logs `Searching for Texture D3D12Resource offset …` once, then the
  eye clones' natives are non-null and readable.

### 2.8 `back_reserved_texture` — **T1 (pure D3D12)** ✅
- **Where:** `src/mods/vr/D3D12Component.cpp` → `back_reserved_texture()`.
- No version-dependent constant. Only Wilds assumption: `create_texture` returns RESERVED (tiled)
  resources — guarded by `desc.Layout == 64KB_UNDEFINED/STANDARD_SWIZZLE`, so it self-skips on committed
  resources. Heap flag `ALLOW_ONLY_RT_DS_TEXTURES` matches the clones' `ALLOW_RENDER_TARGET`.
- **Sanity check:** `[Flat3D] back_reserved_texture: mapped 510 tiles (31 MB) …` per 4K clone (twice —
  one per eye).

### 2.9 Dormant / not load-bearing: `resolve_rtv_pool_device`, `create_render_target_view`
- The abandoned RTV-swap redirect. `create_render_target_view` **device-removes the GPU** when called from
  a render-layer hook on Wilds, so the current harvest never uses it. Left in-tree (harmless, correct for
  engines whose device carries the pool). Not needed to keep Flat3D working; ignore unless reviving the
  redirect. `resolve_rtv_pool_device` reads the RTV-pool device global via
  `"B9 50 59 04 00 E8 …"`-style init-site scanning + `*(*global + 0x18)`.

---

### 2.10 GUI-camera projection hook — **T2 (multi-pattern fallback)**
- **Where:** `src/mods/VR.cpp` → `hijack_camera` (the `g_projection_matrix_hook2` install).
- **What:** the via.render GUI camera's get_ProjectionMatrix. The LEGACY byte pattern silently
  stopped matching on TDB>=74 (the historical "world-space GUI is invisible on Wilds" — elements
  world-placed but projected with the stock ortho). Now a 3-pattern fallback chain (incl.
  `"48 89 F2 E8"`-style for TDB>=74); a diagnostic fire-counter log confirms installation.
- **Re-derive:** find get_ProjectionMatrix via TDB reflection, disassemble the call site the old
  pattern anchored on, cut a new site pattern. Verify with the `GUI camera proj hook fired` log.

### 2.11 NGX (DLSS) depth/MV harvest — **T1-ish (driver-facing, not game-facing)**
- **Where:** `src/mods/vr/Flat3DNGX.cpp`.
- **What:** inline-hooks the game's own `NVSDK_NGX_D3D12_CreateFeature/EvaluateFeature/Release`
  (resolved from the loaded nvngx module by export, not by game pattern) and reads the parameter
  block for exact full-res depth/MV/scales. Immune to game updates; sensitive only to NGX SDK
  parameter-name changes (stable for years). HARD RULE: never record compute/rootsig/heap state on
  the game's DLSS command list (NV driver deferred-binding null-deref at +0x6b9755); copies and
  barriers only, or use an own queue-ordered CommandContext.

### 2.12 Overlay-RT GUI redirect — **T0 (no game constants at all)** ✅
- **Where:** `src/mods/vr/Flat3DGuiRedirect.{hpp,cpp}`.
- **Why it's update-proof:** after `create_target_state` proved unresolvable on Wilds (anchor
  strings deleted; target creation moved behind a name-hash registry) and
  `create_render_target_view` device-removed from layer hooks (§2.9), the redirect was rebuilt at
  the D3D12 DRIVER level: safetyhook detours on vtable entries (CreateRenderTargetView,
  CopyTextureRegion, CopyResource, OMSetRenderTargets) taken from OUR OWN device/command-list
  objects (same implementation class as the engine's). Scoping needs no engine knowledge: the
  pre/post overlay clone copies VR.cpp already records bracket the GUI draw in-stream and surface
  as copy calls with known dst resources = markers. The overlay target's own RTV descriptor
  predates hook install, so it can never be in the handle map — the ONLY unknown single-RTV bind
  inside the GUI window is the main target: learn it, hijack it (`learned main-target RTV handle`
  log). Frame-phase gotcha: world-anchored GUI content is projected during the game UPDATE phase,
  one frame-counter tick behind the render — captures belong in the OPPOSITE eye slot.

### 2.13 Offline re-derivation toolkit — `tools/wilds_re/` ✅
- pefile+capstone+numpy static analysis of the game exe: string→xref→disasm-window→pattern
  workflow, `.pdata` function-span index, executor dispatch-table dumper. Use it to VALIDATE any
  new byte pattern for uniqueness offline before shipping a runtime scan. See its README for the
  full playbook (reflection > string anchor > structure scan > runtime learn > D3D12-level).

### 2.14 Engine-native UI colour+alpha / hudless targets — **INVESTIGATED, PARKED**
- **Why we looked:** PureDark's own AFW build (PureDark/REFramework@RE9AFW) never captures the GUI —
  on RE9 it reads the engine's UI buffer straight off the overlay layer via the reflection field
  `UIBufferTexturePtr` (`layer->get_ui_buffer_tex_d3d12()`) and hands it to the plugin as
  `InUIColorAlpha`. If Wilds had the same, the whole D3D12 overlay-RT redirect (§2.12) AND the
  pre/post hudless clones could be replaced by two reflection reads.
- **What Wilds actually has** (exe string scan + a full TDB probe — `VR::flat3d_probe_engine_ui_targets`,
  enabled by the AFW debug toggle, logs every type exposing `UIColorAlpha`/`Hudless`/`UITarget`):
  - REFLECTED (usable): `via.render.layer.Scene::get_UseUIColorAlpha()`,
    `via.render.DLSSUpscalingInterface::get/set_UseUIColorAlpha()`,
    `via.render.DLSSFrameGenerationInterface::get/set_UseUIColorAlpha()`,
    `ace.cFrameGenerationSetting.cDLSS::_UseUIColorAlpha`,
    `via.render.layer.Overlay::get/set_UseMaskUITarget()`, plus FSR3 `DisableHudless` equivalents.
  - NOT reflected (native-only, though the names exist in the exe): `get_UIColorAlphaTexPtr`,
    `getDisplayUIColorAlphaTexPtr`, `getPresentUIColorAlphaTexPtr`, `getDisplayUIColorAlphaSrvPtr`,
    `preparePresentUIColorAlphaTexture`, `get_HudlessTexPtr`, `getDisplayHudlessTexPtr`,
    `getNonOCIOHudlessTexPtr` (+ Rtv variants), `getGUIBufferUITarget`.
- **Live state:** `Scene.UseUIColorAlpha=false`, `Overlay.UseMaskUITarget=false` — on Wilds this is a
  DLSS frame-generation feature (DLSS-G consumes hudless + UI-alpha), and it is OFF by default.
- **Verdict:** two blockers, so the redirect stays. (a) The feature must first be switched on, which
  means reaching a `DLSSUpscalingInterface`/`DLSSFrameGenerationInterface` instance (or the
  `ace.cFrameGenerationSetting.cDLSS` settings object) — not a managed singleton, so it needs its own
  derivation. (b) Even then the texture getter is native-only and would need the same treatment as
  `get_depth_stencil_d3d12` (pattern/offset RE). Revisit if the redirect ever breaks, or if a future
  Wilds patch reflects the Tex getters.
- **Cheap re-check after an update:** flip the AFW debug toggle and read `[Flat3D-UIProbe]` in the log;
  a `Scene.UseUIColorAlpha=true` line (logged once at the overlay hook) means the engine is producing
  the target and the hunt is worth it.

## 3. Triage recipe after a Wilds update

1. **Black screen (no crash):** the harvest produced nothing. Most likely §2.4 (`copy_engine` pattern
   stale) or §2.2 (`create_texture` string/convention changed). Check the one-shot logs:
   `create_texture(Wilds): ok=?`, `overlay-copy … SRC/DST fmt`. If `create_texture` logs `ok=0` or a bad
   native → §2.2/§2.3. If the copy runs but the clone is black → §2.4 or §2.6.
2. **Crash:** almost certainly a stale **T3 offset** dereferencing garbage — §2.6 (`prime_copy_dest_state`)
   first, then §2.1 (`get_runtime_size`, which corrupts *everything* downstream if wrong).
3. **Fast re-validation harness:** re-add a periodic readback of the eye clone's center texels after the
   harvest (transition `COPY_DEST`→`COPY_SOURCE`, `CopyTextureRegion` a 16×1 patch → a `READBACK` buffer,
   `Map`, log the dwords). Non-zero == harvest works. This is exactly the `[Flat3D] READBACK clone` /
   `[Flat3D-DIAG] BACKING` diagnostic scaffolding that was removed after the feature worked — reconstruct
   it from this description (or `git show` the pre-cleanup working tree if kept).
4. **`GetHeapProperties`** on a clone native distinguishes reserved (`E_INVALIDARG` + `Layout==2`) from
   committed — use it to confirm whether §0's reserved-resource assumption still holds.

## 4. What is safe across versions (no action needed)

Tile-mapping (§2.8), the compose/present pass (pure D3D12 + runtime-compiled HLSL), all settings, and
every reflection-based access (scene layers, camera duplicator, `get_target_state`, `set_output_state`,
`get_output_state_offset` which itself scans for the `via.render.TargetState` typeinfo). These ride on
praydog's SDK, which is maintained per game version upstream.
