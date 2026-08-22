// TargetLines - FFXII style target lines for Windower 4.
//
// Stage 1: draw_scene hook, device acquisition, entity anchoring and a single
// depth-tested straight ribbon. The Bezier beam, textures and packet-driven arc
// tracking land on top of this in later stages.
//
// This is a *binary addon*, not a Windower plugin. It is loaded by the addon's
// own Lua through package.cpath and exports luaopen__TargetLines. It never
// touches Windower's plugin ABI, so Windower and Hook.dll updates cannot break
// it. Everything it needs comes from the game itself:
//
//   Windower's actor pointer  ->  FFXiMain's entity array, found by signature
//   Windower's D3D8 device    ->  a d3d8 resource hanging off the renderer
//   Windower's PostRender     ->  the game's own draw_scene
//
// draw_scene has one prologue and several addons want it, so this module does
// not patch it. It registers a draw callback with SceneHook, Broguypal's shared
// bus, which owns the single patch for the whole process and dispatches to every
// registered addon in the same frame. Load and unload order stop mattering, no
// module is pinned, and TargetRing and GEO-HUD compose with us for free.
//
// Device acquisition and the entity-array scan come from Broguypal's TargetRing
// (BSD 3-Clause, Copyright (c) 2026 Broguypal). SceneHook is his too, copied
// byte-identical as its ABI check requires. The projection and render-state
// handling follow Genoxd's ffxi_world_draw (0BSD). Targeting behaviour is a
// port of Jyouya's Ashita targetlines addon.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "d3d8_min.h"
#include "SceneHook.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace {

// ---------------------------------------------------------------------------
// Lua binding
//
// Windower's Lua is plain 5.1, so the C API is resolved from whichever host
// module exports it rather than linked against. Only the handful of entry
// points actually used are bound.
// ---------------------------------------------------------------------------

using lua_State = struct lua_State;
using lua_CFunction = int(__cdecl*)(lua_State*);

using fn_createtable = void(__cdecl*)(lua_State*, int, int);
using fn_pushcclosure = void(__cdecl*)(lua_State*, lua_CFunction, int);
using fn_setfield = void(__cdecl*)(lua_State*, int, char const*);
using fn_pushvalue = void(__cdecl*)(lua_State*, int);
using fn_pushstring = void(__cdecl*)(lua_State*, char const*);
using fn_gettop = int(__cdecl*)(lua_State*);
using fn_tonumber = double(__cdecl*)(lua_State*, int);

constexpr int kGlobalsIndex = -10002;

struct LuaApi {
    fn_createtable createtable = nullptr;
    fn_pushcclosure pushcclosure = nullptr;
    fn_setfield setfield = nullptr;
    fn_pushvalue pushvalue = nullptr;
    fn_pushstring pushstring = nullptr;
    fn_gettop gettop = nullptr;
    fn_tonumber tonumber = nullptr;

    bool ready() const {
        return createtable && pushcclosure && setfield && pushvalue
            && pushstring && gettop && tonumber;
    }
};

LuaApi g_lua {};

// ---------------------------------------------------------------------------
// Types and tunables
// ---------------------------------------------------------------------------

// FFXI ground coordinates are east/north with height on a separate axis, which
// is not D3D's convention. Kept in FFXI terms everywhere and swapped once, at
// projection time.
struct Position {
    float east = 0.0f;
    float north = 0.0f;
    float height = 0.0f;
};

struct DrawVertex {
    float x;
    float y;
    float z;
    float rhw;
    DWORD color;
    float u;
    float v;
};

struct Line {
    DWORD src_index = 0;
    DWORD dst_index = 0;
    Position src_fallback {};
    Position dst_fallback {};
    DWORD color = 0xFFFFFFFF;
    // How much of the arc to draw, 0..1, measured from the actor. Ashita grows
    // the line out to its target and retracts it as the action expires.
    float progress = 1.0f;
    // Retracting: the arc is drawn from the target back towards the actor.
    bool reverse = false;
    bool active = false;
};

// An area-of-effect indicator: a ring lying flat on the ground. Lua drives the
// animation and hands over the radius and colour it wants this frame, so the
// timing can be tuned without rebuilding the module.
struct Ring {
    DWORD index = 0;
    Position fallback {};
    float radius = 0.0f;
    DWORD color = 0xFFFFFFFF;

    // How the ring's height is found:
    //   0  the entity's feet, raised by `amount` yalms
    //   1  a fraction `amount` of the entity's own height, like the line anchor
    int mode = 0;
    float amount = 0.0f;

    // A partial arc instead of a closed ring: `extent` radians ending at
    // `head`. Zero extent means the whole circle. A partial arc is drawn as a
    // comet -- tapering in width and fading toward its tail, with the
    // travelling orb at its head.
    float head = 0.0f;
    float extent = 0.0f;

    // Thickness relative to the line width. Rings want to be heavier than the
    // arcs by default, because the comet taper thins its own tail to a quarter
    // and a three pixel line ends up sub-pixel there.
    float width = 1.0f;

    bool active = false;
};

constexpr int kMaxRings = 64;
constexpr int kRingSegments = 48;

constexpr int kMaxLines = 64;
constexpr int kMaxBatchVertices = 8190;
// Stage 1 draws straight ribbons, but the emitter is written for a polyline so
// the Bezier can be dropped in without touching it.
constexpr int kMaxRibbonPoints = 96;
constexpr DWORD kMaxIndex = 0x900;

// Entity layout, per Windower's libraries/memory/types.lua. Ashita reaches the
// same structure through GetActorPointer, so its offsets carry over directly:
// what Ashita calls the actor pointer is this display object.
constexpr std::uintptr_t kEntityDisplayPos = 0x004;   // predicted; leads the model
constexpr std::uintptr_t kEntityDisplayPtr = 0x0A0;
constexpr std::uintptr_t kDisplayNameplateBase = 0x678;  // east, height, north

// Doors, lamps and similar fixed-model objects are target_type 3, and their
// entity record sits at a staging position well away from where the mesh is
// actually drawn -- so a line to a door shot off across the room. For those the
// display object's own position is the real one. Same handling as TargetRing.
constexpr std::uintptr_t kDisplayPosition = 0x034;
constexpr std::uintptr_t kEntityTargetType = 0x0EE;
constexpr unsigned char kTargetTypeObject = 3;
constexpr std::uintptr_t kDisplaySkeleton = 0x6B8;

// Skeleton walk, ported from helpers.lua in Jyouya's addon. Bone 2 sits around
// the chest. Unverified on the Windower side, which is why it is not the
// default anchor.
constexpr std::uintptr_t kSkeletonOffsetField = 0x0C;
constexpr std::uintptr_t kSkeletonBoneCount = 0x32;
constexpr std::uintptr_t kSkeletonBuffer = 0x30;
constexpr std::uintptr_t kSkeletonHeaderSize = 0x04;
constexpr std::uintptr_t kBoneStride = 0x1E;
constexpr std::uintptr_t kGeneratorStride = 0x1A;
constexpr std::uintptr_t kGeneratorOrigin = 0x0E;
constexpr int kChestBone = 2;

// Which bone the line attaches to. Ashita uses 2. Selectable because bone
// numbering is not the same across model skeletons, and 2 sits low on some.
int g_bone = kChestBone;

// Defaults for every tunable, in one place so `reset` and the values reported
// by each command cannot drift apart.
//
// Passing this sentinel to any setter restores that one setting. It sits
// outside every valid range, including bow's, which is legitimately negative.
constexpr float kRestoreDefault = -999.0f;

constexpr float kDefaultModelFraction = 0.70f;
constexpr float kDefaultAnchorLift = 0.0f;
constexpr float kDefaultHalfWidth = 1.5f;
constexpr float kDefaultArcRise = 0.18f;
constexpr float kDefaultArcBow = 0.19634954f;  // pi/16, Ashita's
constexpr float kDefaultOrbSize = 11.0f;
constexpr int kDefaultSurveyBones = 13;
constexpr int kDefaultBone = 2;

// Anchor modes, cycled from Lua so they can be compared in game.
enum AnchorMode {
    kAnchorModel = 0,      // fraction of the model's own height. Self-scaling.
    kAnchorBone = 1,       // Ashita's fixed bone, halved against the base.
    kAnchorNameplate = 2,  // display + 0x678, lifted. Proven by TargetRing.
    kAnchorEntity = 3,     // entity + 0x004, lifted. Safest, laggiest.
    kAnchorModeCount = 4,
};

int g_anchor_mode = kAnchorModel;

// Where on the model the line attaches, as a fraction of its height.
//
// No fixed bone index works across models: on a Hume bone 2 sits at 77% of
// height, on a Goobbue at 96%. But the highest bone in the skeleton tracks the
// top of the model on both, so a fraction of it lands in the same place on a
// hare and a dragon alike. 0.70 is roughly the chest.
//
// Held separately for the player and for everyone else, because the two are
// judged differently: your own character is on screen constantly and close to
// the camera, while targets are seen at a distance and at every size.
float g_model_fraction_self = kDefaultModelFraction;
float g_model_fraction_other = kDefaultModelFraction;

// Which entity is the player. Lua refreshes it each frame; zero means unknown,
// in which case everything uses the target fraction.
DWORD g_player_index = 0;

// Scopes for the chest command.
enum ChestScope {
    kChestBoth = 0,
    kChestSelf = 1,
    kChestOther = 2,
};

float model_fraction_for(DWORD index) {
    return (g_player_index != 0 && index == g_player_index)
        ? g_model_fraction_self : g_model_fraction_other;
}

// Extra height on top of whatever the anchor resolved, in yalms. Applies in
// every mode. Zero by default because the model anchor already scales itself;
// the nameplate and entity anchors want roughly 1.4 on a Hume.
float g_anchor_lift = kDefaultAnchorLift;

// Ribbon half-width in pixels. Ashita uses a constant 3px full width.
float g_half_width = kDefaultHalfWidth;

// 0 = depth tested against the world, 1 = always on top the way Ashita draws.
int g_depth_mode = 0;

Line g_lines[kMaxLines] {};
volatile int g_line_count = 0;

Ring g_rings[kMaxRings] {};
volatile int g_ring_count = 0;

// How far above the ground the ring sits, in yalms. Just enough to clear the
// terrain and avoid z-fighting with it.
float g_ring_clearance = 0.05f;

char g_status[192] = "idle";

// ---------------------------------------------------------------------------
// Memory safety
//
// Every game-memory read goes through span_readable first. A stale pointer
// during a zone or a logout is normal, and must never be a crash.
// ---------------------------------------------------------------------------

bool page_readable(DWORD protect) {
    if (protect & (PAGE_GUARD | PAGE_NOACCESS)) {
        return false;
    }

    switch (protect & 0xFF) {
    case PAGE_READONLY:
    case PAGE_READWRITE:
    case PAGE_WRITECOPY:
    case PAGE_EXECUTE_READ:
    case PAGE_EXECUTE_READWRITE:
    case PAGE_EXECUTE_WRITECOPY:
        return true;
    default:
        return false;
    }
}

bool span_readable(std::uintptr_t address, std::size_t size) {
    if (address == 0 || size == 0) {
        return false;
    }

    MEMORY_BASIC_INFORMATION region {};
    if (!VirtualQuery(reinterpret_cast<void const*>(address), &region, sizeof(region))) {
        return false;
    }

    if (region.State != MEM_COMMIT || !page_readable(region.Protect)) {
        return false;
    }

    std::uintptr_t const low = reinterpret_cast<std::uintptr_t>(region.BaseAddress);
    return address >= low && address + size <= low + region.RegionSize;
}

template <typename T>
bool read_memory(std::uintptr_t address, T& out) {
    if (!span_readable(address, sizeof(T))) {
        return false;
    }

    std::memcpy(&out, reinterpret_cast<void const*>(address), sizeof(T));
    return true;
}

bool module_range(char const* name, std::uintptr_t& base, std::size_t& size) {
    base = 0;
    size = 0;

    HMODULE module = GetModuleHandleA(name);
    if (!module) {
        return false;
    }

    base = reinterpret_cast<std::uintptr_t>(module);
    if (!span_readable(base, sizeof(IMAGE_DOS_HEADER))) {
        return false;
    }

    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        return false;
    }

    std::uintptr_t const nt_address = base + static_cast<std::uintptr_t>(dos->e_lfanew);
    if (!span_readable(nt_address, sizeof(IMAGE_NT_HEADERS32))) {
        return false;
    }

    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS32 const*>(nt_address);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        return false;
    }

    size = nt->OptionalHeader.SizeOfImage;
    return size != 0;
}

// Scan a byte range, walking it in contiguous runs of committed readable memory.
//
// Checking that a whole section is readable and then scanning across it is
// wrong: any VirtualProtect ever performed inside FFXiMain's code splits the
// region permanently, and from then on a whole-section check fails even though
// every byte is still readable. Merging adjacent readable runs also means a
// match straddling a split is still found. (The bug, and this fix, are called
// out in SceneHook.h -- it cost us a section of FFXiMain being silently
// skipped.)
std::uintptr_t scan_range(std::uintptr_t begin, std::uintptr_t end,
    unsigned char const* pattern, char const* mask, std::size_t length) {
    std::uintptr_t address = begin;

    while (address < end) {
        MEMORY_BASIC_INFORMATION region {};
        if (!VirtualQuery(reinterpret_cast<void const*>(address), &region, sizeof(region))) {
            return 0;
        }

        std::uintptr_t const region_end =
            reinterpret_cast<std::uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (region_end <= address) {
            return 0;
        }

        if (region.State != MEM_COMMIT || !page_readable(region.Protect)) {
            address = region_end;
            continue;
        }

        std::uintptr_t const run_start = address;
        std::uintptr_t run_end = region_end;
        while (run_end < end) {
            MEMORY_BASIC_INFORMATION next {};
            if (!VirtualQuery(reinterpret_cast<void const*>(run_end), &next, sizeof(next))
                || next.State != MEM_COMMIT
                || !page_readable(next.Protect)) {
                break;
            }

            std::uintptr_t const next_end =
                reinterpret_cast<std::uintptr_t>(next.BaseAddress) + next.RegionSize;
            if (next_end <= run_end) {
                break;
            }

            run_end = next_end;
        }

        std::uintptr_t const limit = run_end < end ? run_end : end;
        if (limit >= run_start + length) {
            auto const* bytes = reinterpret_cast<unsigned char const*>(run_start);
            std::size_t const span = static_cast<std::size_t>(limit - run_start);
            for (std::size_t offset = 0; offset + length <= span; ++offset) {
                bool hit = true;
                for (std::size_t j = 0; j < length; ++j) {
                    if (mask[j] != '?' && bytes[offset + j] != pattern[j]) {
                        hit = false;
                        break;
                    }
                }

                if (hit) {
                    return run_start + offset;
                }
            }
        }

        address = run_end;
    }

    return 0;
}

std::uintptr_t scan_module(char const* name, unsigned char const* pattern,
    char const* mask, std::size_t length) {
    std::uintptr_t base = 0;
    std::size_t image = 0;
    if (!module_range(name, base, image)) {
        return 0;
    }

    auto const* dos = reinterpret_cast<IMAGE_DOS_HEADER const*>(base);
    auto const* nt = reinterpret_cast<IMAGE_NT_HEADERS32 const*>(
        base + static_cast<std::uintptr_t>(dos->e_lfanew));

    auto const* section = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if ((section->Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0) {
            continue;
        }

        std::uintptr_t const start = base + section->VirtualAddress;
        std::size_t const span = section->Misc.VirtualSize;
        if (span <= length) {
            continue;
        }

        std::uintptr_t const hit = scan_range(start, start + span, pattern, mask, length);
        if (hit != 0) {
            return hit;
        }
    }

    return 0;
}

void* vtable_slot(std::uintptr_t object, int index) {
    if (!span_readable(object, sizeof(std::uintptr_t))) {
        return nullptr;
    }

    std::uintptr_t vtable = 0;
    std::memcpy(&vtable, reinterpret_cast<void const*>(object), sizeof(vtable));

    std::uintptr_t const entry = vtable + static_cast<std::uintptr_t>(index) * sizeof(std::uintptr_t);
    if (!span_readable(entry, sizeof(std::uintptr_t))) {
        return nullptr;
    }

    std::uintptr_t address = 0;
    std::memcpy(&address, reinterpret_cast<void const*>(entry), sizeof(address));
    return reinterpret_cast<void*>(address);
}

// ---------------------------------------------------------------------------
// D3D8 device
//
// Called through the vtable by slot rather than through the COM interface, so
// the module never has to agree with the game about d3d8 header versions.
// ---------------------------------------------------------------------------

using fn_get_device = long(__stdcall*)(void*, void**);
using fn_release = unsigned long(__stdcall*)(void*);
using fn_get_transform = long(__stdcall*)(void*, DWORD, D3DMATRIX*);
using fn_get_viewport = long(__stdcall*)(void*, D3DVIEWPORT8*);
using fn_get_render_state = long(__stdcall*)(void*, DWORD, DWORD*);
using fn_set_render_state = long(__stdcall*)(void*, DWORD, DWORD);
using fn_get_vertex_shader = long(__stdcall*)(void*, DWORD*);
using fn_set_vertex_shader = long(__stdcall*)(void*, DWORD);
using fn_get_texture = long(__stdcall*)(void*, DWORD, void**);
using fn_set_texture = long(__stdcall*)(void*, DWORD, void*);
using fn_get_texture_stage_state = long(__stdcall*)(void*, DWORD, DWORD, DWORD*);
using fn_set_texture_stage_state = long(__stdcall*)(void*, DWORD, DWORD, DWORD);
using fn_draw_up = long(__stdcall*)(void*, DWORD, unsigned, void const*, unsigned);

std::uintptr_t g_device = 0;
void* d3d_device_ = nullptr;

// The device methods, resolved once.
//
// Every call used to go through vtable_slot(), which runs span_readable()
// twice -- once for the object, once for the vtable entry -- and each of those
// is a VirtualQuery, a kernel transition. begin_draw_state and end_draw_state
// alone make about sixty device calls, so a frame was spending well over a
// hundred VirtualQuery calls before touching any geometry.
//
// The device is acquired once and its vtable does not move, so the pointers are
// resolved and validated at acquisition and called directly thereafter.
struct DeviceApi {
    fn_get_transform get_transform = nullptr;
    fn_get_viewport get_viewport = nullptr;
    fn_get_render_state get_render_state = nullptr;
    fn_set_render_state set_render_state = nullptr;
    fn_get_vertex_shader get_vertex_shader = nullptr;
    fn_set_vertex_shader set_vertex_shader = nullptr;
    fn_get_texture get_texture = nullptr;
    fn_set_texture set_texture = nullptr;
    fn_get_texture_stage_state get_texture_stage_state = nullptr;
    fn_set_texture_stage_state set_texture_stage_state = nullptr;
    fn_draw_up draw_up = nullptr;
};

DeviceApi g_dev {};

void resolve_device_api() {
    g_dev.get_transform = reinterpret_cast<fn_get_transform>(vtable_slot(g_device, 38));
    g_dev.get_viewport = reinterpret_cast<fn_get_viewport>(vtable_slot(g_device, 41));
    g_dev.set_render_state = reinterpret_cast<fn_set_render_state>(vtable_slot(g_device, 50));
    g_dev.get_render_state = reinterpret_cast<fn_get_render_state>(vtable_slot(g_device, 51));
    g_dev.get_texture = reinterpret_cast<fn_get_texture>(vtable_slot(g_device, 60));
    g_dev.set_texture = reinterpret_cast<fn_set_texture>(vtable_slot(g_device, 61));
    g_dev.get_texture_stage_state = reinterpret_cast<fn_get_texture_stage_state>(
        vtable_slot(g_device, 62));
    g_dev.set_texture_stage_state = reinterpret_cast<fn_set_texture_stage_state>(
        vtable_slot(g_device, 63));
    g_dev.draw_up = reinterpret_cast<fn_draw_up>(vtable_slot(g_device, 72));
    g_dev.set_vertex_shader = reinterpret_cast<fn_set_vertex_shader>(vtable_slot(g_device, 76));
    g_dev.get_vertex_shader = reinterpret_cast<fn_get_vertex_shader>(vtable_slot(g_device, 77));
}

long dev_GetTransform(DWORD state, D3DMATRIX* out) {
    return g_dev.get_transform ? g_dev.get_transform(d3d_device_, state, out) : -1;
}

long dev_GetViewport(D3DVIEWPORT8* out) {
    return g_dev.get_viewport ? g_dev.get_viewport(d3d_device_, out) : -1;
}

long dev_GetRenderState(DWORD state, DWORD* out) {
    return g_dev.get_render_state ? g_dev.get_render_state(d3d_device_, state, out) : -1;
}

long dev_SetRenderState(DWORD state, DWORD value) {
    return g_dev.set_render_state ? g_dev.set_render_state(d3d_device_, state, value) : -1;
}

long dev_GetVertexShader(DWORD* out) {
    return g_dev.get_vertex_shader ? g_dev.get_vertex_shader(d3d_device_, out) : -1;
}

long dev_SetVertexShader(DWORD value) {
    return g_dev.set_vertex_shader ? g_dev.set_vertex_shader(d3d_device_, value) : -1;
}

long dev_GetTexture(DWORD stage, void** out) {
    return g_dev.get_texture ? g_dev.get_texture(d3d_device_, stage, out) : -1;
}

// GetTexture hands back a reference we owe. Released through slot 2 like any
// other COM object, so no interface declaration is needed.
void release_object(void* object) {
    if (!object) {
        return;
    }

    auto release = reinterpret_cast<fn_release>(
        vtable_slot(reinterpret_cast<std::uintptr_t>(object), 2));
    if (release) {
        release(object);
    }
}

long dev_SetTexture(DWORD stage, void* texture) {
    return g_dev.set_texture ? g_dev.set_texture(d3d_device_, stage, texture) : -1;
}

long dev_GetTextureStageState(DWORD stage, DWORD type, DWORD* out) {
    return g_dev.get_texture_stage_state ? g_dev.get_texture_stage_state(d3d_device_, stage, type, out) : -1;
}

long dev_SetTextureStageState(DWORD stage, DWORD type, DWORD value) {
    return g_dev.set_texture_stage_state ? g_dev.set_texture_stage_state(d3d_device_, stage, type, value) : -1;
}

long dev_DrawPrimitiveUP(DWORD type, unsigned count, void const* data, unsigned stride) {
    return g_dev.draw_up ? g_dev.draw_up(d3d_device_, type, count, data, stride) : -1;
}

using fn_create_texture = long(__stdcall*)(void*, unsigned, unsigned, unsigned,
    DWORD, DWORD, DWORD, void**);
using fn_lock_rect = long(__stdcall*)(void*, unsigned, D3DLOCKED_RECT*, RECT const*, DWORD);
using fn_unlock_rect = long(__stdcall*)(void*, unsigned);

long dev_CreateTexture(unsigned width, unsigned height, DWORD format, void** out) {
    auto f = reinterpret_cast<fn_create_texture>(vtable_slot(g_device, 20));
    return f ? f(d3d_device_, width, height, 1, 0, format, D3DPOOL_MANAGED, out) : -1;
}

// IDirect3DTexture8 adds LockRect at slot 16, after IDirect3DResource8's eleven
// entries and IDirect3DBaseTexture8's three.
long tex_LockRect(void* texture, D3DLOCKED_RECT* locked) {
    auto f = reinterpret_cast<fn_lock_rect>(
        vtable_slot(reinterpret_cast<std::uintptr_t>(texture), 16));
    return f ? f(texture, 0, locked, nullptr, 0) : -1;
}

long tex_UnlockRect(void* texture) {
    auto f = reinterpret_cast<fn_unlock_rect>(
        vtable_slot(reinterpret_cast<std::uintptr_t>(texture), 17));
    return f ? f(texture, 0) : -1;
}

// ---------------------------------------------------------------------------
// Textures
//
// Generated rather than loaded. Jyouya's repository carries no licence, so its
// beam.png and orb.png are not shipped here; these are equivalents built from a
// falloff curve, which also means no image decoder and no files to find.
//
// Both are white with the shape carried entirely in the alpha channel. Under
// D3DTOP_BLENDTEXTUREALPHA that gives a white core fading outward through the
// line's own colour -- the look Ashita gets from its PNGs.
// ---------------------------------------------------------------------------

void* g_beam_texture = nullptr;
void* g_orb_texture = nullptr;
bool g_textures_tried = false;

DWORD white_with_alpha(float alpha) {
    if (alpha < 0.0f) {
        alpha = 0.0f;
    } else if (alpha > 1.0f) {
        alpha = 1.0f;
    }

    DWORD const a = static_cast<DWORD>(alpha * 255.0f + 0.5f);
    return (a << 24) | 0x00FFFFFF;
}

bool fill_texture(void* texture, int width, int height, DWORD (*shade)(float, float)) {
    D3DLOCKED_RECT locked {};
    if (tex_LockRect(texture, &locked) < 0 || !locked.pBits) {
        return false;
    }

    auto* rows = static_cast<unsigned char*>(locked.pBits);
    for (int y = 0; y < height; ++y) {
        auto* row = reinterpret_cast<DWORD*>(rows + static_cast<std::size_t>(y) * locked.Pitch);
        float const v = (static_cast<float>(y) + 0.5f) / static_cast<float>(height);
        for (int x = 0; x < width; ++x) {
            float const u = (static_cast<float>(x) + 0.5f) / static_cast<float>(width);
            row[x] = shade(u, v);
        }
    }

    tex_UnlockRect(texture);
    return true;
}

// Across the beam: opaque down the middle, falling away to nothing at the
// edges. Constant along its length, so it can stretch to any distance.
DWORD beam_shade(float u, float v) {
    (void)u;
    float const centred = std::fabs(v * 2.0f - 1.0f);
    float const falloff = 1.0f - centred;
    return white_with_alpha(falloff * falloff * (3.0f - 2.0f * falloff));
}

// A round dot with a soft edge, for the head of a growing arc.
DWORD orb_shade(float u, float v) {
    float const dx = u * 2.0f - 1.0f;
    float const dy = v * 2.0f - 1.0f;
    float const r = std::sqrt(dx * dx + dy * dy);
    if (r >= 1.0f) {
        return white_with_alpha(0.0f);
    }

    float const falloff = 1.0f - r;
    return white_with_alpha(falloff * falloff);
}

void ensure_textures() {
    if (g_textures_tried || !d3d_device_) {
        return;
    }

    g_textures_tried = true;

    void* beam = nullptr;
    if (dev_CreateTexture(8, 32, D3DFMT_A8R8G8B8, &beam) >= 0 && beam) {
        if (fill_texture(beam, 8, 32, &beam_shade)) {
            g_beam_texture = beam;
        } else {
            release_object(beam);
        }
    }

    void* orb = nullptr;
    if (dev_CreateTexture(32, 32, D3DFMT_A8R8G8B8, &orb) >= 0 && orb) {
        if (fill_texture(orb, 32, 32, &orb_shade)) {
            g_orb_texture = orb;
        } else {
            release_object(orb);
        }
    }
}

// ---------------------------------------------------------------------------
// Projection
// ---------------------------------------------------------------------------

D3DMATRIX cached_view_ {};
D3DMATRIX cached_projection_ {};
D3DMATRIX cached_view_projection_ {};
bool projection_matrices_valid_ = false;

// Read live from the device inside the scene pass. This is why it stops
// mattering that Hook.dll clears the matrices before PostRender: we are not in
// PostRender.
bool refresh_projection_matrices() {
    if (!d3d_device_) {
        projection_matrices_valid_ = false;
        return false;
    }

    if (dev_GetTransform(D3DTS_VIEW, &cached_view_) < 0
        || dev_GetTransform(D3DTS_PROJECTION, &cached_projection_) < 0) {
        projection_matrices_valid_ = false;
        return false;
    }

    // A perspective projection always has m[3][3] == 0. Anything else means we
    // caught an orthographic pass, which is not the world.
    if (cached_projection_.m[3][3] != 0.0f) {
        projection_matrices_valid_ = false;
        return false;
    }

    for (int row = 0; row < 4; ++row) {
        for (int column = 0; column < 4; ++column) {
            cached_view_projection_.m[row][column] =
                cached_view_.m[row][0] * cached_projection_.m[0][column]
                + cached_view_.m[row][1] * cached_projection_.m[1][column]
                + cached_view_.m[row][2] * cached_projection_.m[2][column]
                + cached_view_.m[row][3] * cached_projection_.m[3][column];
        }
    }

    projection_matrices_valid_ = true;
    return true;
}

// World point -> screen vertex carrying the depth that places it in the scene.
// The depth is what buys in-world occlusion; leaving it at 0 would put every
// vertex on the near plane and the line would paint over the entire world,
// which is exactly what the Ashita version does.
bool world_to_screen(Position const& point, D3DVIEWPORT8 const& viewport,
    float& screen_x, float& screen_y, float& screen_z, float& screen_rhw) {
    if (!projection_matrices_valid_) {
        return false;
    }

    D3DMATRIX const& vp = cached_view_projection_;
    float const d3d_x = point.east;
    float const d3d_y = point.height;
    float const d3d_z = point.north;

    float const clip_x = d3d_x * vp.m[0][0] + d3d_y * vp.m[1][0]
        + d3d_z * vp.m[2][0] + vp.m[3][0];
    float const clip_y = d3d_x * vp.m[0][1] + d3d_y * vp.m[1][1]
        + d3d_z * vp.m[2][1] + vp.m[3][1];
    float const clip_z = d3d_x * vp.m[0][2] + d3d_y * vp.m[1][2]
        + d3d_z * vp.m[2][2] + vp.m[3][2];
    float const clip_w = d3d_x * vp.m[0][3] + d3d_y * vp.m[1][3]
        + d3d_z * vp.m[2][3] + vp.m[3][3];

    if (std::fabs(clip_w) <= 0.0001f) {
        return false;
    }

    float const ndc_x = clip_x / clip_w;
    float const ndc_y = clip_y / clip_w;
    if (clip_w < 0.0f || ndc_x < -4.0f || ndc_x > 4.0f || ndc_y < -4.0f || ndc_y > 4.0f) {
        return false;
    }

    screen_x = static_cast<float>(viewport.X)
        + (ndc_x + 1.0f) * static_cast<float>(viewport.Width) * 0.5f;
    screen_y = static_cast<float>(viewport.Y)
        + (1.0f - ndc_y) * static_cast<float>(viewport.Height) * 0.5f;

    float const span = viewport.MaxZ - viewport.MinZ;
    float const depth = viewport.MinZ + (clip_z / clip_w) * span;
    screen_z = std::fmax(0.0f, std::fmin(1.0f, depth));
    screen_rhw = 1.0f / clip_w;
    return true;
}

// ---------------------------------------------------------------------------
// Render state and batching
// ---------------------------------------------------------------------------

DWORD saved_shader_ = 0;
DWORD saved_alpha_ = 0;
DWORD saved_src_ = 0;
DWORD saved_dest_ = 0;
DWORD saved_z_ = 0;
DWORD saved_zfunc_ = 0;
DWORD saved_zwrite_ = 0;
DWORD saved_lighting_ = 0;
DWORD saved_cull_ = 0;
DWORD saved_fog_ = 0;
DWORD saved_alphatest_ = 0;
DWORD saved_colorop_ = 0;
DWORD saved_colorarg1_ = 0;
DWORD saved_colorarg2_ = 0;
DWORD saved_alphaop_ = 0;
DWORD saved_alphaarg1_ = 0;
DWORD saved_alphaarg2_ = 0;
DWORD saved_magfilter_ = 0;
DWORD saved_minfilter_ = 0;
DWORD saved_addressu_ = 0;
DWORD saved_addressv_ = 0;
void* saved_texture_ = nullptr;
bool draw_state_active_ = false;

// Whatever texture the pending batch was emitted under. Changing it sends the
// batch first, so beams and orbs never mix in one draw call.
void* g_batch_texture = nullptr;

DrawVertex batch_vertices_[kMaxBatchVertices] {};
int batch_vertex_count_ = 0;

bool begin_draw_state() {
    if (draw_state_active_ || !d3d_device_) {
        return false;
    }

    saved_texture_ = nullptr;
    dev_GetVertexShader(&saved_shader_);
    dev_GetRenderState(D3DRS_ALPHABLENDENABLE, &saved_alpha_);
    dev_GetRenderState(D3DRS_SRCBLEND, &saved_src_);
    dev_GetRenderState(D3DRS_DESTBLEND, &saved_dest_);
    dev_GetRenderState(D3DRS_ZENABLE, &saved_z_);
    dev_GetRenderState(D3DRS_ZFUNC, &saved_zfunc_);
    dev_GetRenderState(D3DRS_ZWRITEENABLE, &saved_zwrite_);
    dev_GetRenderState(D3DRS_LIGHTING, &saved_lighting_);
    dev_GetRenderState(D3DRS_CULLMODE, &saved_cull_);
    dev_GetRenderState(D3DRS_FOGENABLE, &saved_fog_);
    dev_GetRenderState(D3DRS_ALPHATESTENABLE, &saved_alphatest_);
    dev_GetTextureStageState(0, D3DTSS_COLOROP, &saved_colorop_);
    dev_GetTextureStageState(0, D3DTSS_COLORARG1, &saved_colorarg1_);
    dev_GetTextureStageState(0, D3DTSS_COLORARG2, &saved_colorarg2_);
    dev_GetTextureStageState(0, D3DTSS_ALPHAOP, &saved_alphaop_);
    dev_GetTextureStageState(0, D3DTSS_ALPHAARG1, &saved_alphaarg1_);
    dev_GetTextureStageState(0, D3DTSS_ALPHAARG2, &saved_alphaarg2_);
    dev_GetTextureStageState(0, D3DTSS_MAGFILTER, &saved_magfilter_);
    dev_GetTextureStageState(0, D3DTSS_MINFILTER, &saved_minfilter_);
    dev_GetTextureStageState(0, D3DTSS_ADDRESSU, &saved_addressu_);
    dev_GetTextureStageState(0, D3DTSS_ADDRESSV, &saved_addressv_);
    dev_GetTexture(0, &saved_texture_);

    // Texture colour blended over the vertex colour by the texture's alpha: a
    // white core that fades outward through the line's own colour. Alpha is
    // multiplied so a line can still be faded as a whole by its vertex alpha.
    dev_SetTextureStageState(0, D3DTSS_COLOROP, D3DTOP_BLENDTEXTUREALPHA);
    dev_SetTextureStageState(0, D3DTSS_COLORARG1, D3DTA_TEXTURE);
    dev_SetTextureStageState(0, D3DTSS_COLORARG2, D3DTA_DIFFUSE);
    dev_SetTextureStageState(0, D3DTSS_ALPHAOP, D3DTOP_MODULATE);
    dev_SetTextureStageState(0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE);
    dev_SetTextureStageState(0, D3DTSS_ALPHAARG2, D3DTA_DIFFUSE);

    // Clamped and filtered, so the soft edges stay soft and the last texel does
    // not wrap around to the other side of the beam.
    dev_SetTextureStageState(0, D3DTSS_MAGFILTER, D3DTEXF_LINEAR);
    dev_SetTextureStageState(0, D3DTSS_MINFILTER, D3DTEXF_LINEAR);
    dev_SetTextureStageState(0, D3DTSS_ADDRESSU, D3DTADDRESS_CLAMP);
    dev_SetTextureStageState(0, D3DTSS_ADDRESSV, D3DTADDRESS_CLAMP);

    g_batch_texture = nullptr;
    dev_SetTexture(0, nullptr);

    dev_SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    dev_SetRenderState(D3DRS_SRCBLEND, D3DBLEND_SRCALPHA);
    dev_SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
    dev_SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);

    // Inside draw_scene the depth buffer is still bound, so the line can be
    // occluded by real geometry. Depth writes stay off either way so nothing
    // the game draws afterwards is disturbed.
    dev_SetRenderState(D3DRS_ZENABLE, g_depth_mode == 0 ? TRUE : FALSE);
    dev_SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev_SetRenderState(D3DRS_ZWRITEENABLE, FALSE);

    dev_SetRenderState(D3DRS_LIGHTING, FALSE);
    dev_SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev_SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev_SetVertexShader(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1);

    draw_state_active_ = true;
    return true;
}

void end_draw_state() {
    if (!draw_state_active_ || !d3d_device_) {
        return;
    }

    dev_SetTexture(0, saved_texture_);
    release_object(saved_texture_);
    saved_texture_ = nullptr;

    dev_SetTextureStageState(0, D3DTSS_COLOROP, saved_colorop_);
    dev_SetTextureStageState(0, D3DTSS_COLORARG1, saved_colorarg1_);
    dev_SetTextureStageState(0, D3DTSS_COLORARG2, saved_colorarg2_);
    dev_SetTextureStageState(0, D3DTSS_ALPHAOP, saved_alphaop_);
    dev_SetTextureStageState(0, D3DTSS_ALPHAARG1, saved_alphaarg1_);
    dev_SetTextureStageState(0, D3DTSS_ALPHAARG2, saved_alphaarg2_);
    dev_SetTextureStageState(0, D3DTSS_MAGFILTER, saved_magfilter_);
    dev_SetTextureStageState(0, D3DTSS_MINFILTER, saved_minfilter_);
    dev_SetTextureStageState(0, D3DTSS_ADDRESSU, saved_addressu_);
    dev_SetTextureStageState(0, D3DTSS_ADDRESSV, saved_addressv_);
    g_batch_texture = nullptr;

    dev_SetRenderState(D3DRS_ALPHABLENDENABLE, saved_alpha_);
    dev_SetRenderState(D3DRS_SRCBLEND, saved_src_);
    dev_SetRenderState(D3DRS_DESTBLEND, saved_dest_);
    dev_SetRenderState(D3DRS_ZENABLE, saved_z_);
    dev_SetRenderState(D3DRS_ZFUNC, saved_zfunc_);
    dev_SetRenderState(D3DRS_ZWRITEENABLE, saved_zwrite_);
    dev_SetRenderState(D3DRS_LIGHTING, saved_lighting_);
    dev_SetRenderState(D3DRS_CULLMODE, saved_cull_);
    dev_SetRenderState(D3DRS_FOGENABLE, saved_fog_);
    dev_SetRenderState(D3DRS_ALPHATESTENABLE, saved_alphatest_);
    dev_SetVertexShader(saved_shader_);

    draw_state_active_ = false;
}

void flush_batch() {
    if (batch_vertex_count_ < 3 || !d3d_device_) {
        batch_vertex_count_ = 0;
        return;
    }

    dev_DrawPrimitiveUP(D3DPT_TRIANGLELIST,
        static_cast<unsigned>(batch_vertex_count_ / 3), batch_vertices_, sizeof(DrawVertex));
    batch_vertex_count_ = 0;
}

// Beams and orbs use different textures, so switching sends whatever is
// pending before rebinding.
void set_batch_texture(void* texture) {
    if (texture == g_batch_texture) {
        return;
    }

    flush_batch();
    g_batch_texture = texture;
    dev_SetTexture(0, texture);
}

void append_batch(DrawVertex const* vertices, int count) {
    if (count <= 0 || count > kMaxBatchVertices) {
        return;
    }

    if (batch_vertex_count_ + count > kMaxBatchVertices) {
        flush_batch();
    }

    std::memcpy(batch_vertices_ + batch_vertex_count_, vertices,
        static_cast<std::size_t>(count) * sizeof(DrawVertex));
    batch_vertex_count_ += count;
}

// ---------------------------------------------------------------------------
// Ribbon emitter
//
// Takes a polyline in world space, projects it, and extrudes each sample
// perpendicular to the screen-space path. Stage 1 feeds it two points; the
// Bezier will feed it forty without changing anything here.
// ---------------------------------------------------------------------------

struct RibbonSample {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float rhw = 1.0f;
    float nx = 0.0f;  // screen-space normal, already scaled to half width
    float ny = 0.0f;
    bool ok = false;
};

RibbonSample ribbon_[kMaxRibbonPoints] {};

// Scale a colour's alpha, returning it unchanged at 1.
DWORD scale_alpha(DWORD color, float scale) {
    float const alpha = static_cast<float>((color >> 24) & 0xFF) * scale;
    DWORD const clamped = static_cast<DWORD>(
        std::fmax(0.0f, std::fmin(255.0f, alpha)) + 0.5f);
    return (clamped << 24) | (color & 0x00FFFFFF);
}

// `taper` turns the ribbon into a comet: thin and faint at the start, full
// width and brightness at the end. Following the curve used by MogSafe's
// indicator, since it reads well -- alpha rises with the square of the distance
// along, which keeps the tail long and wispy rather than a blunt wedge.
void emit_ribbon(Position const* points, int count, DWORD color,
    D3DVIEWPORT8 const& viewport, bool taper = false, float width_scale = 1.0f) {
    if (count < 2 || count > kMaxRibbonPoints) {
        return;
    }

    for (int i = 0; i < count; ++i) {
        RibbonSample& sample = ribbon_[i];
        sample.ok = world_to_screen(points[i], viewport,
            sample.x, sample.y, sample.z, sample.rhw) && sample.rhw > 0.0f;
    }

    // Direction at each sample comes from its neighbours, so the ribbon keeps a
    // constant width around corners instead of pinching.
    for (int i = 0; i < count; ++i) {
        RibbonSample& sample = ribbon_[i];
        if (!sample.ok) {
            continue;
        }

        int const previous = (i > 0 && ribbon_[i - 1].ok) ? i - 1 : i;
        int const next = (i + 1 < count && ribbon_[i + 1].ok) ? i + 1 : i;
        if (previous == next) {
            sample.ok = false;
            continue;
        }

        float dx = ribbon_[next].x - ribbon_[previous].x;
        float dy = ribbon_[next].y - ribbon_[previous].y;
        float const length = std::sqrt(dx * dx + dy * dy);
        if (length <= 0.0001f) {
            sample.ok = false;
            continue;
        }

        dx /= length;
        dy /= length;

        float width = g_half_width * width_scale;
        if (taper) {
            float const t = static_cast<float>(i) / static_cast<float>(count - 1);
            width *= 0.28f + 0.72f * t;
        }

        sample.nx = -dy * width;
        sample.ny = dx * width;
    }

    set_batch_texture(g_beam_texture);

    // u runs along the beam, v across it, so the texture's soft edges land on
    // the outside of the ribbon whatever shape the curve takes.
    DrawVertex quad[6] {};
    for (int i = 0; i + 1 < count; ++i) {
        RibbonSample const& a = ribbon_[i];
        RibbonSample const& b = ribbon_[i + 1];
        if (!a.ok || !b.ok) {
            continue;
        }

        float const ua = static_cast<float>(i) / static_cast<float>(count - 1);
        float const ub = static_cast<float>(i + 1) / static_cast<float>(count - 1);

        DWORD ca = color;
        DWORD cb = color;
        if (taper) {
            ca = scale_alpha(color, 0.10f + 0.90f * ua * ua);
            cb = scale_alpha(color, 0.10f + 0.90f * ub * ub);
        }

        DrawVertex const a_plus {a.x + a.nx, a.y + a.ny, a.z, a.rhw, ca, ua, 0.0f};
        DrawVertex const a_minus {a.x - a.nx, a.y - a.ny, a.z, a.rhw, ca, ua, 1.0f};
        DrawVertex const b_plus {b.x + b.nx, b.y + b.ny, b.z, b.rhw, cb, ub, 0.0f};
        DrawVertex const b_minus {b.x - b.nx, b.y - b.ny, b.z, b.rhw, cb, ub, 1.0f};

        quad[0] = a_plus;
        quad[1] = a_minus;
        quad[2] = b_plus;
        quad[3] = a_minus;
        quad[4] = b_minus;
        quad[5] = b_plus;
        append_batch(quad, 6);
    }
}

// The bright dot that rides the head of a growing arc. A screen-space square,
// so it keeps its size however far away the action is -- the same trick Ashita
// uses with its 20 pixel orb quad.
float g_orb_size = kDefaultOrbSize;

void emit_orb(Position const& point, DWORD color, D3DVIEWPORT8 const& viewport) {
    if (!g_orb_texture) {
        return;
    }

    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    float rhw = 1.0f;
    if (!world_to_screen(point, viewport, x, y, z, rhw) || rhw <= 0.0f) {
        return;
    }

    set_batch_texture(g_orb_texture);

    float const h = g_orb_size;
    DrawVertex const tl {x - h, y - h, z, rhw, color, 0.0f, 0.0f};
    DrawVertex const tr {x + h, y - h, z, rhw, color, 1.0f, 0.0f};
    DrawVertex const bl {x - h, y + h, z, rhw, color, 0.0f, 1.0f};
    DrawVertex const br {x + h, y + h, z, rhw, color, 1.0f, 1.0f};

    DrawVertex const quad[6] = {tl, bl, tr, bl, br, tr};
    append_batch(quad, 6);
}

// ---------------------------------------------------------------------------
// Arc geometry
//
// A quadratic Bezier between the two anchors, matching Jyouya's Ashita addon:
// the control point is lifted above the midpoint, then rotated about the
// start->end axis so the curve bows sideways as well as up.
//
// The sideways rotation is what stops two entities targeting each other from
// drawing one line on top of another -- the sign flips with direction, so the
// pair form a lens rather than overlapping.
//
// Unlike Ashita, the curve is evaluated in world space and each sample is
// projected separately. Ashita projects the three control points and evaluates
// in screen space, which is cheaper but cannot carry per-vertex depth. Doing it
// in world space is what lets the arc pass behind terrain correctly.
// ---------------------------------------------------------------------------

// Arc rise as a fraction of the distance between the endpoints, so a line
// across a room bows more than one to an adjacent mob.
float g_arc_rise = kDefaultArcRise;
float g_arc_min = 0.40f;
float g_arc_max = 4.00f;

// Sideways bow, radians. Ashita uses pi/16.
float g_arc_bow = kDefaultArcBow;

int g_arc_samples = 40;

// 0 = arc, 1 = straight. Kept so stage 1's behaviour can be compared directly.
int g_arc_mode = 0;

Position bezier_sample(Position const& p0, Position const& p1, Position const& p2, float t) {
    float const u = 1.0f - t;
    float const a = u * u;
    float const b = 2.0f * u * t;
    float const c = t * t;

    return Position{
        a * p0.east + b * p1.east + c * p2.east,
        a * p0.north + b * p1.north + c * p2.north,
        a * p0.height + b * p1.height + c * p2.height,
    };
}

// Rodrigues rotation of v about unit axis k.
void rotate_about(float const* k, float const* v, float angle, float* out) {
    float const s = std::sin(angle);
    float const c = std::cos(angle);
    float const dot = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
    float const m = dot * (1.0f - c);

    out[0] = v[0] * c + (k[1] * v[2] - k[2] * v[1]) * s + k[0] * m;
    out[1] = v[1] * c + (k[2] * v[0] - k[0] * v[2]) * s + k[1] * m;
    out[2] = v[2] * c + (k[0] * v[1] - k[1] * v[0]) * s + k[2] * m;
}

// Fill `out` with `count` points along the arc from source to destination.
// `flip` mirrors the sideways bow for the opposite direction of a pair.
int build_arc(Position const& source, Position const& destination, bool flip,
    float progress, Position* out, int capacity) {
    if (capacity < 2 || progress <= 0.0f) {
        return 0;
    }

    if (progress > 1.0f) {
        progress = 1.0f;
    }

    if (g_arc_mode != 0) {
        out[0] = source;
        out[1] = Position{
            source.east + (destination.east - source.east) * progress,
            source.north + (destination.north - source.north) * progress,
            source.height + (destination.height - source.height) * progress,
        };
        return 2;
    }

    float axis[3] = {
        destination.east - source.east,
        destination.height - source.height,
        destination.north - source.north,
    };

    float const length = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (length < 0.01f) {
        out[0] = source;
        out[1] = destination;
        return 2;
    }

    axis[0] /= length;
    axis[1] /= length;
    axis[2] /= length;

    float rise = length * g_arc_rise;
    rise = std::fmax(g_arc_min, std::fmin(g_arc_max, rise));

    // Control point: the midpoint raised by `rise`. Height is negative-up.
    Position control{
        (source.east + destination.east) * 0.5f,
        (source.north + destination.north) * 0.5f,
        (source.height + destination.height) * 0.5f - rise,
    };

    // Rotate it about the start->end axis so the bow leans to one side.
    float const offset[3] = {
        control.east - source.east,
        control.height - source.height,
        control.north - source.north,
    };

    float rotated[3] {};
    rotate_about(axis, offset, flip ? g_arc_bow : -g_arc_bow, rotated);

    control.east = source.east + rotated[0];
    control.height = source.height + rotated[1];
    control.north = source.north + rotated[2];

    int const samples = g_arc_samples < 2 ? 2
        : (g_arc_samples > capacity ? capacity : g_arc_samples);

    // Walk only as far as `progress`, so the arc reaches out from the actor
    // rather than appearing whole.
    for (int i = 0; i < samples; ++i) {
        float const t = progress * static_cast<float>(i) / static_cast<float>(samples - 1);
        out[i] = bezier_sample(source, control, destination, t);
    }

    return samples;
}

// A circle lying flat on the ground, as a closed polyline. The last point
// repeats the first so the ribbon emitter closes the loop rather than leaving a
// notch at the seam.
//
// Nothing new is needed to draw it: a ring is just a polyline, so it goes
// through the same emitter as the arcs and picks up the same beam texture,
// screen-space thickness and depth handling for free.
// `extent` of zero or more than a full turn gives a closed ring; anything less
// gives an arc ending at `head`, which is drawn as a comet.
// A full ring always uses the same angles, so they are computed once. Comets
// sample an arbitrary arc and still need real trig, but they are short.
float g_ring_cos[kRingSegments + 1] {};
float g_ring_sin[kRingSegments + 1] {};

void build_ring_table() {
    for (int i = 0; i <= kRingSegments; ++i) {
        float const angle = 6.28318530718f * static_cast<float>(i)
            / static_cast<float>(kRingSegments);
        g_ring_cos[i] = std::cos(angle);
        g_ring_sin[i] = std::sin(angle);
    }
}

int build_ring(Position const& centre, float radius, float head, float extent,
    Position* out, int capacity) {
    if (radius <= 0.0f || capacity < 4) {
        return 0;
    }

    constexpr float kTwoPi = 6.28318530718f;
    bool const closed = extent <= 0.0f || extent >= kTwoPi;
    float const span = closed ? kTwoPi : extent;
    float const start = closed ? 0.0f : head - extent;

    // Keep the segment density roughly constant so a short comet is not drawn
    // with the same forty-eight segments as a whole circle.
    int segments = closed
        ? kRingSegments
        : static_cast<int>(std::ceil(span / kTwoPi * kRingSegments));
    if (segments < 6) {
        segments = 6;
    }
    if (segments + 1 > capacity) {
        segments = capacity - 1;
    }

    // A closed ring at full segment count is exactly the table.
    bool const tabulated = closed && segments == kRingSegments;

    for (int i = 0; i <= segments; ++i) {
        float cosine;
        float sine;
        if (tabulated) {
            cosine = g_ring_cos[i];
            sine = g_ring_sin[i];
        } else {
            float const angle = start + span * static_cast<float>(i)
                / static_cast<float>(segments);
            cosine = std::cos(angle);
            sine = std::sin(angle);
        }

        out[i] = Position{
            centre.east + radius * cosine,
            centre.north + radius * sine,
            centre.height,
        };
    }

    return segments + 1;
}

// ---------------------------------------------------------------------------
// Entity anchoring
// ---------------------------------------------------------------------------

std::uintptr_t g_entity_array = 0;
bool g_entity_array_resolved = false;

// mov edx,[esi+0xC] / mov eax,[edx+ebp] / mov eax,[eax*4+imm32] -- the trailing
// imm32 is the array base itself, not a pointer to it. Signature per Windower's
// libraries/memory/types.lua, by way of TargetRing.
std::uintptr_t entity_array() {
    if (g_entity_array_resolved) {
        return g_entity_array;
    }

    g_entity_array_resolved = true;

    static unsigned char const pattern[] = {
        0x8B, 0x56, 0x0C, 0x8B, 0x04, 0x2A, 0x8B, 0x04, 0x85,
    };
    static char const mask[] = "xxxxxxxxx";

    std::uintptr_t const match = scan_module("FFXiMain.dll", pattern, mask, sizeof(pattern));
    if (match == 0) {
        return 0;
    }

    std::uint32_t base = 0;
    if (!read_memory(match + sizeof(pattern), base)) {
        return 0;
    }

    if (!span_readable(base, sizeof(std::uintptr_t) * kMaxIndex)) {
        return 0;
    }

    g_entity_array = base;
    return g_entity_array;
}

std::uintptr_t entity_at(DWORD index) {
    std::uintptr_t const base = entity_array();
    if (base == 0 || index == 0 || index >= kMaxIndex) {
        return 0;
    }

    std::uintptr_t entity = 0;
    if (!read_memory(base + static_cast<std::uintptr_t>(index) * sizeof(std::uintptr_t), entity)) {
        return 0;
    }

    return entity;
}

bool plausible(float const* coords) {
    for (int i = 0; i < 3; ++i) {
        if (!std::isfinite(coords[i]) || std::fabs(coords[i]) > 10000.0f) {
            return false;
        }
    }

    return true;
}

// Which field on the display object actually holds this entity's position.
// Fixed objects keep theirs somewhere different; see kTargetTypeObject.
std::uintptr_t position_field_for(DWORD index) {
    std::uintptr_t const entity = entity_at(index);
    if (entity == 0) {
        return kDisplayNameplateBase;
    }

    unsigned char target_type = 0;
    if (read_memory(entity + kEntityTargetType, target_type)
        && target_type == kTargetTypeObject) {
        return kDisplayPosition;
    }

    return kDisplayNameplateBase;
}

// Ashita's helpers.getBone, ported. The skeleton is reached from the display
// object, walked to its generator array, and the bone's offset is added to the
// model's base position. Every step is guarded because a model that is still
// streaming in has a null or partial skeleton.
// The raw generator offset for one bone, relative to the model base, in
// east/height/north order to match the base position. Also reports how many
// bones the skeleton has, which is the quickest way to tell a real skeleton
// from a garbage read.
bool bone_offset(std::uintptr_t display, int bone, float* local, int* bone_count_out) {
    std::uint32_t skeleton_base = 0;
    if (!read_memory(display + kDisplaySkeleton, skeleton_base) || skeleton_base == 0) {
        return false;
    }

    std::uint32_t skeleton_offset = 0;
    if (!read_memory(skeleton_base + kSkeletonOffsetField, skeleton_offset) || skeleton_offset == 0) {
        return false;
    }

    std::uint32_t skeleton = 0;
    if (!read_memory(static_cast<std::uintptr_t>(skeleton_offset), skeleton) || skeleton == 0) {
        return false;
    }

    std::uint16_t bone_count = 0;
    if (!read_memory(skeleton + kSkeletonBoneCount, bone_count)) {
        return false;
    }

    if (bone_count_out) {
        *bone_count_out = static_cast<int>(bone_count);
    }

    if (bone_count == 0 || bone_count > 512 || bone < 0 || bone >= static_cast<int>(bone_count)) {
        return false;
    }

    std::uintptr_t const generators = skeleton + kSkeletonBuffer + kSkeletonHeaderSize
        + kBoneStride * bone_count + 4;
    std::uintptr_t const origin = generators
        + static_cast<std::uintptr_t>(bone) * kGeneratorStride + kGeneratorOrigin;

    if (!span_readable(origin, sizeof(float) * 3)) {
        return false;
    }

    // Generator layout is east, height, north -- the same ordering as the base
    // position, so the two add componentwise.
    std::memcpy(local, reinterpret_cast<void const*>(origin), sizeof(float) * 3);
    return plausible(local);
}

// The model's body height, taken as the highest bone in the *core* skeleton.
//
// Deliberately not the highest bone overall. Bones past the core are
// appendages -- a Skimmer's wings and antennae, a Moss Eater's neck and raised
// tail -- and they sit well above the body, which put the line in empty air
// above those mobs. Low indices are the root and spine on every skeleton
// sampled so far: bone 12 is the top of the head on both an Elvaan and a
// Goobbue, and the limbs come later.
//
// Heights are negative-up, so the highest bone is the most negative.
int g_survey_bones = kDefaultSurveyBones;

bool model_height(std::uintptr_t display, float& height, int* which) {
    float best = 0.0f;
    int best_bone = -1;

    for (int b = 0; b < g_survey_bones; ++b) {
        float local[3] {};
        if (!bone_offset(display, b, local, nullptr)) {
            break;
        }

        if (local[1] < best) {
            best = local[1];
            best_bone = b;
        }
    }

    // No measurable height means the skeleton is still streaming in; the caller
    // falls back rather than pinning the line to the model's feet.
    if (best > -0.05f) {
        return false;
    }

    if (which) {
        *which = best_bone;
    }

    height = best;
    return true;
}

// Attach at a fraction of the model's own height, so the line meets a hare and
// a dragon at the same point on the body without per-model tuning.
bool model_position(std::uintptr_t display, std::uintptr_t field,
    float fraction, Position& out) {
    float base[3] {};
    if (!span_readable(display + field, sizeof(base))) {
        return false;
    }

    std::memcpy(base, reinterpret_cast<void const*>(display + field), sizeof(base));
    if (!plausible(base)) {
        return false;
    }

    float top = 0.0f;
    if (!model_height(display, top, nullptr)) {
        return false;
    }

    out.east = base[0];
    out.north = base[2];
    out.height = base[1] + top * fraction - g_anchor_lift;

    float const check[3] = {out.east, out.height, out.north};
    return plausible(check);
}

bool bone_position(std::uintptr_t display, std::uintptr_t field,
    int bone, Position& out) {
    float base[3] {};
    if (!span_readable(display + field, sizeof(base))) {
        return false;
    }

    std::memcpy(base, reinterpret_cast<void const*>(display + field), sizeof(base));
    if (!plausible(base)) {
        return false;
    }

    float local[3] {};
    if (!bone_offset(display, bone, local, nullptr)) {
        return false;
    }

    // Ashita halves the bone height against the model base rather than using the
    // bone outright, which keeps the line steadier through animations. The lift
    // is applied on top so it can be trimmed without leaving bone mode -- set it
    // to 0 for exact Ashita parity.
    out.east = base[0] + local[0];
    out.height = base[1] + local[1] * 0.5f - g_anchor_lift;
    out.north = base[2] + local[2];

    float const check[3] = {out.east, out.height, out.north};
    return plausible(check);
}

// Where an entity is standing. Unlike anchor_position this deliberately takes
// no lift and no model fraction: a ring belongs on the ground at the model's
// feet, not up at its chest.
bool ground_position(DWORD index, Position const& fallback, Position& out) {
    std::uintptr_t const entity = entity_at(index);
    std::uintptr_t const field = position_field_for(index);

    if (entity != 0) {
        std::uint32_t display = 0;
        if (read_memory(entity + kEntityDisplayPtr, display) && display != 0) {
            float coords[3] {};
            if (span_readable(display + field, sizeof(coords))) {
                std::memcpy(coords,
                    reinterpret_cast<void const*>(display + field),
                    sizeof(coords));
                if (plausible(coords)) {
                    out.east = coords[0];
                    out.height = coords[1];
                    out.north = coords[2];
                    return true;
                }
            }
        }

        float coords[3] {};
        if (span_readable(entity + kEntityDisplayPos, sizeof(coords))) {
            std::memcpy(coords, reinterpret_cast<void const*>(entity + kEntityDisplayPos),
                sizeof(coords));
            if (plausible(coords)) {
                out.east = coords[0];
                out.height = coords[1];
                out.north = coords[2];
                return true;
            }
        }
    }

    out = fallback;
    return true;
}

// Where a ring sits. Mode 0 puts it on the ground, raised by `amount` yalms;
// mode 1 puts it at a fraction `amount` of the entity's own height, so it
// encircles the body rather than the feet.
bool ring_centre(Ring const& ring, Position& out) {
    if (!ground_position(ring.index, ring.fallback, out)) {
        return false;
    }

    if (ring.mode == 1) {
        std::uintptr_t const entity = entity_at(ring.index);
        std::uint32_t display = 0;
        float top = 0.0f;
        if (entity != 0 && read_memory(entity + kEntityDisplayPtr, display)
            && display != 0 && model_height(display, top, nullptr)) {
            // Heights are negative-up, so a fraction of `top` raises it.
            out.height += top * ring.amount;
            return true;
        }

        // No skeleton yet: fall back to a plausible chest height rather than
        // dropping the ring to the entity's feet.
        out.height -= 1.0f;
        return true;
    }

    out.height -= ring.amount + g_ring_clearance;
    return true;
}

// Resolve where a line should attach for an entity index. Falls back down the
// chain -- bone, then nameplate base, then the entity's own position -- so a
// mob whose model has not streamed in still gets a line.
bool anchor_position(DWORD index, Position const& fallback, Position& out) {
    std::uintptr_t const entity = entity_at(index);
    std::uintptr_t const field = position_field_for(index);

    if (entity != 0) {
        std::uint32_t display = 0;
        if (read_memory(entity + kEntityDisplayPtr, display) && display != 0) {
            if (g_anchor_mode == kAnchorModel
                && model_position(display, field, model_fraction_for(index), out)) {
                return true;
            }

            if (g_anchor_mode == kAnchorBone
                && bone_position(display, field, g_bone, out)) {
                return true;
            }

            if (g_anchor_mode != kAnchorEntity) {
                float coords[3] {};
                if (span_readable(display + field, sizeof(coords))) {
                    std::memcpy(coords,
                        reinterpret_cast<void const*>(display + field),
                        sizeof(coords));
                    if (plausible(coords)) {
                        out.east = coords[0];
                        // FFXI height is negative-up, so a lift subtracts.
                        out.height = coords[1] - g_anchor_lift;
                        out.north = coords[2];
                        return true;
                    }
                }
            }
        }

        float coords[3] {};
        if (span_readable(entity + kEntityDisplayPos, sizeof(coords))) {
            std::memcpy(coords, reinterpret_cast<void const*>(entity + kEntityDisplayPos),
                sizeof(coords));
            if (plausible(coords)) {
                out.east = coords[0];
                out.height = coords[1] - g_anchor_lift;
                out.north = coords[2];
                return true;
            }
        }
    }

    // Nothing readable in game memory: use what Lua sent. These coordinates are
    // the server-side value and lag the model, but a lagging line beats none.
    out.east = fallback.east;
    out.north = fallback.north;
    out.height = fallback.height - g_anchor_lift;
    return true;
}

// ---------------------------------------------------------------------------
// Frame
// ---------------------------------------------------------------------------

void draw_all_lines() {
    if (!d3d_device_) {
        return;
    }

    int const count = g_line_count;
    int const rings = g_ring_count;
    if (count <= 0 && rings <= 0) {
        return;
    }

    D3DVIEWPORT8 viewport {};
    if (dev_GetViewport(&viewport) < 0 || viewport.Width == 0 || viewport.Height == 0) {
        return;
    }

    if (!refresh_projection_matrices()) {
        return;
    }

    ensure_textures();

    if (!begin_draw_state()) {
        return;
    }

    batch_vertex_count_ = 0;

    // Orbs are held back and drawn after every beam, so the texture is switched
    // once per frame rather than once per line.
    struct PendingOrb {
        Position at;
        DWORD color;
    };

    PendingOrb pending[kMaxLines] {};
    int pending_count = 0;

    // Rings first, so an arc crossing one draws over it rather than under.
    for (int i = 0; i < rings && i < kMaxRings; ++i) {
        Ring const& ring = g_rings[i];
        if (!ring.active || ring.radius <= 0.0f) {
            continue;
        }

        Position centre {};
        if (!ring_centre(ring, centre)) {
            continue;
        }

        bool const comet = ring.extent > 0.0f && ring.extent < 6.28318530718f;

        Position path[kMaxRibbonPoints] {};
        int const points = build_ring(centre, ring.radius, ring.head, ring.extent,
            path, kMaxRibbonPoints);
        emit_ribbon(path, points, ring.color, viewport, comet, ring.width);

        // The dot at the head of the comet, the same one the arcs use.
        if (comet && points > 0 && pending_count < kMaxLines) {
            pending[pending_count].at = path[points - 1];
            pending[pending_count].color = ring.color;
            ++pending_count;
        }
    }

    for (int i = 0; i < count && i < kMaxLines; ++i) {
        Line const& line = g_lines[i];
        if (!line.active) {
            continue;
        }

        Position source {};
        Position destination {};
        if (!anchor_position(line.src_index, line.src_fallback, source)
            || !anchor_position(line.dst_index, line.dst_fallback, destination)) {
            continue;
        }

        // A mutual engagement separates itself. A->B and B->A have exactly
        // opposite axes, and Rodrigues gives R(-k, t) == R(k, -t), so rotating
        // by the *same* angle about opposite axes already swings the two arcs
        // to opposite sides.
        //
        // Flipping the angle by index as well would negate that a second time
        // and put both arcs back on the same side, which is precisely how they
        // came to overlap. The angle is therefore constant here.
        bool flip = false;

        // A retracting arc is built from the target end. That reverses the
        // axis for a line that should not move, so here the sign *is* flipped
        // to cancel it out and hold the curve exactly where it was.
        Position from = source;
        Position to = destination;
        if (line.reverse) {
            from = destination;
            to = source;
            flip = !flip;
        }

        Position path[kMaxRibbonPoints] {};
        int const points = build_arc(from, to, flip, line.progress,
            path, kMaxRibbonPoints);
        emit_ribbon(path, points, line.color, viewport);

        // A dot rides the head of the arc while it is still reaching out. A
        // retracting arc does not get one -- it is withdrawing, not travelling.
        if (points > 0 && !line.reverse && line.progress < 1.0f
            && pending_count < kMaxLines) {
            pending[pending_count].at = path[points - 1];
            pending[pending_count].color = line.color;
            ++pending_count;
        }
    }

    for (int i = 0; i < pending_count; ++i) {
        emit_orb(pending[i].at, pending[i].color, viewport);
    }

    flush_batch();
    end_draw_state();
}

// ---------------------------------------------------------------------------
// SceneHook client
//
// draw_scene has exactly one prologue, so only one patch can exist. Rather than
// compete for it -- which is what produced "draw_scene signature not found"
// while TargetRing already owned it -- this registers a callback with
// SceneHook, the shared bus that owns the patch process-wide and dispatches to
// every participant in the same frame.
//
// Nothing here pins a module, nothing is ever unpatched, and load and unload
// order do not matter. TargetRing and GEO-HUD ride the same bus, so all three
// composite in one frame for free.
// ---------------------------------------------------------------------------

SceneBus* g_bus = nullptr;
int g_slot = -1;
std::uintptr_t g_renderer = 0;
volatile bool g_discovery_done = false;

// FFXI does not keep the D3D8 device anywhere reachable by signature, but the
// renderer holds d3d8 resources, and every IDirect3DResource8 can hand back its
// creator via GetDevice at vtable slot 3. Scanning for a resource and asking it
// is far more reliable than trying to recognise the device by its vtable.
void acquire_device(std::uintptr_t renderer) {
    if (g_discovery_done || renderer == 0) {
        return;
    }

    g_discovery_done = true;

    std::uintptr_t d3d_base = 0;
    std::size_t d3d_size = 0;
    if (!module_range("d3d8.dll", d3d_base, d3d_size)) {
        std::snprintf(g_status, sizeof(g_status), "d3d8.dll not loaded");
        return;
    }

    for (std::size_t offset = 0; offset + 4 <= 0x2000; offset += 4) {
        std::uintptr_t const slot = renderer + offset;
        if (!span_readable(slot, sizeof(std::uintptr_t))) {
            break;
        }

        std::uintptr_t resource = 0;
        std::memcpy(&resource, reinterpret_cast<void const*>(slot), sizeof(resource));
        if (resource == 0 || !span_readable(resource, sizeof(std::uintptr_t))) {
            continue;
        }

        std::uintptr_t vtable = 0;
        std::memcpy(&vtable, reinterpret_cast<void const*>(resource), sizeof(vtable));
        if (vtable < d3d_base || vtable >= d3d_base + d3d_size) {
            continue;
        }

        auto get_device = reinterpret_cast<fn_get_device>(vtable_slot(resource, 3));
        if (!get_device) {
            continue;
        }

        void* device = nullptr;
        if (get_device(reinterpret_cast<void*>(resource), &device) < 0 || !device) {
            continue;
        }

        std::uintptr_t const address = reinterpret_cast<std::uintptr_t>(device);
        if (!span_readable(address, sizeof(std::uintptr_t))) {
            continue;
        }

        std::uintptr_t device_vtable = 0;
        std::memcpy(&device_vtable, device, sizeof(device_vtable));
        if (device_vtable < d3d_base || device_vtable >= d3d_base + d3d_size) {
            continue;
        }

        g_device = address;
        d3d_device_ = device;
        resolve_device_api();

        // GetDevice added a reference; the renderer owns the device, not us.
        auto release = reinterpret_cast<fn_release>(vtable_slot(address, 2));
        if (release) {
            release(device);
        }

        std::snprintf(g_status, sizeof(g_status), "running");
        return;
    }

    std::snprintf(g_status, sizeof(g_status), "device not found in renderer");
}

// Runs inside draw_scene with the depth buffer still bound. SceneHook has
// already called the trampoline, so the world is drawn and ours goes on top of
// it, correctly occluded.
//
// Several addons draw back to back in one frame, so this must leave device
// state exactly as it found it -- begin_draw_state and end_draw_state save and
// restore every state they touch, which is what earns us the right to be here.
//
// SCENEHOOK_ALIGN_STACK is required: FFXi's render thread does not guarantee a
// 16-byte aligned stack. It expands to nothing on MSVC and matters on MinGW.
void SCENEHOOK_ALIGN_STACK __cdecl scene_draw(void* user, void* renderer, void* device) {
    (void)user;

    // device is best-effort and currently always null, so the renderer scan is
    // still the real path. Taking it when offered costs nothing.
    if (device && !d3d_device_) {
        g_device = reinterpret_cast<std::uintptr_t>(device);
        d3d_device_ = device;
        g_discovery_done = true;
        std::snprintf(g_status, sizeof(g_status), "running");
    }

    g_renderer = reinterpret_cast<std::uintptr_t>(renderer);

    acquire_device(g_renderer);
    draw_all_lines();
}

// Join the bus and start drawing. Safe to call repeatedly: once we hold a slot
// this only re-arms it.
bool install_hook() {
    if (g_slot >= 0) {
        scenehook_set_enabled(g_bus, g_slot, true);
        std::snprintf(g_status, sizeof(g_status), "running");
        return true;
    }

    g_bus = scenehook_attach();
    if (!g_bus) {
        // Either the mapping failed, or a bus built from a different SceneHook
        // version is already resident and refused to let us guess at its layout.
        std::snprintf(g_status, sizeof(g_status),
            "scene bus unavailable (SceneHook ABI mismatch?)");
        return false;
    }

    // Installs the one process-wide patch if it is not there yet. Only the first
    // caller in the process does any work.
    if (!scenehook_ensure_hook(g_bus)) {
        std::snprintf(g_status, sizeof(g_status), "%s", g_bus->status);
        return false;
    }

    g_slot = scenehook_register(g_bus, &scene_draw, nullptr);
    if (g_slot < 0) {
        std::snprintf(g_status, sizeof(g_status), "scene hook full (64 clients)");
        return false;
    }

    std::snprintf(g_status, sizeof(g_status), "running");
    return true;
}

// Stop drawing but keep the slot, so //tlines on resumes without re-registering.
bool disable_hook() {
    scenehook_set_enabled(g_bus, g_slot, false);
    g_line_count = 0;
    std::snprintf(g_status, sizeof(g_status), "disabled");
    return true;
}

// Leave the bus entirely. Called from the addon's unload event, where waiting
// for an in-flight frame to finish is allowed.
void release_hook(bool may_wait) {
    if (g_slot >= 0) {
        scenehook_unregister(g_bus, g_slot, may_wait);
        g_slot = -1;
    }

    g_line_count = 0;

    // Only once no frame can still be in flight, and never from DllMain:
    // touching D3D objects under the loader lock is not worth the risk. Not
    // releasing there leaks two small textures until the process exits, which
    // is by far the cheaper mistake.
    if (may_wait) {
        release_object(g_beam_texture);
        release_object(g_orb_texture);
        g_beam_texture = nullptr;
        g_orb_texture = nullptr;
        g_textures_tried = false;
    }
}

// ---------------------------------------------------------------------------
// Lua entry points
// ---------------------------------------------------------------------------

int __cdecl lua_start(lua_State* L) {
    install_hook();
    g_lua.pushstring(L, g_status);
    return 1;
}

int __cdecl lua_stop(lua_State* L) {
    disable_hook();
    g_lua.pushstring(L, g_status);
    return 1;
}

int __cdecl lua_clear(lua_State* L) {
    g_line_count = 0;
    for (int i = 0; i < kMaxLines; ++i) {
        g_lines[i].active = false;
    }

    g_ring_count = 0;
    for (int i = 0; i < kMaxRings; ++i) {
        g_rings[i].active = false;
    }

    (void)L;
    return 0;
}

// ring(index, x, y, z, radius, color [, mode, amount, head, extent, width])
//
// A ring at that entity, or at the given coordinates if the entity cannot be
// resolved. Lua animates it by passing the radius and the colour, alpha
// included, that it wants this frame.
//
//   mode 0  on the ground, raised `amount` yalms
//   mode 1  at fraction `amount` of the entity's height, encircling the body
//
// A non-zero `extent` draws an arc of that many radians ending at `head`
// instead of a closed ring, tapered and with the travelling orb at its head.
int __cdecl lua_ring(lua_State* L) {
    int const argc = g_lua.gettop(L);
    if (argc < 6) {
        return 0;
    }

    int const slot = g_ring_count;
    if (slot >= kMaxRings) {
        return 0;
    }

    Ring ring {};
    ring.index = static_cast<DWORD>(g_lua.tonumber(L, 1));
    ring.fallback.east = static_cast<float>(g_lua.tonumber(L, 2));
    ring.fallback.north = static_cast<float>(g_lua.tonumber(L, 3));
    ring.fallback.height = static_cast<float>(g_lua.tonumber(L, 4));
    ring.radius = static_cast<float>(g_lua.tonumber(L, 5));
    ring.color = static_cast<DWORD>(g_lua.tonumber(L, 6));
    ring.mode = argc >= 7 ? static_cast<int>(g_lua.tonumber(L, 7)) : 0;
    ring.amount = argc >= 8 ? static_cast<float>(g_lua.tonumber(L, 8)) : 0.0f;
    ring.head = argc >= 9 ? static_cast<float>(g_lua.tonumber(L, 9)) : 0.0f;
    ring.extent = argc >= 10 ? static_cast<float>(g_lua.tonumber(L, 10)) : 0.0f;
    ring.width = argc >= 11 ? static_cast<float>(g_lua.tonumber(L, 11)) : 1.0f;
    ring.active = true;

    g_rings[slot] = ring;
    g_ring_count = slot + 1;
    return 0;
}

// add(src_index, sx, sy, sz, dst_index, dx, dy, dz, color [, progress, reverse])
//
// src and dst are always the logical actor and target; pass reverse instead of
// swapping them, so the bow direction stays consistent. The indices are what
// let the module read live render positions; the coordinates are only a
// fallback for entities it cannot resolve. progress is how much of the arc to
// draw, 0..1, and defaults to the whole thing.
int __cdecl lua_add(lua_State* L) {
    int const argc = g_lua.gettop(L);
    if (argc < 9) {
        return 0;
    }

    int const slot = g_line_count;
    if (slot >= kMaxLines) {
        return 0;
    }

    Line line {};
    line.src_index = static_cast<DWORD>(g_lua.tonumber(L, 1));
    line.src_fallback.east = static_cast<float>(g_lua.tonumber(L, 2));
    line.src_fallback.north = static_cast<float>(g_lua.tonumber(L, 3));
    line.src_fallback.height = static_cast<float>(g_lua.tonumber(L, 4));
    line.dst_index = static_cast<DWORD>(g_lua.tonumber(L, 5));
    line.dst_fallback.east = static_cast<float>(g_lua.tonumber(L, 6));
    line.dst_fallback.north = static_cast<float>(g_lua.tonumber(L, 7));
    line.dst_fallback.height = static_cast<float>(g_lua.tonumber(L, 8));
    line.color = static_cast<DWORD>(g_lua.tonumber(L, 9));
    line.progress = argc >= 10 ? static_cast<float>(g_lua.tonumber(L, 10)) : 1.0f;
    line.reverse = argc >= 11 && g_lua.tonumber(L, 11) != 0.0;
    line.active = true;

    g_lines[slot] = line;
    g_line_count = slot + 1;
    return 0;
}

int __cdecl lua_anchor(lua_State* L) {
    g_anchor_mode = (g_anchor_mode + 1) % kAnchorModeCount;

    static char const* const names[] = {
        "anchor: model height fraction (self-scaling)",
        "anchor: fixed skeleton bone (Ashita parity)",
        "anchor: nameplate base",
        "anchor: entity position",
    };

    g_lua.pushstring(L, names[g_anchor_mode]);
    return 1;
}

// How many bones the height survey looks at. Too many and appendages -- wings,
// antennae, raised tails -- get measured as body height; too few and tall
// models come out short. Tunable live so the cutoff can be found in game.
int __cdecl lua_bones(lua_State* L) {
    if (g_lua.gettop(L) >= 1) {
        float const value = static_cast<float>(g_lua.tonumber(L, 1));
        if (value == kRestoreDefault) {
            g_survey_bones = kDefaultSurveyBones;
        } else if (value >= 1.0f && value <= 64.0f) {
            g_survey_bones = static_cast<int>(value);
        }
    }

    char report[64] {};
    std::snprintf(report, sizeof(report), "survey: first %d bones (default %d)",
        g_survey_bones, kDefaultSurveyBones);
    g_lua.pushstring(L, report);
    return 1;
}

// How high the arc rises, as a fraction of the distance between the endpoints.
int __cdecl lua_arch(lua_State* L) {
    if (g_lua.gettop(L) >= 1) {
        float const value = static_cast<float>(g_lua.tonumber(L, 1));
        if (value == kRestoreDefault) {
            g_arc_rise = kDefaultArcRise;
        } else if (value >= 0.0f && value <= 2.0f) {
            g_arc_rise = value;
        }
    }

    char report[112] {};
    std::snprintf(report, sizeof(report),
        "arch: %.2f of distance, %.2f-%.2f yalms (default %.2f)",
        g_arc_rise, g_arc_min, g_arc_max, kDefaultArcRise);
    g_lua.pushstring(L, report);
    return 1;
}

// Sideways lean of the arc, in degrees. Ashita's pi/16 is 11.25.
int __cdecl lua_bow(lua_State* L) {
    if (g_lua.gettop(L) >= 1) {
        float const value = static_cast<float>(g_lua.tonumber(L, 1));
        if (value == kRestoreDefault) {
            g_arc_bow = kDefaultArcBow;
        } else if (value >= -90.0f && value <= 90.0f) {
            g_arc_bow = value * 0.01745329252f;
        }
    }

    char report[72] {};
    std::snprintf(report, sizeof(report), "bow: %.1f degrees (default %.1f)",
        g_arc_bow * 57.2957795f, kDefaultArcBow * 57.2957795f);
    g_lua.pushstring(L, report);
    return 1;
}

// Size of the travelling dot, in screen pixels. Ashita's is a 20px quad.
int __cdecl lua_orb(lua_State* L) {
    if (g_lua.gettop(L) >= 1) {
        float const value = static_cast<float>(g_lua.tonumber(L, 1));
        if (value == kRestoreDefault) {
            g_orb_size = kDefaultOrbSize;
        } else if (value >= 0.0f && value <= 128.0f) {
            g_orb_size = value * 0.5f;
        }
    }

    char report[80] {};
    std::snprintf(report, sizeof(report), "orb: %.0f px (default %.0f)%s",
        g_orb_size * 2.0f, kDefaultOrbSize * 2.0f,
        g_orb_texture ? "" : " - texture unavailable");
    g_lua.pushstring(L, report);
    return 1;
}

// As with depth: cycles when called bare, sets when given a value.
int __cdecl lua_arc(lua_State* L) {
    g_arc_mode = g_lua.gettop(L) >= 1
        ? (g_lua.tonumber(L, 1) != 0.0 ? 1 : 0)
        : (g_arc_mode + 1) % 2;
    g_lua.pushstring(L, g_arc_mode == 0 ? "arc: curved" : "arc: straight");
    return 1;
}

// Put every tunable back where it started, including the three cycled modes.
int __cdecl lua_reset(lua_State* L) {
    g_model_fraction_self = kDefaultModelFraction;
    g_model_fraction_other = kDefaultModelFraction;
    g_anchor_lift = kDefaultAnchorLift;
    g_half_width = kDefaultHalfWidth;
    g_arc_rise = kDefaultArcRise;
    g_arc_bow = kDefaultArcBow;
    g_orb_size = kDefaultOrbSize;
    g_survey_bones = kDefaultSurveyBones;
    g_bone = kDefaultBone;
    g_anchor_mode = kAnchorModel;
    g_depth_mode = 0;
    g_arc_mode = 0;

    g_lua.pushstring(L, "reset: everything back to defaults");
    return 1;
}

// Where up the model the line attaches, 0 = feet, 1 = top of head.
// chest(scope, value) -- scope 0 both, 1 the player, 2 everyone else.
int __cdecl lua_chest(lua_State* L) {
    if (g_lua.gettop(L) >= 2) {
        int const scope = static_cast<int>(g_lua.tonumber(L, 1));
        float value = static_cast<float>(g_lua.tonumber(L, 2));

        bool const restore = value == kRestoreDefault;
        if (restore) {
            value = kDefaultModelFraction;
        }

        if (restore || (value >= 0.0f && value <= 1.5f)) {
            if (scope != kChestOther) {
                g_model_fraction_self = value;
            }
            if (scope != kChestSelf) {
                g_model_fraction_other = value;
            }
        }
    }

    char report[128] {};
    std::snprintf(report, sizeof(report),
        "chest: me %.2f, target %.2f (default %.2f)%s",
        g_model_fraction_self, g_model_fraction_other, kDefaultModelFraction,
        g_player_index == 0 ? " - player not identified yet" : "");
    g_lua.pushstring(L, report);
    return 1;
}

// Lua tells the module which entity is the player, so "me" can be told apart
// from everyone else at whichever end of a line it appears.
int __cdecl lua_player(lua_State* L) {
    if (g_lua.gettop(L) >= 1) {
        g_player_index = static_cast<DWORD>(g_lua.tonumber(L, 1));
    }

    return 0;
}

// With no argument this cycles; with one it sets directly, which is what lets
// Lua hold the authoritative value and push it in without having to read back.
int __cdecl lua_depth(lua_State* L) {
    g_depth_mode = g_lua.gettop(L) >= 1
        ? (g_lua.tonumber(L, 1) != 0.0 ? 1 : 0)
        : (g_depth_mode + 1) % 2;
    g_lua.pushstring(L, g_depth_mode == 0
        ? "depth ON: line is hidden where the world is in front of it"
        : "depth OFF: line draws over everything (Ashita behaviour)");
    return 1;
}

int __cdecl lua_lift(lua_State* L) {
    if (g_lua.gettop(L) >= 1) {
        float const value = static_cast<float>(g_lua.tonumber(L, 1));
        if (value == kRestoreDefault) {
            g_anchor_lift = kDefaultAnchorLift;
        } else if (value >= 0.0f && value <= 20.0f) {
            g_anchor_lift = value;
        }
    }

    char report[64] {};
    std::snprintf(report, sizeof(report), "lift: %.2f yalms (default %.2f)",
        g_anchor_lift, kDefaultAnchorLift);
    g_lua.pushstring(L, report);
    return 1;
}

int __cdecl lua_width(lua_State* L) {
    if (g_lua.gettop(L) >= 1) {
        float const value = static_cast<float>(g_lua.tonumber(L, 1));
        if (value == kRestoreDefault) {
            g_half_width = kDefaultHalfWidth;
        } else if (value > 0.0f && value <= 64.0f) {
            g_half_width = value * 0.5f;
        }
    }

    char report[64] {};
    std::snprintf(report, sizeof(report), "width: %.1f px (default %.1f)",
        g_half_width * 2.0f, kDefaultHalfWidth * 2.0f);
    g_lua.pushstring(L, report);
    return 1;
}

// Reports what each anchor mode resolves to for one entity. This is the tool
// for checking the bone offsets in game without guessing.
int __cdecl lua_probe(lua_State* L) {
    char report[192] {};

    if (g_lua.gettop(L) < 1) {
        g_lua.pushstring(L, "probe: needs an entity index");
        return 1;
    }

    DWORD const index = static_cast<DWORD>(g_lua.tonumber(L, 1));
    std::uintptr_t const entity = entity_at(index);
    if (entity == 0) {
        std::snprintf(report, sizeof(report), "probe %u: no entity (array 0x%08X)",
            index, static_cast<unsigned>(entity_array()));
        g_lua.pushstring(L, report);
        return 1;
    }

    std::uint32_t display = 0;
    read_memory(entity + kEntityDisplayPtr, display);

    Position nameplate {};
    bool have_nameplate = false;
    if (display != 0) {
        float coords[3] {};
        std::uintptr_t const field = position_field_for(index);
        if (span_readable(display + field, sizeof(coords))) {
            std::memcpy(coords, reinterpret_cast<void const*>(display + field),
                sizeof(coords));
            if (plausible(coords)) {
                nameplate.east = coords[0];
                nameplate.height = coords[1];
                nameplate.north = coords[2];
                have_nameplate = true;
            }
        }
    }

    // Survey the first several bones rather than just one. A real skeleton
    // gives varied offsets that climb the model; a bad read gives zeros or the
    // same value repeatedly, which one sample cannot distinguish.
    // Heights only, and more of them. Height is the whole question for
    // anchoring, and dropping the horizontals buys enough room to see far
    // enough up the skeleton to find the chest. Negative is up.
    int bone_count = 0;
    char bones[200] {};
    int written = 0;
    for (int b = 0; b < 16 && written < static_cast<int>(sizeof(bones)) - 12; ++b) {
        float local[3] {};
        if (display != 0 && bone_offset(display, b, local, &bone_count)) {
            written += std::snprintf(bones + written,
                sizeof(bones) - static_cast<std::size_t>(written),
                "%s%d:%.2f", b == 0 ? "" : " ", b, local[1]);
        } else if (b == 0) {
            written += std::snprintf(bones + written,
                sizeof(bones) - static_cast<std::size_t>(written), "unreadable");
            break;
        } else {
            break;
        }
    }

    float top = 0.0f;
    int top_bone = -1;
    bool const have_top = display != 0 && model_height(display, top, &top_bone);

    std::snprintf(report, sizeof(report),
        "probe %u: %d bones | height %.2f from bone %d (of first %d) -> anchor "
        "%.2f up | heights (neg=up) %s",
        index, bone_count,
        have_top ? -top : 0.0f, top_bone, g_survey_bones,
        have_top ? -top * model_fraction_for(index) + g_anchor_lift : 0.0f,
        bones);
    g_lua.pushstring(L, report);
    return 1;
}

// Pick which bone the line attaches to, so the right one can be found in game
// rather than guessed at.
int __cdecl lua_bone(lua_State* L) {
    if (g_lua.gettop(L) >= 1) {
        float const value = static_cast<float>(g_lua.tonumber(L, 1));
        if (value == kRestoreDefault) {
            g_bone = kDefaultBone;
        } else if (value >= 0.0f && value < 64.0f) {
            g_bone = static_cast<int>(value);
        }
    }

    char report[64] {};
    std::snprintf(report, sizeof(report), "bone: %d (default %d)", g_bone, kDefaultBone);
    g_lua.pushstring(L, report);
    return 1;
}

// Leave the bus. The addon's unload event calls this, where waiting for an
// in-flight frame is permitted. DllMain repeats it without waiting as the
// backstop that always runs.
int __cdecl lua_release(lua_State* L) {
    release_hook(true);
    std::snprintf(g_status, sizeof(g_status), "released");
    g_lua.pushstring(L, g_status);
    return 1;
}

// Reports the state of the shared scene hook: who owns the patch, how many
// addons are riding it, and whatever the bus last had to say. Replaces the
// prologue survey, which existed only to work out why we could not patch
// draw_scene ourselves -- an answer SceneHook makes moot.
int __cdecl lua_scan(lua_State* L) {
    char report[320] {};

    if (!g_bus) {
        std::snprintf(report, sizeof(report),
            "scene: not attached (entity array 0x%08X)",
            static_cast<unsigned>(entity_array()));
        g_lua.pushstring(L, report);
        return 1;
    }

    char shared[192] {};
    scenehook_describe(g_bus, g_slot, shared, sizeof(shared));

    std::snprintf(report, sizeof(report),
        "scene: %s | slot %d, owner %ld, patched %s | bus: %s",
        shared, g_slot, g_bus->owner_slot,
        g_bus->hook_installed ? "yes" : "no", g_bus->status);
    g_lua.pushstring(L, report);
    return 1;
}

int __cdecl lua_status(lua_State* L) {
    char report[320] {};

    char shared[192] {};
    scenehook_describe(g_bus, g_slot, shared, sizeof(shared));

    char anchor[40] {};
    static char const* const anchors[] = {"model", "bone", "nameplate", "entity"};
    if (g_anchor_mode == kAnchorModel) {
        std::snprintf(anchor, sizeof(anchor), "model me %.2f/tgt %.2f",
            g_model_fraction_self, g_model_fraction_other);
    } else if (g_anchor_mode == kAnchorBone) {
        std::snprintf(anchor, sizeof(anchor), "bone %d", g_bone);
    } else {
        std::snprintf(anchor, sizeof(anchor), "%s", anchors[g_anchor_mode]);
    }

    std::snprintf(report, sizeof(report),
        "%s | %s | anchor %s, lift %.2f, width %.1fpx, DEPTH %s | %s arch %.2f "
        "bow %.1f | %d line(s), %d ring(s)",
        g_status, shared, anchor, g_anchor_lift, g_half_width * 2.0f,
        g_depth_mode == 0 ? "OCCLUDED by world" : "ALWAYS ON TOP",
        g_arc_mode == 0 ? "curved" : "straight", g_arc_rise,
        g_arc_bow * 57.2957795f, g_line_count, g_ring_count);
    g_lua.pushstring(L, report);
    return 1;
}

bool bind_lua() {
    if (g_lua.ready()) {
        return true;
    }

    static char const* const hosts[] = {"LuaCore.dll", "lua51.dll", "lua5.1.dll"};
    for (char const* host : hosts) {
        HMODULE module = GetModuleHandleA(host);
        if (!module) {
            continue;
        }

        g_lua.createtable = reinterpret_cast<fn_createtable>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_createtable")));
        g_lua.pushcclosure = reinterpret_cast<fn_pushcclosure>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_pushcclosure")));
        g_lua.setfield = reinterpret_cast<fn_setfield>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_setfield")));
        g_lua.pushvalue = reinterpret_cast<fn_pushvalue>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_pushvalue")));
        g_lua.pushstring = reinterpret_cast<fn_pushstring>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_pushstring")));
        g_lua.gettop = reinterpret_cast<fn_gettop>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_gettop")));
        g_lua.tonumber = reinterpret_cast<fn_tonumber>(
            reinterpret_cast<void*>(GetProcAddress(module, "lua_tonumber")));

        if (g_lua.ready()) {
            return true;
        }
    }

    return false;
}

}  // namespace

extern "C" __declspec(dllexport) int __cdecl luaopen__TargetLines(lua_State* L) {
    if (!bind_lua()) {
        return 0;
    }

    build_ring_table();

    g_lua.createtable(L, 0, 10);

    g_lua.pushcclosure(L, lua_start, 0);
    g_lua.setfield(L, -2, "start");

    g_lua.pushcclosure(L, lua_stop, 0);
    g_lua.setfield(L, -2, "stop");

    g_lua.pushcclosure(L, lua_clear, 0);
    g_lua.setfield(L, -2, "clear");

    g_lua.pushcclosure(L, lua_add, 0);
    g_lua.setfield(L, -2, "add");

    g_lua.pushcclosure(L, lua_ring, 0);
    g_lua.setfield(L, -2, "ring");

    g_lua.pushcclosure(L, lua_anchor, 0);
    g_lua.setfield(L, -2, "anchor");

    g_lua.pushcclosure(L, lua_depth, 0);
    g_lua.setfield(L, -2, "depth");

    g_lua.pushcclosure(L, lua_lift, 0);
    g_lua.setfield(L, -2, "lift");

    g_lua.pushcclosure(L, lua_width, 0);
    g_lua.setfield(L, -2, "width");

    g_lua.pushcclosure(L, lua_probe, 0);
    g_lua.setfield(L, -2, "probe");

    g_lua.pushcclosure(L, lua_scan, 0);
    g_lua.setfield(L, -2, "scan");

    g_lua.pushcclosure(L, lua_release, 0);
    g_lua.setfield(L, -2, "release");

    g_lua.pushcclosure(L, lua_bone, 0);
    g_lua.setfield(L, -2, "bone");

    g_lua.pushcclosure(L, lua_chest, 0);
    g_lua.setfield(L, -2, "chest");

    g_lua.pushcclosure(L, lua_player, 0);
    g_lua.setfield(L, -2, "player");

    g_lua.pushcclosure(L, lua_bones, 0);
    g_lua.setfield(L, -2, "bones");

    g_lua.pushcclosure(L, lua_arch, 0);
    g_lua.setfield(L, -2, "arch");

    g_lua.pushcclosure(L, lua_bow, 0);
    g_lua.setfield(L, -2, "bow");

    g_lua.pushcclosure(L, lua_arc, 0);
    g_lua.setfield(L, -2, "arc");

    g_lua.pushcclosure(L, lua_orb, 0);
    g_lua.setfield(L, -2, "orb");

    g_lua.pushcclosure(L, lua_reset, 0);
    g_lua.setfield(L, -2, "reset");

    g_lua.pushcclosure(L, lua_status, 0);
    g_lua.setfield(L, -2, "status");

    g_lua.pushvalue(L, -1);
    g_lua.setfield(L, kGlobalsIndex, "_TargetLines");
    return 1;
}

BOOL WINAPI DllMain(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_DETACH) {
        // The one teardown that must never be skipped. DLL_PROCESS_DETACH is
        // the only point guaranteed to run however the addon goes away -- a
        // normal unload, a force-unload, or a Lua error during unload -- and it
        // runs before our image is unmapped, which is what stops SceneHook
        // dispatching into freed code.
        //
        // may_wait is false here: the loader lock is held, so only lock-free
        // stores are performed and the drain is skipped.
        release_hook(false);
    }

    return TRUE;
}
