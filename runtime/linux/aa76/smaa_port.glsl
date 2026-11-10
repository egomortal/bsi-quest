// smaa_port.glsl -- job aa76: SMAA 1x (reference code SMAA.hlsl, MIT, aa76/../smaa_*) on Vulkan without samplers.
// SMAA_CUSTOM_SL: every texture access becomes smaa_lin_<name>() / smaa_pt_<name>() (texture parameters are dummies; the
// shader's own textures are globals). Bilinear filtering is done by hand with texelFetch (the same weights a linear
// sampler uses), screen textures are clamped to the eye column being processed (each eye is a separate image: nothing
// is read across the eye split or from the UI column), the LUTs (area 160x560 RG8, search 64x16 R8) to their size.
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
#define SMAA_RT_METRICS rt
#define SMAATexture2D(tex) int tex
#define SMAATexturePass2D(tex) 0
#define SMAASampleLevelZero(tex, coord) smaa_lin_##tex(coord)
#define SMAASampleLevelZeroPoint(tex, coord) smaa_pt_##tex(coord)
#define SMAASampleLevelZeroOffset(tex, coord, offset) smaa_lin_##tex((coord) + vec2(offset) * rt.xy)
#define SMAASample(tex, coord) smaa_lin_##tex(coord)
#define SMAASamplePoint(tex, coord) smaa_pt_##tex(coord)
#define SMAASampleOffset(tex, coord, offset) smaa_lin_##tex((coord) + vec2(offset) * rt.xy)
#define SMAA_FLATTEN
#define SMAA_BRANCH
#define lerp(a, b, t) mix(a, b, t)
#define saturate(a) clamp(a, 0.0, 1.0)
#define mad(a, b, c) ((a) * (b) + (c))
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define bool2 bvec2
#define bool3 bvec3
#define bool4 bvec4
ivec2 g_lo, g_hi;
vec4 scr_fetch(texture2D t, ivec2 q) { return texelFetch(t, clamp(q, g_lo, g_hi), 0); }
vec4 scr_lin(texture2D t, vec2 uv) {
  vec2 f = uv * rt.zw - 0.5, fl = floor(f), a = f - fl; ivec2 i = ivec2(fl);
  return mix(mix(scr_fetch(t, i), scr_fetch(t, i + ivec2(1, 0)), a.x), mix(scr_fetch(t, i + ivec2(0, 1)), scr_fetch(t, i + ivec2(1, 1)), a.x), a.y);
}
vec4 scr_pt(texture2D t, vec2 uv) { return scr_fetch(t, ivec2(floor(uv * rt.zw))); }
vec4 lut_fetch(texture2D t, ivec2 q, ivec2 sz) { return texelFetch(t, clamp(q, ivec2(0), sz - 1), 0); }
vec4 lut_lin(texture2D t, vec2 uv) {
  ivec2 sz = textureSize(t, 0); vec2 f = uv * vec2(sz) - 0.5, fl = floor(f), a = f - fl; ivec2 i = ivec2(fl);
  return mix(mix(lut_fetch(t, i, sz), lut_fetch(t, i + ivec2(1, 0), sz), a.x), mix(lut_fetch(t, i + ivec2(0, 1), sz), lut_fetch(t, i + ivec2(1, 1), sz), a.x), a.y);
}
#ifdef HAVE_COLOR
vec4 smaa_lin_colorTex(vec2 uv) { return scr_lin(t_color, uv); }
vec4 smaa_pt_colorTex(vec2 uv) { return scr_pt(t_color, uv); }
#else
vec4 smaa_lin_colorTex(vec2 uv) { return vec4(0.0); }
vec4 smaa_pt_colorTex(vec2 uv) { return vec4(0.0); }
#endif
#ifdef HAVE_EDGES
vec4 smaa_lin_edgesTex(vec2 uv) { return scr_lin(t_edges, uv); }
vec4 smaa_pt_edgesTex(vec2 uv) { return scr_pt(t_edges, uv); }
#else
vec4 smaa_lin_edgesTex(vec2 uv) { return vec4(0.0); }
vec4 smaa_pt_edgesTex(vec2 uv) { return vec4(0.0); }
#endif
#ifdef HAVE_BLEND
vec4 smaa_lin_blendTex(vec2 uv) { return scr_lin(t_blend, uv); }
vec4 smaa_pt_blendTex(vec2 uv) { return scr_pt(t_blend, uv); }
#else
vec4 smaa_lin_blendTex(vec2 uv) { return vec4(0.0); }
vec4 smaa_pt_blendTex(vec2 uv) { return vec4(0.0); }
#endif
#ifdef HAVE_LUTS
vec4 smaa_lin_areaTex(vec2 uv) { return lut_lin(t_area, uv); }
vec4 smaa_lin_searchTex(vec2 uv) { return lut_lin(t_search, uv); }
#else
vec4 smaa_lin_areaTex(vec2 uv) { return vec4(0.0); }
vec4 smaa_lin_searchTex(vec2 uv) { return vec4(0.0); }
#endif
vec4 smaa_pt_areaTex(vec2 uv) { return smaa_lin_areaTex(uv); }
vec4 smaa_pt_searchTex(vec2 uv) { return smaa_lin_searchTex(uv); }
// paths aa76 does not use (predication, depth edges, reprojection, SMAA 2x / 4x)
vec4 smaa_lin_tex(vec2 uv) { return vec4(0.0); }
vec4 smaa_pt_tex(vec2 uv) { return vec4(0.0); }
vec4 smaa_lin_velocityTex(vec2 uv) { return vec4(0.0); }
vec4 smaa_pt_velocityTex(vec2 uv) { return vec4(0.0); }
vec4 smaa_pt_currentColorTex(vec2 uv) { return vec4(0.0); }
vec4 smaa_pt_previousColorTex(vec2 uv) { return vec4(0.0); }
vec4 smaa_lin_depthTex(vec2 uv) { return vec4(0.0); }
vec4 smaa_pt_depthTex(vec2 uv) { return vec4(0.0); }
vec4 smaa_pt_predicationTex(vec2 uv) { return vec4(0.0); }
