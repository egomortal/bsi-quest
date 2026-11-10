#version 450
#extension GL_GOOGLE_include_directive : require
// out.frag -- job aa76: replaces the game's FXAA draw into the back buffer for aa=none and aa=taa.
// Eye columns: aa=none -> the AA input unchanged; aa=taa -> the history just resolved, sharpened (CAS-style, cross
// neighbourhood inside the eye column) when taa.z > 0. UI column (x >= 2 eye widths): the AA input unchanged.
#include "aa76_common.glsl"
layout(set = 0, binding = 1) uniform texture2D t_color;
layout(set = 0, binding = 3) uniform texture2D t_hist;
layout(location = 0) out vec4 o;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  if ((prm.w & 1) != 0 && p.x >= 2 * eye.x) discard;   // bb mode: the tone map already wrote the UI column; t_color is 2 eyes wide
  vec4 src = texelFetch(t_color, p, 0);
  if (prm.x != 3 || p.x >= 2 * eye.x || (prm.w & 2) != 0) { o = src; return; }   // (prm.w & 2: cloud diagnostics, show the input)
  int e = eye_of(p.x), x0 = e * eye.x;
  ivec2 lo = ivec2(x0, 0), hi = ivec2(x0 + eye.x - 1, eye.y - 1);
  vec3 c = texelFetch(t_hist, p, 0).rgb;
  if (taa.z > 0.0) {
    vec3 n = texelFetch(t_hist, clamp(p + ivec2(0, -1), lo, hi), 0).rgb;
    vec3 w = texelFetch(t_hist, clamp(p + ivec2(-1, 0), lo, hi), 0).rgb;
    vec3 ea = texelFetch(t_hist, clamp(p + ivec2(1, 0), lo, hi), 0).rgb;
    vec3 s = texelFetch(t_hist, clamp(p + ivec2(0, 1), lo, hi), 0).rgb;
    vec3 mn = min(c, min(min(n, w), min(ea, s))), mx = max(c, max(max(n, w), max(ea, s)));
    // AMD CAS idea: sharpen less where the local contrast is already high (no halos on hard edges)
    vec3 amp = sqrt(clamp(min(mn, 1.0 - mx) / max(mx, vec3(1e-4)), 0.0, 1.0));
    vec3 wgt = -amp / mix(8.0, 5.0, taa.z);
    c = clamp((c + wgt * (n + w + ea + s)) / (1.0 + 4.0 * wgt), 0.0, 1.0);
  }
  o = vec4(c, src.a);
}
