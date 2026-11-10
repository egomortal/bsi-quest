#version 450
#extension GL_GOOGLE_include_directive : require
// smaa_blend.frag -- job aa76: SMAA 1x pass 3 (neighbourhood blending), replaces the game's FXAA draw into the back
// buffer for aa=smaa. UI column (x >= 2 eye widths): the AA input unchanged.
#include "aa76_common.glsl"
layout(set = 0, binding = 1) uniform texture2D t_color;
layout(set = 0, binding = 6) uniform texture2D t_blend;
layout(location = 0) out vec4 o;
#define HAVE_COLOR 1
#define HAVE_BLEND 1
#include "smaa_port.glsl"
#include "SMAA.hlsl"
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  if ((prm.w & 1) != 0 && p.x >= 2 * eye.x) discard;   // bb mode (see out.frag)
  vec4 src = texelFetch(t_color, p, 0);
  if (p.x >= 2 * eye.x) { o = src; return; }
  int x0 = eye_of(p.x) * eye.x; g_lo = ivec2(x0, 0); g_hi = ivec2(x0 + eye.x - 1, eye.y - 1);
  vec2 tc = (vec2(p) + 0.5) * rt.xy; vec4 off;
  SMAANeighborhoodBlendingVS(tc, off);
  o = vec4(SMAANeighborhoodBlendingPS(tc, off, 0, 0).rgb, src.a);
}
