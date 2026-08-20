#pragma once
//
// The minimum of Direct3D 8 that TargetLines needs.
//
// d3d8.h was dropped from the Windows SDK long ago and is not in the June 2010
// DirectX SDK either, so depending on it would mean every builder hunting down
// a legacy SDK or a mingw sysroot. The module never calls the device through
// its COM interface -- every call goes through a vtable slot on an opaque
// pointer -- so all that is actually required is a handful of plain structs and
// the enum values, reproduced here.
//
// Values verified against mingw-w64's d3d8types.h. They are fixed by the D3D8
// ABI and cannot change.

#include <windows.h>

// ---------------------------------------------------------------------------
// Structures. Layout must match the D3D8 ABI exactly; both are passed to the
// device by pointer.
// ---------------------------------------------------------------------------

// The anonymous struct is how the real header names the elements, and the
// Bezier stage wants _11 for the zoom factor the way Ashita's drawArc does.
#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable : 4201)  // nameless struct/union
#endif

typedef struct _D3DMATRIX {
    union {
        struct {
            float _11, _12, _13, _14;
            float _21, _22, _23, _24;
            float _31, _32, _33, _34;
            float _41, _42, _43, _44;
        };
        float m[4][4];
    };
} D3DMATRIX;

#ifdef _MSC_VER
#pragma warning(pop)
#endif

typedef struct _D3DVIEWPORT8 {
    DWORD X;
    DWORD Y;
    DWORD Width;
    DWORD Height;
    float MinZ;
    float MaxZ;
} D3DVIEWPORT8;

typedef struct _D3DLOCKED_RECT {
    INT Pitch;
    void* pBits;
} D3DLOCKED_RECT;

// ---------------------------------------------------------------------------
// Transform state
// ---------------------------------------------------------------------------

#define D3DTS_VIEW        2
#define D3DTS_PROJECTION  3

// ---------------------------------------------------------------------------
// Render states
// ---------------------------------------------------------------------------

#define D3DRS_ZENABLE           7
#define D3DRS_ZWRITEENABLE     14
#define D3DRS_ALPHATESTENABLE  15
#define D3DRS_SRCBLEND         19
#define D3DRS_DESTBLEND        20
#define D3DRS_CULLMODE         22
#define D3DRS_ZFUNC            23
#define D3DRS_ALPHABLENDENABLE 27
#define D3DRS_FOGENABLE        28
#define D3DRS_LIGHTING        137

#define D3DCMP_LESSEQUAL        4

#define D3DBLEND_SRCALPHA       5
#define D3DBLEND_INVSRCALPHA    6

#define D3DCULL_NONE            1

// ---------------------------------------------------------------------------
// Texture stage states
// ---------------------------------------------------------------------------

#define D3DTSS_COLOROP          1
#define D3DTSS_COLORARG1        2
#define D3DTSS_COLORARG2        3
#define D3DTSS_ALPHAOP          4
#define D3DTSS_ALPHAARG1        5
#define D3DTSS_ALPHAARG2        6
#define D3DTSS_ADDRESSU        13
#define D3DTSS_ADDRESSV        14
#define D3DTSS_MAGFILTER       16
#define D3DTSS_MINFILTER       17
#define D3DTSS_MIPFILTER       18

#define D3DTOP_SELECTARG1       2
#define D3DTOP_MODULATE         4
// Blends texture colour over the vertex colour by the texture's own alpha.
// A texture that is white in the middle and transparent at the edges therefore
// gives a white-hot core fading out through the line's colour, which is how
// Ashita's beam gets its glow.
#define D3DTOP_BLENDTEXTUREALPHA 13

#define D3DTA_DIFFUSE           0x00000000
#define D3DTA_TEXTURE           0x00000002

#define D3DTEXF_LINEAR          2
#define D3DTADDRESS_CLAMP       3

// ---------------------------------------------------------------------------
// Textures
// ---------------------------------------------------------------------------

#define D3DFMT_A8R8G8B8        21
#define D3DPOOL_MANAGED         1

// ---------------------------------------------------------------------------
// Vertex format and primitives
// ---------------------------------------------------------------------------

#define D3DFVF_XYZRHW           0x0004
#define D3DFVF_DIFFUSE          0x0040
#define D3DFVF_TEX1             0x0100

#define D3DPT_TRIANGLELIST      4
