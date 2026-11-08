/* Rendering is split in two (see render_rec.c and render_wgpu.c):
 *   recorder  - runs on the game thread; does all CPU work (primitive expansion, vertex shaders,
 *               texture conversion, uniforms) and emits self-contained commands;
 *   executor  - owns every WebGPU object and executes the commands.
 * Natively each command is executed right away. In the browser, WebGPU must live on the page's
 * thread (which returns to its event loop), so commands are queued per frame and replayed there.
 * Commands never point at game objects: textures and surfaces are referred to by numeric ids. */
#ifndef RC_RENDER_CMD_H
#define RC_RENDER_CMD_H

#include <stdint.h>

typedef struct { float x, y, z, w; uint32_t diffuse, specular; float tc[4][4]; } CVert;  /* 88 bytes */

typedef struct {
    float mvp[16];
    float vp[4];
    uint32_t flags[4];
    uint32_t st0[4], st0b[4], st1[4], st1b[4];
    float tfactor[4];
    float misc[4];
    float mat_diffuse[4];
    float ambient[4];
    float psc[8][4];
    float bem[4][4];
    float bel[4][4];
} Uniforms;   /* 416 bytes */

/* vs: GPU vertex program id (0: vertices were processed on the CPU, CVert layout); strides: streams 0-3,
   16 bits each (stream 0 | 1 << 16, stream 2 | 3 << 16), the vertex buffer layouts of a GPU program */
/* sten..swmask: Direct3D stencil state (D3DCMP func, D3DSTENCILOP fail/zfail/pass, read/write masks), all 0 when the
   stencil test is off; samples: set by the executor */
typedef struct PKey { uint32_t blend, src, dst, op, zen, zwrite, zfunc, cull, cmask, topo, ps, vs, strides[2], samples,
                      sten, sfunc, sfail, szfail, spass, smask, swmask; } PKey;

enum { CMD_TARGET = 1, CMD_CLEAR, CMD_DRAW, CMD_UPLOAD_TEX, CMD_DESTROY_TEX, CMD_UPLOAD_SURF, CMD_DESTROY_SURF,
       CMD_READBACK, CMD_PRESENT, CMD_VS_PROGRAM };

typedef struct { uint32_t type, size; } CmdHdr;   /* size includes the header; 8-byte aligned */

/* Render target for the following clears/draws. color_id 0 = the back buffer. depth_id 0 = the
   device's own depth buffer (back buffer) or a scratch one (render targets). */
/* msaa: samples for the back buffer (1 or 4, the game's anti-aliasing setting); render targets are single-sampled */
typedef struct { CmdHdr h; uint32_t color_id, color_w, color_h, depth_id, depth_w, depth_h, lw, lh, msaa; } CmdTarget;

typedef struct { CmdHdr h; uint32_t flags, color, stencil; float z; } CmdClear;

/* Draw layouts on a back buffer wider than 4:3 (widescreen): 3D scenes and full-width plain fills stretch across it
   (the game's cameras use its aspect ratio); 2D (pre-transformed or orthographic) keeps the game's 640x480 screen
   as a 4:3 area, which may extend past its sides; 3D in part of the screen (the replay picture-in-picture) goes in
   that area too. The area is centred, or moved to the window's left (anchor -1) or right (1) side: HUD. */
enum { LAYOUT_STRETCH = 0, LAYOUT_43, LAYOUT_43_3D };

/* texture slot kinds */
enum { TS_NONE = 0, TS_TEX, TS_RTSURF };

typedef struct {
    CmdHdr h;
    PKey key;
    float vp[4];              /* Direct3D viewport (target logical pixels) */
    float minz, maxz;
    uint32_t full_viewport;   /* clear quads: ignore vp, cover the whole target */
    uint32_t slot_kind[4], slot_id[4], samp_key[4];
    uint32_t pixel_shader;    /* empty slots read black (pixel shaders) or white (fixed function) */
    int32_t capture_index;    /* debugging: save the back buffer after this draw (native only); -1 none */
    uint32_t stencil_ref;     /* D3DRS_STENCILREF (dynamic state, not part of the pipeline) */
    uint32_t layout;          /* LAYOUT_*: where the draw goes on a wide back buffer */
    int32_t anchor;           /* -1, 0 or 1: the 4:3 area on the left, in the middle or on the right */
    Uniforms u;
    uint32_t nverts;
    /* GPU vertex program draws (key.vs != 0): the payload holds the vertex shader constants (96 vec4, from
       offset 0), nverts indices (u16 or u32) at vs_idx_off, and stream s's vertices at vs_stream_off[s],
       already rebased so the lowest referenced vertex is vertex 0 */
    uint32_t vs_index32, vs_idx_off, vs_stream_off[4];
    /* otherwise: CVert verts[nverts] follow */
} CmdDraw;
#define VS_CONST_BYTES (96 * 16)

/* A GPU vertex program: its inputs (register, stream, D3DVSDT type, byte offset) and its WGSL (a `vs_gpu`
   function; the executor prepends the common declarations), NUL-terminated, following the header. */
typedef struct { CmdHdr h; uint32_t id, ninputs; uint32_t in_reg[16], in_stream[16], in_type[16], in_offset[16]; } CmdVsProgram;

/* RGBA8 levels follow: for each level, w*h*4 bytes */
/* fmt: TEXFMT_RGBA8, or a BC format whose blocks follow as stored (DXT textures, when the GPU samples BC) */
enum { TEXFMT_RGBA8 = 0, TEXFMT_BC1, TEXFMT_BC2, TEXFMT_BC3 };
typedef struct { CmdHdr h; uint32_t id, levels, fmt, w[14], h_[14]; } CmdUploadTex;
extern int g_gpu_bc;   /* the device can sample BC1-3 (set before the game draws) */
typedef struct { CmdHdr h; uint32_t id; } CmdDestroy;
/* level data follows: RGBA8 rows, or BC blocks (rows of (w+3)/4 blocks) */
typedef struct { CmdHdr h; uint32_t id, w, h_; } CmdUploadSurf;

/* Copies a render target (id 0: back buffer, scaled to w x h) into guest memory in format `fmt`,
   then sets *done (a host address) to 1. */
typedef struct { CmdHdr h; uint32_t id, w, h_, fmt, pitch, guest_mem; uint64_t done_ptr; } CmdReadback;

typedef struct { CmdHdr h; uint32_t frame, screenshot, drawn; } CmdPresent;   /* drawn: draws in the frame */

/* executor entry points */
void gpu_exec(const CmdHdr *c);              /* native: run one command now */
int gpu_init_native(void);                   /* native: create device, surface and pipelines */

#endif
