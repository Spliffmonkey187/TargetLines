# TargetLines

FFXII style target lines for **Windower 4** — a port of Jyouya's Ashita addon.

Curved beams arc between whoever is acting and whatever they are acting on,
coloured by what kind of action it is, growing as the action lands and retracting
as it expires. Because the drawing happens inside the game's own 3D scene, the
arcs are **occluded by terrain and models** — something the Ashita original
cannot do.

## What this is

A **binary addon**, not a Windower plugin. Nothing goes in `plugins/`, there is
no `PluginBase`, and Windower's plugin ABI is never touched — so Windower and
`Hook.dll` updates cannot break it. The DLL lives in this addon's own `libs/`
folder and is loaded by its Lua through `package.cpath`.

This is the pattern the Windower developers pointed at when third-party plugins
reading `LuaCore.dll + 0x1c8400` broke on Hook 4.7.9.3.

## Why a DLL at all

Ashita hands addons LuaJIT, FFI and the live `IDirect3DDevice8`, so its
targetlines draws with raw D3D8 calls straight from Lua. Windower 4 runs plain
Lua 5.1 with no FFI and exposes no device, and its Lua drawing API
(`windower.prim`, texts, images) is axis-aligned 2D rectangles — no triangle
strips, no per-vertex colour, no arbitrary UVs, no depth. A curved tapered beam
cannot be built from those primitives, so the drawing has to happen in native
code.

Everything the module needs it takes from the game, never from Windower:

| Ashita / old plugins | here |
|---|---|
| `LuaCore.dll + 0x1c8400` actor pointer | FFXiMain's entity array, found by signature |
| Windower's `GetDirect3D8Device()` | a d3d8 resource off the renderer, asked `GetDevice` |
| `PostRender` / `d3d_present` | the game's own `draw_scene`, via SceneHook |

## Installing

Drop the `TargetLines` folder into Windower's `addons` folder:

```
Windower/
└── addons/
    └── TargetLines/
        ├── TargetLines.lua
        ├── tracker.lua
        ├── libs/
        │   └── _TargetLines.dll
        ├── SceneHook/
        └── src/
```

Then in game:

```
//lua load TargetLines
```

Nothing goes in the `plugins` folder and there is nothing to configure.

> Windows paths are case-insensitive, so this collides with any existing
> `addons/targetlines` folder. Remove or rename that one first.

## Commands

```
//tlines                          status
//tlines help                     this list
//tlines on | off                 start or stop drawing
//tlines reset                    put every setting back to default
```

**What is drawn**

```
//tlines filter <mode>            all | alliance | party          (default all)
//tlines target                   toggle the always-on line to your target
```

**Shape of the arc**

```
//tlines arc                      curved or straight              (default curved)
//tlines arch <0-2>               rise as a fraction of distance  (default 0.18)
//tlines bow <degrees>            sideways lean                   (default 11.25)
//tlines width <pixels>           beam thickness                  (default 3)
//tlines orb <pixels>             travelling dot, 0 disables      (default 22)
//tlines depth                    world occlusion or always on top (default occluded)
```

**Where lines attach**

```
//tlines chest <0-1>              attach height, both ends        (default 0.70)
//tlines chest me <0-1>           just your character
//tlines chest target <0-1>       just the other end
//tlines anchor                   cycle model / bone / nameplate / entity
//tlines lift <yalms>             extra height, every anchor mode (default 0)
//tlines bones <n>                bones used to measure height    (default 13)
//tlines bone <n>                 which bone, in bone mode only   (default 2)
```

**Diagnostics**

```
//tlines probe [index|me]         model height, bone count, 16 bone heights
//tlines scan                     SceneHook bus: owner, slot, client count
```

Every numeric setting also accepts the word `default` to reset just that one,
for example `//tlines chest me default`. Querying any setting reports its
default alongside the current value.

## Line colours and timing

Straight from Ashita:

| what | colour | lingers |
|---|---|---|
| you or an ally → a monster | blue | 10s |
| a monster → you | red | 10s |
| a cure or buff between allies | green | 5s |
| a monster buffing another monster | orange | 5s |

Each arc runs through three phases. It **grows** out from the actor over half a
second with a bright dot riding its head; a sustained attack line **retracts and
goes** after 2.5 seconds so a long fight does not leave a permanent beam; and an
expiring line **withdraws into its target** over the last half second. Deaths
retract the lines involved early.

An action naming several targets fans out an arc to each.

Two entities targeting each other bow their arcs opposite ways, so the pair form
a lens rather than one hiding the other.

## Where lines attach

The default **model** anchor measures each model and attaches at a fraction of
its height, so one setting works on a Tarutaru and a Goobbue alike.

This is a deliberate departure from Ashita, which uses a fixed bone. No fixed
bone index works across FFXI's model range — bone 2 sits at 77% of height on an
Elvaan but 96% on a Goobbue, and no constant offset can fix a proportional error.
The measurement uses only the first 13 bones, because bones past the core are
appendages: a Skimmer's antennae and a Moss Eater's raised tail sit well above
the body and would otherwise be measured as its height.

Three older anchors remain, reachable with `//tlines anchor`:

- **bone** — Ashita's `helpers.getBone` walk, halved against the model base.
  Exact parity with the original; use `//tlines bone <n>` to pick the bone.
- **nameplate** — `display + 0x678`, the offset TargetRing and GEO-HUD both use.
  Wants a `lift` of roughly 1.4 on a Hume.
- **entity** — `entity + 0x004`. Always readable, but it is the client's
  predicted position and leads the model while a mob runs.

All of them fall back down the chain, and finally to the coordinates Lua sends,
so an entity whose model has not streamed in still gets a line.

`//tlines probe` is the tool for all of this. Target something and run it: it
reports the model height, which bone it came from, the bone count and the first
sixteen bone heights.

## Running alongside TargetRing and GEO-HUD

All three draw inside `draw_scene`, and all three share **SceneHook** —
Broguypal's bus, which owns the single patch for the whole process and dispatches
to every registered addon in the same frame.

Load order does not matter, any of them can be unloaded or reloaded at any time,
and no module is pinned. `//tlines scan` shows the bus state:

```
scene: riding along, 3 clients | slot 2, owner 0, patched yes
```

`SceneHook/` is copied byte-identical from TargetRing, as its ABI check requires.
Every participant must build against the same copy.

> An addon that patches `draw_scene` itself instead of using SceneHook will
> conflict. The loser reports `draw_scene signature not found` and draws nothing.
> MogSafe's TargetLines draws in `PostRender` rather than patching, so it does
> not conflict and can run side by side.

## Building

FFXI is a 32-bit process, so the module must be too.

```sh
cmake -S src -B src/build -A Win32
cmake --build src/build --config Release
copy src\build\Release\_TargetLines.dll libs\
```

No DirectX SDK is required. `d3d8.h` is gone from the Windows SDK *and* the June
2010 DirectX SDK, so the ~30 structs and constants actually needed are vendored
in [`src/d3d8_min.h`](src/d3d8_min.h) — the device is only ever called through
vtable slots on an opaque pointer, so nothing more is needed.

MinGW works too (`-m32`), per the `else()` branch in `src/CMakeLists.txt`.

> The DLL is locked while the addon is loaded, and Windows will refuse to
> overwrite it. Run `//lua unload TargetLines` first, copy, then load again.
> `//lua reload` is not enough.

## Roadmap

- [x] **Stage 1** — scene hook, device, entity anchoring, depth-tested line
- [x] **Stage 2** — quadratic Bézier arc with the sideways bow, generated glow
      texture, and the travelling orb at the leading tip
- [x] **Stage 3** — port of `tracker.lua`: action packets (`0x028`) and message
      packets (`0x029`), the four line colours, the three animation phases, and
      the All / Alliance / Party filter
- [ ] config UI and settings persistence

Engineering detail — verified memory offsets, the bone data behind the anchoring
design, rendering internals and the gotchas — is in
[KNOWLEDGE.md](KNOWLEDGE.md).

### Known open questions

- Whether the 13-bone cutoff used to measure model height holds for every
  skeleton in the game.
- Whether trusts, avatars and charmed pets colour blue rather than red. Ashita
  reads spawn flag `0x100`, which Windower does not expose.
- Whether the generated glow reads well at the default 3px line width.

## Credits

- **Jyouya** — the original [Ashita targetlines](https://github.com/Jyouya/targetlines)
  this ports. That repository carries no licence file, so its `beam.png` and
  `orb.png` are not reused; the beam and orb textures here are generated at load
  from a falloff curve.
- **Broguypal** — [TargetRing](https://github.com/Broguypal/Addons), for SceneHook,
  the device acquisition and the entity-array scan (BSD 3-Clause).
- **Genoxd** — [ffxi_world_draw](https://github.com/genoxd/ffxi_world_draw),
  whose depth mapping, batching and render-state discipline this follows (0BSD).
- **Arcon** for the entity array signature in `libraries/memory/types.lua`, and
  **Rubenator** and **Darkdoom** for the nameplate-bone anchoring advice.
