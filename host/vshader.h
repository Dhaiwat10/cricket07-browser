/* Direct3D 8 vertex shaders on the CPU (see vshader.c). */
#ifndef RC_VSHADER_H
#define RC_VSHADER_H

#include "d3d8.h"

typedef struct {
    const uint8_t *data[16];   /* per stream: host pointer to vertex 0 */
    uint32_t stride[16];
    const float *ff_mvp;       /* world*view*projection, for declaration-only (fixed-function) shaders */
} VsStreams;

typedef struct { float pos[4], d0[4], d1[4], tex[4][4]; } VsOut;

void vs_run(Shader *sh, const float consts[96][4], const VsStreams *st, uint32_t index, VsOut *out);
int vs_is_fixed_function(Shader *sh);

/* GPU translation: inputs the GPU must fetch (register, stream, D3DVSDT type, byte offset), the program as a
   WGSL vertex function `vs_gpu` (NULL with a reason if it can't be translated), and the declaration/def
   constants merged over the game's constants (the GPU reads constants only from the uniform array). */
typedef struct { uint8_t reg, stream, type; uint16_t offset; } VsGpuInput;
int vs_gpu_inputs(Shader *sh, VsGpuInput in[16]);   /* count, or -1 if the shader didn't decode */
char *vs_to_wgsl(Shader *sh, char *why, size_t why_n);
void vs_merge_defs(Shader *sh, float c[96][4]);

#endif
