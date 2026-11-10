// aa76_common.glsl -- job aa76: constants shared by every aa76 shader (layout = VaUbo in vr_aa.h, std140)
#extension GL_EXT_samplerless_texture_functions : require
layout(std140, set = 0, binding = 0) uniform AaParams {
  vec4  rt;     // 1/W, 1/H, W, H of the back buffer
  ivec4 eye;    // eye width, height, x0 / x1 of the area to process
  ivec4 prm;    // mode (0 game, 1 none, 2 smaa, 3 taa), flags, frame, bb mode (1: input = copy of the back buffer's eye area)
  vec4  taa;    // alpha, clip, sharpen, 0
  vec4  jit;    // jitter of this frame (px, x right, y up)
  vec4  m[8];   // reprojection rows: eye 0 rows 0..3, eye 1 rows 0..3 (row vectors: clip = x r0 + y r1 + z r2 + r3)
};
const int F_RESET0 = 1, F_RESET1 = 2, F_CUBIC = 4, F_HIST = 8, F_NODEPTH = 16, F_NOFILTER = 32;
// the eye column of buffer x (no AA outside 2 x eye width)
int eye_of(int x) { return x >= eye.x ? 1 : 0; }
