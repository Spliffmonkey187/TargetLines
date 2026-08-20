# TargetLines — engineering knowledge base

Everything learned building a Windower 4 port of Jyouya's Ashita `targetlines`.
Written so work can resume cold. Last updated 2026-08-20.

---

## 1. The core problem

**Ashita hands addons the D3D8 device in Lua. Windower does not.**

Ashita v4 gives addons LuaJIT + FFI + `require('d3d8').get_device()` and a
`d3d_present` event, so `drawArc.lua` issues raw Direct3D calls straight from
Lua. Windower 4 runs **plain Lua 5.1** (`lua51.dll`, 138 KB — no LuaJIT, no FFI)
and exposes no device at all. Its Lua drawing API (`windower.prim`, texts,
images) is axis-aligned 2D rectangles: no triangle strips, no per-vertex colour,
no arbitrary UVs, no depth.

A curved, tapered, textured beam cannot be built from those primitives. Native
code is the only route — which is why TargetRing and GEO-HUD ship DLLs.

---

## 2. Binary addon, never a plugin

The Windower developers' position (Discord `#development`, Aug 2026): third-party
**plugins** are unsupported and break on updates. `LuaCore.dll + 0x1c8400` was
never an API.

The sanctioned pattern is a **binary addon**:

- DLL lives in the addon's own `libs/`, loaded by its Lua via `package.cpath`
- exports `luaopen__TargetLines`, resolves the Lua C API from `lua51.dll` by name
- **never touches Windower's plugin ABI** — no `PluginBase`, no `CreateInstance`,
  no `PostRender`, no `GetDirect3D8Device`

Consequence: Windower/Hook.dll updates structurally cannot break it. Everything
comes from the *game*, not from Windower:

| Old plugin approach | Binary addon approach |
|---|---|
| `LuaCore.dll + 0x1c8400` actor pointer | FFXiMain entity array, found by signature |
| Windower's `GetDirect3D8Device()` | a d3d8 resource off the renderer, asked `GetDevice` |
| `PostRender` / `d3d_present` | the game's own `draw_scene` |

Bonus: inside `draw_scene` the **depth buffer is still bound**, so geometry is
occluded by the world. Ashita's version sets `D3DRS_ZENABLE, 0` and paints over
everything. Also makes Hook 4.7.9.3 clearing matrices before `PostRender`
irrelevant — we read them live from the device inside the scene pass.

---

## 3. SceneHook — do not patch `draw_scene` yourself

**This is the single most important architectural fact.**

`FFXiMain!draw_scene` has one prologue, so only one addon can patch it.
Broguypal's **SceneHook** (shipped with TargetRing 3.0.0) patches it once
process-wide and dispatches to registered callbacks.

Located at `addons/TargetRing/SceneHook/`. Self-contained, BSD 3-Clause,
explicitly reusable. **Copy it byte-identical** — the ABI version is checked at
runtime and a mismatched module refuses to join.

```cpp
g_bus = scenehook_attach();
scenehook_ensure_hook(g_bus);
g_slot = scenehook_register(g_bus, &scene_draw, nullptr);
```

Two obligations it imposes:

1. **Leave device state as you found it.** Several addons draw back to back in
   one frame. We save/restore every render state, texture stage state and the
   vertex shader in `begin_draw_state`/`end_draw_state`.
2. **`SCENEHOOK_ALIGN_STACK` on the draw callback.** FFXi's render thread does
   not guarantee 16-byte stack alignment. No-op on MSVC, matters on MinGW.

Teardown must call `scenehook_unregister(bus, slot, false)` from
`DLL_PROCESS_DETACH` — the only point guaranteed to run however the addon goes
away, and it runs before the image unmaps.

### The failure mode to recognise

If you patch `draw_scene` yourself while SceneHook owns it, you get:

```
draw_scene signature not found
```

This is **documented expected behaviour**, not a bug. We hit it and wasted a
cycle chasing a phantom game patch. `//tring` showing `driving the frame,
N clients` means SceneHook already owns the patch.

### History (why the code looks like it does)

The module previously hand-rolled its own hook, twice:

1. TargetRing v2-style inline `E9` jmp with trampoline + chaining.
2. Then a resident stub in a `VirtualAlloc`'d page behind an `enabled` flag,
   following vulture's advice not to unhook (unhooking strands anyone who hooked
   on top of you).

Both were deleted (~370 lines) when SceneHook was found. SceneHook implements
the same insight better: an indirect jump through an `owner_ptr` outside every
module, with ownership handed over by one atomic aligned store.

---

## 4. Memory layout and offsets

All verified in game on 2026-08-20.

### Entity array

Signature scanned in `FFXiMain.dll`, per Windower's
`libraries/memory/types.lua` (credit Arcon), by way of TargetRing:

```
8B 56 0C 8B 04 2A 8B 04 85   then imm32 = array base (not a pointer to it)
```

Resolved live at `0x06320B30`. Max index `0x900`.

### Entity / display object

| offset | meaning |
|---|---|
| `entity + 0x004` | predicted position — **leads the model while running** |
| `entity + 0x0A0` | pointer to display object |
| `display + 0x678` | nameplate base: east, height, north (3 floats) |
| `display + 0x6B8` | skeleton pointer |

`display + 0x678` is confirmed by both TargetRing and GEO-HUD. **What Ashita
calls the actor pointer is this display object** — its offsets carry over.

### Skeleton walk (ported from Ashita `helpers.lua`, confirmed working)

```
skeleton_base   = display + 0x6B8
skeleton_offset = skeleton_base + 0x0C
skeleton        = *skeleton_offset
bone_count      = u16 at skeleton + 0x32
generators      = skeleton + 0x30 + 0x04 + 0x1E*bone_count + 4
bone origin     = generators + bone*0x1A + 0x0E   →  3 floats: east, height, north
```

**Heights are negative-up.**

---

## 5. Anchoring — the hardest part, and the data

Where the line attaches to a model. Several approaches were tried and measured.

### Measured bone tables

| bone | Elvaan male (99 bones) | Elder Goobbue (61) | Moss Eater (52) |
|---|---|---|---|
| 2 | −1.60 | −4.25 | −1.70 |
| 5 | 0.00 | −3.74 | 0.00 |
| 12 | **−2.09** | **−4.44** | — |

Bone counts differ per skeleton and bone 2 scales with model size — this is real
skeleton data, not a garbage read.

### What does not work

- **Ashita's `base + bone2/2`** lands at 38% of height on an Elvaan and 48% on a
  Goobbue. That's why the line attached at hip height.
- **Bone 2 un-halved**: 77% on an Elvaan (chest, good), 96% on a Goobbue (its
  head, bad). No fixed bone index works across models.
- **A constant lift** can't fix it — the error is proportional, not additive.
  Lift 1.5 suited a Hume and Goobbue but floated above a Moss Eater.
- **Maximum bone height as "model height"** is fooled by appendages: a Skimmer's
  wings/antennae and a Moss Eater's neck sit far above the body, putting the line
  in empty air.

### What works (current default)

Take the highest bone among the **first 13 only** — low indices are root and
spine, appendages come later — and attach at a **fraction of that height**:

```
anchor_height = base + model_height * 0.70
```

Self-scaling from a hare to a dragon with no per-model tuning. Tunable live:
`//tlines chest <0-1>` and `//tlines bones <n>`.

**Still open:** whether 13 is the right cutoff for every skeleton in the game.
Fallback if not: a percentile of bone heights rather than the max, which is
robust against one outlier limb.

---

## 6. Rendering

Vertices are `D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1` — pre-transformed,
so we project ourselves.

**Depth is what buys occlusion.** Each vertex carries
`z = viewport.MinZ + (clip_z/clip_w) * (MaxZ - MinZ)`, clamped. Leaving it at 0
puts every vertex on the near plane and the line paints over the whole world
(which is what Ashita does). `ZENABLE` on, `ZFUNC LESSEQUAL`, **`ZWRITE off`** so
we never disturb what the game draws after us.

### Coordinates

FFXI is east/north on the ground with height separate; D3D wants x/z on the
ground with y up. Kept in FFXI terms everywhere and swapped once, in
`world_to_screen`. **Height is negative-up**, so a lift *subtracts*.

### The arc

Quadratic Bézier. Control point = midpoint raised by `distance * 0.18`
(clamped 0.4–4.0 yalms), then **rotated about the start→end axis by ±11.25°**
(Ashita's π/16, Rodrigues rotation). The sideways rotation is what stops two
entities targeting each other from drawing one line on top of the other.

**Evaluated in world space**, unlike Ashita which projects the three control
points and evaluates in screen space. Ashita's way is cheaper but cannot carry
per-vertex depth.

**Reversal gotcha:** a retracting arc is drawn from the target end. Reversing the
axis mirrors the bow and makes the curve visibly jump sides. So the DLL takes a
`reverse` flag rather than having Lua swap endpoints, and flips the bow sign to
compensate.

### Textures

Generated procedurally at load, **not shipped** — Jyouya's repo has no LICENSE
file, so `beam.png`/`orb.png` are all-rights-reserved. Equivalents are built from
a falloff curve (smoothstep across the beam, radial for the orb). Also avoids any
image decoder.

Both are white with the shape entirely in alpha. Under
`D3DTOP_BLENDTEXTUREALPHA` that gives a white core fading outward through the
line's own colour — how Ashita gets its glow.

`d3d8.h` is gone from the Windows SDK *and* the June 2010 DirectX SDK. The ~30
structs and constants actually needed are vendored in `src/d3d8_min.h`, verified
against mingw's `d3d8types.h`. The device is only ever called through vtable
slots on an opaque pointer, so nothing more is required.

Vtable slots used: device `CreateTexture` 20, `GetTransform` 38, `GetViewport`
41, `SetRenderState` 50, `GetRenderState` 51, `GetTexture` 60, `SetTexture` 61,
`GetTextureStageState` 62, `SetTextureStageState` 63, `DrawPrimitiveUP` 72,
`SetVertexShader` 76, `GetVertexShader` 77. Texture `LockRect` 16, `UnlockRect`
17. Any COM object: `Release` 2, resource `GetDevice` 3.

---

## 7. Packet tracking

Windower's `packets` library already parses what Ashita bit-unpacks by hand.

**`0x028` action:** `Actor`, `Target Count`, `Category`, and `Target N ID` per
target. Category 4 is a completed spell.

**`0x029` message:** `Actor Index` (0x14), `Target Index` (0x16), `Message`
(0x18) — exactly where Ashita reads them. Death messages:
`6, 20, 97, 113, 406, 605, 646`.

### Colours and timing (Ashita's, unchanged)

| kind | colour | timeout |
|---|---|---|
| player → monster | `0xFF0088FF` blue | 10s |
| monster → player | `0xFFFF1133` red | 10s |
| ally → ally | `0xFF00FF66` green | 5s |
| monster → monster | `0xFFFF8800` orange | 5s |

Three animation phases:

- **fresh** — grows from the actor over 0.5s
- **sustained** — a player line held 2.5s retracts and goes, so a long fight
  doesn't leave a permanent beam
- **expiring** — withdraws into the target over the last 0.5s

Only growing arcs get the travelling orb.

**Uncertain:** pet/trust detection. Ashita reads spawn flag `0x100`; Windower
doesn't expose that bit, so we use `charmed` and `pet_owner_id`. A trust or
avatar attacking a mob **should draw blue, not red** — needs verifying in game.

---

## 8. Build and install

32-bit only. FFXI is a 32-bit process.

```sh
cmake -S src -B src/build -A Win32
cmake --build src/build --config Release
copy src\build\Release\_TargetLines.dll libs\
```

Toolchain present: VS 2022 Build Tools, `VC\Tools\MSVC\14.44.35207\bin\Hostx64\x86\cl.exe`.
No DirectX SDK needed. MinGW works too (`-m32`).

### The install gotcha

**The DLL is locked while the addon is loaded.** Windows refuses to overwrite it,
and hot-swapping it while mapped can crash the game. Workflow:

```
//lua unload TargetLines      (in game)
   ... copy the DLL ...
//lua load TargetLines
```

`//lua reload` is not enough. Installed at `e:\Windower4\addons\TargetLines\`.
Windows paths are case-insensitive, so `TargetLines` and `targetlines` are the
same folder — the old stub addon was renamed to `targetlines_OLD`.

### Diagnostics built into the module

```
//tlines scan             SceneHook bus state: owner, slot, client count
//tlines probe [index|me] model height, bone count, first 16 bone heights
//tlines status           everything at once
```

`//tlines probe` was the tool that solved anchoring. Add diagnostics to the
module rather than guessing — see §9.

---

## 9. Hard-won gotchas

**`FFXiMain.dll` is packed on disk.** `.text` has ~3.3 MB virtual size and
**zero raw bytes**; the payload sits in a `POL1` section. Scanning the file for
byte patterns returns nothing even when the pattern is present at runtime. Only
scan the **live process**. Reading the file is still fine for PE headers and the
patch date.

**Never check a whole section is readable then scan it.** Any `VirtualProtect`
ever performed inside FFXiMain splits the region permanently, after which a
whole-section check fails though every byte is readable. Walk contiguous
readable runs and merge adjacent ones. (Called out in SceneHook.h; it silently
skipped a section of FFXiMain for us. `scan_range` in the module now does this.)

**Every game-memory read goes through `span_readable` first.** A stale pointer
during a zone or logout is normal and must never crash.

**Windower Lua nil arguments become 0.** `f(nil)` reaches C as `gettop == 1` with
`lua_tonumber` returning 0 — a "report current value" command silently sets it to
zero. Pass nothing rather than nil.

**Ashita's `helpers.getBone` axis order** is x at `+0x0`, height at `+0x4`,
north at `+0x8` — *not* sequential x/y/z. Matches `display + 0x678`'s ordering
so the two add componentwise.

---

## 10. Project state

**Working:** SceneHook client (slot 2 alongside TargetRing and GEO-HUD),
live entity positions, self-scaling model anchoring, depth-occluded rendering,
world-space Bézier arc with sideways bow, packet-driven combat tracking with
four colours and three animation phases, procedural glow texture, travelling orb.

**Untested at time of writing:** the textured beam and orb (built and installed,
not yet seen in game).

**Open questions:**
- Is a 13-bone cutoff right for all skeletons? Percentile fallback if not.
- Do trusts/avatars/pets colour correctly?
- Does the glow read well, or does the beam need more width for the gradient?
- Does anything in TargetRing/GEO-HUD glitch from our texture-stage changes?

**Not started:** config UI, settings persistence.

---

## 11. Other people's work

| what | who | licence | role here |
|---|---|---|---|
| [targetlines](https://github.com/Jyouya/targetlines) (Ashita) | Jyouya | **none** | the original being ported; assets not reused |
| [TargetRing](https://github.com/Broguypal/Addons) + SceneHook | Broguypal | BSD 3-Clause | SceneHook, device acquisition, entity scan |
| [ffxi_world_draw](https://github.com/genoxd/ffxi_world_draw) | Genoxd | 0BSD | depth mapping, batching, render-state discipline |
| entity array signature | Arcon | — | `libraries/memory/types.lua` |
| nameplate-bone anchoring | Rubenator, Darkdoom | — | advice |
| resident-stub hook lifecycle | vulture | — | advice (superseded by SceneHook) |

**MogSafe's TargetLines** (`../MogSafe/`) is a separate, more feature-complete
implementation — but a **Windower plugin** (`PluginBase`, DLL in `plugins/`),
drawing in `PostRender` with `ZENABLE FALSE`, so it's a flat overlay with no
depth and will break on Windower updates. It does *not* patch `draw_scene`, so it
won't fight SceneHook and can run side by side for comparison. ~3,400 lines,
worth mining for packet-handling detail.
