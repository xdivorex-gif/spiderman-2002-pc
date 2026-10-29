/*
 * Direct3D 11 render back end for the NV2A pushbuffer executor.
 *
 * Spider-Man links the XDK's D3D statically, so the toolkit executes its
 * pushbuffer and, by default, rasterises every triangle on the CPU. In the
 * training level that is ~780k triangles per report window and about six
 * cores of work for 5-8 fps. This takes the rasterising onto the GPU through
 * the executor's back-end interface (xboxrecomp/src/kernel/nv2a_backend.h):
 * the executor still transforms and clips on the CPU and hands over
 * triangles in surface pixels; this draws them.
 *
 * Opt-in: RECOMP_GPU_RENDER=1 registers it (main.c). Without it the CPU
 * rasteriser runs as before, which keeps it one switch away for comparisons.
 *
 *   Device       the adapter with the most dedicated video memory (the
 *                discrete GPU on a hybrid laptop), no swap chain of its own.
 *   Surfaces     one BGRA8 render target per colour surface the title draws
 *                into (main buffers, render-to-texture), one D32S8 depth
 *                buffer per zeta surface. A batch whose texture is one of
 *                our render targets samples it directly.
 *   Textures     decoded once with nv2a_backend_decode_texture, uploaded and
 *                cached by address/size/format; a sparse checksum of the
 *                guest bytes notices a texture rewritten in place (video).
 *                Titles that store mip levels get GPU-generated mips.
 *   Presentation at flip, every render target drawn since the last flip is
 *                read back into its guest surface, so the toolkit's window,
 *                presenter and frame dumps see the frame as before.
 *
 *   Combiners    a batch that carries the register-combiner inputs (vertex
 *                programs, fixed function) draws through a second pipeline
 *                that runs the combiners and all four texture stages, as the
 *                CPU rasteriser does; the rest is stage 0 x diffuse.
 *
 * Not handled yet: cube-map and bump/dot-product stages (they read white or
 * zero), CLEAR_RECT (a clear covers the whole surface). No culling, as the
 * CPU rasteriser.
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <d3dcompiler.h>
#include <wincodec.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_backend.h"

extern ptrdiff_t xbox_GetMemoryOffset(void);

#define GUEST(va) ((uint8_t *)xbox_GetMemoryOffset() + (va))
/* Guest surface and texture addresses come both as window VAs (0x8xxxxxxx)
 * and as the physical offsets behind them; compare the low 28 bits. */
#define ADDR_KEY(va) ((va) & 0x0FFFFFFFu)

static ID3D11Device        *s_dev;
static ID3D11DeviceContext *s_ctx;
static int                  s_failed;
/* set_cull's states ([none, front, back][ccw]) and the one bound. A blit sets
 * the no-cull state: its full-surface triangle must never be culled. */
static ID3D11RasterizerState *s_cull_st[3][2];
static ID3D11RasterizerState *s_rs_init;
static int s_cull_cur = -1;

/* ---- shaders --------------------------------------------------------- */

static const char s_hlsl[] =
"cbuffer C : register(b0) {\n"
"  float2 surf;      float zscale; float textured;\n"
"  float2 texsize;   float alpha_func; float alpha_ref;\n"
"  float  zmin;      float zmax;   float zclip; float pad;\n"
"};\n"
"Texture2D t : register(t0); SamplerState s : register(s0);\n"
"struct VI { float2 xy : POSITION; float z : DEPTH; float rhw : RHW;\n"
"            float4 c : COLOR; float2 uv : TEXCOORD; };\n"
"struct VO { float4 p : SV_Position; float4 c : COLOR; float2 uv : TEXCOORD; };\n"
"VO vs(VI i) {\n"
"  VO o; float w = i.rhw > 0 ? 1.0 / i.rhw : 1.0;\n"
"  float2 ndc = float2(i.xy.x / surf.x * 2 - 1, 1 - i.xy.y / surf.y * 2);\n"
"  o.p = float4(ndc * w, i.z * zscale * w, w);\n"
"  o.c = i.c; o.uv = i.uv / texsize; return o; }\n"
"bool cmp(float f, float a, float b) {\n"
"  if (f < 0.5) return false;  if (f < 1.5) return a <  b;\n"
"  if (f < 2.5) return a == b; if (f < 3.5) return a <= b;\n"
"  if (f < 4.5) return a >  b; if (f < 5.5) return a != b;\n"
"  if (f < 6.5) return a >= b; return true; }\n"
"float4 ps(VO i) : SV_Target {\n"
"  float4 c = i.c;\n"
"  if (textured > 0.5) c *= t.Sample(s, i.uv);\n"
"  if (alpha_func >= 0 && !cmp(alpha_func, round(c.a * 255), alpha_ref)) discard;\n"
"  if (zclip > 0.5) { float zw = i.p.z / zscale;\n"
"    if (zw < zmin || zw > zmax) discard; }\n"
"  return c; }\n";

/* The register-combiner pipeline: batches that carry Nv2aBatch.ex. The pixel
 * shader is nv2a_combiner.c's nv2a_rc_eval, line for line, fed what the CPU
 * rasteriser feeds it (rc_stage_fetch in nv2a_pb_exec.c): four stages fetched
 * by their SHADER_STAGE_PROGRAM mode, v0/v1, fog, eight general stages and
 * the final combiner. Same constants b0 as above for the fixed parts. */
static const char s_hlsl_rc[] =
"cbuffer C : register(b0) {\n"
"  float2 surf;      float zscale; float textured;\n"
"  float2 texsize;   float alpha_func; float alpha_ref;\n"
"  float  zmin;      float zmax;   float zclip; float pad;\n"
"};\n"
"cbuffer RC : register(b1) {\n"
"  uint4 ccw[8];                 /* color icw, alpha icw, color ocw, alpha ocw */\n"
"  float4 fac0[8]; float4 fac1[8];\n"
"  uint4 fin;                    /* final0, final1, control, stage_program */\n"
"  float4 fc0; float4 fc1; float4 fogc;\n"
"  uint4 misc;                   /* clip_plane_mode, usable-stage mask */\n"
"};\n"
"Texture2D t0 : register(t0); Texture2D t1 : register(t1);\n"
"Texture2D t2 : register(t2); Texture2D t3 : register(t3);\n"
"SamplerState s0 : register(s0); SamplerState s1 : register(s1);\n"
"SamplerState s2 : register(s2); SamplerState s3 : register(s3);\n"
"struct VI { float2 xy : POSITION; float z : DEPTH; float rhw : RHW;\n"
"            float4 c : COLOR; float2 uv : TEXCOORD;\n"
"            float4 x0 : STAGE0; float4 x1 : STAGE1; float4 x2 : STAGE2; float4 x3 : STAGE3;\n"
"            float4 spec : SPECULAR; float fog : FOG; };\n"
"struct VO { float4 p : SV_Position; float4 c : COLOR0; float4 spec : COLOR1;\n"
"            float4 x0 : TEXCOORD0; float4 x1 : TEXCOORD1; float4 x2 : TEXCOORD2;\n"
"            float4 x3 : TEXCOORD3; float fog : TEXCOORD4; };\n"
"VO vs_rc(VI i) {\n"
"  VO o; float w = i.rhw > 0 ? 1.0 / i.rhw : 1.0;\n"
"  float2 ndc = float2(i.xy.x / surf.x * 2 - 1, 1 - i.xy.y / surf.y * 2);\n"
"  o.p = float4(ndc * w, i.z * zscale * w, w);\n"
"  o.c = i.c; o.spec = i.spec; o.x0 = i.x0; o.x1 = i.x1; o.x2 = i.x2; o.x3 = i.x3;\n"
"  o.fog = i.fog; return o; }\n"
"bool cmp(float f, float a, float b) {\n"
"  if (f < 0.5) return false;  if (f < 1.5) return a <  b;\n"
"  if (f < 2.5) return a == b; if (f < 3.5) return a <= b;\n"
"  if (f < 4.5) return a >  b; if (f < 5.5) return a != b;\n"
"  if (f < 6.5) return a >= b; return true; }\n"
/* PS_INPUTMAPPING and the output scale/bias (map_in, map_out). */
"float mapin(float x, uint b) {\n"
"  float p = max(x, 0.0);\n"
"  switch ((b >> 5) & 7) {\n"
"  case 0: return p;            case 1: return 1.0 - saturate(x);\n"
"  case 2: return 2.0 * p - 1.0; case 3: return -2.0 * p + 1.0;\n"
"  case 4: return p - 0.5;      case 5: return -p + 0.5;\n"
"  case 6: return x;            default: return -x; } }\n"
"float3 mapout(float3 x, uint m) {\n"
"  if (m == 0x08) return x - 0.5;  if (m == 0x10) return x * 2.0;\n"
"  if (m == 0x18) return (x - 0.5) * 2.0; if (m == 0x20) return x * 4.0;\n"
"  if (m == 0x30) return x * 0.5;  return x; }\n"
/* The register file as named values, read and written through compares
 * rather than an array index: an indexed float4[16] went to scratch memory,
 * and at 1920x1080 that pixel shader alone dropped levels to 9-20 fps.
 * Numbers as in nv2a_combiner.c: 0 zero, 1 c0, 2 c1, 3 fog, 4 v0, 5 v1,
 * 8-11 t0-t3, 12 r0, 13 r1, 14 v1+r0 sum, 15 E*F (6 and 7 read zero). */
"struct Regs { float4 c0, c1, fog, v0, v1, t0, t1, t2, t3, r0, r1, sm, ef; };\n"
"float4 rget(Regs R, uint i) {\n"
"  float4 v = float4(0, 0, 0, 0);\n"
"  v = i == 1 ? R.c0 : v;  v = i == 2 ? R.c1 : v;  v = i == 3 ? R.fog : v;\n"
"  v = i == 4 ? R.v0 : v;  v = i == 5 ? R.v1 : v;  v = i == 8 ? R.t0 : v;\n"
"  v = i == 9 ? R.t1 : v;  v = i == 10 ? R.t2 : v; v = i == 11 ? R.t3 : v;\n"
"  v = i == 12 ? R.r0 : v; v = i == 13 ? R.r1 : v; v = i == 14 ? R.sm : v;\n"
"  v = i == 15 ? R.ef : v; return v; }\n"
/* Write the channels m selects of register i (0 discards). */
"void rset(inout Regs R, uint i, float4 v, bool4 m) {\n"
"  if (i == 1) R.c0 = m ? v : R.c0;   if (i == 2) R.c1 = m ? v : R.c1;\n"
"  if (i == 3) R.fog = m ? v : R.fog; if (i == 4) R.v0 = m ? v : R.v0;\n"
"  if (i == 5) R.v1 = m ? v : R.v1;   if (i == 8) R.t0 = m ? v : R.t0;\n"
"  if (i == 9) R.t1 = m ? v : R.t1;   if (i == 10) R.t2 = m ? v : R.t2;\n"
"  if (i == 11) R.t3 = m ? v : R.t3;  if (i == 12) R.r0 = m ? v : R.r0;\n"
"  if (i == 13) R.r1 = m ? v : R.r1;  if (i == 14) R.sm = m ? v : R.sm;\n"
"  if (i == 15) R.ef = m ? v : R.ef; }\n"
"float3 rdrgb(Regs R, uint b) {\n"
"  float4 r = rget(R, b & 15); float3 v = (b & 0x10) ? r.aaa : r.rgb;\n"
"  return float3(mapin(v.x, b), mapin(v.y, b), mapin(v.z, b)); }\n"
"float rda(Regs R, uint b) {\n"
"  float4 r = rget(R, b & 15); return mapin((b & 0x10) ? r.a : r.b, b); }\n"
/* A stage's texel, white when the stage has no usable texture (rc_texel). */
"float4 tex(Texture2D t, SamplerState s, uint st, float2 uv) {\n"
"  return ((misc.y >> st) & 1) ? t.Sample(s, uv) : float4(1, 1, 1, 1); }\n"
/* rc_stage_fetch. `keep` goes false where a CLIPPLANE stage kills the pixel. */
"float4 fetch(Texture2D t, SamplerState s, uint st, float4 c, inout bool keep) {\n"
"  uint mode = (fin.w >> (st * 5)) & 31;\n"
"  float q = c.w != 0 ? c.w : 1.0;\n"
"  if (mode == 0) return float4(0, 0, 0, 1);\n"
"  if (mode == 1 || mode == 2) return tex(t, s, st, c.xy / q);\n"
"  if (mode == 3) return tex(t, s, st, c.xy);\n"
"  if (mode == 4) return saturate(c);\n"
"  if (mode == 5) {\n"
"    [unroll] for (uint j = 0; j < 4; j++) {\n"
"      bool ge = (misc.x >> (st * 4 + j)) & 1;\n"
"      if (ge ? c[j] >= 0 : c[j] < 0) keep = false; }\n"
"    return float4(0, 0, 0, 0); }\n"
"  return float4(0, 0, 0, 0); }\n"
"float4 ps_rc(VO i) : SV_Target {\n"
"  Regs R; bool keep = true; uint s;\n"
"  R = (Regs)0;\n"
"  R.fog = float4(fogc.rgb, i.fog); R.v0 = i.c; R.v1 = i.spec;\n"
"  R.t0 = fetch(t0, s0, 0, i.x0, keep); R.t1 = fetch(t1, s1, 1, i.x1, keep);\n"
"  R.t2 = fetch(t2, s2, 2, i.x2, keep); R.t3 = fetch(t3, s3, 3, i.x3, keep);\n"
"  if (!keep) discard;\n"
/* r0.a starts as texture 0's alpha, or 1 with stage 0 off. */
"  R.r0.a = (fin.w & 31) ? R.t0.a : 1.0;\n"
"  uint n = min(fin.z & 0xFF, 8), flags = fin.z >> 8;\n"
"  [unroll] for (s = 0; s < 8; s++) {\n"
"    if (s >= n) break;\n"
"    uint icw = ccw[s].x, aicw = ccw[s].y, ocw = ccw[s].z, aocw = ccw[s].w;\n"
"    uint fl = ocw >> 12, afl = aocw >> 12;\n"
"    R.c0 = (flags & 0x010) ? fac0[s] : fac0[0]; R.c1 = (flags & 0x100) ? fac1[s] : fac1[0];\n"
"    bool mux = (flags & 1) ? R.r0.a >= 0.5 : (((int)(R.r0.a * 255.0)) & 1);\n"
"    float3 A = rdrgb(R, (icw >> 24) & 255), B = rdrgb(R, (icw >> 16) & 255);\n"
"    float3 C = rdrgb(R, (icw >> 8) & 255),  D = rdrgb(R, icw & 255);\n"
"    float aA = rda(R, (aicw >> 24) & 255), aB = rda(R, (aicw >> 16) & 255);\n"
"    float aC = rda(R, (aicw >> 8) & 255),  aD = rda(R, aicw & 255);\n"
"    float3 pab = (fl & 2) ? dot(A, B).xxx : A * B;\n"
"    float3 pcd = (fl & 1) ? dot(C, D).xxx : C * D;\n"
"    float3 sum = (fl & 4) ? (mux ? pcd : pab) : pab + pcd;\n"
"    float3 ab = clamp(mapout(pab, fl & 0x38), -1, 1);\n"
"    float3 cd = clamp(mapout(pcd, fl & 0x38), -1, 1);\n"
"    float3 ms = clamp(mapout(sum, fl & 0x38), -1, 1);\n"
"    float apab = aA * aB, apcd = aC * aD;\n"
"    float asum = (afl & 4) ? (mux ? apcd : apab) : apab + apcd;\n"
"    float aab = clamp(mapout(apab.xxx, afl & 0x38).x, -1, 1);\n"
"    float acd = clamp(mapout(apcd.xxx, afl & 0x38).x, -1, 1);\n"
"    float ams = clamp(mapout(asum.xxx, afl & 0x38).x, -1, 1);\n"
"    const bool4 RGB = bool4(true, true, true, false), A_ = bool4(false, false, false, true);\n"
"    rset(R, (ocw >> 4) & 15, float4(ab, ab.b), bool4(true, true, true, (fl & 0x80) != 0));\n"
"    rset(R, ocw & 15,        float4(cd, cd.b), bool4(true, true, true, (fl & 0x40) != 0));\n"
"    rset(R, (ocw >> 8) & 15, float4(ms, 0), RGB);\n"
"    rset(R, (aocw >> 4) & 15, aab.xxxx, A_);\n"
"    rset(R, aocw & 15,        acd.xxxx, A_);\n"
"    rset(R, (aocw >> 8) & 15, ams.xxxx, A_); }\n"
"  float4 o;\n"
"  if (fin.x || fin.y) {\n"
"    uint f0 = fin.x, f1 = fin.y, ff = f1 & 0xFF;\n"
"    R.c0 = fc0; R.c1 = fc1;\n"
"    float3 a = (ff & 0x40) ? 1.0 - R.v1.rgb : R.v1.rgb;\n"
"    float3 b = (ff & 0x20) ? 1.0 - R.r0.rgb : R.r0.rgb;\n"
"    R.sm = float4((ff & 0x80) ? saturate(a + b) : a + b, 0);\n"
"    float3 E = rdrgb(R, (f1 >> 24) & 255), F = rdrgb(R, (f1 >> 16) & 255);\n"
"    R.ef = float4(E * F, 0);\n"
"    float3 A = rdrgb(R, (f0 >> 24) & 255), B = rdrgb(R, (f0 >> 16) & 255);\n"
"    float3 C = rdrgb(R, (f0 >> 8) & 255),  D = rdrgb(R, f0 & 255);\n"
"    o = float4(D + C * (1.0 - A) + B * A, rda(R, (f1 >> 8) & 255));\n"
"  } else o = R.r0;\n"
"  o = saturate(o);\n"
"  if (alpha_func >= 0 && !cmp(alpha_func, round(o.a * 255), alpha_ref)) discard;\n"
"  if (zclip > 0.5) { float zw = i.p.z / zscale;\n"
"    if (zw < zmin || zw > zmax) discard; }\n"
"  return o; }\n";

typedef struct {
    float surf[2], zscale, textured;
    float texsize[2], alpha_func, alpha_ref;
    float zmin, zmax, zclip, pad;
} Consts;

/* cbuffer RC above. */
typedef struct {
    uint32_t ccw[8][4];
    float    fac0[8][4], fac1[8][4];
    uint32_t fin[4];
    float    fc0[4], fc1[4], fogc[4];
    uint32_t misc[4];
} RcConsts;

static ID3D11VertexShader *s_vs, *s_vs_rc;
static ID3D11PixelShader  *s_ps, *s_ps_rc;
static ID3D11InputLayout  *s_layout, *s_layout_rc;
static ID3D11Buffer       *s_cb, *s_vb, *s_cb_rc, *s_vbx;
static int                 s_rc_ok;       /* the combiner pipeline compiled */
static int                 s_pipe;        /* bound: 0 stage-0 pipeline, 1 combiners */
#define VB_BYTES  (8u << 20)
#define VBX_BYTES (32u << 20)              /* Nv2aVertexEx for VB_BYTES of vertices */
static UINT s_vb_used, s_vbx_used;

/* ---- render size ----------------------------------------------------- */

/* RECOMP_RENDER_SIZE=<w>x<h> renders above the title's 640x480: every colour
 * and depth target is s_sx x s_sy times its guest size (1920x1080: 3 x 2.25,
 * the picture stretched to 16:9). Vertices stay in guest pixels -- the
 * shaders map them through the guest size, only the viewport grows -- so
 * nothing the title sends changes. At flip a scaled target is shrunk back to
 * its guest size for the copy into guest memory, and the shown one goes to
 * the window at full size (xbox_FramebufferWindowPresentHiRes). */
#define GUEST_W 640.0f
#define GUEST_H 480.0f
static float s_sx = 1.0f, s_sy = 1.0f;
static int   s_scaled;
/* RECOMP_OUTPUT_SIZE: what the window is given, when smaller than the render
 * size -- supersampling renders at twice the output and the shown frame is
 * shrunk on the GPU first (a 2:1 bilinear tap at pixel centres averages each
 * 2x2 block). 0: the render size. */
static uint32_t s_ow, s_oh;

extern uint32_t xbox_FramebufferWindowPresentedVa(void);
extern void xbox_FramebufferWindowPresentHiRes(const void *pixels, uint32_t w, uint32_t h,
                                               uint32_t pitch);

static void render_size_init(void)
{
    const char *e = getenv("RECOMP_RENDER_SIZE");
    unsigned w = 0, h = 0;
    if (!e || sscanf(e, "%ux%u", &w, &h) != 2 || w < 640 || h < 480 || w > 7680 || h > 4320)
        return;
    s_sx = (float)w / GUEST_W;
    s_sy = (float)h / GUEST_H;
    s_scaled = w != 640 || h != 480;
    e = getenv("RECOMP_OUTPUT_SIZE");
    if (e && sscanf(e, "%ux%u", &s_ow, &s_oh) == 2 && s_ow && s_oh && s_ow <= w && s_oh <= h
        && (s_ow < w || s_oh < h)) {
        /* kept */
    } else {
        s_ow = s_oh = 0;
    }
    if (s_scaled)
        fprintf(stderr, "  [GPU-BE] rendering at %ux%u (%.2fx%.2f the guest size)%s\n",
                w, h, s_sx, s_sy, s_ow ? ", shown smaller" : "");
    if (s_ow)
        fprintf(stderr, "  [GPU-BE] output %ux%u\n", s_ow, s_oh);
}

/* ---- surfaces -------------------------------------------------------- */

typedef struct {
    uint32_t key, va, w, h, pitch, bpp;     /* w, h: guest pixels */
    uint32_t rw, rh;                        /* the target's own, scaled */
    ID3D11Texture2D          *tex, *staging;
    ID3D11RenderTargetView   *rtv;
    ID3D11ShaderResourceView *srv;
    /* Scaled only: a guest-size copy, for readback and for loading what the
     * guest surface held when the target was made. */
    ID3D11Texture2D          *guest;
    ID3D11RenderTargetView   *guest_rtv;
    ID3D11ShaderResourceView *guest_srv;
    int dirty, used;
    int pending;                /* a copy is in staging, to deliver at flip */
} Surf;

typedef struct {
    uint32_t key, w, h;
    ID3D11Texture2D        *tex;
    ID3D11DepthStencilView *dsv;
    int used;
} Zbuf;

#define MAX_SURF 16
#define MAX_ZBUF 8
static Surf s_surf[MAX_SURF];
static Zbuf s_zbuf[MAX_ZBUF];
static unsigned s_tick;

/* ---- textures -------------------------------------------------------- */

typedef struct {
    uint32_t key, w, h, color, pitch, levels, sum;
    uint32_t tw, th;            /* the D3D texture's size: w x h, or a pack image's */
    int hud;                    /* one of the HUD's (texpack\hud.txt) */
    ID3D11Texture2D          *tex;
    ID3D11ShaderResourceView *srv;
    unsigned used;
} Tex;

#define MAX_TEX 768
static Tex s_tex[MAX_TEX];
static uint32_t *s_decode;     /* scratch for nv2a_backend_decode_texture */
static size_t    s_decode_cap;

/* ---- stats ----------------------------------------------------------- */

static unsigned long s_n_draws, s_n_tris, s_n_uploads, s_n_flips;
/* Time spent inside draw() and in flip()'s readbacks, for the stats line. */
static LARGE_INTEGER s_qpf;
static LONGLONG s_draw_ticks, s_flip_ticks;
/* draw() by step: target, texture, constants, state, vertices, draw call. */
static LONGLONG s_seg[6];
static LONGLONG s_probe[2];     /* surface_for, zbuf_for alone */
static unsigned long s_ivl[4];  /* flip intervals, see be_flip */
static unsigned long s_n_binds;

static LONGLONG ticks(void)
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

/* ===================================================================== */

static void fail(const char *what, HRESULT hr)
{
    fprintf(stderr, "  [GPU-BE] %s failed (0x%08lX); drawing nothing\n",
            what, (unsigned long)hr);
    fflush(stderr);
    s_failed = 1;
}

/* The adapter with the most dedicated video memory: on a hybrid laptop that
 * is the discrete GPU, which the default adapter often is not. */
static IDXGIAdapter1 *pick_adapter(void)
{
    IDXGIFactory1 *f = NULL;
    IDXGIAdapter1 *best = NULL, *a;
    SIZE_T best_mem = 0;
    UINT i;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f)))
        return NULL;
    for (i = 0; IDXGIFactory1_EnumAdapters1(f, i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d))
                && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
                && d.DedicatedVideoMemory > best_mem) {
            if (best)
                IDXGIAdapter1_Release(best);
            best = a;
            best_mem = d.DedicatedVideoMemory;
            continue;
        }
        IDXGIAdapter1_Release(a);
    }
    IDXGIFactory1_Release(f);
    return best;
}

/* The combiner pipeline. A failure here is not fatal: batches then draw
 * through the stage-0 pipeline, as before it existed. */
static void init_rc_pipeline(void)
{
    static const D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DEPTH",    0, DXGI_FORMAT_R32_FLOAT,          0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "RHW",      0, DXGI_FORMAT_R32_FLOAT,          0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM,     0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,       0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "STAGE",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "STAGE",    1, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "STAGE",    2, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 32, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "STAGE",    3, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 48, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "SPECULAR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 1, 64, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "FOG",      0, DXGI_FORMAT_R32_FLOAT,          1, 80, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    HRESULT hr;

    hr = D3DCompile(s_hlsl_rc, sizeof s_hlsl_rc - 1, "gpu_backend_rc", NULL, NULL, "vs_rc",
                    "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
    if (SUCCEEDED(hr))
        hr = D3DCompile(s_hlsl_rc, sizeof s_hlsl_rc - 1, "gpu_backend_rc", NULL, NULL, "ps_rc",
                        "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
    if (FAILED(hr)) {
        if (err)
            fprintf(stderr, "%s\n", (const char *)ID3D10Blob_GetBufferPointer(err));
        fprintf(stderr, "  [GPU-BE] combiner shaders did not compile (0x%08lX): stage 0 only\n",
                (unsigned long)hr);
        return;
    }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs_rc);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &s_ps_rc);
    hr = ID3D11Device_CreateInputLayout(s_dev, el, sizeof el / sizeof el[0],
                                        ID3D10Blob_GetBufferPointer(vsb),
                                        ID3D10Blob_GetBufferSize(vsb), &s_layout_rc);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    if (FAILED(hr) || !s_vs_rc || !s_ps_rc)
        return;

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(RcConsts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb_rc)))
        return;
    bd.ByteWidth = VBX_BYTES;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    if (FAILED(ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vbx)))
        return;
    s_rc_ok = 1;
}

static int init_device(void)
{
    static const D3D11_INPUT_ELEMENT_DESC el[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 0,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "DEPTH",    0, DXGI_FORMAT_R32_FLOAT,      0, 8,  D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "RHW",      0, DXGI_FORMAT_R32_FLOAT,      0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR",    0, DXGI_FORMAT_B8G8R8A8_UNORM, 0, 16, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,   0, 20, D3D11_INPUT_PER_VERTEX_DATA, 0 },
    };
    IDXGIAdapter1 *ad;
    ID3DBlob *vsb = NULL, *psb = NULL, *err = NULL;
    D3D11_BUFFER_DESC bd;
    HRESULT hr;

    if (s_dev || s_failed)
        return s_dev != NULL;
    /* After the early return: enumerating adapters costs ~0.3 ms, and this
     * runs on every draw. */
    ad = pick_adapter();

    hr = D3D11CreateDevice((IDXGIAdapter *)ad,
                           ad ? D3D_DRIVER_TYPE_UNKNOWN : D3D_DRIVER_TYPE_HARDWARE,
                           NULL, 0, NULL, 0, D3D11_SDK_VERSION, &s_dev, NULL, &s_ctx);
    if (ad) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(IDXGIAdapter1_GetDesc1(ad, &d)))
            fprintf(stderr, "  [GPU-BE] rendering on %ls (%u MB)\n", d.Description,
                    (unsigned)(d.DedicatedVideoMemory >> 20));
        IDXGIAdapter1_Release(ad);
    }
    if (FAILED(hr)) { fail("D3D11CreateDevice", hr); return 0; }
    hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "gpu_backend", NULL, NULL, "vs",
                    "vs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &vsb, &err);
    if (SUCCEEDED(hr))
        hr = D3DCompile(s_hlsl, sizeof s_hlsl - 1, "gpu_backend", NULL, NULL, "ps",
                        "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &psb, &err);
    if (FAILED(hr)) {
        if (err)
            fprintf(stderr, "%s\n", (const char *)ID3D10Blob_GetBufferPointer(err));
        fail("D3DCompile", hr);
        return 0;
    }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &s_ps);
    hr = ID3D11Device_CreateInputLayout(s_dev, el, 5, ID3D10Blob_GetBufferPointer(vsb),
                                        ID3D10Blob_GetBufferSize(vsb), &s_layout);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    if (FAILED(hr)) { fail("CreateInputLayout", hr); return 0; }

    memset(&bd, 0, sizeof bd);
    bd.ByteWidth = sizeof(Consts);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_cb);
    bd.ByteWidth = VB_BYTES;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    hr = ID3D11Device_CreateBuffer(s_dev, &bd, NULL, &s_vb);
    if (FAILED(hr)) { fail("CreateBuffer", hr); return 0; }
    init_rc_pipeline();
    render_size_init();
    if (s_scaled && !init_blit()) {
        fprintf(stderr, "  [GPU-BE] resize shaders failed: rendering at the guest size\n");
        s_scaled = 0;
        s_sx = s_sy = 1.0f;
    }

    {
        D3D11_RASTERIZER_DESC rd;
        ID3D11RasterizerState *rs;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE;
        rd.DepthClipEnable = FALSE;     /* SET_CLIP_MIN/MAX clip, in the PS */
        ID3D11Device_CreateRasterizerState(s_dev, &rd, &rs);
        ID3D11DeviceContext_RSSetState(s_ctx, rs);
        s_rs_init = rs;                 /* no culling */
    }
    ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
    ID3D11DeviceContext_IASetPrimitiveTopology(s_ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
    ID3D11DeviceContext_VSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 0, 1, &s_cb);
    return 1;
}

/* ---- surfaces -------------------------------------------------------- */

/* A surface is its address. Its size is the full surface -- pitch / bytes
 * per pixel wide, the tallest clip seen high -- not the clip rectangle, which
 * a title changes between draws: keyed by the clip, every change made a new
 * 640x480 target and re-uploaded it, ~0.5 ms per draw and the frame's
 * earlier draws lost. A taller clip than before grows it. */
static uint32_t surface_width(const Nv2aSurface *s)
{
    uint32_t w = s->bytes_per_pixel ? s->pitch / s->bytes_per_pixel : 0;
    return w > s->width && w <= 4096 ? w : s->width;
}

static unsigned long s_n_targets;      /* created, for the stats line */
/* Set whenever a target or depth buffer is (re)created: a slot that grew
 * keeps its address but not its views, so bind_target must bind again. */
static int s_rebind = 1;

/* Resizing copies between a scaled target and its guest-size twin: one
 * triangle over the destination, sampled bilinear. */
static const char s_hlsl_blit[] =
"Texture2D t : register(t0); SamplerState s : register(s0);\n"
"struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };\n"
"V vs(uint i : SV_VertexID) { V o; float2 q = float2((i << 1) & 2, i & 2);\n"
"  o.uv = q; o.p = float4(q * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
"float4 ps(V i) : SV_Target { return t.Sample(s, i.uv); }\n";
static ID3D11VertexShader *s_vs_blit;
static ID3D11PixelShader  *s_ps_blit;
static ID3D11SamplerState *s_samp_blit;

static int init_blit(void)
{
    ID3DBlob *vsb = NULL, *psb = NULL;
    D3D11_SAMPLER_DESC sd;

    if (FAILED(D3DCompile(s_hlsl_blit, sizeof s_hlsl_blit - 1, "blit", NULL, NULL, "vs",
                          "vs_5_0", 0, 0, &vsb, NULL))
        || FAILED(D3DCompile(s_hlsl_blit, sizeof s_hlsl_blit - 1, "blit", NULL, NULL, "ps",
                             "ps_5_0", 0, 0, &psb, NULL))) {
        if (vsb) ID3D10Blob_Release(vsb);
        return 0;
    }
    ID3D11Device_CreateVertexShader(s_dev, ID3D10Blob_GetBufferPointer(vsb),
                                    ID3D10Blob_GetBufferSize(vsb), NULL, &s_vs_blit);
    ID3D11Device_CreatePixelShader(s_dev, ID3D10Blob_GetBufferPointer(psb),
                                   ID3D10Blob_GetBufferSize(psb), NULL, &s_ps_blit);
    ID3D10Blob_Release(vsb);
    ID3D10Blob_Release(psb);
    memset(&sd, 0, sizeof sd);
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11Device_CreateSamplerState(s_dev, &sd, &s_samp_blit);
    return s_vs_blit && s_ps_blit && s_samp_blit;
}

/* Draw src over all of dst (w x h). Leaves the pipeline to be rebound: the
 * next draw's bind_target and pipeline switch see to it. */
static void blit(ID3D11ShaderResourceView *src, ID3D11RenderTargetView *dst,
                 uint32_t w, uint32_t h)
{
    ID3D11ShaderResourceView *none = NULL;
    D3D11_VIEWPORT vp;

    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &dst, NULL);
    memset(&vp, 0, sizeof vp);
    vp.Width = (float)w; vp.Height = (float)h; vp.MaxDepth = 1.0f;
    ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
    ID3D11DeviceContext_IASetInputLayout(s_ctx, NULL);
    ID3D11DeviceContext_VSSetShader(s_ctx, s_vs_blit, NULL, 0);
    ID3D11DeviceContext_PSSetShader(s_ctx, s_ps_blit, NULL, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &src);
    ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &s_samp_blit);
    ID3D11DeviceContext_OMSetBlendState(s_ctx, NULL, NULL, 0xFFFFFFFFu);
    if (s_cull_cur > 0 && s_cull_st[0][0]) {
        ID3D11DeviceContext_RSSetState(s_ctx, s_cull_st[0][0]);
        s_cull_cur = 0;
    } else if (s_cull_cur > 0) {
        ID3D11DeviceContext_RSSetState(s_ctx, s_rs_init);
        s_cull_cur = -1;
    }
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, NULL, 0);
    ID3D11DeviceContext_Draw(s_ctx, 3, 0);
    ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &none);
    ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 0, NULL, NULL);
    s_rebind = 1;
    s_pipe = -1;
}

static Surf *surface_for(const Nv2aSurface *s, int create)
{
    uint32_t key = ADDR_KEY(s->color_va), w = surface_width(s), h = s->height;
    Surf *victim = NULL;
    int i;

    for (i = 0; i < MAX_SURF; i++) {
        Surf *r = &s_surf[i];
        if (r->tex && r->key == key) {
            r->used = ++s_tick;
            r->va = s->color_va; r->pitch = s->pitch; r->bpp = s->bytes_per_pixel;
            if (w <= r->w && h <= r->h)
                return r;
            if (h < r->h) h = r->h;
            if (w < r->w) w = r->w;
            victim = r;                 /* grow it */
            break;
        }
    }
    if (!create || !w || !h || w > 4096 || h > 4096)
        return NULL;
    s_n_targets++;
    s_rebind = 1;
    /* A free slot, else the least recently used. */
    for (i = 0; i < MAX_SURF && !victim; i++)
        if (!s_surf[i].tex)
            victim = &s_surf[i];
    if (!victim) {
        victim = &s_surf[0];
        for (i = 1; i < MAX_SURF; i++)
            if (s_surf[i].used < victim->used)
                victim = &s_surf[i];
    }
    if (victim->tex) {
        ID3D11RenderTargetView_Release(victim->rtv);
        ID3D11ShaderResourceView_Release(victim->srv);
        ID3D11Texture2D_Release(victim->staging);
        ID3D11Texture2D_Release(victim->tex);
        if (victim->guest) {
            ID3D11RenderTargetView_Release(victim->guest_rtv);
            ID3D11ShaderResourceView_Release(victim->guest_srv);
            ID3D11Texture2D_Release(victim->guest);
        }
        memset(victim, 0, sizeof *victim);
    }
    {
        D3D11_TEXTURE2D_DESC td;
        uint32_t rw = s_scaled ? (uint32_t)(w * s_sx + 0.5f) : w;
        uint32_t rh = s_scaled ? (uint32_t)(h * s_sy + 0.5f) : h;
        memset(&td, 0, sizeof td);
        td.Width = rw; td.Height = rh;
        td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &victim->tex)))
            return NULL;
        ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)victim->tex,
                                            NULL, &victim->rtv);
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)victim->tex,
                                              NULL, &victim->srv);
        td.Width = w; td.Height = h;
        if (rw != w || rh != h) {
            ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &victim->guest);
            ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)victim->guest,
                                                NULL, &victim->guest_rtv);
            ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)victim->guest,
                                                  NULL, &victim->guest_srv);
        }
        td.Usage = D3D11_USAGE_STAGING;
        td.BindFlags = 0;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &victim->staging);
        victim->rw = rw; victim->rh = rh;
    }
    victim->key = key; victim->va = s->color_va;
    victim->w = w; victim->h = h;
    victim->pitch = s->pitch; victim->bpp = s->bytes_per_pixel;
    victim->used = ++s_tick;
    /* Start from what the guest surface holds: a title can draw over a frame
     * the CPU put there (a movie, a loading screen). Scaled, it goes in at
     * guest size and is stretched up. */
    if (victim->bpp == 4 && victim->pitch >= victim->w * 4) {
        ID3D11Texture2D *in = victim->guest ? victim->guest : victim->tex;
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)in, 0,
                                              NULL, GUEST(victim->va), victim->pitch, 0);
        if (victim->guest && victim->guest_srv)
            blit(victim->guest_srv, victim->rtv, victim->rw, victim->rh);
    }
    return victim;
}

static Zbuf *zbuf_for(uint32_t zeta_va, uint32_t w, uint32_t h)
{
    uint32_t key = ADDR_KEY(zeta_va);
    Zbuf *victim = NULL;
    int i;

    if (!zeta_va)
        return NULL;
    for (i = 0; i < MAX_ZBUF; i++)
        if (s_zbuf[i].tex && s_zbuf[i].key == key && s_zbuf[i].w == w && s_zbuf[i].h == h) {
            s_zbuf[i].used = ++s_tick;
            return &s_zbuf[i];
        }
    for (i = 0; i < MAX_ZBUF && !victim; i++)
        if (!s_zbuf[i].tex)
            victim = &s_zbuf[i];
    if (!victim) {
        victim = &s_zbuf[0];
        for (i = 1; i < MAX_ZBUF; i++)
            if (s_zbuf[i].used < victim->used)
                victim = &s_zbuf[i];
    }
    if (victim->tex) {
        ID3D11DepthStencilView_Release(victim->dsv);
        ID3D11Texture2D_Release(victim->tex);
        memset(victim, 0, sizeof *victim);
    }
    s_n_targets++;
    s_rebind = 1;
    {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &victim->tex)))
            return NULL;
        ID3D11Device_CreateDepthStencilView(s_dev, (ID3D11Resource *)victim->tex,
                                            NULL, &victim->dsv);
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, victim->dsv,
            D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
    victim->key = key; victim->w = w; victim->h = h;
    victim->used = ++s_tick;
    return victim;
}

/* Bind surface s (and its depth buffer) as the render target. Vertices are
 * in surface pixels, so the viewport is the whole target, not the clip. The
 * binding is skipped when it is already the one in place. */
static Surf *bind_target(const Nv2aSurface *s, const Nv2aRenderState *rs, Zbuf **zout)
{
    static Surf *bound_r;
    static Zbuf *bound_z;
    Surf *r = surface_for(s, 1);
    Zbuf *z;

    if (!r)
        return NULL;
    z = rs ? zbuf_for(rs->zeta_va, r->rw, r->rh) : NULL;
    if (s_rebind || r != bound_r || z != bound_z) {
        s_rebind = 0;
        s_n_binds++;
        D3D11_VIEWPORT vp;
        ID3D11DeviceContext_OMSetRenderTargets(s_ctx, 1, &r->rtv, z ? z->dsv : NULL);
        memset(&vp, 0, sizeof vp);
        vp.Width = (float)r->rw; vp.Height = (float)r->rh; vp.MaxDepth = 1.0f;
        ID3D11DeviceContext_RSSetViewports(s_ctx, 1, &vp);
        bound_r = r;
        bound_z = z;
    }
    if (zout)
        *zout = z;
    return r;
}

/* ---- textures -------------------------------------------------------- */

/* A sparse checksum of the guest bytes: notices a texture rewritten in
 * place (video, streamed) without hashing megabytes per draw. Spans level 0;
 * the caller has already refused a texture that runs past guest RAM. */
static uint32_t tex_sum(const Nv2aTexture *t, uint32_t n)
{
    const uint8_t *p = GUEST(t->offset);
    uint32_t sum = 2166136261u, i, step;

    if (n < 4)
        return 0;
    step = n / 64 > 4 ? (n / 64) & ~3u : 4;
    for (i = 0; i + 4 <= n; i += step)
        sum = (sum ^ *(const uint32_t *)(p + i)) * 16777619u;
    return sum ^ *(const uint32_t *)(p + n - 4);
}

static int pow2(uint32_t v) { return v && !(v & (v - 1)); }

/* FNV-1a over decoded texels: names a texture by what it looks like, not
 * where it was loaded, so the same art is recognised at any address. */
static uint32_t texels_hash(const uint32_t *px, size_t n)
{
    uint32_t h = 2166136261u;
    size_t i;
    for (i = 0; i < n; i++)
        h = (h ^ px[i]) * 16777619u;
    return h;
}

/* RECOMP_GPU_TEXDUMP=<folder>: each texture the first time it is decoded,
 * as a 32-bit BMP named <hash>_<w>x<h>_fmt<NN>.bmp -- for finding a piece of
 * art to replace. */
static void texture_dump(const Nv2aTexture *t, const uint32_t *px, uint32_t hash)
{
    static const char *dir = (const char *)-1;
    static uint32_t seen[4096];
    static int nseen;
    char path[MAX_PATH];
    uint8_t hdr[54];
    uint32_t total = 54 + t->width * t->height * 4, y;
    int32_t neg_h = -(int32_t)t->height;
    FILE *f;
    int i;

    if (dir == (const char *)-1)
        dir = getenv("RECOMP_GPU_TEXDUMP");
    if (!dir || !*dir)
        return;
    for (i = 0; i < nseen; i++)
        if (seen[i] == hash)
            return;
    if (nseen < 4096)
        seen[nseen++] = hash;
    _snprintf(path, sizeof path, "%s\\%08X_%ux%u_fmt%02X.bmp", dir, hash, t->width,
              t->height, t->color);
    path[sizeof path - 1] = 0;
    if (!(f = fopen(path, "wb")))
        return;
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &total, 4);
    hdr[10] = 54; hdr[14] = 40;
    memcpy(hdr + 18, &t->width, 4);
    memcpy(hdr + 22, &neg_h, 4);              /* top-down */
    hdr[26] = 1; hdr[28] = 32;
    fwrite(hdr, 1, sizeof hdr, f);
    for (y = 0; y < t->height; y++)
        fwrite(px + (size_t)y * t->width, 4, t->width, f);
    fclose(f);
}

/* Art swapped in for the title's own, by texels hash. RECOMP_BUTTON_PROMPTS=ps5
 * or ps4 (the launcher's Button prompts) replaces the strip every button
 * prompt is drawn from -- A/B/X/Y, the triggers, White/Black -- with the same
 * layout in PlayStation buttons: white symbols as on the DualSense, or the
 * PS4's coloured ones (assets\ps5_buttons.bmp / ps4_buttons.bmp, built by
 * tools\make_ps_buttons.ps1, linked in as RCDATA 101 / 102 by spiderman.rc). */
#define HASH_BUTTON_STRIP 0x73E8ECF1u
#define RES_PS5_BUTTONS   101
#define RES_PS4_BUTTONS   102

/* A 32-bit top-down BMP resource's pixels, if it is w x h. */
static const uint32_t *bmp_resource(int id, uint32_t w, uint32_t h)
{
    HRSRC r = FindResourceA(NULL, MAKEINTRESOURCEA(id), (LPCSTR)RT_RCDATA);
    const uint8_t *b;
    int32_t bw, bh;
    DWORD size;

    if (!r || !(b = (const uint8_t *)LockResource(LoadResource(NULL, r))))
        return NULL;
    size = SizeofResource(NULL, r);
    if (size < 54 || b[0] != 'B' || b[1] != 'M' || b[28] != 32)
        return NULL;
    memcpy(&bw, b + 18, 4);
    memcpy(&bh, b + 22, 4);
    if ((uint32_t)bw != w || bh != -(int32_t)h || size < 54 + w * h * 4)
        return NULL;
    return (const uint32_t *)(b + 54);
}

/* 0: Xbox prompts, 4: PS4, 5: PS5 ("ps" and "playstation" too). */
static int prompts_ps(void)
{
    static int ps = -1;
    if (ps < 0) {
        const char *e = getenv("RECOMP_BUTTON_PROMPTS");
        ps = !e || !*e || !_stricmp(e, "xbox") ? 0 : !_stricmp(e, "ps4") ? 4 : 5;
    }
    return ps;
}

static void texture_replace(const Nv2aTexture *t, uint32_t *px, uint32_t hash)
{
    static int ps = -1;
    static const uint32_t *strip;

    if (ps < 0) {
        ps = prompts_ps();
        if (ps && !(strip = bmp_resource(ps == 4 ? RES_PS4_BUTTONS : RES_PS5_BUTTONS, 512, 64))) {
            fprintf(stderr, "  [GPU-BE] PlayStation button art missing from the exe\n");
            ps = 0;
        }
    }
    if (ps && hash == HASH_BUTTON_STRIP && t->width == 512 && t->height == 64)
        memcpy(px, strip, 512 * 64 * 4);
}

/* ---- texture packs ----------------------------------------------------
 *
 * texpack\<hash>*.png beside the exe replaces the texture whose decoded
 * texels hash to <hash> (8 hex digits, as RECOMP_GPU_TEXDUMP names its
 * dumps), at the image's own size: upscaled menu art and fonts. With
 * PlayStation prompts <hash>_ps5.png or <hash>_ps4.png is used before
 * <hash>.png, and never otherwise (the upscaled button strip). Texture
 * coordinates are normalised in both pipelines, so a larger image needs
 * nothing else. Images are read with WIC on first use and kept.
 * RECOMP_TEXPACK=0 turns packs off. */
typedef struct {
    uint32_t hash, w, h;
    int ps;                     /* <hash>_ps5 / _ps4: 5 / 4, else 0 */
    uint32_t *px;               /* BGRA, loaded on first use */
    int failed;
    char path[MAX_PATH];
} PackImg;

static PackImg *s_pack;
static int      s_npack = -1;

static void pack_scan(void)
{
    char dir[MAX_PATH], pat[MAX_PATH], *slash;
    WIN32_FIND_DATAA fd;
    HANDLE h;
    const char *e = getenv("RECOMP_TEXPACK");

    s_npack = 0;
    if (e && *e == '0')
        return;
    GetModuleFileNameA(NULL, dir, sizeof dir);
    if ((slash = strrchr(dir, '\\')) != NULL)
        slash[1] = 0;
    strcat(dir, "texpack\\");
    _snprintf(pat, sizeof pat, "%s*.png", dir);
    pat[sizeof pat - 1] = 0;
    if ((h = FindFirstFileA(pat, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        unsigned hash;
        PackImg *n;
        if (sscanf(fd.cFileName, "%8x", &hash) != 1)
            continue;
        n = (PackImg *)realloc(s_pack, (s_npack + 1) * sizeof *s_pack);
        if (!n)
            break;
        s_pack = n;
        memset(&s_pack[s_npack], 0, sizeof *s_pack);
        s_pack[s_npack].hash = hash;
        if (!_strnicmp(fd.cFileName + 8, "_ps5.", 5))
            s_pack[s_npack].ps = 5;
        else if (!_strnicmp(fd.cFileName + 8, "_ps4.", 5))
            s_pack[s_npack].ps = 4;
        _snprintf(s_pack[s_npack].path, MAX_PATH, "%s%s", dir, fd.cFileName);
        s_npack++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    fprintf(stderr, "  [GPU-BE] texture pack: %d image(s) in %s\n", s_npack, dir);
}

/* A PNG (or anything WIC reads) as tightly packed BGRA. */
static uint32_t *wic_load(const char *path, uint32_t *w, uint32_t *h)
{
    static IWICImagingFactory *f;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    uint32_t *px = NULL;
    WCHAR wpath[MAX_PATH];
    UINT iw, ih;

    if (!f) {
        CoInitializeEx(NULL, COINIT_MULTITHREADED);
        if (FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                    &IID_IWICImagingFactory, (void **)&f)))
            return NULL;
    }
    MultiByteToWideChar(CP_ACP, 0, path, -1, wpath, MAX_PATH);
    if (SUCCEEDED(IWICImagingFactory_CreateDecoderFromFilename(f, wpath, NULL, GENERIC_READ,
                      WICDecodeMetadataCacheOnDemand, &dec))
        && SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame))
        && SUCCEEDED(IWICImagingFactory_CreateFormatConverter(f, &conv))
        && SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame,
                      &GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0.0,
                      WICBitmapPaletteTypeCustom))
        && SUCCEEDED(IWICBitmapFrameDecode_GetSize(frame, &iw, &ih))
        && iw && ih && iw <= 8192 && ih <= 8192
        && (px = (uint32_t *)malloc((size_t)iw * ih * 4)) != NULL) {
        if (FAILED(IWICFormatConverter_CopyPixels(conv, NULL, iw * 4, iw * ih * 4, (BYTE *)px))) {
            free(px);
            px = NULL;
        } else {
            *w = iw; *h = ih;
        }
    }
    if (conv) IWICFormatConverter_Release(conv);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec) IWICBitmapDecoder_Release(dec);
    return px;
}

static const PackImg *pack_find(uint32_t hash)
{
    int i, pass, ps = prompts_ps();
    if (s_npack < 0)
        pack_scan();
    /* The image for these prompts first, then a plain one. */
    for (pass = ps ? 0 : 1; pass < 2; pass++)
    for (i = 0; i < s_npack; i++) {
        PackImg *p = &s_pack[i];
        if (p->hash != hash || p->failed || p->ps != (pass == 0 ? ps : 0))
            continue;
        if (!p->px && !(p->px = wic_load(p->path, &p->w, &p->h))) {
            fprintf(stderr, "  [GPU-BE] texture pack: could not read %s\n", p->path);
            p->failed = 1;
            return NULL;
        }
        return p;
    }
    return NULL;
}

/* texpack\hud.txt: hashes (8 hex digits, a name after) of the HUD's
 * textures (tools/hud_upscale.py), drawn their own shape in widescreen. */
static uint32_t *s_hud;
static int s_nhud = -1;

static int hud_texture(uint32_t hash)
{
    int i;
    if (s_nhud < 0) {
        char path[MAX_PATH], line[128], *slash;
        FILE *f;
        s_nhud = 0;
        GetModuleFileNameA(NULL, path, sizeof path);
        if ((slash = strrchr(path, '\\')) != NULL)
            strcpy(slash + 1, "texpack\\hud.txt");
        if ((f = fopen(path, "r")) != NULL) {
            unsigned h;
            while (fgets(line, sizeof line, f))
                if (sscanf(line, "%8x", &h) == 1) {
                    uint32_t *n = (uint32_t *)realloc(s_hud, (s_nhud + 1) * sizeof *s_hud);
                    if (!n)
                        break;
                    s_hud = n;
                    s_hud[s_nhud++] = h;
                }
            fclose(f);
        }
    }
    for (i = 0; i < s_nhud; i++)
        if (s_hud[i] == hash)
            return 1;
    return 0;
}

/* A texture just decoded from guest memory, before upload. Returns a pack
 * image to upload instead, or NULL; *hud whether it is one of the HUD's. */
static const PackImg *texture_decoded(const Nv2aTexture *t, uint32_t *px, int *hud)
{
    uint32_t hash = texels_hash(px, (size_t)t->width * t->height);
    texture_dump(t, px, hash);
    texture_replace(t, px, hash);
    *hud = hud_texture(hash);
    return pack_find(hash);
}

static int s_srv_hud;           /* texture_srv's last: a HUD texture */

static ID3D11ShaderResourceView *texture_srv(const Nv2aTexture *t)
{
    uint32_t key = ADDR_KEY(t->offset), sum, bytes, uw, uh;
    Tex *hit = NULL, *victim = NULL;
    const PackImg *pack;
    const uint32_t *up;
    int i, mips, hud;

    s_srv_hud = 0;
    /* One of our render targets, sampled: use it where it is. */
    for (i = 0; i < MAX_SURF; i++)
        if (s_surf[i].tex && s_surf[i].key == key)
            return s_surf[i].srv;

    /* A stale texture whose image would end past guest RAM: draw untextured,
     * as the CPU path does with a texture it cannot read. */
    bytes = nv2a_backend_texture_bytes(t);
    if (!bytes)
        return NULL;
    sum = tex_sum(t, bytes);
    for (i = 0; i < MAX_TEX && !hit; i++) {
        Tex *e = &s_tex[i];
        if (e->tex && e->key == key && e->w == t->width && e->h == t->height
                && e->color == t->color && e->pitch == t->pitch)
            hit = e;
    }
    if (hit && hit->sum == sum) {
        hit->used = ++s_tick;
        s_srv_hud = hit->hud;
        return hit->srv;
    }
    if ((size_t)t->width * t->height > s_decode_cap) {
        free(s_decode);
        s_decode_cap = (size_t)t->width * t->height;
        s_decode = (uint32_t *)malloc(s_decode_cap * 4);
        if (!s_decode) { s_decode_cap = 0; return NULL; }
    }
    if (!nv2a_backend_decode_texture(t, s_decode))
        return NULL;
    pack = texture_decoded(t, s_decode, &hud);
    s_srv_hud = hud;
    up = pack ? pack->px : s_decode;
    uw = pack ? pack->w : t->width;
    uh = pack ? pack->h : t->height;
    s_n_uploads++;

    /* Same size and format: overwrite in place, no reallocation. */
    if (hit && hit->tw == uw && hit->th == uh) {
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)hit->tex, 0,
                                              NULL, up, uw * 4, 0);
        if (hit->levels > 1)
            ID3D11DeviceContext_GenerateMips(s_ctx, hit->srv);
        hit->sum = sum;
        hit->hud = hud;
        hit->used = ++s_tick;
        return hit->srv;
    }
    if (hit)
        victim = hit;           /* now (or no longer) a pack image: remade */
    for (i = 0; i < MAX_TEX && !victim; i++)
        if (!s_tex[i].tex)
            victim = &s_tex[i];
    if (!victim) {
        victim = &s_tex[0];
        for (i = 1; i < MAX_TEX; i++)
            if (s_tex[i].used < victim->used)
                victim = &s_tex[i];
    }
    if (victim->tex) {
        ID3D11ShaderResourceView_Release(victim->srv);
        ID3D11Texture2D_Release(victim->tex);
        memset(victim, 0, sizeof *victim);
    }
    /* A pack image gets mips whenever its size allows: it is usually drawn
     * smaller than itself. */
    mips = (t->levels > 1 || pack) && pow2(uw) && pow2(uh);
    {
        D3D11_TEXTURE2D_DESC td;
        memset(&td, 0, sizeof td);
        td.Width = uw; td.Height = uh;
        td.MipLevels = mips ? 0 : 1; td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE | (mips ? D3D11_BIND_RENDER_TARGET : 0);
        td.MiscFlags = mips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
        if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &victim->tex)))
            return NULL;
        ID3D11DeviceContext_UpdateSubresource(s_ctx, (ID3D11Resource *)victim->tex, 0,
                                              NULL, up, uw * 4, 0);
        ID3D11Device_CreateShaderResourceView(s_dev, (ID3D11Resource *)victim->tex,
                                              NULL, &victim->srv);
        if (mips)
            ID3D11DeviceContext_GenerateMips(s_ctx, victim->srv);
    }
    victim->key = key; victim->w = t->width; victim->h = t->height;
    victim->tw = uw; victim->th = uh;
    victim->color = t->color; victim->pitch = t->pitch;
    victim->levels = mips ? 2 : 1;
    victim->sum = sum;
    victim->hud = hud;
    victim->used = ++s_tick;
    return victim->srv;
}

/* ---- state objects, cached by the raw values ------------------------- */

static D3D11_BLEND blend_factor(uint32_t gl, int alpha)
{
    switch (gl) {
    case 0x0000: return D3D11_BLEND_ZERO;
    case 0x0001: return D3D11_BLEND_ONE;
    case 0x0300: return alpha ? D3D11_BLEND_SRC_ALPHA : D3D11_BLEND_SRC_COLOR;
    case 0x0301: return alpha ? D3D11_BLEND_INV_SRC_ALPHA : D3D11_BLEND_INV_SRC_COLOR;
    case 0x0302: return D3D11_BLEND_SRC_ALPHA;
    case 0x0303: return D3D11_BLEND_INV_SRC_ALPHA;
    case 0x0304: return D3D11_BLEND_DEST_ALPHA;
    case 0x0305: return D3D11_BLEND_INV_DEST_ALPHA;
    case 0x0306: return alpha ? D3D11_BLEND_DEST_ALPHA : D3D11_BLEND_DEST_COLOR;
    case 0x0307: return alpha ? D3D11_BLEND_INV_DEST_ALPHA : D3D11_BLEND_INV_DEST_COLOR;
    case 0x0308: return D3D11_BLEND_SRC_ALPHA_SAT;
    case 0x8001: case 0x8003: return D3D11_BLEND_BLEND_FACTOR;
    case 0x8002: case 0x8004: return D3D11_BLEND_INV_BLEND_FACTOR;
    default:     return D3D11_BLEND_ONE;
    }
}

static D3D11_BLEND_OP blend_op(uint32_t gl)
{
    switch (gl) {
    case 0x800A: return D3D11_BLEND_OP_SUBTRACT;
    case 0x800B: return D3D11_BLEND_OP_REV_SUBTRACT;
    case 0x8007: return D3D11_BLEND_OP_MIN;
    case 0x8008: return D3D11_BLEND_OP_MAX;
    default:     return D3D11_BLEND_OP_ADD;
    }
}

static UINT8 write_mask(uint32_t m)
{
    return (UINT8)(((m & 0x00010000u) ? D3D11_COLOR_WRITE_ENABLE_RED : 0)
                 | ((m & 0x00000100u) ? D3D11_COLOR_WRITE_ENABLE_GREEN : 0)
                 | ((m & 0x00000001u) ? D3D11_COLOR_WRITE_ENABLE_BLUE : 0)
                 | ((m & 0x01000000u) ? D3D11_COLOR_WRITE_ENABLE_ALPHA : 0));
}

typedef struct { uint32_t k[5]; ID3D11BlendState *s; } BlendEntry;
static BlendEntry s_blends[64];
static int s_nblends;

static ID3D11BlendState *blend_state(const Nv2aRenderState *rs)
{
    uint32_t k[5] = { rs->blend_enable, rs->blend_src, rs->blend_dst,
                      rs->blend_eq, rs->color_mask };
    D3D11_BLEND_DESC bd;
    D3D11_RENDER_TARGET_BLEND_DESC *t = &bd.RenderTarget[0];
    int i;

    for (i = 0; i < s_nblends; i++)
        if (!memcmp(s_blends[i].k, k, sizeof k))
            return s_blends[i].s;
    memset(&bd, 0, sizeof bd);
    t->BlendEnable = rs->blend_enable != 0;
    t->SrcBlend = blend_factor(rs->blend_src, 0);
    t->DestBlend = blend_factor(rs->blend_dst, 0);
    t->BlendOp = blend_op(rs->blend_eq);
    t->SrcBlendAlpha = blend_factor(rs->blend_src, 1);
    t->DestBlendAlpha = blend_factor(rs->blend_dst, 1);
    t->BlendOpAlpha = t->BlendOp;
    if (t->BlendOp == D3D11_BLEND_OP_MIN || t->BlendOp == D3D11_BLEND_OP_MAX) {
        t->SrcBlend = t->DestBlend = D3D11_BLEND_ONE;
        t->SrcBlendAlpha = t->DestBlendAlpha = D3D11_BLEND_ONE;
    }
    t->RenderTargetWriteMask = write_mask(rs->color_mask);
    if (s_nblends == 64)
        s_nblends = 0;          /* a full cache restarts; states are cheap */
    if (s_blends[s_nblends].s)
        ID3D11BlendState_Release(s_blends[s_nblends].s);
    memcpy(s_blends[s_nblends].k, k, sizeof k);
    ID3D11Device_CreateBlendState(s_dev, &bd, &s_blends[s_nblends].s);
    return s_blends[s_nblends++].s;
}

static D3D11_COMPARISON_FUNC cmp_func(uint32_t gl)
{
    /* GL_NEVER..GL_ALWAYS and D3D11 NEVER..ALWAYS are in the same order. */
    return gl >= 0x200 && gl <= 0x207
         ? (D3D11_COMPARISON_FUNC)(gl - 0x200 + 1) : D3D11_COMPARISON_ALWAYS;
}

static D3D11_STENCIL_OP stencil_op(uint32_t gl)
{
    switch (gl) {
    case 0x0000: return D3D11_STENCIL_OP_ZERO;
    case 0x1E01: return D3D11_STENCIL_OP_REPLACE;
    case 0x1E02: return D3D11_STENCIL_OP_INCR_SAT;
    case 0x1E03: return D3D11_STENCIL_OP_DECR_SAT;
    case 0x150A: return D3D11_STENCIL_OP_INVERT;
    case 0x8507: return D3D11_STENCIL_OP_INCR;
    case 0x8508: return D3D11_STENCIL_OP_DECR;
    default:     return D3D11_STENCIL_OP_KEEP;
    }
}

typedef struct { uint32_t k[10]; ID3D11DepthStencilState *s; } DepthEntry;
static DepthEntry s_depths[64];
static int s_ndepths;

static ID3D11DepthStencilState *depth_state(const Nv2aRenderState *rs)
{
    uint32_t k[10] = { rs->depth_test_enable, rs->depth_func, rs->depth_write,
                       rs->stencil_enable, rs->stencil_func, rs->stencil_func_mask,
                       rs->stencil_write_mask, rs->stencil_fail, rs->stencil_zfail,
                       rs->stencil_zpass };
    D3D11_DEPTH_STENCIL_DESC d;
    int i;

    for (i = 0; i < s_ndepths; i++)
        if (!memcmp(s_depths[i].k, k, sizeof k))
            return s_depths[i].s;
    memset(&d, 0, sizeof d);
    d.DepthEnable = rs->depth_test_enable != 0;
    d.DepthWriteMask = rs->depth_write ? D3D11_DEPTH_WRITE_MASK_ALL
                                       : D3D11_DEPTH_WRITE_MASK_ZERO;
    d.DepthFunc = cmp_func(rs->depth_func);
    d.StencilEnable = rs->stencil_enable != 0;
    d.StencilReadMask = (UINT8)rs->stencil_func_mask;
    d.StencilWriteMask = (UINT8)rs->stencil_write_mask;
    d.FrontFace.StencilFunc = cmp_func(rs->stencil_func);
    d.FrontFace.StencilFailOp = stencil_op(rs->stencil_fail);
    d.FrontFace.StencilDepthFailOp = stencil_op(rs->stencil_zfail);
    d.FrontFace.StencilPassOp = stencil_op(rs->stencil_zpass);
    d.BackFace = d.FrontFace;
    if (s_ndepths == 64)
        s_ndepths = 0;
    if (s_depths[s_ndepths].s)
        ID3D11DepthStencilState_Release(s_depths[s_ndepths].s);
    memcpy(s_depths[s_ndepths].k, k, sizeof k);
    ID3D11Device_CreateDepthStencilState(s_dev, &d, &s_depths[s_ndepths].s);
    return s_depths[s_ndepths++].s;
}

/* Face culling (SET_CULL_FACE_ENABLE / SET_CULL_FACE / SET_FRONT_FACE).
 * Nothing culled before: a closed or two-layered model drew its back faces
 * too, which fight its front faces in depth (flicker) and show through
 * blended surfaces. Vertices reach the VS in surface pixels, y down, and the
 * NDC flip keeps their on-screen winding, so the title's front-face winding
 * maps straight to FrontCounterClockwise. RECOMP_GPU_CULL=0 turns it off;
 * RECOMP_GPU_CULL=flip inverts the winding (if everything vanishes).
 * Returns 0 when the state culls both faces. */
static int set_cull(const Nv2aRenderState *rs)
{
    static int mode = -1;               /* 0 off, 1 on, 2 flipped */
    ID3D11RasterizerState *(*st)[2] = s_cull_st;
    int which = 0, ccw = 0, idx;

    if (mode < 0) {
        const char *e = getenv("RECOMP_GPU_CULL");
        mode = !e || !*e ? 1 : (*e == '0' ? 0 : (*e == 'f' ? 2 : 1));
        fprintf(stderr, "  [GPU-BE] face culling %s\n",
                mode == 0 ? "off" : mode == 2 ? "on (flipped winding)" : "on");
    }
    if (mode && rs->cull_enable) {
        if (rs->cull_face == 0x0408)    /* GL_FRONT_AND_BACK */
            return 0;
        which = rs->cull_face == 0x0404 ? 1 : 2;    /* GL_FRONT : GL_BACK */
        ccw = rs->front_face == 0x0901;             /* GL_CCW */
        if (mode == 2)
            ccw = !ccw;
    }
    idx = which * 2 + ccw;
    if (idx == s_cull_cur)
        return 1;
    if (!st[which][ccw]) {
        D3D11_RASTERIZER_DESC rd;
        memset(&rd, 0, sizeof rd);
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = which == 0 ? D3D11_CULL_NONE
                    : which == 1 ? D3D11_CULL_FRONT : D3D11_CULL_BACK;
        rd.FrontCounterClockwise = ccw;
        rd.DepthClipEnable = FALSE;
        if (FAILED(ID3D11Device_CreateRasterizerState(s_dev, &rd, &st[which][ccw])))
            return 1;
    }
    ID3D11DeviceContext_RSSetState(s_ctx, st[which][ccw]);
    s_cull_cur = idx;
    return 1;
}

typedef struct { uint32_t k[3]; ID3D11SamplerState *s; } SampEntry;
static SampEntry s_samps[32];
static int s_nsamps;

static D3D11_TEXTURE_ADDRESS_MODE address(uint32_t m)
{
    switch (m) {
    case 2:  return D3D11_TEXTURE_ADDRESS_MIRROR;
    case 3:  return D3D11_TEXTURE_ADDRESS_CLAMP;
    case 4:  return D3D11_TEXTURE_ADDRESS_BORDER;
    case 5:  return D3D11_TEXTURE_ADDRESS_CLAMP;
    default: return D3D11_TEXTURE_ADDRESS_WRAP;
    }
}

static ID3D11SamplerState *sampler(const Nv2aTexture *t)
{
    /* MAG in filter bits 24-27: 1 is nearest. */
    uint32_t point = ((t->filter >> 24) & 0xF) == 1;
    uint32_t k[3] = { t->addr_u, t->addr_v, point };
    D3D11_SAMPLER_DESC sd;
    int i;

    for (i = 0; i < s_nsamps; i++)
        if (!memcmp(s_samps[i].k, k, sizeof k))
            return s_samps[i].s;
    memset(&sd, 0, sizeof sd);
    sd.Filter = point ? D3D11_FILTER_MIN_MAG_MIP_POINT : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = address(t->addr_u);
    sd.AddressV = address(t->addr_v);
    sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (s_nsamps == 32)
        s_nsamps = 0;
    if (s_samps[s_nsamps].s)
        ID3D11SamplerState_Release(s_samps[s_nsamps].s);
    memcpy(s_samps[s_nsamps].k, k, sizeof k);
    ID3D11Device_CreateSamplerState(s_dev, &sd, &s_samps[s_nsamps].s);
    return s_samps[s_nsamps++].s;
}

/* ---- the three callbacks --------------------------------------------- */

/* Window z to [0, 1]: Z24S8 surfaces use 24-bit z (SET_CLIP_MAX 16777215),
 * Z16 ones 16-bit. */
static float z_scale(const Nv2aRenderState *rs)
{
    return rs->depth_max > 65535.5f ? 1.0f / 16777215.0f : 1.0f / 65535.0f;
}

static void be_clear(const Nv2aSurface *s, const Nv2aRenderState *rs,
                     uint32_t flags, uint32_t argb, uint32_t zstencil)
{
    Surf *r;
    Zbuf *z;

    if (!init_device())
        return;
    r = bind_target(s, rs, &z);
    if (!r)
        return;
    if (flags & 0xF0u) {
        float c[4] = { ((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f,
                       (argb & 0xFF) / 255.0f, (argb >> 24) / 255.0f };
        ID3D11DeviceContext_ClearRenderTargetView(s_ctx, r->rtv, c);
        r->dirty = 1;
    }
    if (z && (flags & 0x3u)) {
        int z24 = rs->depth_max > 65535.5f;
        float depth = z24 ? (float)(zstencil >> 8) / 16777215.0f
                          : (float)(zstencil & 0xFFFF) / 65535.0f;
        UINT f = ((flags & 1u) ? D3D11_CLEAR_DEPTH : 0)
               | ((flags & 2u) ? D3D11_CLEAR_STENCIL : 0);
        ID3D11DeviceContext_ClearDepthStencilView(s_ctx, z->dsv, f, depth,
                                                  (UINT8)(zstencil & 0xFF));
    }
}

static void be_draw_inner(const Nv2aSurface *s, const Nv2aBatch *b);

/* Widescreen's 2D (RECOMP_HOR_PLUS, settings.c: the screen's width over
 * 4:3). The 3D is drawn that much wider (Hor+, recomp_manual.c) into the
 * 640x480 frame stretched to the screen; the HUD and menus, drawn in screen
 * pixels (every vertex rhw 1), come out that much wider. As on the PC
 * version (checked full-screen at 16:9) only the HUD is drawn its own shape
 * -- the head icon round -- and the menus fill the screen, stretched. The
 * HUD is known by its textures (texpack\hud.txt: head icons, health bar,
 * compass, height meter): a draw with one is narrowed by the factor, by
 * the edge it lies against if wholly in the outer quarter (so the health
 * bar and compass stay where they were), else round the centre. Telling
 * play from the menus by 3D on screen failed: menus have 3D too (the Load
 * screen's camera), and their text, narrowed glyph by glyph, tore apart.
 */
static int hud_widescreen(const Nv2aSurface *s, const Nv2aRenderState *rs, int hud,
                          const Nv2aVertex *v, Nv2aVertex *out, UINT n)
{
    static float f = -1.0f;
    float lo = 1e9f, hi = -1e9f, w = (float)s->width;
    UINT i;

    if (f < 0.0f) {
        const char *e = getenv("RECOMP_HOR_PLUS");
        f = e && *e ? (float)atof(e) : 0.0f;
        if (f <= 1.0f || f > 4.0f)
            f = 0.0f;
    }
    if (f == 0.0f || s->width != 640 || s->height != 480 || !n || !rs)
        return 0;
    for (i = 0; i < n; i++) {
        if (v[i].rhw != 1.0f)
            return 0;
        if (v[i].x < lo) lo = v[i].x;
        if (v[i].x > hi) hi = v[i].x;
    }
    {
        static int nlog = -1;
        if (nlog < 0)
            nlog = getenv("RECOMP_HUD_LOG") ? 300 : 0;
        if (nlog > 0 && hi - lo > 40.0f) {
            float ylo = 1e9f, yhi = -1e9f;
            for (i = 0; i < n; i++) {
                if (v[i].y < ylo) ylo = v[i].y;
                if (v[i].y > yhi) yhi = v[i].y;
            }
            fprintf(stderr, "[HUD2D] n %u x %.1f..%.1f y %.1f..%.1f blend %u src %X dst %X z %.3f tex %d\n",
                    n, lo, hi, ylo, yhi, rs->blend_enable, rs->blend_src, rs->blend_dst,
                    v[0].z, 0);
            nlog--;
        }
    }
    if (!hud)
        return 0;
    for (i = 0; i < n; i++) {
        out[i] = v[i];
        if (hi <= w * 0.25f)
            out[i].x = v[i].x / f;
        else if (lo >= w * 0.75f)
            out[i].x = w - (w - v[i].x) / f;
        else
            out[i].x = w * 0.5f + (v[i].x - w * 0.5f) / f;
    }
    return 1;
}

static void be_draw(const Nv2aSurface *s, const Nv2aBatch *b)
{
    LONGLONG t0 = ticks();
    be_draw_inner(s, b);
    s_draw_ticks += ticks() - t0;
}

static void be_draw_inner(const Nv2aSurface *s, const Nv2aBatch *b)
{
    const Nv2aRenderState *rs = b->state;
    D3D11_MAPPED_SUBRESOURCE m;
    UINT bytes = b->count * (UINT)sizeof(Nv2aVertex), stride = sizeof(Nv2aVertex);
    UINT offset;
    Surf *r;
    Zbuf *z;
    Consts c;
    ID3D11ShaderResourceView *srv = NULL;
    float bf[4];
    int rc, hud = 0;

    LONGLONG t = ticks();
#define SEG(i) do { LONGLONG n_ = ticks(); s_seg[i] += n_ - t; t = n_; } while (0)

    if (!b->count || !init_device())
        return;
    {
        LONGLONG a = ticks();
        Surf *probe = surface_for(s, 1);
        s_probe[0] += ticks() - a;
        a = ticks();
        if (probe && rs)
            zbuf_for(rs->zeta_va, probe->rw, probe->rh);
        s_probe[1] += ticks() - a;
        t = ticks();
    }
    r = bind_target(s, rs, &z);
    if (!r)
        return;
    SEG(0);

    memset(&c, 0, sizeof c);
    c.surf[0] = (float)r->w; c.surf[1] = (float)r->h;
    c.zscale = z_scale(rs);
    c.alpha_func = rs->alpha_test_enable ? (float)(rs->alpha_func - 0x200) : -1.0f;
    c.alpha_ref = (float)rs->alpha_ref;
    c.zmin = rs->depth_min; c.zmax = rs->depth_max;
    c.zclip = rs->depth_max > rs->depth_min ? 1.0f : 0.0f;
    rc = s_rc_ok && b->ex && b->combiner;
    if (rc) {
        /* All four stages; a stage without a usable texture reads white. */
        const Nv2aCombiner *k = b->combiner;
        ID3D11ShaderResourceView *srvs[4] = { NULL, NULL, NULL, NULL };
        ID3D11SamplerState *sss[4] = { NULL, NULL, NULL, NULL };
        RcConsts q;
        int st, i;

        memset(&q, 0, sizeof q);
        for (st = 0; st < 4; st++) {
            if (!b->stage[st])
                continue;
            srvs[st] = texture_srv(b->stage[st]);
            hud |= s_srv_hud;
            if (srvs[st] == r->srv)
                srvs[st] = NULL;
            if (srvs[st]) {
                sss[st] = sampler(b->stage[st]);
                q.misc[1] |= 1u << st;
            }
        }
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 4, srvs);
        ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 4, sss);
        for (i = 0; i < 8; i++) {
            q.ccw[i][0] = k->color_icw[i]; q.ccw[i][1] = k->alpha_icw[i];
            q.ccw[i][2] = k->color_ocw[i]; q.ccw[i][3] = k->alpha_ocw[i];
            nv2a_rc_unpack(k->factor0[i], q.fac0[i]);
            nv2a_rc_unpack(k->factor1[i], q.fac1[i]);
        }
        q.fin[0] = k->final0; q.fin[1] = k->final1;
        q.fin[2] = k->control; q.fin[3] = k->stage_program;
        nv2a_rc_unpack(k->final_c0, q.fc0);
        nv2a_rc_unpack(k->final_c1, q.fc1);
        nv2a_rc_unpack(b->fog_color, q.fogc);
        q.misc[0] = b->clip_plane_mode;
        if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb_rc, 0,
                                              D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memcpy(m.pData, &q, sizeof q);
            ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb_rc, 0);
        }
        c.texsize[0] = c.texsize[1] = 1.0f;
    } else {
        if (b->texture) {
            srv = texture_srv(b->texture);
            hud = s_srv_hud;
            /* A texture that is the target being drawn: no sampling, no hazard. */
            if (srv == r->srv)
                srv = NULL;
            if (srv) {
                ID3D11SamplerState *ss = sampler(b->texture);
                c.textured = 1.0f;
                c.texsize[0] = (float)(b->texture->width ? b->texture->width : 1);
                c.texsize[1] = (float)(b->texture->height ? b->texture->height : 1);
                ID3D11DeviceContext_PSSetSamplers(s_ctx, 0, 1, &ss);
            }
        }
        if (!c.textured) { c.texsize[0] = c.texsize[1] = 1.0f; }
        ID3D11DeviceContext_PSSetShaderResources(s_ctx, 0, 1, &srv);
    }
    /* Switch pipelines only when the kind of batch changes. */
    if (rc != s_pipe) {
        if (rc) {
            ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout_rc);
            ID3D11DeviceContext_VSSetShader(s_ctx, s_vs_rc, NULL, 0);
            ID3D11DeviceContext_PSSetShader(s_ctx, s_ps_rc, NULL, 0);
            ID3D11DeviceContext_PSSetConstantBuffers(s_ctx, 1, 1, &s_cb_rc);
        } else {
            ID3D11DeviceContext_IASetInputLayout(s_ctx, s_layout);
            ID3D11DeviceContext_VSSetShader(s_ctx, s_vs, NULL, 0);
            ID3D11DeviceContext_PSSetShader(s_ctx, s_ps, NULL, 0);
        }
        s_pipe = rc;
    }
    SEG(1);

    if (SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_cb, 0,
                                          D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        memcpy(m.pData, &c, sizeof c);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_cb, 0);
    }
    SEG(2);

    bf[0] = ((rs->blend_color >> 16) & 0xFF) / 255.0f;
    bf[1] = ((rs->blend_color >> 8) & 0xFF) / 255.0f;
    bf[2] = (rs->blend_color & 0xFF) / 255.0f;
    bf[3] = (rs->blend_color >> 24) / 255.0f;
    ID3D11DeviceContext_OMSetBlendState(s_ctx, blend_state(rs), bf, 0xFFFFFFFFu);
    ID3D11DeviceContext_OMSetDepthStencilState(s_ctx, z ? depth_state(rs) : NULL,
                                               rs->stencil_ref & 0xFF);
    if (!set_cull(rs))
        return;                 /* GL_FRONT_AND_BACK: nothing is drawn */
    SEG(3);

    /* Vertices into a ring: no-overwrite until it wraps, then discard. */
    if (bytes > VB_BYTES)
        bytes = VB_BYTES - VB_BYTES % (3 * stride);
    if (s_vb_used + bytes > VB_BYTES)
        s_vb_used = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vb, 0,
                                       s_vb_used ? D3D11_MAP_WRITE_NO_OVERWRITE
                                                 : D3D11_MAP_WRITE_DISCARD, 0, &m)))
        return;
    if (!hud_widescreen(s, rs, hud, b->vertices, (Nv2aVertex *)((uint8_t *)m.pData + s_vb_used),
                        bytes / stride))
        memcpy((uint8_t *)m.pData + s_vb_used, b->vertices, bytes);
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vb, 0);
    if (rc) {
        /* The combiner inputs of the same vertices, in a ring of their own. */
        UINT xstride = sizeof(Nv2aVertexEx), xbytes = bytes / stride * xstride, xoff;
        if (s_vbx_used + xbytes > VBX_BYTES)
            s_vbx_used = 0;
        if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)s_vbx, 0,
                                           s_vbx_used ? D3D11_MAP_WRITE_NO_OVERWRITE
                                                      : D3D11_MAP_WRITE_DISCARD, 0, &m)))
            return;
        memcpy((uint8_t *)m.pData + s_vbx_used, b->ex, xbytes);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)s_vbx, 0);
        xoff = s_vbx_used;
        s_vbx_used += xbytes;
        ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 1, 1, &s_vbx, &xstride, &xoff);
    }
    SEG(4);
    offset = s_vb_used;
    s_vb_used += bytes;
    ID3D11DeviceContext_IASetVertexBuffers(s_ctx, 0, 1, &s_vb, &stride, &offset);
    ID3D11DeviceContext_Draw(s_ctx, bytes / stride, 0);
    SEG(5);
#undef SEG

    r->dirty = 1;
    s_n_draws++;
    s_n_tris += bytes / stride / 3;
}

/* Copy a render target back into its guest surface, in two halves a flip
 * apart: queue_readback asks the GPU for a copy, deliver_readback writes the
 * copy queued at the previous flip into guest memory. Mapping the copy at the
 * flip that queued it made this thread wait for the GPU to finish the frame,
 * 8-10% of it at ~55 fps; a flip later the copy is long done. The cost is a
 * frame more latency in what the window shows. */
static void queue_readback(Surf *r)
{
    if (r->guest) {
        /* Scaled: shrink to guest size on the GPU, and read that. */
        blit(r->srv, r->guest_rtv, r->w, r->h);
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging,
                                         (ID3D11Resource *)r->guest);
    } else {
        ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)r->staging,
                                         (ID3D11Resource *)r->tex);
    }
    r->pending = 1;
}

/* The full-size picture to the window: the target the flip showed, copied at
 * this flip and handed over at the next, like the readbacks above. */
static void present_scaled(void)
{
    static ID3D11Texture2D *stage;
    static ID3D11Texture2D *out;            /* RECOMP_OUTPUT_SIZE, when set */
    static ID3D11RenderTargetView *out_rtv;
    static uint32_t sw, sh;
    static int pending;
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t key = ADDR_KEY(xbox_FramebufferWindowPresentedVa());
    int i;

    if (pending && SUCCEEDED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)stage, 0,
                                                     D3D11_MAP_READ, 0, &m))) {
        /* The costume panel draws over the paused game (costume_panel.c). */
        extern void costume_panel_draw(const void **pixels, uint32_t w, uint32_t h,
                                       uint32_t *pitch);
        const void *px = m.pData;
        uint32_t pitch = m.RowPitch;
        costume_panel_draw(&px, sw, sh, &pitch);
        xbox_FramebufferWindowPresentHiRes(px, sw, sh, pitch);
        ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)stage, 0);
    }
    pending = 0;
    if (!key)
        return;
    for (i = 0; i < MAX_SURF; i++) {
        Surf *r = &s_surf[i];
        uint32_t ow, oh;
        if (!r->tex || r->key != key || !r->guest)
            continue;
        /* The output size scales with the target as the render size does. */
        ow = s_ow ? (uint32_t)((uint64_t)r->rw * s_ow / (uint32_t)(GUEST_W * s_sx + 0.5f)) : r->rw;
        oh = s_oh ? (uint32_t)((uint64_t)r->rh * s_oh / (uint32_t)(GUEST_H * s_sy + 0.5f)) : r->rh;
        if (!stage || sw != ow || sh != oh) {
            D3D11_TEXTURE2D_DESC td;
            if (stage) ID3D11Texture2D_Release(stage);
            if (out_rtv) ID3D11RenderTargetView_Release(out_rtv);
            if (out) ID3D11Texture2D_Release(out);
            stage = NULL; out = NULL; out_rtv = NULL;
            memset(&td, 0, sizeof td);
            td.Width = ow; td.Height = oh; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
            if (ow != r->rw || oh != r->rh) {
                td.Usage = D3D11_USAGE_DEFAULT;
                td.BindFlags = D3D11_BIND_RENDER_TARGET;
                if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &out))
                    || FAILED(ID3D11Device_CreateRenderTargetView(s_dev, (ID3D11Resource *)out,
                                                                  NULL, &out_rtv)))
                    return;
                td.BindFlags = 0;
            }
            td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            if (FAILED(ID3D11Device_CreateTexture2D(s_dev, &td, NULL, &stage)))
                return;
            sw = ow; sh = oh;
        }
        if (out) {
            blit(r->srv, out_rtv, ow, oh);
            ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stage,
                                             (ID3D11Resource *)out);
        } else {
            ID3D11DeviceContext_CopyResource(s_ctx, (ID3D11Resource *)stage,
                                             (ID3D11Resource *)r->tex);
        }
        pending = 1;
        return;
    }
}

static void deliver_readback(Surf *r)
{
    D3D11_MAPPED_SUBRESOURCE m;
    uint32_t y, x;

    r->pending = 0;
    if (FAILED(ID3D11DeviceContext_Map(s_ctx, (ID3D11Resource *)r->staging, 0,
                                       D3D11_MAP_READ, 0, &m)))
        return;
    for (y = 0; y < r->h; y++) {
        const uint8_t *src = (const uint8_t *)m.pData + (size_t)y * m.RowPitch;
        uint8_t *dst = GUEST(r->va) + (size_t)y * r->pitch;
        if (r->bpp == 4) {
            memcpy(dst, src, (size_t)r->w * 4);
        } else if (r->bpp == 2) {      /* R5G6B5 */
            for (x = 0; x < r->w; x++) {
                const uint8_t *p = src + x * 4;
                ((uint16_t *)dst)[x] = (uint16_t)(((p[2] >> 3) << 11)
                                                | ((p[1] >> 2) << 5) | (p[0] >> 3));
            }
        }
    }
    ID3D11DeviceContext_Unmap(s_ctx, (ID3D11Resource *)r->staging, 0);
}

/* Every flip since start, for main.c's hang watch (s_n_flips is per stats
 * window). */
static volatile LONG s_total_flips;

unsigned long gpu_backend_flip_count(void)
{
    return (unsigned long)s_total_flips;
}

static void be_flip(void)
{
    static DWORD last;
    DWORD now;
    int i;

    InterlockedIncrement(&s_total_flips);

    LONGLONG t0 = ticks();

    if (!s_dev)
        return;
    if (!s_qpf.QuadPart)
        QueryPerformanceFrequency(&s_qpf);
    for (i = 0; i < MAX_SURF; i++) {
        Surf *r = &s_surf[i];
        if (!r->tex)
            continue;
        if (r->pending)
            deliver_readback(r);       /* the copy queued a flip ago */
        if (r->dirty) {
            queue_readback(r);
            r->dirty = 0;
        }
    }
    if (s_scaled)
        present_scaled();
    s_flip_ticks += ticks() - t0;
    /* Flip-to-flip intervals, bucketed: <18 ms (one vblank), 18-26, 26-36
     * (two vblanks), longer. An average fps hides which it is. */
    {
        static LONGLONG prev;
        if (prev) {
            double ms = 1000.0 * (t0 - prev) / s_qpf.QuadPart;
            s_ivl[ms < 18.0 ? 0 : ms < 26.0 ? 1 : ms < 36.0 ? 2 : 3]++;
        }
        prev = t0;
    }
    s_n_flips++;
    now = GetTickCount();
    if (!last)
        last = now;
    if (now - last >= 5000) {
        double span = (now - last) / 1000.0;
        fprintf(stderr, "  [GPU-BE] %.1f flips/s, %lu draws, %lu tris, %lu texture uploads"
                        " in %.1f s; draw() %.0f%% (%.0f us each), readback %.0f%%\n",
                s_n_flips / span, s_n_draws, s_n_tris, s_n_uploads, span,
                100.0 * s_draw_ticks / s_qpf.QuadPart / span,
                s_n_draws ? 1e6 * s_draw_ticks / s_qpf.QuadPart / s_n_draws : 0.0,
                100.0 * s_flip_ticks / s_qpf.QuadPart / span);
        if (s_n_draws) {
            double us = 1e6 / s_qpf.QuadPart / s_n_draws;
            fprintf(stderr, "  [GPU-BE]   per draw us: target %.0f texture %.0f consts %.0f"
                            " state %.0f verts %.0f draw %.0f; %lu targets created\n",
                    s_seg[0] * us, s_seg[1] * us, s_seg[2] * us, s_seg[3] * us,
                    s_seg[4] * us, s_seg[5] * us, s_n_targets);
            fprintf(stderr, "  [GPU-BE]   lookups us: surface %.1f zbuf %.1f; %lu binds\n",
                    s_probe[0] * us, s_probe[1] * us, s_n_binds);
            s_probe[0] = s_probe[1] = 0; s_n_binds = 0;
            s_n_targets = 0;
        }
        fflush(stderr);
        fprintf(stderr, "  [GPU-BE]   flip intervals: <18ms %lu, 18-26 %lu, 26-36 %lu,"
                        " longer %lu\n", s_ivl[0], s_ivl[1], s_ivl[2], s_ivl[3]);
        memset(s_ivl, 0, sizeof s_ivl);
        s_n_flips = s_n_draws = s_n_tris = s_n_uploads = 0;
        s_draw_ticks = s_flip_ticks = 0;
        memset(s_seg, 0, sizeof s_seg);
        last = now;
    }
}

static const Nv2aBackend s_backend = { be_clear, be_draw, be_flip };

void gpu_backend_register(void)
{
    nv2a_backend_register(&s_backend);
    fprintf(stderr, "[BOOT] GPU render back end registered (RECOMP_GPU_RENDER)\n");
}
