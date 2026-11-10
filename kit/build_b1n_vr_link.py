#!/usr/bin/env python3
"""VR test (versionCode 55): build 54 b1n sources + the engine-level two-view stereo prototype (kit/src_vr/, kit/objs_vr/).
Overlays: kit/src_vr/ first, then kit/src_b1n/, then kit/src/. kit/objs_vr/part007.o and part012.o (stereo hooks) replace the
same-named objects in kit/objs/. Stereo is off unless /sdcard/BSI/vr_stereo.txt / BSI_VR_STEREO turn it on.
B1.6 Quest gate: libbsi.so with the b1 native renderer (BSI_RHI=b1n) from saved v0.9.1 inputs.
Same as build_b0_link.py, plus an overlay: every source in kit/src_b1n/ replaces the file of the same name in kit/src/
(the kit's own sources are never modified). Added: linux/rhi_b1.c, linux/rhi_b1_com.c (C) and linux/rhi_b1_dxvk.cpp,
linux/rhi_b1_shader.cpp (C++, DXVK core). The renderer is off unless /sdcard/BSI/env.txt has BSI_RHI=b1n.
apex_fix (2026-10-14): optional kit/objs_apex/ overlay (see the comment at the object list and apex_fix/README.md).
build 68 (2026-10-15): four opt-in gameplay mods added to RUNTIME (linux/gear_mod.c, linux/weapon_perks.c, linux/weapon_wheel.c,
linux/economy.c, all in kit/src_b1n/linux/ together with the merged patches.c, rt_linux.c and hle_input.c). Each mod stays off
unless its config file exists (/sdcard/BSI/gear_mod.txt, weapon_perks.txt, weapon_wheel.txt, economy.txt); see b68_mods/README.md.
ext72 (2026-10-16): linux/legendary.c (legendary drops, "The Chosen One") and linux/ext_menu.c (built-in mod defaults + in-game
mod menu) added to RUNTIME; ext_builtin.h is next to them in kit/src_b1n/linux/. See ext72/README.md.
b76 merge (2026-10-17): pp76 + aa76; no list change; version b1n76(vr). pp76 (2026-10-16): (post-process per eye + FXAA diagnostics: src_b1n rhi_b1_dxvk.cpp, rhi_b1_vr.inc,
new rhi_b1_pp76.inc (included by rhi_b1_vr.inc), rhi_b1_shader.cpp; src_vr vr_stereo.c/.h).
ao74 (2026-10-16): no list change; version b1n75(vr) (AO per eye: src_b1n rhi_b1_*.cpp/.inc, src_vr vr_stereo.c/.h).
perf73 (2026-10-16): no list change; version b1n73(vr). kit/src_b1n/linux/android_glue.c (stereo: VR size from xr_glue, not
resolution.txt), com.c, rhi_b1_com.c, rhi_b1_dxvk.cpp; kit/src_vr/linux/ xr_glue.c, vr_stereo.c, vr_stereo.h. See perf73/README.md.
build 69 (2026-10-15): VR stereo on the native renderer (src_vr + rhi_b1_vr.inc), S1 v2 (third shader, cheaper map, A/B), objs_apex with
the last emulator fallbacks translated (24 replaced, 4 added). See README_B69.md.
b77 (2026-10-19): no list change; version b1n77(vr). src_b1n/linux/hle_more.c = main tree copy: restores the offline Steam
glue (SteamAPI_Init log + SteamInternal_FindOrCreateUserInterface -> local_profile.c); the b74 overlay had dropped it = no saves b74-b76.
b78 (2026-10-19): + linux/autopilot.c (lab autopilot: /sdcard/BSI/autopilot.txt one-shot scenario; synthetic pad, scripted head,
A/B file copies, markers, clean quit); src_b1n android_glue.c (pad merge + start) and patches.c (map tick), src_vr xr_glue.c
(scripted head pose with real IPD/FOV, xrRequestExitSession); version b1n78(vr). See README_B78.md.
b80 (2026-10-20, chat 6 format lab): no list change; version b1n80(vr). src_b1n rhi_b1_dxvk.cpp (rc80: BSI_B1_LODBIAS sampler mip-bias ceiling test, off by default). See README_B80.md.
b81 (2026-10-20, chat 5 CPU): no list change; version b1n81(vr). C1a inline LSE atomics: C++ files get -mcpu=cortex-a78c and the
DXVK static libs come from dxvk_android_build_006 (same source and options as 005 + -mcpu=cortex-a78c); src_b1n rhi_b1_com.c
(C6 hitch logger for b1n, C5 dynamic-buffer copy dedupe, BSI_B1_PENDALL=1 = old). See README_B81.md.
b82 (2026-10-20, chat 4 GPU): no list change; version b1n82(vr). src_b1n rhi_b1_dxvk.cpp + rhi_b1_vr.inc (AO right-view crop via
vkCmdDispatchBase, BSI_B1_AOCROP=0 off) + NEW rhi_b1_g4.inc (shadow-mask pass coalescing, BSI_B1_SHCOAL=0 off / 2 check) + rhi_b1_shadoweo.inc (1 line). See README_B82.md.
b79 (2026-10-19, chat 3 measurement): no list change; version b1n79(vr). src_b1n rhi_b1_dxvk.cpp (m79: live profiler
controls in /sdcard/BSI/gpuprof.txt: [b1gq] per pass, [b1gs] per draw / material shader, [b1cen] census) + rhi_b1_shader.cpp
(b1dx_shader_name). See README_B79.md.
b83 (2026-10-20, chat 5): no list change; version b1n83(vr). src_b1n rhi_b1_com.c: the b81 hitch-logger wrapper classified every
COM call by string compares (~5 ms render-thread CPU per frame at the Fair, j0076); now one classification per method index.
[b1] dynamic-buffer copies line (PENDALL acknowledgement). See README_B83.md.
b84 (2026-10-20, chat 5): no list change; version b1n84(vr). src_b1n rhi_b1_dxvk.cpp /* c5 */ hooks + NEW rhi_b1_c5.inc: raw-pointer
captures in the hot emit lambdas with a CS-sequence graveyard (BSI_B1_RAWCAP=1, default off). See README_B84.md, RAWCAP_REPORT.md.
b85 (2026-10-20, chat 4 GPU): no list change; version b1n85(vr). src_b1n rhi_b1_dxvk.cpp (BSI_B1_MAXANISO=N sampler aniso cap, off by
default), src_vr xr_glue.c (XR_KHR_visibility_mask probe: logs the lens hidden-area fraction, writes vismask_<eye>.txt). See README_B85.md.
b86 (2026-10-26, chat 4 GPU): no list change; version b1n86(vr). src_b1n rhi_b1_dxvk.cpp es86: eye-column scissor for scene draws into
back-buffer-sized targets (BSI_B1_EYESCIS, default on; _MARGIN 128; _CLEARS off), gpuprof.txt clears=1 timing rows. See README_B86.md.
b87 (2026-10-26, chat 4 GPU): no list change; version b1n87(vr). NEW src_b1n rhi_b1_sb87.inc (shadow-projection quads scissored to the
stencil volume bbox + depth-bounds test, BSI_B1_SHBBOX / BSI_B1_SHDB, default on, 2 = CHECK, 3 = diagnostic) + hooks in rhi_b1_dxvk.cpp,
rhi_b1_pp76.inc, rhi_b1_shader.cpp. See README_B87.md.
b88 (2026-10-26, chat 4 GPU): no list change; version b1n88(vr). src_b1n rhi_b1_dxvk.cpp dc88 (dead-clear elision: colour clears deferred
and dropped when the next draw provably overwrites them, BSI_B1_DEADCLEAR default on), rhi_b1_sb87.inc fixes (clear-quad proof,
static VB copies, per-rect depth bounds), rhi_b1_shader.cpp (b1dx_shader_has_discard, POSITION scan). See README_B88.md.
b89 (2026-10-26, chat 4 GPU): no list change; version b1n89(vr). dc88 per-condition diagnostics + write mask = format channels,
sb87 cleared-rect list (right-eye replay quads), 5-line stats. See README_B89.md.
b90 (2026-10-26, chat 4 GPU): no list change; version b1n90(vr). rhi_b1_shader.cpp discard scan fixed (DXBC header offsets 28/32:
b88/b89 flagged nearly every PS as "may discard", so dc88 elided nothing), rhi_b1_sb87.inc cascade depth bounds with clipped vertices,
z-range loss causes. See README_B90.md.
b91 (2026-10-26, chat 4 deep round): no list change; version b1n91(vr). xr_glue.c display refresh default 120 -> 72 Hz (j0134: -3 ms/frame
exact; refresh.txt still overrides), rhi_b1_dxvk.cpp [b1gq] per-pass per-frame minimum ("min X" + "[b91] sum of per-pass minima") = the share
of each pass stolen by compositor preemption / content. See README_B91.md.
b92 (2026-10-26 night, chat 4): no list change; version b1n92(vr). rhi_b1_sb87.inc + rhi_b1_dxvk.cpp: BSI_B1_SHBBOX=4 = NOT-EXACT ceiling probe
(stencil volumes not drawn, projection quads without stencil test; measurement only, default 1 unchanged). See README_B92.md.
b93 (2026-10-26 night, chat 4): no list change; version b1n93(vr). rhi_b1_sb87.inc: receiver-mesh stencil draws (vs.03ccec40, z-pass counting,
UE3 per-object shadows) now give the projection quads a bbox scissor from the mesh AABB (static VB/IB copies kept at creation, cb0 LocalToWorld
x cb1 ViewProjection, 8 corners); no depth bounds for them (open meshes). Exact; CHECK mode 2 covers it. See README_B93.md.
b94 (2026-10-26 night, chat 4): no list change; version b1n94(vr). rhi_b1_sb87.inc: the static-buffer store also mirrors big CPU writes
(Lock/Unlock uploads; b93 saw 0 receiver meshes with data), + "receiver mesh N NO DATA" diagnostics. See README_B93.md.
b95 (2026-10-27 night, chat 4): no list change; version b1n95(vr). rhi_b1_sb87.inc: the static store also mirrors UE3 dynamic pool
buffers written piecewise (validity per 64-byte block; b94 found the receiver meshes in pools: "index bytes unknown"). APK: kit\\AndroidManifest_override.axml
(hand-tracking support declared) used by build_b0_apk.py when present -> no "Switch to Controllers" dialog in unattended runs. See README_B95.md.
b96 (2026-10-27 night, chat 4): no list change; version b1n96(vr). rhi_b1_sb87.inc: BSI_B1_SHCENSUS=1 diagnostic = an occlusion query around
every shadow projection quad (pixels passing the stencil test = pixels its kernel runs on), histogram in the [b1sb] stats. Default off. See README_B96.md.
b97 (2026-10-27 night, chat 4): no list change; version b1n97(vr). rhi_b1_sb87.inc: BSI_B1_SHDEPTHALWAYS (default 1, EXACT): shadow projection quads
whose state has no depth test get depth test ALWAYS / no write, so the early depth-stencil stage runs and the stencil rejects pixels BEFORE the
kernel (b96 census: the kernels ran on every rasterised pixel; regions are 1-25 % of the eye). See README_B97.md.
b98 (2026-10-27 night, chat 4): no list change; version b1n98(vr). rhi_b1_sb87.inc: BSI_B1_SHBBOX=5 NOT-EXACT probe (every projection quad scissored
to the centred 25 % area: raster-proportional vs per-quad fixed cost). b97 depth-ALWAYS measured neutral (early stencil was already early). See README_B98.md.
b99 (2026-10-27 03:10 EDT, chat 4): no list change; version b1n99(vr). rhi_b1_sb87.inc: BSI_B1_SHMARK (default 1, EXACT): the shadow SUBJECT markers
(UE3 material VS/PS, stencil REPLACE ref 1) are bounded by their projected vertices (DXBC dataflow match of the UE3 SM5 position sequence, MeshExtension/
MeshOrigin decode, float3 / snorm16x4 positions) instead of poisoning the region; receiver meshes / markers partly behind the eye get a near-plane-clipped
bbox instead of the whole rect. See README_B99.md.
b100 (2026-10-27 07:00 EDT, chat 4): no list change; version b1n100(vr). rhi_b1_sb87.inc: BSI_B1_SHBBOX=6 EXACT timing probe (the render pass is ended
with a fragment -> vertex barrier before every projection quad = the overhead a GPU tile mask would pay) + a histogram of the unknown stencil writes by
(vs, ps). b99 measured: exact (CHECK 0 texels) but no gain (bbox area mean 84 %, median 100 % at Rooftops). See README_B100.md.
b101 (2026-10-27 08:05 EDT, chat 4): no list change; version b1n101(vr). rhi_b1_sb87.inc: BSI_B1_SHSKIN (default 1, EXACT): the SKINNED shadow subjects
(UE3 GPU-skin VS ff4b9172 / cc3229bb / 40c5f4d7, allow-list BSI_B1_SHBBOX_VS3; 82k + 22k stencil writes per Rooftops run) are bounded by per-bone
AABBs x bone matrices (cb3 VSBoneConstants) x LocalToWorld x ViewProjection; small static VB/IB creation data now kept in the big store. See README_B101.md.
b102 (2026-10-27 10:30 EDT, chat 4): no list change; version b1n102(vr). TILED projection quads, opt-in BSI_B1_SHTILE=1 (NOT bit-exact: INVISIBLE tier,
user side-by-side): rhi_b1_sb87.inc (stencil -> tile mask compute pass, GPU-built index list, drawIndexedIndirect with the game VS/PS, right-eye replay),
NEW rhi_b1_sb87_spv.h (SPIR-V of sb87/tile.comp), rhi_b1_dxvk.cpp (draw paths + dxvk_shader_spirv.h), rhi_b1_pp76.inc (replay draw). See README_B102.md.
b103 (2026-10-27 11:20 EDT, chat 4): no list change; version b1n103(vr). fp16 MATERIALS, opt-in BSI_B1_FP16=1 (NOT bit-exact: INVISIBLE tier):
NEW rhi_b1_fp16.h (RelaxedPrecision pass over a PS's SPIR-V, texture-coordinate / discard / integer slices kept fp32), rhi_b1_shader.cpp (RelaxedShader
wrapper for PS >= BSI_B1_FP16_MIN=150 instructions with < 8 shadow-compare fetches), rhi_b1_dxvk.cpp + rhi_b1_sb87.inc (stats line). See README_B103.md.
b105 (2026-10-27, chat 5 CPU round 2): no list change; version b1n105(vr). src_b1n com.c + rt_linux.c (BSI_B1_COMFAST: per-method
fast dispatch of the hot context methods), rhi_b1_com.c (ctx_method shared switch, BSI_B1_DRAWBATCH deferred state burst, exact
viewport/scissor skip), rhi_b1_dxvk.cpp (db103 RmuGuard + b1dx_c_begin/end_batch; dyn103 hooks) + NEW rhi_b1_dyn.inc
(BSI_B1_DYNRING dynamic-buffer ring). All default off. See README_B105.md.
b106 (2026-10-27 12:10 EDT, chat 4): no list change; version b1n106(vr). Fixes on b103/b105: rhi_b1_fp16.h (shadow-compare census: gather4_c is
opcode 126 -> the PCSS kernels / S1 twins are no longer wrapped), rhi_b1_shader.cpp (BSI_B1_FP16_MIN default 40: materials are 50-170 DXBC instr),
rhi_b1_sb87.inc (SHTILE fallback diagnostic: the projection draws are not +-1 quads), rhi_b1_pp76.inc (FXAA check readbacks OFF by default,
BSI_B1_FXCHECK=3 restores; chat 5 request). Base = chat 5 b105 (rhi_b1_dxvk.cpp 823fa099 kept). See README_B106.md.
b107 (2026-10-27 15:05 EDT, chat 4): no list change; version b1n107(vr). rhi_b1_sb87.inc: chat 5's b105 profile found 2.0-2.8 ms of sb87 CPU
bounding + ~1.8 ms of diagnostic snprintf per frame on the render thread; the receiver / marker / skinned bounds (GPU gain ~0, b93-b101) are
now OFF by default (BSI_B1_SHRECV / SHMARK / SHSKIN = 1 restore), the static buffer store only fills when one of them is on, and the depth-write
/ volume diagnostics are formatted only while they can still be printed. Volume bbox scissors (SHBBOX=1) stay on (cheap, exact; the tile
machinery needs them). See README_B107.md.
b108 (2026-10-27 15:40 EDT, chat 4): no list change; version b1n108(vr). Tiles v2 (BSI_B1_SHTILE=1, still opt-in / INVISIBLE tier): the UE3
projection draws are the light frustum (36 vertices, vs.3863988d), not a quad; rhi_b1_sb87.inc now draws the tiles with its own vertex shader
(sb87/tile.vert -> rhi_b1_sb87_spv.h, same outputs as vs.3863988d from clip-space tile corners, reads the game VS cb0) with culling off.
b109 (2026-10-27, chat 5): no list change; version b1n109(vr). Defaults flipped ON (measured exact in b105/b106): BSI_B1_DYNRING
(8 chunks) and BSI_B1_COMFAST (=0 turns each off). src_b1n rhi_b1_dyn.inc, com.c, rhi_b1_com.c. See README_B109.md.
b110 (2026-10-27 17:20 EDT, chat 4): no list change; version b1n110(vr). rhi_b1_sb87.inc: tile diagnostics - lit-tile statistics (counter
readback every 16th tiled draw) and BSI_B1_SHTILE_PROBE=1 (no tile passes: overhead floor) / =2 (every tile drawn). b108 tiles v2 engaged (0
fallbacks, CHECK mostly 0 texels) but the mask block got slower (15.7 -> 18.4 ms at Rooftops): the lit-tile share decides. Base = chat 5 b109.
b111 (2026-10-27 19:45 EDT, chat 4): no list change; version b1n111(vr). FRAME AUDIT: gpuprof.txt audit=1 with draw=N -> in each draw-level frame
every draw gets a [b1audit] line (GPU ms, pass, PS/VS, depth/stencil/blend/cull state, viewport/scissor, index/instance counts, fragments
passing depth/stencil via an occlusion query) (rhi_b1_dxvk.cpp gp:: + audit_meta); SHCENSUS=2 overdraw census split opaque / blended (rhi_b1_sb87.inc).
b112 (2026-10-27 23:50 EDT, chat 4): no list change; version b1n112(vr). BSI_B1_SHCLR=1 (opt-in, EXACT by construction): the UE3 stencil-clear
quads (full-eye, no colour / depth write, stencil ops proven to leave 0 by sb87, pixel-aligned rect) are replaced by in-pass stencil clears
(DxvkContext::clearImageView -> vkCmdClearAttachments), left eye and the pp76 right-eye replay (rhi_b1_sb87.inc, rhi_b1_dxvk.cpp, rhi_b1_pp76.inc).
b113 (2026-10-28 00:45 EDT, chat 4): no list change; version b1n113(vr). rhi_b1_sb87.inc: [b1dxs] DXVK stat counters per frame every 600
presents (render passes, pipeline barriers, queue submits, draw calls, dispatches, graphics pipelines / libraries) - for the GMEM / barrier /
pipeline-library (GPL fast-link) investigation. Same code paths otherwise as b112.
b114 (2026-10-28 02:00 EDT, chat 4): no list change; version b1n114(vr). rhi_b1_dxvk.cpp: DXVK graphics pipeline libraries OFF by default
(DXVK_CONFIG=dxvk.enableGraphicsPipelineLibrary=False unless set; BSI_B1_GPL=1 restores): measured -1.9..-3.2 ms Fair, -1.9..-2.4 ms Gondola, same
hitches, identical pixels (j0305-j0308). See README_B114.md.
b115 (2026-10-28 03:55 EDT, chat 4): no list change (new rhi_b1_shrep.inc is #included); version b1n115(vr). rhi_b1_dxvk.cpp + rhi_b1_shrep.inc:
shadow-map repeat counter BSI_B1_SHREP=1 (opt-in diagnostic, [b1rep]); gpuprof clears=1 also labels submit points. See README_B115.md.
b116 (2026-10-28 05:50 EDT, chat 4): no list change; version b1n116(vr). Shadow-map repeat counter v2: only the slots each bound shader DECLARES
(rhi_b1_shader.cpp DXBC dcl scan, b1dx_shader_slots) - b115 saw stale render-target SRVs on 100 % of shadow draws. See README_B116.md.
b117 (2026-10-28 09:30 EDT, chat 4): no list change; version b1n117(vr). rhi_b1_dxvk.cpp: draw-count submit every 1024 draws (was 256;
BSI_B1_FLUSH overrides): each mid-frame submit left the GPU idle ~0.67 ms; -0.3..-1.6 ms at Fair / Gondola / Rooftops / Cab ride, exact. See README_B117.md.
b118 (2026-10-28 10:15 EDT, chat 4): no list change; version b1n118(vr). rhi_b1_dxvk.cpp: adaptive submit BSI_B1_FLUSH_ADAPT=N (opt-in):
submit at a target change when >= N draws are pending and the GPU has finished every submitted command list (per-submit fence). See README_B118.md.
b119 (2026-10-28 11:50 EDT, chat 4): no list change; version b1n119(vr). rhi_b1_dxvk.cpp: copy trim BSI_B1_COPYTRIM=1 (opt-in): copies between stereo
scene targets only copy the eye columns + es86 margin (the game's copies and s2's executed deferred copies). See README_B119.md.
b120 (2026-11-02 11:04 EST, chat 4): version b1n120(vr). rhi_b1_dxvk.cpp split into 7 modules rhi_b1_m_*.inc (prof, res, state, objs, s2, ctx, present), included at the same places: preprocessed translation unit identical to b119 (g++ -E md5 66ace24b both); otherwise = b119 (COPYTRIM opt-in). See README_B120.md.
b121 (2026-11-02 14:12 EST): version b1n121(vr). shadow-map repeat counter OUTPUT mode BSI_B1_SHREP=2 (opt-in diagnostic): every 300 frames one 2048x2048 depth map is copied to a host buffer in two consecutive frames and compared byte for byte ([b1rep] out) - tests whether UE3's camera-relative constants hide shadow maps that come out identical (cached-shadow-map lever). See README_B121.md.
b122 (2026-11-02 14:23 EST): version b1n122(vr). ceiling probe BSI_B1_SHEYEPROBE=1 (NOT exact, measurement only): right-eye shadow projections skipped (game-drawn ones in sb87, the sun replay in pp76) = the most that sharing shadows between the eyes could save. See README_B122.md.
b123 (2026-11-02 18:23 EST): version b1n123(vr). BSI_B1_SHEYE=2 shared-eye shadow CHECK (measure only): every 300 frames one shadow-projection pass with both eyes is copied (attenuation RT + scene depth), right-eye pixels reprojected to the left eye and compared byte for byte ([b1sheye]). Default (env unset) = build 122. See README_B123.md.
b124 (2026-11-02 19:25 EST): version b1n124(vr). FXAA edge/stencil split BSI_B1_FXAASPLIT (0 off default | 1 on | 2 CHECK byte compare every 300 frames): twin of the game's FXAA PS (preamble + discard_nz of its own edge test) marks non-edge pixels in a D24S8 stencil, the unchanged game shader then runs only on edge pixels; exact by construction See README_B124.md.
b125 (2026-11-02 19:50 EST): version b1n125(vr). BSI_B1_SHCLASS=1 measurement: shadow-projection pixel classes (S1-settled / uniform window missed by S1 / penumbra / kernel pre-test) + 8x8/16x8 wave density, CPU replay of captured atlas+depth+stencil+constants; default off = build 124 See README_B125.md.
b126 (2026-11-02 21:21 EST): version b1n126(vr). Phase 0b tooling, all off by default: BSI_B1_ABAB=<switch>:<N> (GPUPROF, SLACK; [b1abab], tools/bsi/abab.py), BSI_B1_SLACK=<start|present|rtN|csN>:<clears> (+_TS=1 calibration; [b1slack]), BSI_B1_GPUTS_K game timestamp speed ([b1gts]), BSI_B1_VISHASH=1 ([b1vis]), BSI_B1_XRHIST=1 refreshes per game frame ([xr] hist). New rhi_b1_tool.inc. See README_B126.md.
b127 (2026-11-02 21:31 EST): version b1n127(vr). SHCLASS v2 measurement (BSI_B1_SHCLASS=1, default off = b126): captures from frame 3300 every 300, twin-exact wrap-shift S1 emulation with straddle/far split, exact sun N.L from the sun light draw's G-buffer normal + cb0[1], attenuation-RT reader census See README_B127.md.
b128 (2026-11-03 00:40 EST): version b1n128(vr). DXVK libs from dxvk_android_build_007 = 006 + libdxvk.a with BSI_DXVK_UNSYNC (env; 0 default = upstream 3.1.1; 1 = depth render passes may start unsynchronized, DXVK tracks hazards instead of a global end-of-pass barrier + drain; 2 = 1 + no 64-draw cap; [dxvk-unsync] counters). Roadmap P1-02 part 2. See README_B128.md.
b129 (2026-11-03 06:42 EST): version b1n129(vr). BSI_B1_SUNNL sun N.L PCSS skip (0 off default | 1 skip | 3 skip 3x3 | 2/4 CHECK: end-to-end light-buffer byte compare with garbage at skip pixels) + attenuation copy-pass census (rhi_b1_sunnl.h/.inc) See README_B129.md.
b130 (2026-11-03 07:11 EST): version b1n130(vr). BSI_B1_SUNNL CHECK fixed: mask via PS UAV u5 (no blending; build 129's blended mask marked 0 texels = vacuous CHECK), all channels of masked texels garbaged, non-attenuation copy draws mirrored into altcopy See README_B130.md.
b131 (2026-11-03 08:19 EST): version b1n131(vr). BSI_B1_SUNNL CHECK fix: mask redraw with stencil test + depth write off, mask-twin counters (PS u6: pixels / enabled / would skip), BSI_B1_SUNNL_CTL=1 positive control (garbage at every texel -> must differ) See README_B131.md.
b132 (2026-11-03 09:26 EST): version b1n132(vr). BSI_B1_SUNNL CHECK: the mask is recorded BY the real cascade draw (mask twin = original code + mask store + counters, same picture; u5/u6 bound for that draw incl. the right-eye replay); b131's post-draw redraw rasterised no pixel See README_B132.md.
b133 (2026-11-03 11:28 EST): version b1n133(vr). GPU counter readback fix: counter buffers (sunnl, S1 stats, S1 A/B, AO-blur A/B) declare HOST stage/access so DXVK emits the host barrier (they read 0 before); sunnl reads after ctx_flush/cs_sync/waitForIdle; SUNNL SELFTEST (compare of known different/identical images + transfer pattern) gates the CHECK See README_B133.md.
b134 (2026-11-03 13:09 EST): version b1n134(vr). BSI_B1_SKINPROBE=1 (NOT exact, timing-only ceiling probe, default off): the 12 bone-blend mul/mad of all GPU-skin VS replaced by movs from cb3[0..2] (skinned meshes collapse to bone 0) to size the 'skin once per frame' lever. New rhi_b1_skinprobe.h; hook in rhi_b1_shader.cpp b1dx_shader_create. See README_B134.md.
b135 (2026-11-03 13:15 EST): version b1n135(vr). atomic counter fix: STORAGE_BUFFER usage on every DXBC typed-buffer UAV with atomics (sunnl counters, S1 stats / A/B / atlas no-NaN flag, AO-blur A/B): dxbc-spirv lowers typed atomics to raw SSBO on Turnip and DXVK binds the raw descriptor, null before -> atomics dropped. SUNNL: readers by declared slot + cb0[0] channel masks vs cascade write mask, every reader CHECK-compared, garbage only in cascade channels See README_B135.md.
b136 (2026-11-03 15:11 EST): version b1n136(vr). SUNNL CHECK: reader redraw against a pre-draw depth-stencil snapshot (the real light draw changes stencil), DIFFER split inside/outside the skip mask, CTL=2 identity control, 64-bit counter totals, exposure census only once the cascade write mask is known See README_B136.md.
b137 (2026-11-04 03:50 EST, chat 4 fork30): version b1n137vr. BSI_B1_SUNNL=5 IN-SHADER exactness check of the sun N.L skip (twin of the sun light PS used by the real draw: recomputes its result with garbage attenuation where the skip would apply, bit compare, counters in PS u6; BSI_B1_SUNNL_CTL=1 positive control) + BSI_B1_SKINCENSUS=1 (skinned draws keyed by VB/IB/range/bone-constant hash, per pass class). New rhi_b1_skincensus.inc. Both default off. See README_B137.md.
b138 (2026-11-04 13:50 EST): version b1n138(vr). BSI_B1_SUNNL default ON (unset = 1, the centre-pixel sun N.L PCSS skip; =0 off; modes 2-5/CTL unchanged). User approved 4 Nov 13:48; proven exact by b137 SUNNL=5 (0 mismatches of 280 M Fair / 780 M Rooftops, positive control 1.23 G). See README_B138.md.
b139 (2026-11-04 20:16 EST): version b1n139(vr). BSI_B1_PCSSCENSUS=1 PCSS census (default off, picture unchanged): per projection shader draws/atlas sizes/viewport area, and every 97th S1 map build an L1 block-map readback counting bit-uniform non-clear 4x4/8x8 cells; [b1pcc] every 300 frames. Run with SHADOWEO=2 (pixel classes), SHADOWEO_PCF=2 (S1c safe share), DRAWPROF=1 (per-shader ms). See README_B139.md.
b140 (2026-11-04 21:52 EST): version b1n140(vr). render-thread lock trim: g_rmu = inline futex lock (BSI_B1_FASTLOCK, default 1, =0 std::mutex as before); RmuGuard/begin/end_batch use a global owner id instead of a thread_local (no dynamic-TLS resolver per context entry). Synchronisation only, command stream identical. See README_B140.md.
b143 (2026-11-05, chat 4 fork38): version b1n143vr. RTLOCK follows a restarted RenderingThread (BSI_B1_RTFOLLOW, default 1): fixes the Fair ~85 ms slow state. File: src_b1n/linux rhi_b1_dxvk.cpp. No list change.
b141 (2026-11-05, chat 5 CPU): version b1n141vr. Render-thread CPU trims, all exact (GPU command stream unchanged), each behind a switch (default 1, =0 = b140): C side BSI_B1_COMFAST2 / BSI_B1_IDMEMO / BSI_B1_PDFAST (+ BSI_CICACHE default 0, BSI_B1_DTSTAT default 0 = draw census off), C++ side BSI_B1_RTLOCK / BSI_B1_HOOKGATE / BSI_B1_DYNRAW / BSI_B1_ENTRYFAST. Files: src_b1n/linux rhi_b1_com.c, com.c, rt_linux.c, rhi_b1_dxvk.cpp, rhi_b1_m_ctx/objs/res/present.inc, rhi_b1_dyn.inc, rhi_b1_g4.inc. No list change. See README_B141.md.
b142 (2026-11-05 02:00 EST): version b1n142(vr). chat 4 fork35: BSI_B1_S1CX (default 0) S1c extension: per-tap reuse of the search bits on pixels S1c rejects (window straddles a binade) where the tap's search coordinate chain is exact in fp32 (exp(fl(u+k*dz)) <= exp(u) for the x prefix, same for the row's v); =2 in-shader check u7[40..45], BSI_B1_S1CX_CTL=1 positive control. Files rhi_b1_shadoweo_pcf.h, rhi_b1_shadoweo.inc. See README_B142.md.
b143 (2026-11-05 10:35 EST): version b1n143(vr). Build 143 = 142 + fork38: RTLOCK follows a restarted RenderingThread (BSI_B1_RTFOLLOW, default 1; 0 = b142). Fixes the Fair ~85 ms slow state (UE3 restarted RenderingThread; biased owner stale -> membarrier on every entry). File rhi_b1_dxvk.cpp. See README_B143.md.
b143 (2026-11-05 10:35 EST): version b1n143(vr). Build 143 = 142 + fork38: RTLOCK follows a restarted RenderingThread (BSI_B1_RTFOLLOW, default 1; 0 = b142). Fixes the Fair ~85 ms slow state. File src_b1n/linux/rhi_b1_dxvk.cpp. See README_B143.md.
b144 (2026-11-10 17:33): version b1n144(vr). Build 144 = 143 + BSI_READMAP (default 0 = off = b143): read map of the game files in hle_file.c. =1 (or =/path): one bit per 64 KiB block that ReadFile returned data from, per file under the game root, merged with earlier sessions, written to /sdcard/BSI/readmap.txt at most every 60 s while reads happen ([readmap] log lines). Purpose: find game data that is never read (installer trim option). I/O bookkeeping only; picture and command stream unchanged. File src_b1n/linux/hle_file.c. See README_B144.md."""
import glob
import argparse
import os
import platform
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

KIT = os.path.dirname(os.path.abspath(__file__))
RUNTIME = """
linux/rt_linux.c linux/hle_crt.c linux/hle_k32.c linux/hle_file.c linux/hle_more.c linux/hle_ui.c
linux/hle_input.c linux/hle_crt2.c linux/memdump.c linux/patches.c linux/natives.c linux/xaudio.c linux/emu.c linux/com.c
linux/com_tab.c linux/imps.c linux/hle_dll.c linux/hle_k32b.c linux/hle_eh.c eh/eh_rt.c linux/bridge.c linux/bridge_gen.c
linux/android_glue.c linux/xr_glue.c linux/vulkan_caps_probe.c linux/prof_android.c linux/dsp_probe.c linux/memwatch.c linux/leakwatch.c
linux/vr_head.c linux/setsweep.c linux/audio_native.c linux/rhi_hook.c linux/rhi_native.c linux/wwise_tune.c
linux/hle_crypt.c linux/mb_off.c linux/rhi_b1.c linux/rhi_b1_com.c linux/local_profile.c
linux/vr_stereo.c linux/xr_stereo_copy.c linux/vr_ui.c linux/vr_ui_rhi.c linux/vr_aim.c
linux/gear_mod.c linux/weapon_perks.c linux/weapon_wheel.c linux/economy.c
linux/legendary.c linux/ext_menu.c linux/autopilot.c linux/rhi_native_b1n.c
""".split()
DXVK_CPPS = ["linux/dxvk_core_probe.cpp", "linux/rhi_b1_dxvk.cpp", "linux/rhi_b1_shader.cpp"]


def run(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr).strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ndk", required=True)
    ap.add_argument("--output-dir", required=True, help="new directory under this kit")
    ap.add_argument("--jobs", type=int, default=4)
    args = ap.parse_args()
    host = {"Windows": "windows-x86_64", "Linux": "linux-x86_64", "Darwin": "darwin-x86_64"}.get(platform.system())
    if not host:
        sys.exit("unsupported host for the NDK build")
    prebuilt = os.path.join(args.ndk, "toolchains", "llvm", "prebuilt", host)
    clang = os.path.join(prebuilt, "bin", "clang" + (".exe" if host.startswith("windows") else ""))
    sysroot = os.path.join(prebuilt, "sysroot")
    resource_root = os.path.join(prebuilt, "lib", "clang")
    resources = os.path.join(resource_root, "18")
    if not os.path.isfile(clang) or not os.path.isdir(sysroot) or not os.path.isfile(os.path.join(resources, "include", "stddef.h")):
        sys.exit(f"NDK clang/sysroot/resource headers not found under {prebuilt}")
    out = os.path.abspath(os.path.join(KIT, args.output_dir))
    if os.path.commonpath([KIT, out]) != KIT:
        sys.exit("output directory must stay inside this v0.9.1 build folder")
    if os.path.exists(out):
        sys.exit(f"refusing to overwrite existing output directory: {out}")
    os.makedirs(os.path.join(out, "rt"))
    lib = os.path.join(out, "lib")
    os.makedirs(lib)
    src = os.path.join(KIT, "src")
    overlay = os.path.join(KIT, "src_b1n")
    vroverlay = os.path.join(KIT, "src_vr")
    def srcpath(rel):   # VR overlay first, then the b1n overlay, then the kit's own source
        v = os.path.join(vroverlay, rel)
        if os.path.isfile(v): return v
        o = os.path.join(overlay, rel)
        return o if os.path.isfile(o) else os.path.join(src, rel)
    base = [clang, "--target=aarch64-linux-android29", "--sysroot=" + sysroot,
            "-resource-dir=" + resources, "-fPIC"]
    cflags = ["-O2", "-mcpu=cortex-a78c", "-g", "-w", "-Wno-error=implicit-function-declaration",
              "-fno-strict-aliasing", "-DBSI_ANDROID", '-DBSI_VERSION="0.9.1-b1n144vr"',
              "-I" + os.path.join(src, "gen"), "-I" + os.path.join(vroverlay, "linux"), "-I" + os.path.join(overlay, "linux"), "-I" + os.path.join(src, "linux"), "-I" + os.path.join(KIT, "include")]

    def compile_one(rel):
        obj = os.path.join(out, "rt", os.path.splitext(os.path.basename(rel))[0] + ".o")
        return rel, *run(base + cflags + ["-c", srcpath(rel), "-o", obj])

    print(f"Compiling {len(RUNTIME)} v0.9.1 runtime files...")
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as pool:
        failures = [(rel, msg) for rel, code, msg in pool.map(compile_one, RUNTIME) if code]
    for rel, msg in failures:
        print(f"FAILED {rel}\n{msg}\n")
    if failures:
        sys.exit(f"{len(failures)} runtime compile(s) failed")

    dxvk = os.path.join(os.path.dirname(KIT), "dxvk_android_build_007")   # b128: 006 + libdxvk.a with BSI_DXVK_UNSYNC (default 0 = upstream), dxvk_android_build_007\README.txt; b81: DXVK rebuilt with -mcpu=cortex-a78c (inline LSE atomics), README_B81.md
    dxvk_src = os.path.join(os.path.dirname(KIT), "deps", "dxvk-v3.1.1")
    cpp = os.path.join(prebuilt, "bin", "clang++.exe" if host.startswith("windows") else "clang++")
    cpp_inc = [
        "-I" + os.path.join(dxvk, "src/dxvk/libdxvk.a.p"), "-I" + os.path.join(dxvk, "src/dxvk"), "-I" + os.path.join(dxvk_src, "src/dxvk"),
        "-I" + os.path.join(dxvk, "include"), "-I" + os.path.join(dxvk_src, "include"),
        "-I" + os.path.join(dxvk_src, "include/vulkan/include"),
        "-I" + os.path.join(dxvk_src, "include/spirv/include"),
        "-I" + os.path.join(dxvk, "include/native"), "-I" + os.path.join(dxvk_src, "include/native"),
        "-I" + os.path.join(dxvk_src, "include/native/windows"),
        "-I" + os.path.join(dxvk_src, "include/native/directx"),
        "-I" + os.path.join(dxvk, "subprojects/dxbc-spirv"), "-I" + os.path.join(dxvk_src, "subprojects/dxbc-spirv"),
        "-I" + dxvk,
    ]
    # B1.1 (2026-10-12): keep every DXVK/C++ symbol of the probe local to libbsi.so (the bridge loads its own DXVK).
    cpp_defs = ["-std=c++20", "-fvisibility=hidden", "-fvisibility-inlines-hidden", "-D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_FAST", "-DDXVK_WSI_ANDROID",
                "-fPIC", "-pthread", "-O2",
                # b81 (chat 5, C1a): -mcpu=cortex-a78c for the C++ files too. Without it clang targets generic armv8-a and
                # every Rc<> refcount in our emit lambdas became a call to __aarch64_ldadd4_acq_rel (9.2 % of CPU samples in
                # the Step 0 profile). With LSE guaranteed the atomics are single ldadd/cas instructions.
                "-mcpu=cortex-a78c"]
    for rel in DXVK_CPPS:
        cpp_out = os.path.join(out, "rt", os.path.splitext(os.path.basename(rel))[0] + ".o")
        print("Compiling " + rel + " ...")
        extra = ["-DBSI_B1_NO_LOGGER"] if rel.endswith("rhi_b1_dxvk.cpp") else []   # dxvk_core_probe.cpp owns DXVK's Logger
        code, msg = run([cpp, *base[1:], *cpp_defs, *extra, "-w", *cpp_inc, "-c", srcpath(rel), "-o", cpp_out])
        if code:
            sys.exit(rel + " compile failed:\n" + msg)

    objdir = os.path.join(KIT, "objs")
    vrobjdir = os.path.join(KIT, "objs_vr")   # stereo-hooked game parts replace the same-named ones
    vrobjs = set(f for f in os.listdir(vrobjdir) if f.endswith(".o")) if os.path.isdir(vrobjdir) else set()
    if vrobjs != {"part007.o", "part012.o"}:
        sys.exit("kit/objs_vr must hold exactly part007.o and part012.o (stereo hooks)")
    objs = sorted(os.path.join(vrobjdir if f in vrobjs else objdir, f) for f in os.listdir(objdir) if f.endswith(".o"))
    # apex_fix (2026-10-14): kit/objs_apex/ = emulator-leftover translations. A same-named object there replaces the one in
    # kit/objs/ (functab.o, the *missing.o stubs, exe parts); every other object there is added (fix_*.o). Optional folder.
    apexdir = os.path.join(KIT, "objs_apex")
    if os.path.isdir(apexdir):
        apexobjs = sorted(f for f in os.listdir(apexdir) if f.endswith(".o"))
        if set(apexobjs) & vrobjs:
            sys.exit("kit/objs_apex must not contain the stereo-hooked parts " + ", ".join(sorted(set(apexobjs) & vrobjs)))
        if "functab.o" not in apexobjs or not any(f.startswith("fix_") for f in apexobjs):
            sys.exit("kit/objs_apex is incomplete: it needs functab.o and the fix_*.o objects together")
        objs = [os.path.join(apexdir, os.path.basename(o)) if os.path.basename(o) in apexobjs else o for o in objs]
        objs += [os.path.join(apexdir, f) for f in apexobjs if not os.path.isfile(os.path.join(objdir, f))]
        print(f"objs_apex: {sum(1 for f in apexobjs if os.path.isfile(os.path.join(objdir, f)))} objects replaced, "
              f"{sum(1 for f in apexobjs if not os.path.isfile(os.path.join(objdir, f)))} added")
    objs += sorted(os.path.join(out, "rt", f) for f in os.listdir(os.path.join(out, "rt")) if f.endswith(".o"))
    unidir = os.path.join(KIT, "prebuilt", "unicorn")
    objs += [os.path.join(unidir, f) for f in ("libunicorn-static.a", "libx86_64-softmmu.a", "libunicorn-common.a")]
    rsp = os.path.join(out, "link.rsp")
    with open(rsp, "w", encoding="utf-8") as f:
        f.write("\n".join('"' + p.replace("\\", "/") + '"' for p in objs))
    so = os.path.join(lib, "libbsi.so")
    core_libs = [
        os.path.join(dxvk, "src/dxvk/libdxvk.a"), os.path.join(dxvk, "src/wsi/libwsi.a"),
        os.path.join(dxvk, "src/spirv/libspirv.a"), os.path.join(dxvk, "src/vulkan/libvkcommon.a"),
        os.path.join(dxvk, "src/util/libutil.a"), os.path.join(dxvk, "subprojects/dxbc-spirv/libdxbc_spv.a"),
        os.path.join(dxvk, "subprojects/libdisplay-info/libdisplay-info.a"),
    ]
    missing = [p for p in core_libs if not os.path.isfile(p)]
    if missing:
        sys.exit("DXVK Android static libraries missing: " + ", ".join(missing))
    android_lib = os.path.join(sysroot, "usr", "lib", "aarch64-linux-android")
    unwind_cands = [os.path.join(android_lib, "libunwind.a")] + sorted(
        glob.glob(os.path.join(resources, "lib", "linux", "aarch64", "libunwind.a")) +
        glob.glob(os.path.join(resources, "lib", "linux", "libunwind-aarch64.a")))
    unwind_lib = next((c for c in unwind_cands if os.path.exists(c)), None)
    if not unwind_lib:
        sys.exit("libunwind.a not found in the NDK (looked in: " + ", ".join(unwind_cands) + ")")
    cmd = base + ["-shared", "-o", so, "@" + rsp, "-fuse-ld=lld", "-rtlib=compiler-rt", "--unwindlib=none",
                  "-Wl,--start-group", *core_libs, "-Wl,--end-group",
                  # 2026-10-12 fix (Claude): no "-L" + android_lib. That folder holds Android's STATIC libc.a/libm.a/libdl.a,
                  # and -L made -lm/-ldl/-lc pick them, which put bionic's gwp_asan initial-exec TLS into libbsi.so and made
                  # Android refuse to load it (dlopen "TLS symbol ... using IE access model"; builds 45-48 never started).
                  # Link only the C++ runtime archives by full path; libc/libm/libdl stay the shared system libraries.
                  os.path.join(android_lib, "libc++_static.a"), os.path.join(android_lib, "libc++abi.a"), unwind_lib,
                  "-Wl,--no-undefined", "-Wl,-Bsymbolic-functions", "-landroid", "-laaudio", "-llog", "-lEGL", "-lm", "-ldl",
                  "-Wl,-z,max-page-size=16384", "-Wl,--export-dynamic",
                  # B1.1: symbols from static archives (DXVK core, dxbc-spirv, libc++, unicorn) are not exported, so the
                  # bridge's libdxvk_d3d11.so can never bind to our DXVK copy. Our own objects stay exported as before.
                  "-Wl,--exclude-libs,ALL"]
    print("Linking v0.9.1 plus the b1 native renderer (opt-in, BSI_RHI=b1n)...")
    code, msg = run(cmd)
    if code:
        sys.exit("link failed:\n" + msg)
    # Load-safety check (2026-10-12): Android refuses a dlopen'd library that carries initial-exec TLS
    # (static bionic, gwp_asan). Fail the build here instead of on the Quest.
    readelf = os.path.join(os.path.dirname(clang), "llvm-readelf" + (".exe" if os.name == "nt" else ""))
    rc, dyn = run([readelf, "--dynamic", so])
    rc2, syms = run([readelf, "--syms", so])
    if rc or rc2:
        sys.exit("llvm-readelf failed; cannot verify the library:\n" + dyn + syms)
    if "STATIC_TLS" in dyn or "gwp_asan" in syms:
        sys.exit("libbsi.so contains static libc / initial-exec TLS (STATIC_TLS or gwp_asan): Android would refuse to load it")
    rc3, dsyms = run([readelf, "--dyn-syms", "--wide", so])
    leaked = [l for l in dsyms.splitlines() if "N4dxvk" in l and " UND " not in l]
    if rc3 or leaked:
        sys.exit("libbsi.so exports DXVK symbols (B1.1 requires none):\n" + "\n".join(leaked[:20]))
    print("load-safety check passed (no STATIC_TLS, no gwp_asan, no exported DXVK symbols)")
    for name in os.listdir(os.path.join(KIT, "prebuilt", "so")):
        if name.endswith(".so"):
            shutil.copy2(os.path.join(KIT, "prebuilt", "so", name), os.path.join(lib, name))
    print(f"linked {so} ({os.path.getsize(so)} bytes); no APK was installed")


if __name__ == "__main__":
    main()
