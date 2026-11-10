#!/bin/sh
# VR stereo + UI layer + controller-aim hooks (notes/VR_STEREO.md §11). Supersedes regress/vr_stereo_part007.sh.
# Rebuilds two parts of the generated code with call sites routed to linux/vr_stereo.c, linux/vr_ui.c (logic) - the
# generated code itself is never committed; this script recreates the hooked copies from gen_promo/:
#   part007:
#     f_006633b0 (ULocalPlayer::CalcSceneView) wrapped: from UGameViewportClient::Draw (return 0x66a91c) it runs once per eye
#       (or once over the eye area, mask 4, when the UI column is on and stereo is not); the viewport size (FViewport vf 8/0xc)
#       is passed so the [ L | R | UI ] layout can be used; a plain call (mask 0) still lets the view ctor apply the
#       controller-aim view correction (vr_stereo_csv_mono);
#     the FSceneView ctor call at 0x664119 -> vr_stereo_view_ctor_args() (per-eye View/Projection, aim correction);
#     in UGameViewportClient::Draw (f_0066a3e0):
#       0x66ae03, 0x66b060  DrawTile of the areas no view covers -> vr_ui_tile (UI column cleared to alpha 0)
#       0x66ae0f            PopTransform after the cover tiles   -> vr_ui_after_cover (HUD view -> UI rect, UI-only tag)
#       0x66b32a            FSubtitleManager::DisplaySubtitles   -> rect mapped into the UI rect
#       0x66b36f            UCanvas::Update of the full-viewport canvas -> size = UI rect + translation pushed
#   part012:
#     f_00856ca0 (FGFxEngine viewport layout): GFxMovieView::SetViewport vcalls at 0x856e5a (kept, counted), 0x856fbd
#       (full viewport) and 0x857157 (bFillOnlyPlayerOwnerViewport) -> vr_ui_gfx_viewport (rect mapped into the UI rect)
# Every hook is a pass-through unless the stereo layout started with ui=1 (or, for the aim path, vr_aim.txt is on).
#
#   TARGET=android (default): NDK clang + objl_and_promo/.genflags -> $OUT/part007.o, part012.o  (OUT default /tmp/claude-0/ovr_vrui)
#   TARGET=linux:             gcc + objl_promo/.genflags            -> $OUT/part007.o, part012.o  (OUT default /tmp/claude-0/ovr_vrui_linux)
#   PARTS="007 012" (default) to build only some.
# Android: every object of BASE_OVR (default /tmp/claude-0/ovr_087: head-look part005 + natives parts) is copied next to them, so one
# override dir carries all hooks:  OVR=$OUT OBJDIR=objl_and_promo sh <link script>
# Runtime files to add to the link list: linux/vr_stereo.c linux/xr_stereo_copy.c linux/vr_ui.c linux/vr_ui_rhi.c linux/vr_aim.c
set -e
# v0.9.1 copy (stereo_kit, 2026-10-13): sources from GEN (default /tmp/claude-0/full091/gen = the v0.9.1 "top" stack,
# BSI_PROMO_XMM/FLAGABI/EA64/STRFAST/INLIMP generated parts) compiled with their own cpu.h/funcs.h and the same flags
# full091/ovr_lin|ovr_and used (regress/xmm_abi/build_ovr.sh: objl_promo/.genflags | NDK clang + objl_and_promo/.genflags).
# No BASE_OVR merge here (the v0.9.1 Android OVR dir is full091/ovr_and; combine at link time).
GEN=${GEN:-/tmp/claude-0/full091/gen}
cd /home/claude/bsi/recomp
TARGET=${TARGET:-android}
PARTS=${PARTS:-"007 012"}
if [ "$TARGET" = linux ]; then OUT=${OUT:-/tmp/claude-0/ovr_vrui_linux}; else OUT=${OUT:-/tmp/claude-0/ovr_vrui}; fi
W=${WORK:-/tmp/claude-0/vrui_src.$$}
mkdir -p $W $OUT
one(){   # $1 = pattern that must match exactly one line in $2 (between lines $3..$4 if given)
  if [ -n "$3" ]; then N=$(sed -n "$3,$4p" $2 | grep -c "$1" || true); else N=$(grep -c "$1" $2 || true); fi
  [ "$N" = 1 ] || { echo "hook site '$1': $N matches in $2 (expected 1)"; exit 1; }
}
compile(){   # $1 = source, $2 = object
  if [ "$TARGET" = linux ]; then
    gcc $(cat objl_promo/.genflags) -I$GEN -c $1 -o $2
  else
    S=/home/claude/a64/ndk/android-ndk-r27c/toolchains/llvm/prebuilt/linux-x86_64
    clang --target=aarch64-linux-android29 --sysroot=$S/sysroot -resource-dir $(ls -d $S/lib/clang/* | head -1) -fPIC $(cat objl_and_promo/.genflags) -I$GEN -c $1 -o $2
  fi
}

for P in $PARTS; do
case $P in
007)
SRC=$GEN/part007.c
L=$(grep -n '^void f_006633b0(CPU\*c){' $SRC | cut -d: -f1); [ -n "$L" ] || { echo "f_006633b0 not found"; exit 1; }
E=$(awk -v s=$L 'NR>s && /^void f_/{print NR; exit}' $SRC); [ -n "$E" ] || { echo "end of f_006633b0 not found"; exit 1; }
one 'PUSH32L(0x66411eu);.*f_0072ce80(c);' $SRC $L $E
one 'PUSH32L(0x66a91cu);.*f_006633b0(c);' $SRC
one 'PUSH32L(0x66ae08u);.*f_0073ab10(c);' $SRC
one 'PUSH32L(0x66b065u);.*f_0073ab10(c);' $SRC
one 'PUSH32L(0x66ae14u);.*f_006f8a00(c);' $SRC
one 'PUSH32L(0x66b32fu);.*f_006d67f0(c);' $SRC
one 'PUSH32L(0x66b374u);.*f_006d9d90(c);' $SRC
sed -e "${L}s/^void f_006633b0(CPU\*c){/static void vr_stereo_ctor(CPU*c);\nstatic void f_006633b0_orig(CPU*c){/" \
    -e "${L},${E}s/\(PUSH32L(0x66411eu);.*\)f_0072ce80(c);/\1vr_stereo_ctor(c);/" \
    -e "s/\(PUSH32L(0x66ae08u);.*\)f_0073ab10(c);/\1vr_ui_tile_w(c);/" \
    -e "s/\(PUSH32L(0x66b065u);.*\)f_0073ab10(c);/\1vr_ui_tile_w(c);/" \
    -e "s/\(PUSH32L(0x66ae14u);.*\)f_006f8a00(c);/\1vr_ui_cover_w(c);/" \
    -e "s/\(PUSH32L(0x66b32fu);.*\)f_006d67f0(c);/\1vr_ui_sub_w(c);/" \
    -e "s/\(PUSH32L(0x66b374u);.*\)f_006d9d90(c);/\1vr_ui_canvas_w(c);/" $SRC > $W/part007.c
# the wrappers are referenced before their definitions: forward declarations right after the includes
sed -i "0,/^void f_/s//static void vr_ui_tile_w(CPU*c); static void vr_ui_cover_w(CPU*c); static void vr_ui_sub_w(CPU*c); static void vr_ui_canvas_w(CPU*c);\nvoid f_/" $W/part007.c
cat >> $W/part007.c <<'EOT'

/* ---- VR stereo / UI / aim hooks (regress/vr/vr_hook_parts.sh; logic in linux/vr_stereo.c, linux/vr_ui.c) ---- */
int vr_stereo_csv_begin2(uint32_t lp, uint32_t ret, int vw, int vh); void vr_stereo_csv_mono(uint32_t ret); int vr_stereo_ui_on(void);
int vr_stereo_need_viewstate2(void); void vr_stereo_set_viewstate2(uint32_t vs);
void vr_stereo_csv_eye(uint32_t lp, int eye); void vr_stereo_csv_end(uint32_t lp, uint32_t v0, uint32_t v1); void vr_stereo_view_ctor_args(uint32_t esp);
typedef struct { float x, y, w, h; int clear; } VrTile;
int vr_ui_tile(float x, float y, float w, float h, int vw, int vh, VrTile out[2]); void vr_ui_after_cover(int vw, int vh);
void vr_ui_subtitle_rect(uint32_t rect, int vw, int vh); int vr_ui_full_canvas(uint32_t ucanvas, int vw, int vh, uint32_t *matrix);
uint32_t vr_ui_zero_color(void);
static float vr_f32(uint32_t a){ uint32_t v = R32(a); float f; memcpy(&f, &v, 4); return f; }
static void vr_wf32(uint32_t a, float f){ uint32_t v; memcpy(&v, &f, 4); W32(a, v); }
/* thiscall virtual with no arguments (FRenderTarget::GetSizeX = vf 8, GetSizeY = vf 0xc); CPU state restored */
static uint32_t vr_vcall0(CPU*c, uint32_t obj, uint32_t off){
  CPU s = *c; c->ecx = obj; PUSH32(c, 0x66a91cu); call_indirect(c, R32(R32(obj) + off)); uint32_t r = c->eax; *c = s; return r; }
static void vr_rt_size(CPU*c, uint32_t rt, int *w, int *h){ *w = *h = 0; if(rt >= 0x10000u && rt < 0xC0000000u){ *w = (int)vr_vcall0(c, rt, 8); *h = (int)vr_vcall0(c, rt, 12); } }
static void vr_stereo_ctor(CPU*c){ vr_stereo_view_ctor_args(c->esp); f_0072ce80(c); }   /* FSceneView ctor, from CalcSceneView only */
void f_006633b0(CPU*c){
  uint32_t lp = c->ecx, esp0 = c->esp, ret = R32(esp0);
  int vw = 0, vh = 0;
  if(ret == 0x66a91cu && vr_stereo_ui_on()) vr_rt_size(c, R32(esp0 + 16), &vw, &vh);   /* arg 3 = FViewport* */
  int mask = vr_stereo_csv_begin2(lp, ret, vw, vh);   /* 1 left, 2 right, 3 both, 4 one view over the eye area; 0 = untouched */
  if(!mask){ vr_stereo_csv_mono(ret); f_006633b0_orig(c); return; }
  if(mask == 4){ vr_stereo_csv_eye(lp, 2); f_006633b0_orig(c); vr_stereo_csv_end(lp, c->eax, 0); return; }
  if(vr_stereo_need_viewstate2()){   /* f_00731760 = AllocateViewState() (cdecl, no args), as in the ULocalPlayer ctor at 0x661620 */
    CPU s = *c; PUSH32(c, 0x66a91cu); f_00731760(c); uint32_t vs = c->eax; *c = s; vr_stereo_set_viewstate2(vs); }
  uint32_t frame[6]; for(int i = 0; i < 6; i++) frame[i] = R32(esp0 + 4u * i);   /* return address + 5 args: the callee may reuse arg slots */
  CPU s = *c, r0 = *c; uint32_t v[2] = { 0, 0 }; int first = 1;
  for(int e = 0; e < 2; e++){
    if(!(mask >> e & 1)) continue;
    if(!first){ *c = s; for(int i = 0; i < 6; i++) W32(esp0 + 4u * i, frame[i]); }
    vr_stereo_csv_eye(lp, e); f_006633b0_orig(c); v[e] = c->eax;
    if(first){ r0 = *c; first = 0; if(!v[e]) break; }   /* no view (no player actor / zero size): stop */
  }
  *c = r0;   /* return the first eye's view and machine state, as one normal call would */
  vr_stereo_csv_end(lp, v[0], v[1]);
}
/* DrawTile(FCanvas*, X, Y, SizeX, SizeY, U, V, SizeU, SizeV, const FLinearColor&, const FTexture*, UBOOL AlphaBlend), cdecl */
static void vr_ui_tile_w(CPU*c){
  if(!vr_stereo_ui_on()){ f_0073ab10(c); return; }
  uint32_t a = c->esp + 4; int vw, vh; vr_rt_size(c, R32(R32(a) + 4), &vw, &vh);   /* FCanvas+4 = its FRenderTarget */
  VrTile t[2]; int n = vr_ui_tile(vr_f32(a + 4), vr_f32(a + 8), vr_f32(a + 12), vr_f32(a + 16), vw, vh, t);
  if(!n){ f_0073ab10(c); return; }
  uint32_t sv[12]; for(int i = 0; i < 12; i++) sv[i] = R32(a + 4u * i);
  CPU s = *c;
  for(int k = 0; k < n; k++){
    *c = s; for(int i = 0; i < 12; i++) W32(a + 4u * i, sv[i]);
    vr_wf32(a + 4, t[k].x); vr_wf32(a + 8, t[k].y); vr_wf32(a + 12, t[k].w); vr_wf32(a + 16, t[k].h);
    if(t[k].clear){ uint32_t z = vr_ui_zero_color(); if(z){ W32(a + 36, z); W32(a + 40, 0); W32(a + 44, 0); } }   /* (0,0,0,0), no texture, opaque */
    f_0073ab10(c);
  }
  for(int i = 0; i < 12; i++) W32(a + 4u * i, sv[i]);
}
/* FCanvas::PopTransform after the cover tiles (ecx = FCanvas): the renderer has the views; now lay the UI out */
static void vr_ui_cover_w(CPU*c){
  if(vr_stereo_ui_on()){ int vw, vh; vr_rt_size(c, R32(c->ecx + 4), &vw, &vh); vr_ui_after_cover(vw, vh); }
  f_006f8a00(c);
}
/* FSubtitleManager::DisplaySubtitles(FCanvas*, FIntRect&), thiscall */
static void vr_ui_sub_w(CPU*c){
  if(vr_stereo_ui_on()){ int vw, vh; vr_rt_size(c, R32(R32(c->esp + 4) + 4), &vw, &vh); vr_ui_subtitle_rect(R32(c->esp + 8), vw, vh); }
  f_006d67f0(c);
}
/* UCanvas::Update of the full-viewport canvas (ecx = UCanvas, SizeX/SizeY = viewport size just written by Draw) */
static void vr_ui_canvas_w(CPU*c){
  uint32_t uc = c->ecx, mat = 0;
  int on = vr_stereo_ui_on() && vr_ui_full_canvas(uc, (int)R32(uc + 0x54), (int)R32(uc + 0x58), &mat);
  f_006d9d90(c);
  if(on && mat){ uint32_t fc = R32(c->esp + 0x24);   /* Draw's FCanvas* (its [esp+0x24], as used right after this call) */
    if(fc >= 0x10000u && fc < 0xC0000000u){ CPU s = *c; c->ecx = fc; PUSH32(c, mat); PUSH32(c, 0x66b374u); f_006eb1f0(c); *c = s; } }   /* PushAbsoluteTransform */
}
EOT
compile $W/part007.c $OUT/part007.o
rm -f $W/part007.c
echo "part007 hooked -> $OUT/part007.o"
;;
012)
SRC=$GEN/part012.c
one 'PUSH32L(0x856e5cu);.*call_indirect(c,_t);' $SRC
one 'PUSH32L(0x856fbfu);.*call_indirect(c,_t);' $SRC
one 'PUSH32L(0x857159u);.*call_indirect(c,_t);' $SRC
sed -e "s/\(PUSH32L(0x856e5cu);.*\)call_indirect(c,_t);/\1vr_ui_gfx_vp(c,_t,0);/" \
    -e "s/\(PUSH32L(0x856fbfu);.*\)call_indirect(c,_t);/\1vr_ui_gfx_vp(c,_t,1);/" \
    -e "s/\(PUSH32L(0x857159u);.*\)call_indirect(c,_t);/\1vr_ui_gfx_vp(c,_t,2);/" $SRC > $W/part012.c
sed -i "0,/^void f_/s//static void vr_ui_gfx_vp(CPU*c, uint32_t t, int site);\nvoid f_/" $W/part012.c
cat >> $W/part012.c <<'EOT'

/* ---- VR UI layer: Scaleform viewports into the UI rect (regress/vr/vr_hook_parts.sh, logic in linux/vr_ui.c) ---- */
void vr_ui_gfx_viewport(uint32_t pview, uint32_t vp, int site);
/* GFxMovieView::SetViewport(const GViewport&, ...), thiscall: ecx = the movie view, arg 0 = the GViewport */
static void vr_ui_gfx_vp(CPU*c, uint32_t t, int site){ vr_ui_gfx_viewport(c->ecx, R32(c->esp + 4), site); call_indirect(c, t); }
EOT
compile $W/part012.c $OUT/part012.o
rm -f $W/part012.c
echo "part012 hooked -> $OUT/part012.o"
;;
*) echo "unknown part $P"; exit 1;;
esac
done
rmdir $W 2>/dev/null || true
echo VR_HOOK_PARTS_OK $OUT
