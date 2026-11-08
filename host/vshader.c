/* Direct3D 8 vertex shaders (vs.1.1) and vertex declarations, run on the CPU.
 *
 * The renderer already converts every vertex on the CPU, so programmable vertex processing is done
 * the same way: the declaration says how stream data maps onto input registers v0-v15, the shader
 * runs per vertex, and its outputs (oPos in clip space, oD0/oD1, oT0-oT7) feed the fixed-function
 * pixel stage. Each shader is decoded once into a compact instruction list. */
#include "vshader.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>

enum { RT_TEMP = 0, RT_INPUT = 1, RT_CONST = 2, RT_ADDR = 3, RT_RASTOUT = 4, RT_ATTROUT = 5, RT_TEXOUT = 6 };

typedef struct { uint8_t type, rel, neg, swz[4]; uint16_t num; } Src;
typedef struct { uint16_t op; uint8_t dtype, dmask; uint16_t dnum; uint8_t nsrc; Src s[3]; } Insn;

typedef struct {
    Insn *code; uint32_t n;
    /* declaration: input register sources */
    struct { uint8_t used, stream, type; uint16_t offset; } in[16];
    float defs[96][4]; uint8_t has_def[96];        /* constants from the declaration and `def` */
    int fixed_function;                            /* declaration without a program */
    int ok;
} VsProgram;

/* operand count (destination + sources) per vs.1.1 opcode */
static int nparams(uint32_t op) {
    switch (op) {
    case 0: return 0;
    case 1: case 6: case 7: case 14: case 15: case 16: case 19: case 78: case 79: return 2;
    case 2: case 3: case 5: case 8: case 9: case 10: case 11: case 12: case 13: case 17:
    case 20: case 21: case 22: case 23: case 24: return 3;
    case 4: case 18: return 4;
    default: return -1;
    }
}

static void parse_decl(VsProgram *p, const uint32_t *t, uint32_t n) {
    uint32_t stream = 0, offset = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t tok = t[i];
        if (tok == 0xFFFFFFFFu) break;
        switch (tok >> 29) {
        case 1: stream = tok & 0xF; offset = 0; break;               /* D3DVSD_STREAM */
        case 2:
            if (tok & 0x10000000u) offset += 4 * ((tok >> 16) & 0xF);   /* D3DVSD_SKIP */
            else {
                uint32_t reg = tok & 0x1F, type = (tok >> 16) & 0xF;
                if (reg < 16) { p->in[reg].used = 1; p->in[reg].stream = (uint8_t)stream; p->in[reg].type = (uint8_t)type;
                                p->in[reg].offset = (uint16_t)offset; }
                static const uint8_t sz[8] = { 4, 8, 12, 16, 4, 4, 4, 8 };
                offset += type < 8 ? sz[type] : 4;
            }
            break;
        case 4: {                                                    /* D3DVSD_CONST */
            uint32_t cnt = (tok >> 25) & 0xF, addr = tok & 0x7F;
            for (uint32_t k = 0; k < cnt && i + 4 <= n; k++, addr++) {
                if (addr < 96) { memcpy(p->defs[addr], &t[i + 1], 16); p->has_def[addr] = 1; }
                i += 4;
            }
            break;
        }
        case 5: i += (tok >> 24) & 0x1F; break;                     /* D3DVSD_EXT */
        default: break;
        }
    }
}

static Src decode_src(uint32_t t) {
    Src s;
    s.type = (uint8_t)((t >> 28) & 7);
    s.num = (uint16_t)(t & 0x1FFF);
    s.rel = (t >> 13) & 1;
    s.neg = ((t >> 24) & 0xF) == 1;
    for (int k = 0; k < 4; k++) s.swz[k] = (uint8_t)((t >> (16 + 2 * k)) & 3);
    return s;
}

static int parse_code(VsProgram *p, const uint32_t *t, uint32_t n) {
    if (!n || (t[0] & 0xFFFF0000u) != 0xFFFE0000u) return 0;
    p->code = calloc(n, sizeof(Insn));
    for (uint32_t i = 1; i < n;) {
        uint32_t tok = t[i++], op = tok & 0xFFFF;
        if (tok == 0x0000FFFFu) break;
        if (op == 0xFFFE) { i += (tok >> 16) & 0x7FFF; continue; }   /* comment */
        if (op == 81) {                                             /* def c#, f, f, f, f */
            uint32_t r = t[i] & 0x1FFF;
            if (r < 96) { memcpy(p->defs[r], &t[i + 1], 16); p->has_def[r] = 1; }
            i += 5;
            continue;
        }
        int np = nparams(op);
        if (np < 0) { fprintf(stderr, "[vs] unsupported opcode %u\n", op); return 0; }
        if (np == 0) continue;
        Insn *in = &p->code[p->n++];
        in->op = (uint16_t)op;
        uint32_t d = t[i++];
        in->dtype = (uint8_t)((d >> 28) & 7);
        in->dnum = (uint16_t)(d & 0x1FFF);
        in->dmask = (uint8_t)((d >> 16) & 0xF);
        in->nsrc = (uint8_t)(np - 1);
        for (int k = 0; k < np - 1; k++) {
            uint32_t s = t[i++];
            in->s[k] = decode_src(s);
            if (in->s[k].rel && (t[0] & 0xFFFF) >= 0x0200) i++;     /* vs.2.0 relative token (not expected) */
        }
    }
    return 1;
}

static VsProgram *program_for(Shader *sh) {
    if (sh->backend) return sh->backend;
    VsProgram *p = calloc(1, sizeof *p);
    parse_decl(p, sh->decl, sh->decl_len);
    if (!sh->code) { p->fixed_function = 1; p->ok = 1; }
    else p->ok = parse_code(p, sh->code, sh->code_len);
    if (!p->ok) fprintf(stderr, "[vs] could not decode shader (version %08x)\n", sh->code ? sh->code[0] : 0);
    if (getenv("RC_DUMP_VS")) {   /* raw tokens, for offline disassembly */
        fprintf(stderr, "[vsdump %p] decl", (void *)sh);
        for (uint32_t i = 0; i < sh->decl_len; i++) fprintf(stderr, " %08x", sh->decl[i]);
        fprintf(stderr, "\n[vsdump %p] code", (void *)sh);
        for (uint32_t i = 0; i < sh->code_len; i++) fprintf(stderr, " %08x", sh->code[i]);
        fprintf(stderr, "\n");
    }
    sh->backend = p;
    return p;
}

int vs_is_fixed_function(Shader *sh) { return program_for(sh)->fixed_function; }

/* ------------------------------------------------------------------ input fetch */

static void fetch(const uint8_t *src, uint32_t type, float *o) {
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    switch (type) {
    case 0: memcpy(o, src, 4); break;
    case 1: memcpy(o, src, 8); break;
    case 2: memcpy(o, src, 12); break;
    case 3: memcpy(o, src, 16); break;
    case 4: o[0] = src[2] / 255.f; o[1] = src[1] / 255.f; o[2] = src[0] / 255.f; o[3] = src[3] / 255.f; break;  /* D3DCOLOR */
    case 5: for (int k = 0; k < 4; k++) o[k] = src[k]; break;                                                 /* UBYTE4 */
    case 6: { int16_t s[2]; memcpy(s, src, 4); o[0] = s[0]; o[1] = s[1]; break; }                           /* SHORT2 */
    case 7: { int16_t s[4]; memcpy(s, src, 8); for (int k = 0; k < 4; k++) o[k] = s[k]; break; }            /* SHORT4 */
    }
}

/* ------------------------------------------------------------------ execution */

static inline void read_src(const Src *s, float r[][4], const float v[16][4], const float c[96][4], int a0,
                            float *o) {
    const float *base;
    static const float zero[4];
    switch (s->type) {
    case RT_TEMP: base = s->num < 12 ? r[s->num] : zero; break;
    case RT_INPUT: base = s->num < 16 ? v[s->num] : zero; break;
    case RT_CONST: { int i = s->num + (s->rel ? a0 : 0); base = i >= 0 && i < 96 ? c[i] : zero; break; }
    default: base = zero; break;
    }
    for (int k = 0; k < 4; k++) o[k] = s->neg ? -base[s->swz[k]] : base[s->swz[k]];
}

static inline float dp3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline float dp4(const float *a, const float *b) { return dp3(a, b) + a[3] * b[3]; }

int vs_trace;   /* set by the renderer to trace one vertex */

void vs_run(Shader *sh, const float consts[96][4], const VsStreams *st, uint32_t index, VsOut *out) {
    VsProgram *p = program_for(sh);
    float v[16][4];
    for (int i = 0; i < 16; i++) {
        if (!p->in[i].used || !st->data[p->in[i].stream]) { v[i][0] = v[i][1] = v[i][2] = 0; v[i][3] = 1; continue; }
        const uint8_t *src = st->data[p->in[i].stream] + (size_t)index * st->stride[p->in[i].stream] + p->in[i].offset;
        fetch(src, p->in[i].type, v[i]);
    }
    if (p->fixed_function) {
        /* declaration-only shader: D3D's fixed register mapping (v0 position, v5/v6 colors, v7+ texcoords) */
        const float *m = st->ff_mvp;
        const float *x = v[0];
        for (int k = 0; k < 4; k++) out->pos[k] = x[0] * m[k] + x[1] * m[4 + k] + x[2] * m[8 + k] + m[12 + k];
        if (p->in[5].used) memcpy(out->d0, v[5], 16); else for (int k = 0; k < 4; k++) out->d0[k] = 1;
        if (p->in[6].used) memcpy(out->d1, v[6], 16); else memset(out->d1, 0, 16);
        for (int t = 0; t < 4; t++) memcpy(out->tex[t], v[7 + t], 16);
        return;
    }
    if (!p->ok) { memset(out, 0, sizeof *out); return; }

    /* constants: declaration/def values override nothing set later by SetVertexShaderConstant, but D3D
       applies declaration constants at SetVertexShader time; using them as a base is close enough */
    float r[12][4] = { { 0 } };
    float o_pos[4] = { 0, 0, 0, 1 }, o_d[2][4] = { { 1, 1, 1, 1 }, { 0, 0, 0, 0 } }, o_t[8][4];
    for (int t = 0; t < 8; t++) { o_t[t][0] = o_t[t][1] = o_t[t][2] = 0; o_t[t][3] = 1; }
    float dummy[4];
    int a0 = 0;
    const float (*c)[4] = consts;
    float cbuf[96][4];
    int any_def = 0;
    for (int i = 0; i < 96 && !any_def; i++) any_def = p->has_def[i];
    if (any_def) {
        memcpy(cbuf, consts, sizeof cbuf);
        for (int i = 0; i < 96; i++) if (p->has_def[i]) memcpy(cbuf[i], p->defs[i], 16);
        c = (const float (*)[4])cbuf;
    }

    for (uint32_t i = 0; i < p->n; i++) {
        const Insn *in = &p->code[i];
        float s0[4], s1[4], s2[4], res[4] = { 0, 0, 0, 0 };
        if (in->nsrc > 0) read_src(&in->s[0], r, (const float (*)[4])v, c, a0, s0);
        if (in->nsrc > 1) read_src(&in->s[1], r, (const float (*)[4])v, c, a0, s1);
        if (in->nsrc > 2) read_src(&in->s[2], r, (const float (*)[4])v, c, a0, s2);
        switch (in->op) {
        case 1: memcpy(res, s0, 16); break;                                          /* mov */
        case 2: for (int k = 0; k < 4; k++) res[k] = s0[k] + s1[k]; break;          /* add */
        case 3: for (int k = 0; k < 4; k++) res[k] = s0[k] - s1[k]; break;          /* sub */
        case 4: for (int k = 0; k < 4; k++) res[k] = s0[k] * s1[k] + s2[k]; break;  /* mad */
        case 5: for (int k = 0; k < 4; k++) res[k] = s0[k] * s1[k]; break;          /* mul */
        case 6: { float x = s0[3]; float y = x == 1.f ? 1.f : x == 0.f ? INFINITY : 1.f / x;   /* rcp */
                  for (int k = 0; k < 4; k++) res[k] = y; break; }
        case 7: { float x = fabsf(s0[3]); float y = x == 1.f ? 1.f : x == 0.f ? INFINITY : 1.f / sqrtf(x);  /* rsq */
                  for (int k = 0; k < 4; k++) res[k] = y; break; }
        case 8: { float d = dp3(s0, s1); for (int k = 0; k < 4; k++) res[k] = d; break; }
        case 9: { float d = dp4(s0, s1); for (int k = 0; k < 4; k++) res[k] = d; break; }
        case 10: for (int k = 0; k < 4; k++) res[k] = s0[k] < s1[k] ? s0[k] : s1[k]; break;
        case 11: for (int k = 0; k < 4; k++) res[k] = s0[k] >= s1[k] ? s0[k] : s1[k]; break;
        case 12: for (int k = 0; k < 4; k++) res[k] = s0[k] < s1[k] ? 1.f : 0.f; break;   /* slt */
        case 13: for (int k = 0; k < 4; k++) res[k] = s0[k] >= s1[k] ? 1.f : 0.f; break;  /* sge */
        case 14: { float y = exp2f(s0[3]); for (int k = 0; k < 4; k++) res[k] = y; break; }   /* exp */
        case 15: { float x = fabsf(s0[3]); float y = x == 0.f ? -INFINITY : log2f(x);         /* log */
                   for (int k = 0; k < 4; k++) res[k] = y; break; }
        case 16: {                                                                   /* lit */
            float pw = s0[3] < -128.f ? -128.f : s0[3] > 128.f ? 128.f : s0[3];
            res[0] = 1; res[3] = 1;
            res[1] = s0[0] > 0 ? s0[0] : 0;
            res[2] = s0[0] > 0 && s0[1] > 0 ? powf(s0[1], pw) : 0;
            break;
        }
        case 17: res[0] = 1; res[1] = s0[1] * s1[1]; res[2] = s0[2]; res[3] = s1[3]; break;   /* dst */
        case 18: for (int k = 0; k < 4; k++) res[k] = s0[k] * (s1[k] - s2[k]) + s2[k]; break; /* lrp */
        case 19: for (int k = 0; k < 4; k++) res[k] = s0[k] - floorf(s0[k]); break;           /* frc */
        case 20: case 21: case 22: case 23: case 24: {                               /* m4x4 m4x3 m3x4 m3x3 m3x2 */
            int rows = in->op == 20 || in->op == 22 ? 4 : in->op == 24 ? 2 : 3, four = in->op <= 21;
            Src row = in->s[1];
            for (int k = 0; k < rows; k++) {
                float m[4];
                read_src(&row, r, (const float (*)[4])v, c, a0, m);
                res[k] = four ? dp4(s0, m) : dp3(s0, m);
                row.num++;
            }
            break;
        }
        case 78: { float w = s0[3]; float f = floorf(w);                             /* expp */
                   res[0] = exp2f(f); res[1] = w - f; res[2] = exp2f(w); res[3] = 1; break; }
        case 79: { float x = fabsf(s0[3]);                                           /* logp */
                   if (x == 0.f) { res[0] = res[2] = -INFINITY; res[1] = 1; }
                   else { int e; float m = frexpf(x, &e); res[0] = (float)(e - 1); res[1] = m * 2; res[2] = log2f(x); }
                   res[3] = 1; break; }
        default: break;
        }
        float *dst;
        switch (in->dtype) {
        case RT_TEMP: dst = in->dnum < 12 ? r[in->dnum] : dummy; break;
        case RT_ADDR: if (in->dmask & 1) a0 = (int)floorf(res[0]); continue;
        case RT_RASTOUT: dst = in->dnum == 0 ? o_pos : dummy; break;
        case RT_ATTROUT: dst = in->dnum < 2 ? o_d[in->dnum] : dummy; break;
        case RT_TEXOUT: dst = in->dnum < 8 ? o_t[in->dnum] : dummy; break;
        default: dst = dummy; break;
        }
        for (int k = 0; k < 4; k++) if (in->dmask & (1 << k)) dst[k] = res[k];
    }
    if (vs_trace) {
        fprintf(stderr, "[vstrace %p] idx %u\n", (void *)sh, index);
        for (int k = 0; k < 16; k++) if (p->in[k].used)
            fprintf(stderr, "  v%d = %g %g %g %g\n", k, v[k][0], v[k][1], v[k][2], v[k][3]);
        for (int k = 0; k < 12; k++) fprintf(stderr, "  r%d = %g %g %g %g\n", k, r[k][0], r[k][1], r[k][2], r[k][3]);
        fprintf(stderr, "  oPos = %g %g %g %g  oD0 = %g %g %g %g  oD1 = %g %g %g %g  oT0 = %g %g\n", o_pos[0], o_pos[1],
                o_pos[2], o_pos[3], o_d[0][0], o_d[0][1], o_d[0][2], o_d[0][3], o_d[1][0], o_d[1][1], o_d[1][2],
                o_d[1][3], o_t[0][0], o_t[0][1]);
        for (int k = 0; k < 96; k++)
            fprintf(stderr, "  c%d = %g %g %g %g\n", k, c[k][0], c[k][1], c[k][2], c[k][3]);
    }
    memcpy(out->pos, o_pos, 16);
    memcpy(out->d0, o_d[0], 16);
    memcpy(out->d1, o_d[1], 16);
    for (int t = 0; t < 4; t++) memcpy(out->tex[t], o_t[t], 16);
}

/* ------------------------------------------------------------------ GPU translation
 *
 * The same decoded program, translated to a WGSL vertex function (`vs_gpu`) so the GPU runs it instead
 * of vs_run(). Inputs come straight from the game's vertex streams (vertex attributes at location =
 * input register), constants from a uniform array (`vc`, declaration/def constants merged in by the
 * caller, see vs_merge_defs), and the outputs feed the same VO struct as the fixed-function vertex stage.
 * Semantics follow vs_run() so both paths draw the same picture. */

int vs_gpu_inputs(Shader *sh, VsGpuInput in[16]) {
    VsProgram *p = program_for(sh);
    if (!p->ok) return -1;
    int n = 0;
    for (int r = 0; r < 16; r++) {
        if (!p->in[r].used) continue;
        if (p->fixed_function && r != 0 && r != 5 && r != 6 && (r < 7 || r > 10)) continue;   /* unread */
        in[n].reg = (uint8_t)r; in[n].stream = p->in[r].stream; in[n].type = p->in[r].type; in[n].offset = p->in[r].offset;
        n++;
    }
    return n;
}

void vs_merge_defs(Shader *sh, float c[96][4]) {
    VsProgram *p = program_for(sh);
    for (int i = 0; i < 96; i++) if (p->has_def[i]) memcpy(c[i], p->defs[i], 16);
}

typedef struct { char *s; size_t n, cap; } Buf;
static void bput(Buf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void bput(Buf *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(b->s ? b->s + b->n : NULL, b->s ? b->cap - b->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) return;
        if (b->s && b->n + (size_t)k < b->cap) { b->n += (size_t)k; return; }
        b->cap = (b->cap + (size_t)k + 1) * 2;
        b->s = realloc(b->s, b->cap);
        if (b->n == 0) b->s[0] = 0;
    }
}

/* a source operand as a WGSL vec4 expression; `add` offsets the register number (matrix rows) */
static void src_expr(Buf *b, const Src *s, int add, const uint8_t used_in[16]) {
    char base[48];
    int num = s->num + add;
    switch (s->type) {
    case RT_TEMP: if (num < 12) snprintf(base, sizeof base, "r%d", num); else snprintf(base, sizeof base, "vec4<f32>(0.0)"); break;
    case RT_INPUT:
        if (num < 16 && used_in[num]) snprintf(base, sizeof base, "v%d", num);
        else snprintf(base, sizeof base, "vec4<f32>(0.0, 0.0, 0.0, 1.0)");
        break;
    case RT_CONST:
        if (s->rel) snprintf(base, sizeof base, "vc_at(a0 + %d)", num);
        else if (num < 96) snprintf(base, sizeof base, "vc[%d]", num);
        else snprintf(base, sizeof base, "vec4<f32>(0.0)");
        break;
    default: snprintf(base, sizeof base, "vec4<f32>(0.0)"); break;
    }
    static const char comp[4] = { 'x', 'y', 'z', 'w' };
    int ident = s->swz[0] == 0 && s->swz[1] == 1 && s->swz[2] == 2 && s->swz[3] == 3;
    bput(b, "%s%s", s->neg ? "(-" : "", base);
    if (!ident) bput(b, ".%c%c%c%c", comp[s->swz[0]], comp[s->swz[1]], comp[s->swz[2]], comp[s->swz[3]]);
    if (s->neg) bput(b, ")");
}

static const char *VS_WGSL_HELPERS =
"@group(0) @binding(1) var<uniform> vc: array<vec4<f32>, 96>;\n"
"fn vc_at(i: i32) -> vec4<f32> { if (i < 0 || i >= 96) { return vec4<f32>(0.0); } return vc[i]; }\n"
"fn d3d_rcp(x: f32) -> f32 { if (x == 1.0) { return 1.0; } return 1.0 / x; }\n"
"fn d3d_rsq(x0: f32) -> f32 { let x = abs(x0); if (x == 1.0) { return 1.0; } return inverseSqrt(x); }\n"
"fn d3d_log(x0: f32) -> f32 { return log2(abs(x0)); }\n"
"fn d3d_lit(s: vec4<f32>) -> vec4<f32> {\n"
"  let pw = clamp(s.w, -128.0, 128.0);\n"
"  var r = vec4<f32>(1.0, max(s.x, 0.0), 0.0, 1.0);\n"
"  if (s.x > 0.0 && s.y > 0.0) { r.z = pow(s.y, pw); }\n"
"  return r;\n"
"}\n"
"fn d3d_expp(w: f32) -> vec4<f32> { let f = floor(w); return vec4<f32>(exp2(f), w - f, exp2(w), 1.0); }\n"
"fn d3d_logp(w: f32) -> vec4<f32> {\n"
"  let x = abs(w);\n"
"  let e = floor(log2(x));\n"
"  return vec4<f32>(e, x / exp2(e), log2(x), 1.0);\n"
"}\n";

/* WGSL type and vec4 conversion per declaration type */
static const char *in_type(uint32_t t) {
    static const char *ty[8] = { "f32", "vec2<f32>", "vec3<f32>", "vec4<f32>", "vec4<f32>", "vec4<u32>", "vec2<i32>", "vec4<i32>" };
    return t < 8 ? ty[t] : NULL;
}
static void in_convert(Buf *b, int r, uint32_t t) {
    switch (t) {
    case 0: bput(b, "  var v%d = vec4<f32>(i.a%d, 0.0, 0.0, 1.0);\n", r, r); break;
    case 1: bput(b, "  var v%d = vec4<f32>(i.a%d, 0.0, 1.0);\n", r, r); break;
    case 2: bput(b, "  var v%d = vec4<f32>(i.a%d, 1.0);\n", r, r); break;
    case 3: bput(b, "  var v%d = i.a%d;\n", r, r); break;
    case 4: bput(b, "  var v%d = i.a%d.zyxw;\n", r, r); break;   /* D3DCOLOR bytes are B, G, R, A */
    case 5: bput(b, "  var v%d = vec4<f32>(i.a%d);\n", r, r); break;
    case 6: bput(b, "  var v%d = vec4<f32>(vec2<f32>(i.a%d), 0.0, 1.0);\n", r, r); break;
    case 7: bput(b, "  var v%d = vec4<f32>(i.a%d);\n", r, r); break;
    }
}

char *vs_to_wgsl(Shader *sh, char *why, size_t why_n) {
    VsProgram *p = program_for(sh);
    if (!p->ok) { snprintf(why, why_n, "not decoded"); return NULL; }
    VsGpuInput ins[16];
    int nin = vs_gpu_inputs(sh, ins);
    uint8_t used_in[16] = { 0 };
    Buf b = { 0 };
    bput(&b, "%s", VS_WGSL_HELPERS);
    bput(&b, "struct VG {\n");
    for (int k = 0; k < nin; k++) {
        const char *ty = in_type(ins[k].type);
        if (!ty) { snprintf(why, why_n, "input type %u", ins[k].type); free(b.s); return NULL; }
        bput(&b, "  @location(%u) a%u: %s,\n", ins[k].reg, ins[k].reg, ty);
        used_in[ins[k].reg] = 1;
    }
    if (!nin) bput(&b, "  @builtin(vertex_index) vid: u32,\n");   /* WGSL structs can't be empty */
    bput(&b, "};\n@vertex fn vs_gpu(i: VG) -> VO {\n");
    for (int k = 0; k < nin; k++) in_convert(&b, ins[k].reg, ins[k].type);
    bput(&b, "  var oPos = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n  var oD0 = vec4<f32>(1.0);\n  var oD1 = vec4<f32>(0.0);\n");
    for (int t = 0; t < 4; t++) bput(&b, "  var oT%d = vec4<f32>(0.0, 0.0, 0.0, 1.0);\n", t);

    if (p->fixed_function) {
        /* declaration-only shader: v0 position (through world*view*projection), v5/v6 colors, v7+ texcoords */
        bput(&b, "  oPos = u.mvp * vec4<f32>(%s.xyz, 1.0);\n", used_in[0] ? "v0" : "vec4<f32>(0.0)");
        if (used_in[5]) bput(&b, "  oD0 = v5;\n");
        if (used_in[6]) bput(&b, "  oD1 = v6;\n");
        for (int t = 0; t < 4; t++) if (used_in[7 + t]) bput(&b, "  oT%d = v%d;\n", t, 7 + t);
    } else {
        for (int r = 0; r < 12; r++) bput(&b, "  var r%d = vec4<f32>(0.0);\n", r);
        bput(&b, "  var a0: i32 = 0;\n  var t: vec4<f32>;\n");
        for (uint32_t n = 0; n < p->n; n++) {
            const Insn *in = &p->code[n];
            Buf e = { 0 }, s0 = { 0 }, s1 = { 0 }, s2 = { 0 };
            if (in->nsrc > 0) src_expr(&s0, &in->s[0], 0, used_in);
            if (in->nsrc > 1) src_expr(&s1, &in->s[1], 0, used_in);
            if (in->nsrc > 2) src_expr(&s2, &in->s[2], 0, used_in);
            const char *a = s0.s ? s0.s : "", *c1 = s1.s ? s1.s : "", *c2 = s2.s ? s2.s : "";
            int ok = 1;
            switch (in->op) {
            case 1: bput(&e, "%s", a); break;
            case 2: bput(&e, "%s + %s", a, c1); break;
            case 3: bput(&e, "%s - %s", a, c1); break;
            case 4: bput(&e, "%s * %s + %s", a, c1, c2); break;
            case 5: bput(&e, "%s * %s", a, c1); break;
            case 6: bput(&e, "vec4<f32>(d3d_rcp((%s).w))", a); break;
            case 7: bput(&e, "vec4<f32>(d3d_rsq((%s).w))", a); break;
            case 8: bput(&e, "vec4<f32>(dot((%s).xyz, (%s).xyz))", a, c1); break;
            case 9: bput(&e, "vec4<f32>(dot(%s, %s))", a, c1); break;
            case 10: bput(&e, "select(%s, %s, %s < %s)", c1, a, a, c1); break;    /* min: s0 < s1 ? s0 : s1 */
            case 11: bput(&e, "select(%s, %s, %s >= %s)", c1, a, a, c1); break;   /* max: s0 >= s1 ? s0 : s1 */
            case 12: bput(&e, "select(vec4<f32>(0.0), vec4<f32>(1.0), %s < %s)", a, c1); break;
            case 13: bput(&e, "select(vec4<f32>(0.0), vec4<f32>(1.0), %s >= %s)", a, c1); break;
            case 14: bput(&e, "vec4<f32>(exp2((%s).w))", a); break;
            case 15: bput(&e, "vec4<f32>(d3d_log((%s).w))", a); break;
            case 16: bput(&e, "d3d_lit(%s)", a); break;
            case 17: bput(&e, "vec4<f32>(1.0, (%s).y * (%s).y, (%s).z, (%s).w)", a, c1, a, c1); break;
            case 18: bput(&e, "%s * (%s - %s) + %s", a, c1, c2, c2); break;
            case 19: bput(&e, "fract(%s)", a); break;
            case 20: case 21: case 22: case 23: case 24: {
                int rows = in->op == 20 || in->op == 22 ? 4 : in->op == 24 ? 2 : 3, four = in->op <= 21;
                bput(&e, "vec4<f32>(");
                for (int k = 0; k < 4; k++) {
                    if (k) bput(&e, ", ");
                    if (k >= rows) { bput(&e, "0.0"); continue; }
                    Buf row = { 0 };
                    src_expr(&row, &in->s[1], k, used_in);
                    if (four) bput(&e, "dot(%s, %s)", a, row.s);
                    else bput(&e, "dot((%s).xyz, (%s).xyz)", a, row.s);
                    free(row.s);
                }
                bput(&e, ")");
                break;
            }
            case 78: bput(&e, "d3d_expp((%s).w)", a); break;
            case 79: bput(&e, "d3d_logp((%s).w)", a); break;
            default: ok = 0; break;
            }
            free(s0.s); free(s1.s); free(s2.s);
            if (!ok) { snprintf(why, why_n, "opcode %u", in->op); free(e.s); free(b.s); return NULL; }
            /* destination */
            char dst[16] = "";
            switch (in->dtype) {
            case RT_TEMP: if (in->dnum < 12) snprintf(dst, sizeof dst, "r%u", in->dnum); break;
            case RT_ADDR: break;
            case RT_RASTOUT: if (in->dnum == 0) snprintf(dst, sizeof dst, "oPos"); break;
            case RT_ATTROUT: if (in->dnum < 2) snprintf(dst, sizeof dst, "oD%u", in->dnum); break;
            case RT_TEXOUT: if (in->dnum < 4) snprintf(dst, sizeof dst, "oT%u", in->dnum); break;
            default: break;
            }
            if (in->dtype == RT_ADDR) {
                if (in->dmask & 1) bput(&b, "  t = %s;\n  a0 = i32(floor(t.x));\n", e.s);
            } else if (dst[0] && in->dmask) {
                bput(&b, "  t = %s;\n", e.s);
                if (in->dmask == 0xF) bput(&b, "  %s = t;\n", dst);
                else for (int k = 0; k < 4; k++) if (in->dmask & (1 << k)) bput(&b, "  %s.%c = t.%c;\n", dst, "xyzw"[k], "xyzw"[k]);
            }
            free(e.s);
        }
    }
    /* Direct3D's half-pixel offset, as in the fixed-function vertex stage; colors clamp like vs_run()'s */
    bput(&b,
         "  var o: VO;\n"
         "  var q = oPos;\n"
         "  q.x = q.x + q.w / u.vp.z;\n"
         "  q.y = q.y - q.w / u.vp.w;\n"
         "  o.pos = q;\n"
         "  if (u.misc.z != 0.0) { o.pos.x = o.pos.x * u.misc.z + o.pos.w * u.misc.w; }\n"
         "  o.diffuse = saturate(oD0);\n"
         "  o.specular = saturate(oD1);\n"
         "  o.tc0 = oT0; o.tc1 = oT1; o.tc2 = oT2; o.tc3 = oT3;\n"
         "  return o;\n"
         "}\n");
    return b.s;
}
