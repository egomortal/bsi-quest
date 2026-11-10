/* rhi_b1_com.c -- B1.5 at the COM seam (guest side): mirror the game's D3D11 device/context calls onto the b1 renderer.
   Work log "B1 plan", decision of 12 Oct: the RHI slots that draw do engine-internal work (constant-buffer commit, state
   caches), so instead of reimplementing them we take the D3D11 call stream they produce (about 45 methods) and run it
   on our own DXVK device (rhi_b1_dxvk.cpp). No engine structures are needed. The RHI seam (rhi_hook.c) stays for
   install timing and for later engine-level shortcuts.
   Hooks: com.c com_dispatch -> rhi_b1_com (every COM call, caller thread, before the bridge and its queue);
          bridge.c bridge_pre/bridge_post -> b1com_create_pre/post (device creations: out pointer = the guest proxy).
   Guest objects are keyed by their proxy pointer; a creation at a reused proxy address releases the old shadow object. */
#include "hle.h"
#include "com.h"
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* rhi_b1_dxvk.cpp / rhi_b1_shader.cpp */
uint32_t b1dx_buffer_create2(uint32_t size, uint32_t bind, uint32_t usage, const void *data);
uint32_t b1dx_tex2d_create(uint32_t w, uint32_t h, uint32_t mips, uint32_t layers, uint32_t dxgi, uint32_t bind, uint32_t misc,
                           const void *const *subdata, const uint32_t *subpitch);
void b1dx_release(uint32_t id);
uint32_t b1dx_shader_create(int stage, const void *code, uint32_t len, int compile_now);
void b1dx_shader_release(uint32_t id);
uint32_t b1dx_view_create(int type, uint32_t res, uint32_t fmt, uint32_t dim, uint32_t a, uint32_t b, uint32_t c, uint32_t d, int hasdesc);
uint32_t b1dx_il_create(const int32_t *elems, uint32_t n);
int b1dx_isgn_lookup(const void *code, uint32_t len, const char *const *names, const uint32_t *idx, uint32_t n, int32_t *out);
uint32_t b1dx_blend_create(const uint32_t *d);
uint32_t b1dx_dss_create(const uint32_t *d);
uint32_t b1dx_rs_create(const uint32_t *d);
uint32_t b1dx_sampler_create(const uint32_t *d);
void b1dx_cobj_release(int kind, uint32_t id);
void b1dx_c_shader(int st, uint32_t sh);
void b1dx_c_cbs(int st, uint32_t start, uint32_t n, const uint32_t *ids);
void b1dx_c_srvs(int st, uint32_t start, uint32_t n, const uint32_t *ids);
void b1dx_c_samplers(int st, uint32_t start, uint32_t n, const uint32_t *ids);
void b1dx_c_il(uint32_t id);
void b1dx_c_vbs(uint32_t start, uint32_t n, const uint32_t *ids, const uint32_t *strides, const uint32_t *offsets);
void b1dx_c_ib(uint32_t id, uint32_t fmt, uint32_t off);
void b1dx_c_topology(uint32_t t);
void b1dx_c_rs(uint32_t id);
void b1dx_c_viewports(uint32_t n, const float *v);
void b1dx_c_scissors(uint32_t n, const int32_t *r);
void b1dx_c_blend(uint32_t id, const float *factor, uint32_t mask);
void b1dx_c_dss(uint32_t id, uint32_t ref);
void b1dx_c_rts(uint32_t n, const uint32_t *rtvs, uint32_t dsv);
void b1dx_c_clear_rtv(uint32_t id, const float *rgba);
void b1dx_c_clear_dsv(uint32_t id, uint32_t flags, float depth, uint32_t stencil);
void b1dx_c_update(uint32_t id, uint32_t sub, const uint32_t *box, const void *data, uint32_t rowpitch, uint32_t slicepitch);
uint32_t b1dx_tex3d_create(uint32_t w, uint32_t h, uint32_t d, uint32_t mips, uint32_t dxgi, uint32_t bind, const void *const *subdata, const uint32_t *rowp, const uint32_t *slicep);
void b1dx_c_unmap(uint32_t id, uint32_t sub, const void *data, uint32_t rowpitch, uint32_t slicepitch, int discard);
void b1dx_c_copy_region(uint32_t dst, uint32_t dsub, uint32_t x, uint32_t y, uint32_t z, uint32_t src, uint32_t ssub, const uint32_t *box);
void b1dx_c_draw(uint32_t count, uint32_t inst, uint32_t first, uint32_t firstinst);
void b1dx_c_uavs(uint32_t start, uint32_t n, const uint32_t *ids);
void b1dx_c_dispatch(uint32_t x, uint32_t y, uint32_t z);
void b1dx_c_clear_uav(uint32_t view, const uint32_t *vals, int isfloat);
void b1dx_c_genmips(uint32_t srv);
void b1dx_compute_stats(long *disp, long *clr, long *gm);
void b1dx_c_draw_indexed(uint32_t count, uint32_t inst, uint32_t first, int32_t base, uint32_t firstinst);
uint32_t b1dx_bb_id(void);
uint32_t b1dx_backbuffer(uint32_t w, uint32_t h);
int  b1dx_c_map_discard(uint32_t id);
uint32_t b1dx_buf_info(uint32_t id, int *hostvis);
void b1dx_c_bufwrite(uint32_t id, uint32_t off, uint32_t size, const void *data);
int  b1dx_end_frame(void);
void b1dx_dump_request(int tag);
int  b1dx_present_init(void *hostwnd, uint32_t w, uint32_t h);
void b1dx_present_resize(uint32_t w, uint32_t h);
int  b1dx_present(uint32_t sync_interval);
void b1dx_dump_path(const char *path);
void b1dx_com_stats(char *out, int n);
uint32_t b1dx_query_create(uint32_t type); void b1dx_query_release(uint32_t id);
void b1dx_c_query_begin(uint32_t id); void b1dx_c_query_end(uint32_t id); int b1dx_c_query_data(uint32_t id, void *out, uint32_t size, uint32_t flags);
int b1dx_query_peek(uint32_t id, uint64_t *samples);
static int qcmp(void) { static int v = -1; if (v < 0) v = getenv("BSI_B1_QCMP") != 0; return v; }
void *br_handle(uint32_t g);
int  br_native_present(char *path, int n);
int  rhi_b1_dev(void);   /* rhi_b1.c: start the shadow device once; 1 = up */

void b1com_stats(char *out, int n); static void b1_perf_line(int frames);
extern int g_bridge_on;   /* bridge.c */
enum { K_RES = 1, K_SH = 2, K_VIEW = 3, K_IL = 4, K_BS = 5, K_DSS = 6, K_RS = 7, K_SS = 8, K_BB = 9, K_Q = 10 };

/* ---- proxy -> shadow object ---- */
#define CO_N (1u << 17)
typedef struct { uint32_t p; uint16_t kind, sub; uint32_t id; } CObj;
static CObj g_co[CO_N];
static pthread_mutex_t g_co_mu = PTHREAD_MUTEX_INITIALIZER;
static long g_co_full, g_unknown_refs, g_unk_by[256];
static int t_cur = 255;   /* context method being mirrored (diagnostics). perf73: a plain global (it only names the method in the
                            unknown-object log): a dynamic-TLS write per context call cost a resolver call on the Quest */
static CObj *co_slot(uint32_t p, int create) {   /* caller holds g_co_mu */
  uint32_t i = (p * 2654435761u) >> 15;
  for (uint32_t n = 0; n < CO_N; n++, i = (i + 1) & (CO_N - 1)) {
    if (g_co[i].p == p) return &g_co[i];
    if (!g_co[i].p) { if (!create) return 0; g_co[i].kind = 0; g_co[i].id = 0; __atomic_store_n(&g_co[i].p, p, __ATOMIC_RELEASE); return &g_co[i]; }
  }
  g_co_full++; return 0;
}
/* perf: lookups take no lock. The table is insert-only (an entry is never removed, only rebound when the guest reuses a
   proxy address, and the guest cannot use a proxy while it is being recreated), and a slot is published by its proxy
   field (release) after its other fields were cleared, so a reader sees either a complete entry or none. Writers
   (co_bind) still serialise on g_co_mu. */
static CObj *co_find(uint32_t p) {
  uint32_t i = (p * 2654435761u) >> 15;
  for (uint32_t n = 0; n < CO_N; n++, i = (i + 1) & (CO_N - 1)) {
    uint32_t q = __atomic_load_n(&g_co[i].p, __ATOMIC_ACQUIRE);
    if (q == p) return &g_co[i];
    if (!q) return 0;
  }
  return 0;
}
/* perf73: in-object copy of the binding. co_find probes a 1.5 MB table at a hashed (cache-missing) slot for every bound
   object of every context call (co_get + ids_of ~4 % of the Quest render thread). In native mode (b1n) every proxy is a
   64-byte null-device object (com.c newobj, zeroed at creation) whose bytes 48..59 are unused in native mode (bridge_fast.c
   tags +56/+60 only with the D3D11 bridge on); the binding is mirrored there
   (id, kind | sub << 16, tag = proxy ^ PX_TAG) whenever the table entry changes, so a lookup reads the object the game has
   just touched. The table stays the authority: no tag (or another mode) = the table lookup as before. BSI_B1_PXCACHE=0: off;
   =2: verify (every lookup also asks the table; mismatches counted in the [b1] COM line). */
#define PX_OFF 48u
#define PX_TAG 0xB1C0DE5Au
static int g_px = -1; static long g_px_bad, g_px_hit;
static int native(void);
static inline int px_on(void) { if (g_px < 0) { const char *e = getenv("BSI_B1_PXCACHE"); g_px = native() ? (e ? atoi(e) : 1) : 0; } return g_px; }
static inline void px_set(uint32_t p, int kind, int sub, uint32_t id) {   /* caller holds g_co_mu */
  if (!px_on() || !p) return;
  uint32_t *w = (uint32_t *)MEMP(p + PX_OFF);
  __atomic_store_n(&w[2], 0u, __ATOMIC_RELAXED);   /* untag while rewriting (seqlock: untag, fence, data, release tag) */
  __atomic_thread_fence(__ATOMIC_RELEASE);
  __atomic_store_n(&w[0], id, __ATOMIC_RELAXED); __atomic_store_n(&w[1], (uint32_t)kind | ((uint32_t)sub << 16), __ATOMIC_RELAXED);
  if (kind) __atomic_store_n(&w[2], p ^ PX_TAG, __ATOMIC_RELEASE);
}
/* 1 = answered from the object (*id may be 0 = kind mismatch, as co_id_of); 0 = ask the table.
   c5b141: *kout = the kind the binding has (K_BB when the back buffer stood in for K_RES), for the id memo below */
static inline int px_get(uint32_t p, int kind, int *sub, uint32_t *id, int *kout) {
  const uint32_t *w = (const uint32_t *)MEMP(p + PX_OFF);
  if (__atomic_load_n(&w[2], __ATOMIC_ACQUIRE) != (p ^ PX_TAG)) return 0;
  uint32_t ks = __atomic_load_n(&w[1], __ATOMIC_RELAXED), i = __atomic_load_n(&w[0], __ATOMIC_RELAXED); int k = (int)(ks & 0xffff);
  __atomic_thread_fence(__ATOMIC_ACQUIRE);
  if (__atomic_load_n(&w[2], __ATOMIC_RELAXED) != (p ^ PX_TAG)) return 0;   /* rewritten meanwhile */
  *kout = k;   /* c5b141 */
  if (k != kind && !(kind == K_RES && k == K_BB)) { *id = 0; return 1; }
  if (sub) *sub = (int)(ks >> 16);
  *id = k == K_BB ? b1dx_bb_id() : i; return 1;
}
static inline uint32_t co_id_of(CObj *o, int kind, int *sub, int *kout) {
  int k = __atomic_load_n(&o->kind, __ATOMIC_RELAXED);
  *kout = k;   /* c5b141 */
  if (k != kind && !(kind == K_RES && k == K_BB)) return 0;
  if (sub) *sub = o->sub;
  return k == K_BB ? b1dx_bb_id() : __atomic_load_n(&o->id, __ATOMIC_RELAXED);
}
static void db_flush_any(void);   /* BSI_B1_DRAWBATCH: issue the render thread's deferred state sets (from any thread) */
static void obj_release(int kind, uint32_t id) {
  if (!id) return;
  db_flush_any();   /* DRAWBATCH hazard (a): a deferred Set must reach the renderer before the id it names can be freed / reused */
  switch (kind) { case K_RES: b1dx_release(id); break; case K_SH: b1dx_shader_release(id); break; case K_BB: break; case K_Q: b1dx_query_release(id); break; default: b1dx_cobj_release(kind, id); break; }
}
/* the null device freed a guest object (refcount 0): release its shadow now instead of when the address is reused.
   13 Oct: shadows lived until address reuse, so DXVK samplers piled up past its 2048-sampler pool. BSI_B1_FREEHOOK=0: off */
/* shadow-mode diagnostics: when the bridge dumps every draw of a frame (BSI_DUMPFRAME), dump ours too, same order:
   <dumpdir>/b1_f<frame>_<seq>_<call>.ppm (BSI_B1_DRAWDUMP=0: off) */
int b1dx_dump_rt0(const char *path);
static void draw_dump(const char *call) {
  extern int br_will_dump(void) __attribute__((weak)); if (!br_will_dump) return;
  static int on = -1, want = -1;   /* c5b141: env decision first (br_will_dump is a pure read): no cross-file call per draw when off */
  if (on < 0) { const char *e = getenv("BSI_B1_DRAWDUMP"), *bb = getenv("BSI_BBONLY"), *df = getenv("BSI_DUMPFRAME");
    on = !(e && atoi(e) == 0) && !(bb && atoi(bb) != 0) && df; want = df ? atoi(df) : -1; }
  if (!on) return;
  int f = br_will_dump(); if (f < 0) return;
  if (f != want) return;   /* only the BSI_DUMPFRAME frame, as the bridge's per-draw dump */
  static int lastf = -1, seq, mx = -1; if (mx < 0) { const char *e = getenv("BSI_B1_DRAWDUMP_MAX"); mx = e && atoi(e) > 0 ? atoi(e) : 4000; }
  if (f != lastf) { lastf = f; seq = 0; } if (seq >= mx) return;
  const char *dir = getenv("BSI_DUMPDIR"); char path[512];
  snprintf(path, sizeof path, "%s/b1_f%d_%04d_%s.ppm", dir ? dir : "/tmp", f, seq++, call);
  b1dx_dump_rt0(path);
}
/* c5b141 (BSI_B1_IDMEMO, see co_get_m below): generation of ALL proxy -> id bindings. Bumped (acq_rel RMW, under g_co_mu,
   AFTER the table entry and the in-object copy were rewritten) wherever an existing binding changes or disappears:
   co_bind (every bind, also a fresh one) and rhi_b1_on_free (a binding cleared). Those two are the only writers of g_co
   kind/id and of px_set; co_slot only creates kind-0 entries for new proxies; the back buffer's id (K_BB) is never memoized. */
static uint64_t g_pxgen = 1;
void rhi_b1_on_free(uint32_t p) {
  static int on = -1; if (on < 0) { const char *e = getenv("BSI_B1_FREEHOOK"); on = !(e && atoi(e) == 0); }
  if (!on || !p) return;
  pthread_mutex_lock(&g_co_mu);
  CObj *o = co_slot(p, 0); int kind = 0; uint32_t id = 0;
  if (o && o->kind) { kind = o->kind; id = o->id; o->kind = 0; o->id = 0; }
  if (o) px_set(p, 0, 0, 0);   /* perf73 */
  if (kind) __atomic_add_fetch(&g_pxgen, 1, __ATOMIC_ACQ_REL);   /* c5b141: before the id can be released / recycled */
  pthread_mutex_unlock(&g_co_mu);
  if (kind && kind != K_BB) obj_release(kind, id);
}
static void co_bind(uint32_t p, int kind, int sub, uint32_t id) {
  pthread_mutex_lock(&g_co_mu);
  CObj *o = co_slot(p, 1);
  if (o) { if (o->kind) obj_release(o->kind, o->id); o->kind = (uint16_t)kind; o->sub = (uint16_t)sub; o->id = id; px_set(p, kind, sub, id);   /* perf73: px_set */
           __atomic_add_fetch(&g_pxgen, 1, __ATOMIC_ACQ_REL); }   /* c5b141 */
  pthread_mutex_unlock(&g_co_mu);
  if (!o) obj_release(kind, id);
}
static uint32_t co_get_k(uint32_t p, int kind, int *sub, int *kout) {   /* kind K_RES also accepts the back buffer; c5b141: *kout = kind found */
  *kout = 0;
  if (!p) return 0;
  uint32_t id = 0; int k = 0;
  if (px_on() && px_get(p, kind, sub, &id, &k)) {   /* perf73 */
    if (g_px == 2) { int s2 = 0, k2; CObj *o2 = co_find(p); uint32_t id2 = o2 ? co_id_of(o2, kind, &s2, &k2) : 0; if (id2 != id || (sub && id && s2 != *sub)) g_px_bad++; }
    if (id) { *kout = k; return id; } }
  CObj *o = co_find(p); k = 0; id = o ? co_id_of(o, kind, sub, &k) : 0;
  if (!id) { if (g_unknown_refs < 12) fprintf(stderr, "[b1] unknown object %08x (want kind %d, have %d) in context method %d\n", p, kind, o ? o->kind : -1, t_cur);
             g_unknown_refs++; g_unk_by[t_cur & 255]++; }
  *kout = k; return id;
}
static uint32_t co_get(uint32_t p, int kind, int *sub) { int k; return co_get_k(p, kind, sub, &k); }
/* c5b141 BSI_B1_IDMEMO (default 1; =0 off; off with BSI_B1_PXCACHE=2 so its verify counts every lookup): per bind site and
   slot, the last raw guest proxy and the id it resolved to, stamped with g_pxgen as read (acquire) BEFORE that lookup.
   A hit (same proxy, same generation) returns the stored id without touching the proxy object (the px seqlock read, often
   a cache miss: the game never reads the SRV/sampler/CB objects it binds) or the table. Exact: a lookup's result depends
   only on the binding of p (table entry + in-object copy, both written only by co_bind / rhi_b1_on_free, which bump the
   generation after writing) and, for K_BB, on b1dx_bb_id() (never memoized: an entry is stored only when the binding's
   kind equals the wanted kind and the id is non-zero, so a zero / unknown-object result is always looked up and logged
   as before). Unchanged generation since the stamp => no binding changed since before the stored lookup => the same
   lookup now returns the same id. Visibility is the same as the lookups' own: a binding the context thread can see
   through the game's synchronisation was bumped before it (acq_rel), and the stamp is read with acquire. The memos are
   written and read only inside ctx_method (the immediate context: one thread at a time, like g_ia / g_vp). The
   generation is 64-bit (no wrap). */
typedef struct { uint32_t p, id; uint64_t g; } IdMemo;
static int g_idm = -1; static long g_idm_hit, g_idm_miss, g_idm_bad;   /* g_idm_bad: BSI_B1_IDMEMO=2 verify mismatches */
static IdMemo g_mm_srv[6][128], g_mm_ss[6][16], g_mm_cb[6][14], g_mm_vb[32], g_mm_rtv[8], g_mm_uav[64];
static IdMemo g_mm_sh[6], g_mm_il, g_mm_ib, g_mm_bs, g_mm_dss, g_mm_rs, g_mm_dsv;
static __attribute__((noinline)) void idm_init(void) {
  const char *e = getenv("BSI_B1_IDMEMO"); int v = e ? (atoi(e) == 2 ? 2 : atoi(e) != 0) : 1; if (px_on() == 2) v = 0;
  fprintf(stderr, "[b1] IDMEMO=%d: per-slot proxy -> id memo, validated by the binding generation (c5b141)%s\n", v, v == 2 ? ", every hit verified against a fresh lookup" : "");
  g_idm = v;
}
static inline int idm_on(void) { if (__builtin_expect(g_idm < 0, 0)) idm_init(); return g_idm; }
static __attribute__((noinline)) void idm_verify(uint32_t p, int kind, uint32_t id) {   /* BSI_B1_IDMEMO=2: a hit against a quiet fresh lookup */
  uint32_t id2 = 0; int k = 0;
  if (!(px_on() && px_get(p, kind, 0, &id2, &k) && id2)) { CObj *o = co_find(p); id2 = o ? co_id_of(o, kind, 0, &k) : 0; }
  if (id2 != id) { if (g_idm_bad < 8) fprintf(stderr, "[b1] IDMEMO verify: proxy %08x kind %d memo id %u, lookup %u\n", p, kind, id, id2); g_idm_bad++; }
}
static inline uint32_t co_get_m(uint32_t p, int kind, IdMemo *e) {   /* co_get(p, kind, 0) through the memo e */
  if (!p) return 0;
  if (!idm_on()) return co_get(p, kind, 0);
  uint64_t g = __atomic_load_n(&g_pxgen, __ATOMIC_ACQUIRE);
  if (e->p == p && e->g == g) { g_idm_hit++; if (__builtin_expect(g_idm == 2, 0)) idm_verify(p, kind, e->id); return e->id; }
  int k; uint32_t id = co_get_k(p, kind, 0, &k); g_idm_miss++;
  if (id && k == kind) { e->p = p; e->id = id; e->g = g; }
  return id;
}

static int g_on = -1, g_ok, g_native = -1;   /* g_native: BSI_RHI=b1n, the bridge is off */
static int native(void) { if (g_native < 0) { const char *e = getenv("BSI_RHI"); g_native = e && strstr(e, "b1n") != 0; } return g_native; }
static int on(void) {
  if (__builtin_expect(g_ok == 1, 1)) return 1;   /* c5b141: g_ok becomes 1 only below, after g_on was found non-zero (g_on never changes again) */
  if (g_on < 0) { const char *e = getenv("BSI_RHI"), *c = getenv("BSI_B1_COM"); g_on = e && strstr(e, "b1") && (!c || atoi(c) != 0); }
  if (!g_on) return 0;
  if (!g_ok) g_ok = rhi_b1_dev() ? 1 : -1;
  return g_ok == 1;
}
int b1com_active(void) { return on(); }

/* ---- BSI_B1_DRAWBATCH (chat 5, render-thread CPU): deferred state sets ----
   Every b1dx_c_* entry takes g_rmu (rhi_b1_dxvk.cpp CTX_GUARD), 15-20 times per draw (~2.0 ms/frame of lock/unlock at
   the Fair). rhi_b1_com.c cannot change that lock (the C++ file is off limits here), so the pure state-set methods
   (IASet.., VS/PS/GS/HS/DS/CS Set.., RSSetState/Viewports/ScissorRects, OMSetBlendState/DepthStencilState) are RECORDED in a
   render-thread pending list, with their ids already resolved (same co_get/ids_of calls at the same time as before), and
   issued in one burst, in the original order, right before the first call that is not a deferrable Set: Draw.., Dispatch,
   Clear.., Copy.., UpdateSubresource, Map, Unmap, Begin, End, GetData, OMSetRenderTargets, every non-context method (creations, Release,
   QueryInterface ...), Present. Nothing in the renderer observes a bind before the next such call (every reader of the
   bound state lives inside those b1dx_c_* entries), so the burst leaves the renderer in exactly the state the immediate
   calls would have left it in, through the same calls with the same arguments.
   =1: EXACT. Consecutive records of the same kind and stage whose slot ranges are contiguous and in ascending order are
       merged into ONE b1dx_c_* call (per-slot work in the C++ entry is independent and done in ascending slot order; the
       only range-dependent lines there test `start == 0 && n`, which the merged call satisfies iff the first part did):
       UE3 binds textures one slot at a time, so PSSetShaderResources(0,1),(1,1),(2,1) becomes one b1dx_c_srvs(0..2) =
       one g_rmu acquisition instead of three. Lock count: fewer only by the merges; the burst runs back to back.
   =2: STATE-EQUIVALENT, not call-exact (A/B only): also merges with an earlier (non-adjacent) record of the same kind
       and stage when contiguous, and drops a record whose slot range a later record of the same kind/stage covers
       (last write wins per slot). Shader sets and UAV sets are never collapsed (their C++ entries drive pattern hooks:
       b1vr::ps_set, b1eo, dc88, aoblur).
   Hazards: (a) ids are recycled (Table::del free list): obj_release (every shadow release: rhi_b1_on_free, proxy
   rebinding, create_post) first replays the pending list from the releasing thread under g_db_lock, so a recorded id
   always reaches the renderer before its release, as it did when the Set was immediate. (b) readers of the current
   state (Map/Unmap pend logic, b1dx_c_rts render-pass flush, s2/pp76/g4/sb87/dc88 hooks) sit in non-deferrable entries:
   the flush precedes each of them. (c) order among Sets: replayed as recorded (level 1 merges keep per-slot order).
   The C-side bookkeeping (g_ia, g_vscb, g_cur_*, g_vp/g_scr) is still updated at record time, as before.
   When rhi_b1_dxvk.cpp grows b1dx_c_begin_batch/end_batch (kit/DRAWBATCH_cpp_proposal.patch: one g_rmu hold across the
   burst), the flush uses them (weak symbols) and the lock count per draw drops to one. */
enum { DB_NOP = 0, DB_CBS, DB_SRVS, DB_SS, DB_UAVS, DB_SH, DB_IL, DB_VBS, DB_IB, DB_TOPO, DB_RS, DB_VP, DB_SC, DB_BLEND, DB_DSS };
typedef struct { uint8_t op, st; uint16_t off; uint32_t start, n, a, b, c2; } DbRec;   /* off: index into g_db_pool */
#define DB_NREC 512
#define DB_NPOOL 8192
static DbRec g_db_rec[DB_NREC]; static uint32_t g_db_pool[DB_NPOOL];
static int g_db_n, g_db_np;              /* records / pool dwords used (written by the recording thread, read by any under g_db_lock) */
static int g_db_on = -1;                 /* -1 undecided, 0 off, 1 exact, 2 state-equivalent */
static int g_db_lock;                    /* spinlock: the recording (render) thread per record/flush, a releasing thread per obj_release */
static long g_db_calls, g_db_merged, g_db_dropped, g_db_flushes, g_db_xflush;   /* [b1] COM: line */
static uint8_t g_db_defer[256];          /* context method index -> deferrable */
void b1dx_c_begin_batch(void) __attribute__((weak)); void b1dx_c_end_batch(void) __attribute__((weak));   /* proposed C++ entries (not present yet) */
static inline void db_lock(void) { int spins = 0; while (__atomic_exchange_n(&g_db_lock, 1, __ATOMIC_ACQUIRE)) { if (++spins > 64) { sched_yield(); spins = 0; } } }
static inline void db_unlock(void) { __atomic_store_n(&g_db_lock, 0, __ATOMIC_RELEASE); }
static void db_init(void) {
  const char *e = getenv("BSI_B1_DRAWBATCH"); int v = e ? atoi(e) : 0;
  if (v > 0 && (!native() || !on())) v = 0;
  if (v > 2) v = 2;
  static const uint8_t defer_idx[] = { 7, 8, 9, 10, 11, 16, 17, 18, 19, 22, 23, 24, 25, 26, 31, 32, 35, 36, 43, 44, 45, 59, 60, 61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71 };
  for (unsigned i = 0; i < sizeof defer_idx; i++) g_db_defer[defer_idx[i]] = 1;
  if (v) fprintf(stderr, "[b1] DRAWBATCH=%d: context state sets deferred to the next draw/dispatch/clear/copy/map/RT change (%s)%s\n", v,
                 v == 1 ? "exact: adjacent contiguous merges only" : "state-equivalent: cross-record merges + last-write-wins", b1dx_c_begin_batch ? ", one g_rmu hold per burst" : "");
  __atomic_store_n(&g_db_on, v, __ATOMIC_RELEASE);
}
/* caller holds g_db_lock */
static void db_replay(void) {
  if (!g_db_n) return;
  g_db_flushes++;
  if (b1dx_c_begin_batch) b1dx_c_begin_batch();
  for (int i = 0; i < g_db_n; i++) {
    const DbRec *r = &g_db_rec[i]; const uint32_t *p = g_db_pool + r->off;
    switch (r->op) {
    case DB_CBS:   b1dx_c_cbs(r->st, r->start, r->n, p); break;
    case DB_SRVS:  b1dx_c_srvs(r->st, r->start, r->n, p); break;
    case DB_SS:    b1dx_c_samplers(r->st, r->start, r->n, p); break;
    case DB_UAVS:  b1dx_c_uavs(r->start, r->n, p); break;
    case DB_SH:    b1dx_c_shader(r->st, r->a); break;
    case DB_IL:    b1dx_c_il(r->a); break;
    case DB_VBS:   b1dx_c_vbs(r->start, r->n, p, p + r->n, p + 2 * r->n); break;
    case DB_IB:    b1dx_c_ib(r->a, r->b, r->c2); break;
    case DB_TOPO:  b1dx_c_topology(r->a); break;
    case DB_RS:    b1dx_c_rs(r->a); break;
    case DB_VP:    b1dx_c_viewports(r->a, (const float *)p); break;
    case DB_SC:    b1dx_c_scissors(r->a, r->c2 ? (const int32_t *)p : 0); break;
    case DB_BLEND: b1dx_c_blend(r->a, r->c2 ? (const float *)p : 0, r->b); break;
    case DB_DSS:   b1dx_c_dss(r->a, r->b); break;
    default: break;
    }
  }
  if (b1dx_c_end_batch) b1dx_c_end_batch();
  g_db_n = 0; g_db_np = 0;
}
static void db_flush(void) { if (!__atomic_load_n(&g_db_n, __ATOMIC_RELAXED)) return; db_lock(); db_replay(); db_unlock(); }   /* recording thread */
static void db_flush_any(void) {   /* any thread, before a shadow id is released; the lock is taken even when the list looks empty (a record may be in flight) */
  if (__atomic_load_n(&g_db_on, __ATOMIC_ACQUIRE) <= 0) return;
  db_lock(); if (g_db_n) { g_db_xflush++; db_replay(); } db_unlock();
}
/* record helpers (recording thread; take and release g_db_lock) */
static inline uint32_t *db_room(int ndw) {   /* caller holds g_db_lock; a full list is replayed first (still in order) */
  if (g_db_n >= DB_NREC || g_db_np + ndw > DB_NPOOL) db_replay();
  return g_db_pool + g_db_np;
}
static void db_rec_arr(int op, int st, uint32_t start, uint32_t n, const uint32_t *ids) {   /* CBS / SRVS / SS / UAVS */
  db_lock(); g_db_calls++;
  if (n) {
    int lvl = g_db_on;
    /* level 1: merge with the immediately preceding record when it is the same kind/stage and this range starts where it ends */
    if (g_db_n) { DbRec *l = &g_db_rec[g_db_n - 1];
      if (l->op == op && l->st == st && l->start + l->n == start && (int)l->off + (int)l->n == g_db_np && g_db_np + (int)n <= DB_NPOOL) {
        memcpy(g_db_pool + g_db_np, ids, 4 * n); g_db_np += (int)n; l->n += n; g_db_merged++; db_unlock(); return; } }
    if (lvl >= 2 && op != DB_UAVS) {   /* level 2: cross-record contiguous merge, or drop records this range covers */
      for (int i = g_db_n - 1; i >= 0; i--) { DbRec *l = &g_db_rec[i]; if (l->op != op || l->st != st) continue;
        if (l->start + l->n == start || start + n == l->start) {   /* contiguous (either side): one record, re-laid out at the end of the pool */
          uint32_t tn = l->n + n; if (g_db_np + (int)tn > DB_NPOOL) break;
          uint32_t *d = g_db_pool + g_db_np; if (l->start + l->n == start) { memcpy(d, g_db_pool + l->off, 4 * l->n); memcpy(d + l->n, ids, 4 * n); }
          else { memcpy(d, ids, 4 * n); memcpy(d + n, g_db_pool + l->off, 4 * l->n); l->start = start; }
          l->off = (uint16_t)g_db_np; g_db_np += (int)tn; l->n = tn; g_db_merged++; db_unlock(); return; }
        if (l->start >= start && l->start + l->n <= start + n) { l->op = DB_NOP; g_db_dropped++; } }
    }
  }
  uint32_t *p = db_room((int)n); DbRec *r = &g_db_rec[g_db_n++];
  r->op = (uint8_t)op; r->st = (uint8_t)st; r->off = (uint16_t)g_db_np; r->start = start; r->n = n; r->a = r->b = r->c2 = 0;
  if (n) memcpy(p, ids, 4 * n); g_db_np += (int)n;
  db_unlock();
}
static void db_rec_vbs(uint32_t start, uint32_t n, const uint32_t *ids, const uint32_t *st, const uint32_t *of) {
  db_lock(); g_db_calls++;
  if (n && g_db_n) { DbRec *l = &g_db_rec[g_db_n - 1];   /* level 1 adjacent merge (ids, strides, offsets re-laid out as three runs) */
    if (l->op == DB_VBS && l->start + l->n == start && g_db_np + 3 * (int)(l->n + n) <= DB_NPOOL) {
      uint32_t tn = l->n + n; uint32_t *d = g_db_pool + g_db_np, *o = g_db_pool + l->off;
      memcpy(d, o, 4 * l->n); memcpy(d + l->n, ids, 4 * n); memcpy(d + tn, o + l->n, 4 * l->n); memcpy(d + tn + l->n, st, 4 * n);
      memcpy(d + 2 * tn, o + 2 * l->n, 4 * l->n); memcpy(d + 2 * tn + l->n, of, 4 * n);
      l->off = (uint16_t)g_db_np; g_db_np += 3 * (int)tn; l->n = tn; g_db_merged++; db_unlock(); return; } }
  if (g_db_on >= 2 && n) for (int i = g_db_n - 1; i >= 0; i--) { DbRec *l = &g_db_rec[i];   /* level 2: covered records drop */
    if (l->op == DB_VBS && l->start >= start && l->start + l->n <= start + n) { l->op = DB_NOP; g_db_dropped++; } }
  uint32_t *p = db_room(3 * (int)n); DbRec *r = &g_db_rec[g_db_n++];
  r->op = DB_VBS; r->st = 0; r->off = (uint16_t)g_db_np; r->start = start; r->n = n; r->a = r->b = r->c2 = 0;
  if (n) { memcpy(p, ids, 4 * n); memcpy(p + n, st, 4 * n); memcpy(p + 2 * n, of, 4 * n); } g_db_np += 3 * (int)n;
  db_unlock();
}
/* single-object sets; data = ndw dwords copied into the pool (viewports / scissors / blend factor: the C++ entry reads them now) */
static void db_rec1(int op, int st, uint32_t a, uint32_t b, uint32_t c2, const void *data, int ndw) {
  db_lock(); g_db_calls++;
  if (g_db_on >= 2 && op != DB_SH) for (int i = g_db_n - 1; i >= 0; i--) { DbRec *l = &g_db_rec[i];   /* level 2: last write wins */
    if (l->op == op && l->st == st) { l->op = DB_NOP; g_db_dropped++; break; } }
  uint32_t *p = db_room(ndw); DbRec *r = &g_db_rec[g_db_n++];
  r->op = (uint8_t)op; r->st = (uint8_t)st; r->off = (uint16_t)g_db_np; r->start = 0; r->n = 0; r->a = a; r->b = b; r->c2 = c2;
  if (ndw) memcpy(p, data, 4 * ndw); g_db_np += ndw;
  db_unlock();
}
/* the context-method cases call these instead of b1dx_c_* directly: immediate when DRAWBATCH is off */
static inline void x_cbs(int st, uint32_t start, uint32_t n, const uint32_t *ids)      { if (g_db_on > 0) db_rec_arr(DB_CBS, st, start, n, ids); else b1dx_c_cbs(st, start, n, ids); }
static inline void x_srvs(int st, uint32_t start, uint32_t n, const uint32_t *ids)     { if (g_db_on > 0) db_rec_arr(DB_SRVS, st, start, n, ids); else b1dx_c_srvs(st, start, n, ids); }
static inline void x_samplers(int st, uint32_t start, uint32_t n, const uint32_t *ids) { if (g_db_on > 0) db_rec_arr(DB_SS, st, start, n, ids); else b1dx_c_samplers(st, start, n, ids); }
static inline void x_uavs(uint32_t start, uint32_t n, const uint32_t *ids)             { if (g_db_on > 0) db_rec_arr(DB_UAVS, 0, start, n, ids); else b1dx_c_uavs(start, n, ids); }
static inline void x_shader(int st, uint32_t sh)                                        { if (g_db_on > 0) db_rec1(DB_SH, st, sh, 0, 0, 0, 0); else b1dx_c_shader(st, sh); }
static inline void x_il(uint32_t id)                                                    { if (g_db_on > 0) db_rec1(DB_IL, 0, id, 0, 0, 0, 0); else b1dx_c_il(id); }
static inline void x_vbs(uint32_t start, uint32_t n, const uint32_t *ids, const uint32_t *st, const uint32_t *of) { if (g_db_on > 0) db_rec_vbs(start, n, ids, st, of); else b1dx_c_vbs(start, n, ids, st, of); }
static inline void x_ib(uint32_t id, uint32_t fmt, uint32_t off)                        { if (g_db_on > 0) db_rec1(DB_IB, 0, id, fmt, off, 0, 0); else b1dx_c_ib(id, fmt, off); }
static inline void x_topology(uint32_t t)                                               { if (g_db_on > 0) db_rec1(DB_TOPO, 0, t, 0, 0, 0, 0); else b1dx_c_topology(t); }
static inline void x_rs(uint32_t id)                                                    { if (g_db_on > 0) db_rec1(DB_RS, 0, id, 0, 0, 0, 0); else b1dx_c_rs(id); }
static inline void x_viewports(uint32_t n, const float *v)                              { if (g_db_on > 0) db_rec1(DB_VP, 0, n, 0, 0, v, 6 * (int)(n > 16 ? 16 : n)); else b1dx_c_viewports(n, v); }   /* the C++ entry reads min(n,16)*24 bytes */
static inline void x_scissors(uint32_t n, const int32_t *r)                             { if (g_db_on > 0) db_rec1(DB_SC, 0, n, 0, r != 0, r, r ? 4 * (int)(n > 16 ? 16 : n) : 0); else b1dx_c_scissors(n, r); }
static inline void x_blend(uint32_t id, const float *f, uint32_t mask)                  { if (g_db_on > 0) db_rec1(DB_BLEND, 0, id, mask, f != 0, f, f ? 4 : 0); else b1dx_c_blend(id, f, mask); }
static inline void x_dss(uint32_t id, uint32_t ref)                                     { if (g_db_on > 0) db_rec1(DB_DSS, 0, id, ref, 0, 0, 0); else b1dx_c_dss(id, ref); }

/* ---- device creations: bridge_pre creates the shadow object, bridge_post binds it to the proxy written to *pp ---- */
static __thread uint32_t t_pp; static __thread int t_kind, t_sub; static __thread uint32_t t_id;
static __thread uint32_t t_gb_pp;
static __thread uint32_t t_gd_p, t_gd_n, t_gd_q; static long g_qc[4];   /* compare: both 0, both >0, bridge 0 / ours >0, bridge >0 / ours 0 */ static long g_gd[4], g_gdnull;   /* GetData answers: 8-byte zero / nonzero / S_FALSE / other */
static __thread const char *t_chk_name; static __thread uint32_t t_chk_a0, t_chk_a1, t_chk_a2, t_chk_a3;
void b1com_create_pre(const char *name, CPU *c) {
  if (name[0] != 'I') return;
  if (!strcmp(name, "IDXGISwapChain::GetBuffer")) { if (on()) t_gb_pp = R32(c->esp + 16); return; }   /* (Buffer, riid, ppSurface) */
  /* the shadow back buffer must exist before the game makes its RTV (inside RHI CreateViewport) */
  if (!strcmp(name, "IDXGIFactory::CreateSwapChain") || !strcmp(name, "IDXGIFactory1::CreateSwapChain")) {   /* (pDevice, pDesc, ppSC) */
    uint32_t d = R32(c->esp + 12); if (d && on()) { uint32_t w = R32(d), h = R32(d + 4); fprintf(stderr, "[b1] swap chain %ux%u -> shadow back buffer\n", w, h); b1dx_backbuffer(w, h);
      if (native()) {
        /* as the bridge's CreateSwapChain (scd_g2h): the window takes the swap chain size unless BSI_WIN_W/H say otherwise */
        if (!getenv("BSI_WIN_W") && w && h) { char t[16]; snprintf(t, sizeof t, "%u", w); setenv("BSI_WIN_W", t, 1); snprintf(t, sizeof t, "%u", h); setenv("BSI_WIN_H", t, 1); }
        void *wnd = br_handle(R32(d + 44)); if (!b1dx_present_init(wnd, w, h)) fprintf(stderr, "[b1] native: no presenter, frames stay offscreen\n"); } } return; }
  if (!strcmp(name, "IDXGISwapChain::ResizeBuffers")) {   /* (Count, W, H, Fmt, Flags) */
    uint32_t w = R32(c->esp + 12), h = R32(c->esp + 16); if (w && h && on()) { b1dx_backbuffer(w, h); if (native()) b1dx_present_resize(w, h); } return; }
  if (!strcmp(name, "ID3D11DeviceContext::GetData")) { static int lg = -1; if (lg < 0) lg = getenv("BSI_B1_CAPLOG") != 0 || qcmp(); if (lg) { t_gd_q = R32(c->esp + 8); t_gd_p = R32(c->esp + 12); t_gd_n = R32(c->esp + 16); } return; }
  if (!strncmp(name, "ID3D11Device::Check", 19)) {   /* diagnostics: what the game asks and what it is told (BSI_B1_CAPLOG=1) */
    static int lg = -1; if (lg < 0) lg = getenv("BSI_B1_CAPLOG") != 0;
    if (lg) { t_chk_name = name; t_chk_a0 = R32(c->esp + 8); t_chk_a1 = R32(c->esp + 12); t_chk_a2 = R32(c->esp + 16); t_chk_a3 = R32(c->esp + 20); }
    return; }
  if (strncmp(name, "ID3D11Device::Create", 20) || !on()) return;
  const char *m = name + 20; uint32_t a0 = R32(c->esp + 8), a1 = R32(c->esp + 12), a2 = R32(c->esp + 16), a3 = R32(c->esp + 20), a4 = R32(c->esp + 24);
  uint32_t id = 0, pp = 0; int kind = 0, sub = 0;
  if (!strcmp(m, "Buffer")) {
    uint32_t sys = a1 ? R32(a1) : 0;
    id = b1dx_buffer_create2(R32(a0), R32(a0 + 8), R32(a0 + 4), sys ? MEMP(sys) : 0); kind = K_RES; pp = a2; sub = 1;
  } else if (!strcmp(m, "Texture2D")) {
    uint32_t d = a0, w = R32(d), h = R32(d + 4), mips = R32(d + 8), arr = R32(d + 12), fmt = R32(d + 16), smp = R32(d + 20), bind = R32(d + 32), misc = R32(d + 40);
    if (smp <= 1) {
      const void *sub_[256]; uint32_t pitch[256]; uint32_t n = (mips ? mips : 1) * (arr ? arr : 1);
      int have = a1 && mips && n <= 256;
      if (have) for (uint32_t k = 0; k < n; k++) { uint32_t p = R32(a1 + 12 * k); sub_[k] = p ? MEMP(p) : 0; pitch[k] = R32(a1 + 12 * k + 4); }
      id = b1dx_tex2d_create(w, h, mips, arr, fmt, bind, misc, have ? sub_ : 0, have ? pitch : 0);
    }
    kind = K_RES; pp = a2; sub = 2;
  } else if (!strcmp(m, "Texture3D")) {   /* desc: W, H, D, Mips, Format, Usage, Bind, CPU, Misc */
    uint32_t d = a0, mips = R32(d + 12); const void *sd[16]; uint32_t rp[16], sp[16]; int have = a1 && mips && mips <= 16;
    if (have) for (uint32_t k = 0; k < mips; k++) { uint32_t p = R32(a1 + 12 * k); sd[k] = p ? MEMP(p) : 0; rp[k] = R32(a1 + 12 * k + 4); sp[k] = R32(a1 + 12 * k + 8); }
    id = b1dx_tex3d_create(R32(d), R32(d + 4), R32(d + 8), mips, R32(d + 16), R32(d + 24), have ? sd : 0, rp, sp);
    kind = K_RES; pp = a2; sub = 3;
  } else if (!strcmp(m, "ShaderResourceView") || !strcmp(m, "RenderTargetView") || !strcmp(m, "DepthStencilView") || !strcmp(m, "UnorderedAccessView")) {
    int type = m[0] == 'S' ? 1 : m[0] == 'R' ? 2 : m[0] == 'D' ? 3 : 4;   /* 4 = UAV (build 60) */
    uint32_t res = co_get(a0, K_RES, 0);
    if (res) {
      if (a1) { uint32_t f = R32(a1), dim = R32(a1 + 4), u = type == 3 ? a1 + 12 : a1 + 8;   /* DSV desc: Format, Dim, Flags, union */
                id = b1dx_view_create(type, res, f, dim, R32(u), R32(u + 4), R32(u + 8), R32(u + 12), 1); }
      else id = b1dx_view_create(type, res, 0, 0, 0, 0, 0, 0, 0);
    }
    kind = K_VIEW; pp = a2; sub = type;
  } else if (!strcmp(m, "InputLayout")) {
    uint32_t n = a1 > 32 ? 32 : a1; const char *names[32]; uint32_t idx[32]; int32_t reg[32], el[32 * 6]; char nb[32][64];
    for (uint32_t i = 0; i < n; i++) { uint32_t e = a0 + 28 * i; g2host_a(R32(e), nb[i], 64); names[i] = nb[i]; idx[i] = R32(e + 4); }
    if (a2 && b1dx_isgn_lookup(MEMP(a2), a3, names, idx, n, reg)) {
      for (uint32_t i = 0; i < n; i++) { uint32_t e = a0 + 28 * i;
        el[6 * i] = reg[i]; el[6 * i + 1] = R32(e + 8); el[6 * i + 2] = R32(e + 12); el[6 * i + 3] = R32(e + 16); el[6 * i + 4] = R32(e + 20) == 1; el[6 * i + 5] = R32(e + 24); }
      id = b1dx_il_create(el, n);
    }
    kind = K_IL; pp = a4;
  } else if (!strcmp(m, "VertexShader") || !strcmp(m, "PixelShader") || !strcmp(m, "GeometryShader") ||
             !strcmp(m, "HullShader") || !strcmp(m, "DomainShader") || !strcmp(m, "ComputeShader")) {
    int st = m[0] == 'V' ? 0 : m[0] == 'P' ? 1 : m[0] == 'G' ? 2 : m[0] == 'H' ? 3 : m[0] == 'D' ? 4 : 5;
    static int now = -1; if (now < 0) { const char *e = getenv("BSI_B1_COMPILE"); now = e ? atoi(e) : 0; }
    id = b1dx_shader_create(st, a0 ? MEMP(a0) : 0, a1, now); kind = K_SH; sub = st; pp = a3;
  } else if (!strcmp(m, "BlendState"))        { id = a0 ? b1dx_blend_create((const uint32_t *)MEMP(a0)) : 0;   kind = K_BS;  pp = a1; }
  else if (!strcmp(m, "DepthStencilState"))   { id = a0 ? b1dx_dss_create((const uint32_t *)MEMP(a0)) : 0;     kind = K_DSS; pp = a1;
    { static int nl; if (a0 && nl < 40 && getenv("BSI_B1_DSSLOG")) { nl++; fprintf(stderr, "[b1dss] create id %u: depth %u write %u func %u stencil %u\n", id, R32(a0), R32(a0 + 4), R32(a0 + 8), R32(a0 + 12)); } } }
  else if (!strcmp(m, "RasterizerState"))     { id = a0 ? b1dx_rs_create((const uint32_t *)MEMP(a0)) : 0;      kind = K_RS;  pp = a1; }
  else if (!strcmp(m, "Query"))               { if (!native() && !qcmp()) return; id = a0 ? b1dx_query_create(R32(a0)) : 0; kind = K_Q; pp = a1;
                                                { static long n[8]; uint32_t t = a0 ? R32(a0) : 7; n[t < 7 ? t : 7]++; if (((n[0]+n[1]+n[2]+n[3]+n[4]+n[5]+n[6]+n[7]) % 50) == 0) fprintf(stderr, "[b1q] CreateQuery by type: %ld %ld %ld %ld %ld %ld %ld %ld\n", n[0], n[1], n[2], n[3], n[4], n[5], n[6], n[7]); } }
  else if (!strcmp(m, "SamplerState"))        { id = a0 ? b1dx_sampler_create((const uint32_t *)MEMP(a0)) : 0; kind = K_SS;  pp = a1; }
  else return;
  t_pp = pp; t_kind = kind; t_sub = sub; t_id = id;
}
void b1com_create_post(const char *name, CPU *c) {
  (void)name;
  if (t_gd_n) {
    if (c->eax == 1) g_gd[2]++; else if (t_gd_n == 8 && t_gd_p) { if (R32(t_gd_p) | R32(t_gd_p + 4)) g_gd[1]++; else g_gd[0]++; } else g_gd[3]++;
    if (qcmp() && c->eax == 0 && t_gd_n == 8 && t_gd_p) { uint32_t id = co_get(t_gd_q, K_Q, 0); uint64_t mine;
      if (id && b1dx_query_peek(id, &mine) == 0) { int b = (R32(t_gd_p) | R32(t_gd_p + 4)) != 0, m = mine != 0; g_qc[b == m ? (b ? 1 : 0) : (b ? 3 : 2)]++;
        if (b != m && g_qc[2] + g_qc[3] < 15) fprintf(stderr, "[b1q] mismatch: bridge %u ours %llu\n", R32(t_gd_p), (unsigned long long)mine);
        if (((g_qc[0] + g_qc[1] + g_qc[2] + g_qc[3]) % 5000) == 0) fprintf(stderr, "[b1q] occlusion compare: both 0 %ld, both >0 %ld, bridge 0 ours >0 %ld, bridge >0 ours 0 %ld\n", g_qc[0], g_qc[1], g_qc[2], g_qc[3]); } }
    t_gd_n = 0; if (((g_gd[0] + g_gd[1] + g_gd[2] + g_gd[3]) % 20000) == 0) fprintf(stderr, "[b1cap] GetData: 8-byte zero %ld nonzero %ld, S_FALSE %ld, other %ld\n", g_gd[0], g_gd[1], g_gd[2], g_gd[3]);
    return; }
  if (t_chk_name) {
    const char *m = t_chk_name + 19; t_chk_name = 0;
    if (!strcmp(m, "FormatSupport")) fprintf(stderr, "[b1cap] CheckFormatSupport fmt %u -> hr %08x support %08x\n", t_chk_a0, c->eax, t_chk_a1 ? R32(t_chk_a1) : 0);
    else if (!strcmp(m, "MultisampleQualityLevels")) fprintf(stderr, "[b1cap] CheckMultisampleQualityLevels fmt %u count %u -> hr %08x levels %u\n", t_chk_a0, t_chk_a1, c->eax, t_chk_a2 ? R32(t_chk_a2) : 0);
    else if (!strcmp(m, "FeatureSupport")) { char hx[80]; int o = 0; for (uint32_t i = 0; i < t_chk_a2 && i < 24 && t_chk_a1; i++) o += snprintf(hx + o, sizeof hx - o, "%02x", ((uint8_t *)MEMP(t_chk_a1))[i]);
      fprintf(stderr, "[b1cap] CheckFeatureSupport feature %u size %u -> hr %08x data %s\n", t_chk_a0, t_chk_a2, c->eax, o ? hx : "-"); }
    else fprintf(stderr, "[b1cap] %s\n", m);
    return; }
  if (t_gb_pp) { uint32_t p = (int32_t)c->eax >= 0 ? R32(t_gb_pp) : 0; if (p) co_bind(p, K_BB, 2, 0); t_gb_pp = 0; return; }   /* back buffer proxy */
  if (!t_kind) return;
  uint32_t proxy = (t_pp && (int32_t)c->eax >= 0) ? R32(t_pp) : 0;
  if (proxy) co_bind(proxy, t_kind, t_sub, t_id); else obj_release(t_kind, t_id);
  t_kind = 0; t_pp = 0; t_id = 0;
}

/* ---- dynamic buffers: Unmap leaves the game's bytes at pData (DXVK memory in the guest window, stays valid this frame);
   each draw copies the range it reads. pend = buffers mapped since their last DISCARD. ---- */
typedef struct { uint32_t id; const uint8_t *data; uint32_t size; uint64_t clo, chi; } Pend;   /* [clo,chi) = bytes already copied since this Unmap (b80) */
static Pend g_pend[16]; static int g_npend;
static struct { uint32_t vb[32], stride[32], off[32]; uint32_t ib, ibfmt, iboff; } g_ia;
static long g_dyn_bytes; static int g_inq, g_qlog; static uint32_t g_inq_id; static uint32_t g_cur_dsv, g_cur_rtv0;
int b1dx_depth_probe(float *frac, float *mind);
void b1dx_state_dump(void);
int b1dx_buf_read(uint32_t id, uint32_t off, uint32_t n, void *out);
static uint32_t g_vscb[14];
static float g_vp[16][6]; static int32_t g_scr[16][4]; static uint32_t g_nvp, g_nscr;   /* last RSSetViewports / RSSetScissorRects */
int b1dx_view_info(uint32_t id, uint32_t *res, uint32_t *w, uint32_t *h, uint32_t *fmt, uint32_t *mip, uint32_t *layer); static uint32_t g_cur_dss_proxy, g_cur_ref;
static Pend *pend_find(uint32_t id) { for (int i = 0; i < g_npend; i++) if (g_pend[i].id == id) return &g_pend[i]; return 0; }
/* c5b141 BSI_B1_PDFAST (default 1; =0 the previous loop): pre_draw walks only the VB slots that hold an id (g_vbmask, kept
   by IASetVertexBuffers, the only writer of g_ia.vb; ascending bit order = the old ascending slot order) and remembers per
   slot (32 = the IB) the pend_find result for the id it last looked up, valid while g_pend_gen is unchanged. pend_set is
   the only writer of g_pend / g_npend and bumps g_pend_gen on every call (insert, eviction memmove, refresh), so a cached
   Pend pointer is exactly what pend_find would return now. Same pend_copy calls, same order. */
static uint32_t g_vbmask, g_pend_gen = 1; static int g_pdfast = -1; static long g_pd_bad;   /* =2: verify (mismatches in the full-path line) */
static struct { uint32_t id, gen; Pend *p; } g_pfc[33];
static inline Pend *pend_find_c(int slot, uint32_t id) {
  if (g_pfc[slot].id == id && g_pfc[slot].gen == g_pend_gen) { if (__builtin_expect(g_pdfast == 2, 0) && pend_find(id) != g_pfc[slot].p) g_pd_bad++; return g_pfc[slot].p; }
  Pend *p = pend_find(id); g_pfc[slot].id = id; g_pfc[slot].gen = g_pend_gen; g_pfc[slot].p = p; return p;
}
static void pend_set(uint32_t id, const uint8_t *data, uint32_t size) {
  g_pend_gen++;   /* c5b141 */
  Pend *p = pend_find(id);
  if (!p) { if (g_npend == 16) { memmove(g_pend, g_pend + 1, 15 * sizeof(Pend)); g_npend--; } p = &g_pend[g_npend++]; }
  p->id = id; p->data = data; p->size = size; p->clo = p->chi = 0;
}
/* b80 (chat 5, C5 memcpy): every draw used to copy its whole vertex/index range again, although the bytes at pData cannot
   change between two Unmaps (D3D11: a DISCARD map gets new storage, a NO_OVERWRITE map may not touch data in flight).
   Step 0 profile: memcpy 2.3 % of the render thread. Now each Pend remembers one copied interval, reset at Unmap; a draw
   copies only the part of its range outside it (adjacent / overlapping ranges merge, a disjoint range is copied whole and
   becomes the interval when larger). Same bytes reach the GPU in the same order: EXACT. BSI_B1_PENDALL=1 restores the old
   behaviour for A/B. */
static long g_dyn_skipped;
static void pend_copy(Pend *p, uint64_t off, uint64_t len) {
  if (!p || off >= p->size) return;
  if (off + len > p->size) len = p->size - off;
  if (!len) return;
  static int all = -1; if (all < 0) { all = getenv("BSI_B1_PENDALL") != 0; fprintf(stderr, "[b1] dynamic-buffer copies: %s\n", all ? "every draw copies its whole range (BSI_B1_PENDALL=1)" : "copied-range dedupe (b81)"); }
  uint64_t end = off + len;
  if (!all && p->chi > p->clo) {
    if (off >= p->clo && end <= p->chi) { g_dyn_skipped += len; return; }   /* fully copied already */
    if (off <= p->chi && end >= p->clo) {   /* overlapping or adjacent: copy only the new parts */
      if (off < p->clo) { b1dx_c_bufwrite(p->id, (uint32_t)off, (uint32_t)(p->clo - off), p->data + off); g_dyn_bytes += p->clo - off; }
      if (end > p->chi) { b1dx_c_bufwrite(p->id, (uint32_t)p->chi, (uint32_t)(end - p->chi), p->data + p->chi); g_dyn_bytes += end - p->chi; }
      g_dyn_skipped += len - ((off < p->clo ? p->clo - off : 0) + (end > p->chi ? end - p->chi : 0));
      if (off < p->clo) p->clo = off; if (end > p->chi) p->chi = end; return; }
  }
  b1dx_c_bufwrite(p->id, (uint32_t)off, (uint32_t)len, p->data + off); g_dyn_bytes += len;
  if (p->chi <= p->clo || len > p->chi - p->clo) { p->clo = off; p->chi = end; }
}
static void qdraw_log(uint32_t vcount, uint32_t vfirst, int indexed, uint32_t icount, uint32_t ifirst, int32_t base) {
  { static long at = -1; static int re; if (at < 0) { const char *e = getenv("BSI_B1_QLOG_AT"); at = e ? atol(e) : 0; }
    extern long b1dx_frames(void); long fr = at ? b1dx_frames() : 0;
    if (at && !re && fr >= at) { re = 1; g_qlog = 0; extern void b1dx_depth_rearm(void); b1dx_depth_rearm(); fprintf(stderr, "[b1q] query log re-armed at frame %ld\n", fr); } }
  if (!g_inq || g_qlog >= 12) return; g_qlog++;
  Pend *vp = pend_find(g_ia.vb[0]), *ip = pend_find(g_ia.ib);
  b1dx_state_dump();
  if (g_qlog == 1) { float f = -1, m = -1; b1dx_depth_probe(&f, &m); fprintf(stderr, "[b1q]   depth now: %.1f%% < 0.999, min %.4f\n", f * 100, m); }
  fprintf(stderr, "[b1q]   query id %u\n", g_inq_id);
  if (ip && icount <= 64) { uint64_t o = (uint64_t)g_ia.iboff + (uint64_t)ifirst * 2; char b[400]; int n = 0;
    if (o + icount * 2 <= ip->size) for (uint32_t k = 0; k < icount && n < 380; k++) n += snprintf(b + n, sizeof b - n, "%u ", ((const uint16_t *)(ip->data + o))[k]);
    fprintf(stderr, "[b1q]   indices: %s\n", n ? b : "?"); }
  for (int k = 0; k < 2; k++) { float m[16]; if (g_vscb[k] && b1dx_buf_read(g_vscb[k], 0, 64, m))
    fprintf(stderr, "[b1q]   vs cb%d: %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g\n", k, m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],m[8],m[9],m[10],m[11],m[12],m[13],m[14],m[15]); }
  uint32_t d = g_cur_dss_proxy ? R32(g_cur_dss_proxy + 12) : 0;
  fprintf(stderr, "[b1q] query draw: %s v %u+%u i %u+%u base %d | vb0 id %u stride %u off %u %s | ib id %u fmt %u off %u %s | dss %s depth %u write %u func %u\n",
          indexed ? "indexed" : "draw", vfirst, vcount, ifirst, icount, base, g_ia.vb[0], g_ia.stride[0], g_ia.off[0], vp ? "DYNAMIC" : "static",
          g_ia.ib, g_ia.ibfmt, g_ia.iboff, ip ? "DYNAMIC" : "static", d ? "" : "(none)", d ? R32(d) : 0, d ? R32(d + 4) : 0, d ? R32(d + 8) : 0);
  { uint32_t r, w, h, f, m, l; int t = b1dx_view_info(g_cur_dsv, &r, &w, &h, &f, &m, &l); fprintf(stderr, "[b1q]   dsv view %u type %d res %u %ux%u vkfmt %u mip %u layer %u", g_cur_dsv, t, r, w, h, f, m, l);
    t = b1dx_view_info(g_cur_rtv0, &r, &w, &h, &f, &m, &l); fprintf(stderr, " | rtv0 view %u type %d res %u %ux%u\n", g_cur_rtv0, t, r, w, h); }
  if (vp && g_ia.stride[0]) { uint64_t o = (uint64_t)g_ia.off[0] + (uint64_t)(indexed ? base : vfirst) * g_ia.stride[0];
    if (o + 8 * g_ia.stride[0] <= vp->size) for (int k = 0; k < 8; k++) { const float *f = (const float *)(vp->data + o + k * g_ia.stride[0]); fprintf(stderr, "[b1q]   v%d %.2f %.2f %.2f\n", k, f[0], f[1], f[2]); } }
}
static void pre_draw(uint32_t vcount, uint32_t vfirst, int indexed, uint32_t icount, uint32_t ifirst, int32_t base, int inst) {
  qdraw_log(vcount, vfirst, indexed, icount, ifirst, base);
  if (!g_npend) return;
  if (__builtin_expect(g_pdfast < 0, 0)) { const char *e = getenv("BSI_B1_PDFAST"); g_pdfast = e ? (atoi(e) == 2 ? 2 : atoi(e) != 0) : 1; }   /* c5b141 */
  uint32_t vmin = vfirst, vmax = vfirst + vcount;   /* [vmin, vmax) */
  if (indexed) {
    uint32_t isz = g_ia.ibfmt == 42 ? 4 : 2; Pend *ip = g_pdfast ? pend_find_c(32, g_ia.ib) : pend_find(g_ia.ib);   /* c5b141 */
    if (ip) {
      uint64_t o = (uint64_t)g_ia.iboff + (uint64_t)ifirst * isz; pend_copy(ip, o, (uint64_t)icount * isz);
      uint32_t lo = ~0u, hi = 0;
      if (o + (uint64_t)icount * isz <= ip->size) for (uint32_t k = 0; k < icount; k++) {
        uint32_t v = isz == 4 ? ((const uint32_t *)(ip->data + o))[k] : ((const uint16_t *)(ip->data + o))[k];
        if (v < lo) lo = v; if (v > hi) hi = v; }
      if (lo <= hi) { vmin = (uint32_t)((int64_t)lo + base); vmax = (uint32_t)((int64_t)hi + base) + 1; } else { vmin = 0; vmax = ~0u; }
    } else { vmin = 0; vmax = ~0u; }   /* static index buffer over a dynamic vertex buffer: copy the whole mapped data */
  }
  if (g_pdfast) {   /* c5b141: the same loop over the non-zero slots only, pend_find memoized per slot */
    for (uint32_t m = g_vbmask; m; m &= m - 1) { int s = __builtin_ctz(m); Pend *p = pend_find_c(s, g_ia.vb[s]); if (!p) continue;
      if (inst || vmax == ~0u || !g_ia.stride[s]) pend_copy(p, g_ia.off[s], p->size);
      else pend_copy(p, (uint64_t)g_ia.off[s] + (uint64_t)vmin * g_ia.stride[s], (uint64_t)(vmax - vmin) * g_ia.stride[s]); }
    return; }
  for (int s = 0; s < 32; s++) { if (!g_ia.vb[s]) continue; Pend *p = pend_find(g_ia.vb[s]); if (!p) continue;
    if (inst || vmax == ~0u || !g_ia.stride[s]) pend_copy(p, g_ia.off[s], p->size);
    else pend_copy(p, (uint64_t)g_ia.off[s] + (uint64_t)vmin * g_ia.stride[s], (uint64_t)(vmax - vmin) * g_ia.stride[s]); }
}

#define A(i) R32(c->esp + 8 + 4 * (i))
/* ---- b1n: immutable state objects deduplicated by description ----
   The game creates ~440 sampler states a frame (and releases them again). The D3D11 runtime, and DXVK's D3D11 under
   the bridge, return the existing object for an identical description; the null device made a new guest object, a
   new shadow object and a new proxy binding every time. Here CreateSamplerState / CreateRasterizerState /
   CreateDepthStencilState / CreateBlendState with a description seen before return the same proxy (AddRef'd), so the
   game sees D3D11's behaviour and nothing is created. The cache owns one reference on each proxy, so a cached proxy
   (and its shadow object, keyed by that proxy) is never freed or reused: guest Release only drops the game's
   references (atomic, com.c). Bounded by the number of distinct descriptions (D3D11 allows 4096 per kind).
   BSI_B1_NODEDUP=1 turns it off. */
#define SC_N 8192u
typedef struct { uint32_t proxy, hash; uint16_t kind, nw; uint32_t d[66]; } SCE;   /* blend desc: 264 bytes = 66 dwords */
static SCE *g_sc; static uint32_t g_sc_used; static pthread_mutex_t g_sc_mu = PTHREAD_MUTEX_INITIALIZER;
static long g_sc_hits, g_sc_miss; static uint32_t g_sc_ss;   /* g_sc_ss: cached sampler states */
/* Samplers are capped (BSI_B1_SSCAP, default 1024): every cached proxy pins its shadow DxvkSampler for the whole
   session, and DXVK's sampler pool holds at most 2048 live samplers (DxvkSamplerPool::MaxSamplerCount). Past that DXVK
   logs "Failed to allocate sampler, using default one." and hands out its default sampler (CLAMP_TO_EDGE, no aniso):
   tiled surfaces turn into flat smears, and the dedup cache would keep that default bound to the description for good.
   UE3's D3D11 RHI makes a new description per MipLODBias fade step / resident-mip MinLOD, so the distinct count grows
   with texture streaming. Uncached samplers are released when the game releases them (DXVK recycles them, LRU). */
static uint32_t ss_cap(void) { static int v = -1; if (v < 0) { const char *e = getenv("BSI_B1_SSCAP"); v = e ? atoi(e) : 1024; } return (uint32_t)v; }
static uint8_t g_sck[8192];   /* per COM method: 0 not a state creation, else kind (K_BS .. K_SS); 0xff = not yet classified */
static __thread int t_sc_kind; static __thread uint32_t t_sc_pp, t_sc_hash; static __thread uint32_t t_sc_d[66];
static int dedup_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("BSI_B1_NODEDUP"); v = !(e && atoi(e) > 0); } return v; }
static uint32_t sc_nw(int kind) { return kind == K_BS ? 66u : kind == K_RS ? 10u : 13u; }   /* desc dwords: blend 264, raster 40, DS/sampler 52 */
static uint32_t sc_hash(int kind, const uint32_t *d, uint32_t nw) {
  uint32_t h = 2166136261u ^ (uint32_t)kind; for (uint32_t i = 0; i < nw; i++) { h ^= d[i]; h *= 16777619u; h ^= h >> 15; } return h ? h : 1u; }
static SCE *sc_find(int kind, uint32_t h, const uint32_t *d, uint32_t nw, int *empty) {   /* caller holds g_sc_mu */
  for (uint32_t n = 0, i = h & (SC_N - 1); n < SC_N; n++, i = (i + 1) & (SC_N - 1)) {
    SCE *e = &g_sc[i];
    if (!e->proxy) { *empty = 1; return e; }
    if (e->hash == h && e->kind == kind && !memcmp(e->d, d, nw * 4)) { *empty = 0; return e; }
  }
  *empty = 0; return 0;
}
/* pre-call: 1 = answered from the cache (the call is done), 0 = create normally (the result is cached in sc_post) */
static int sc_pre(CPU *c, int gi) {
  int kind = g_sck[gi];
  if (kind == 0xff) { const char *m = g_com_methods[gi].name + 20;
    kind = !strcmp(m, "SamplerState") ? K_SS : !strcmp(m, "RasterizerState") ? K_RS : !strcmp(m, "DepthStencilState") ? K_DSS : !strcmp(m, "BlendState") ? K_BS : 0;
    g_sck[gi] = (uint8_t)kind; }
  t_sc_kind = 0;
  if (!kind || !dedup_on()) return 0;
  uint32_t desc = A(0), pp = A(1); if (!desc || !pp) return 0;   /* pp == NULL: validation call, let it through */
  uint32_t nw = sc_nw(kind); memcpy(t_sc_d, MEMP(desc), nw * 4);
  uint32_t h = sc_hash(kind, t_sc_d, nw); int empty = 0; uint32_t proxy = 0;
  pthread_mutex_lock(&g_sc_mu);
  if (!g_sc) g_sc = (SCE *)calloc(SC_N, sizeof(SCE));
  SCE *e = g_sc ? sc_find(kind, h, t_sc_d, nw, &empty) : 0;
  if (e && !empty) { proxy = e->proxy; __atomic_add_fetch((uint32_t *)MEMP(proxy + 4), 1, __ATOMIC_ACQ_REL); g_sc_hits++; }
  pthread_mutex_unlock(&g_sc_mu);
  if (proxy) { W32(pp, proxy); c->eax = 0; c->edx = 0; c->esp += 4 + (uint32_t)g_com_methods[gi].argbytes; return 1; }
  t_sc_kind = kind; t_sc_pp = pp; t_sc_hash = h;
  return 0;
}
/* post-call of a creation that missed: cache the new proxy (only when it has its shadow object) and take the cache's reference */
static void sc_post(CPU *c, int have_shadow) {
  int kind = t_sc_kind; t_sc_kind = 0;
  if (!kind || (int32_t)c->eax < 0 || !have_shadow) return;
  uint32_t proxy = R32(t_sc_pp); if (!proxy) return;
  uint32_t nw = sc_nw(kind); int empty = 0;
  pthread_mutex_lock(&g_sc_mu);
  SCE *e = (g_sc && g_sc_used < SC_N / 2 && (kind != K_SS || g_sc_ss < ss_cap())) ? sc_find(kind, t_sc_hash, t_sc_d, nw, &empty) : 0;
  if (e && empty) { e->hash = t_sc_hash; e->kind = (uint16_t)kind; e->nw = (uint16_t)nw; memcpy(e->d, t_sc_d, nw * 4);
    __atomic_add_fetch((uint32_t *)MEMP(proxy + 4), 1, __ATOMIC_ACQ_REL); __atomic_store_n(&e->proxy, proxy, __ATOMIC_RELEASE); g_sc_used++; if (kind == K_SS) g_sc_ss++; }
  g_sc_miss++;
  pthread_mutex_unlock(&g_sc_mu);
}

/* ---- context calls (pre-call, caller thread) ---- */
static uint16_t g_cls[8192]; static uint8_t g_cre[8192], g_adesc[8192];
int b1dx_adapter_desc(char *name, int n, uint32_t *ven, uint32_t *dev, uint64_t *vram, uint64_t *shared);   /* 0 unknown, 1 other, 0x100|idx context, 0x200|idx swapchain */
static long g_nctx, g_npresent;
/* Map bookkeeping. pData/RowPitch/DepthPitch are snapshotted right after the Map call returns (rhi_b1_com_post), not
   read back from the game's D3D11_MAPPED_SUBRESOURCE at Unmap: that struct is often a stack local of the lock function
   and is reused before Unmap (cloud: the colour-LUT update f_0089b350/f_0089ab10 reads RowPitch 1091656680 at Unmap). */
static __thread struct { uint32_t res, sub, pm, type, pd, rp, dp; } t_map[8];
static __thread int t_map_post = -1;   /* slot whose Map call is in flight (snapshot in rhi_b1_com_post) */
static int g_map_inflight;   /* perf73: Map calls between rhi_b1_com and rhi_b1_com_post, all threads: the post hook reads the
                                (dynamic-TLS, resolver call on Android) t_map_post only while one is in flight */
/* perf: lock-free lookups (co_find); misses go through co_get for its logging */
/* c5b141: mm = the site's memo row (slot start + i, mcap slots; 0 = none), see co_get_m */
static void ids_of(uint32_t arr, uint32_t n, int kind, uint32_t *out, IdMemo *mm, uint32_t mcap, uint32_t start) {
  if (!arr) { for (uint32_t i = 0; i < n; i++) out[i] = 0; return; }
  uint32_t miss = 0;
  if (mm && !idm_on()) mm = 0;
  uint64_t g = mm ? __atomic_load_n(&g_pxgen, __ATOMIC_ACQUIRE) : 0;   /* c5b141: stamp read before the lookups */
  for (uint32_t i = 0; i < n; i++) {
    uint32_t p = R32(arr + 4 * i), id = 0; int k = 0; if (!p) { out[i] = 0; continue; }
    IdMemo *e = mm && (uint64_t)start + i < mcap ? &mm[start + i] : 0;   /* c5b141 */
    if (e) { if (e->p == p && e->g == g) { out[i] = e->id; g_idm_hit++; if (__builtin_expect(g_idm == 2, 0)) idm_verify(p, kind, e->id); continue; } g_idm_miss++; }
    if (px_on() && px_get(p, kind, 0, &id, &k)) {   /* perf73 */
      if (g_px == 2) { int k2; CObj *o2 = co_find(p); uint32_t id2 = o2 ? co_id_of(o2, kind, 0, &k2) : 0; if (id2 != id) g_px_bad++; }
      out[i] = id; if (!id) miss = 1; else if (e && k == kind) { e->p = p; e->id = id; e->g = g; } continue; }
    CObj *o = co_find(p); if (o) id = co_id_of(o, kind, 0, &k);
    out[i] = id; if (!id) miss = 1; else if (e && k == kind) { e->p = p; e->id = id; e->g = g; }   /* c5b141 */
  }
  if (miss) for (uint32_t i = 0; i < n; i++) { uint32_t p = R32(arr + 4 * i); if (p && !out[i]) out[i] = co_get(p, kind, 0); }
}

/* ---- build 64: what the game asks for (texture sizes, streaming copies, sampler LOD), in shadow AND native mode, so the
   two can be compared: if native's game-side requests drift (smaller textures, higher MinLOD) the game decided differently;
   if they match, the difference is in how we draw. One "[b1tx]" line every 600 presents (BSI_B1_TXSTAT=0: off). */
static uint8_t g_txk[8192];   /* 0 unclassified, 1 none, 2 CreateTexture2D, 3 CreateShaderResourceView, 4 CreateSamplerState, 5 Update, 6 Copy */
static long g_tx_n, g_tx_mips, g_tx_w[6], g_tx_full, g_srv_n, g_srv_mip0, g_ss_n, g_ss_minlod, g_ss_bias, g_up_tex[3], g_cp_tex;
static double g_ss_minlod_sum, g_tx_mpix;
static void tx_stat(CPU *c, int gi) {
  uint8_t k = g_txk[gi];
  if (!k) { const char *n = gi < g_com_nmethods ? g_com_methods[gi].name : "";
    k = !strcmp(n, "ID3D11Device::CreateTexture2D") ? 2 : !strcmp(n, "ID3D11Device::CreateShaderResourceView") ? 3 : !strcmp(n, "ID3D11Device::CreateSamplerState") ? 4
      : !strcmp(n, "ID3D11DeviceContext::UpdateSubresource") ? 5 : !strcmp(n, "ID3D11DeviceContext::CopySubresourceRegion") ? 6 : 1;
    g_txk[gi] = k; }
  if (k == 1) return;
  static int on = -1; if (on < 0) { const char *e = getenv("BSI_B1_TXSTAT"); on = !(e && atoi(e) == 0); } if (!on) return;
  if (k == 2) { uint32_t d = A(0); if (!d || !A(2)) return; uint32_t w = R32(d), h = R32(d + 4), m = R32(d + 8), bind = R32(d + 32);
    if (bind & 0x60) return;   /* render targets / depth: not streamed */
    uint32_t full = 1; for (uint32_t x = (w > h ? w : h); x > 1; x >>= 1) full++;
    g_tx_n++; g_tx_mips += m ? m : full; if (!m || m == full) g_tx_full++; g_tx_mpix += (double)w * h / 1e6;
    g_tx_w[w <= 128 ? 0 : w <= 256 ? 1 : w <= 512 ? 2 : w <= 1024 ? 3 : w <= 2048 ? 4 : 5]++; }
  else if (k == 3) { uint32_t d = A(1); if (!d || !A(2)) return; uint32_t dim = R32(d + 4); if (dim == 4 || dim == 5 || dim == 9) { g_srv_n++; if (R32(d + 8) == 0) g_srv_mip0++; } }
  else if (k == 4) { uint32_t d = A(0); if (!d) return; float mnl, bias; memcpy(&bias, MEMP(d + 16), 4); memcpy(&mnl, MEMP(d + 44), 4);
    g_ss_n++; if (mnl > 0.0f && mnl < 1000.0f) { g_ss_minlod++; g_ss_minlod_sum += mnl; } if (bias != 0.0f) g_ss_bias++;
    { static int on = -1; if (on < 0) on = getenv("BSI_B1_SSCALLER") != 0;
      if (on && g_npresent > 900) { static int nl; uint32_t ra = R32(c->esp);
        if (nl < 80) { nl++; fprintf(stderr, "[b1ss] CreateSamplerState caller %08x MinLOD %.3f bias %.3f MaxLOD %.3g filter %x addr %u/%u/%u aniso %u presents %ld\n", ra, mnl, bias, *(float *)MEMP(d + 48), R32(d), R32(d + 4), R32(d + 8), R32(d + 12), R32(d + 20), g_npresent); } } } }
  else if (k == 5) { uint32_t sub = A(1); g_up_tex[sub == 0 ? 0 : sub < 4 ? 1 : 2]++; }
  else if (k == 6) g_cp_tex++;
}
/* draws per render-target size in the last window (who draws the extra draws) */
static uint32_t g_dt_key[32]; static long g_dt_n[32], g_dt_win, g_dt_lastp; static uint32_t g_cur_rtkey, g_cur_ps, g_cur_vs;
static uint32_t g_ds_rt[128], g_ds_sh[128]; static long g_ds_n[128];   /* draws by (target, PS/VS) */
static int g_ds_last = -1, g_dt_last = -1;   /* perf73: the slot the previous draw hit (same target/shaders: no scan) */
static int g_dtstat = -1;   /* c5b141: BSI_B1_DTSTAT=1 restores the per-target/per-shader draw census ([b1dt]/[b1ds] breakdown); default only counts draws */
static void dt_count(void) { uint32_t k = g_cur_rtkey; int i; g_dt_win++;
  if (__builtin_expect(g_dtstat != 1, 1)) { if (g_dtstat < 0) { const char *e = getenv("BSI_B1_DTSTAT"); g_dtstat = (e && atoi(e) == 1) ? 1 : 0; } if (g_dtstat != 1) return; }   /* c5b141 */
  { uint32_t sh = ((g_cur_ps & 0xffff) << 16) | (g_cur_vs & 0xffff); int j;
    if (g_ds_last >= 0 && g_ds_n[g_ds_last] > 0 && g_ds_rt[g_ds_last] == k && g_ds_sh[g_ds_last] == sh) { g_ds_n[g_ds_last]++; goto ds_done; }
    for (j = 0; j < 128; j++) if (g_ds_n[j] && g_ds_rt[j] == k && g_ds_sh[j] == sh) { g_ds_n[j]++; g_ds_last = j; break; }
    if (j == 128) for (j = 0; j < 128; j++) if (!g_ds_n[j]) { g_ds_rt[j] = k; g_ds_sh[j] = sh; g_ds_n[j] = 1; g_ds_last = j; break; } }
  ds_done:
  if (g_dt_last >= 0 && g_dt_n[g_dt_last] > 0 && g_dt_key[g_dt_last] == k) { g_dt_n[g_dt_last]++; return; }
  for (i = 0; i < 32; i++) if (g_dt_n[i] && g_dt_key[i] == k) { g_dt_n[i]++; g_dt_last = i; return; }
  for (i = 0; i < 32; i++) if (!g_dt_n[i]) { g_dt_key[i] = k; g_dt_n[i] = 1; g_dt_last = i; return; } }
static void dt_line(long presents) {
  char b[700]; int o = 0; long fr = presents - g_dt_lastp; if (fr <= 0) fr = 1;
  o += snprintf(b + o, sizeof b - o, "[b1dt] %ld presents: draws/frame %.1f by target (WxH/RTs: draws/frame):", presents, (double)g_dt_win / fr);
  for (int r = 0; r < 10; r++) { int bi = -1; for (int i = 0; i < 32; i++) if (g_dt_n[i] > 0 && (bi < 0 || g_dt_n[i] > g_dt_n[bi])) bi = i; if (bi < 0) break;
    uint32_t k = g_dt_key[bi]; if (o < (int)sizeof b - 40) o += snprintf(b + o, sizeof b - o, " %ux%u/%u: %.1f", k >> 18, (k >> 4) & 0x3fff, k & 15, (double)g_dt_n[bi] / fr); g_dt_n[bi] = -1; }
  fprintf(stderr, "%s\n", b); memset(g_dt_n, 0, sizeof g_dt_n); g_dt_win = 0;
  o = snprintf(b, sizeof b, "[b1ds] %ld presents: top draws by target/PS/VS (draws/frame):", presents);
  for (int r = 0; r < 14; r++) { int bi = -1; for (int i = 0; i < 128; i++) if (g_ds_n[i] > 0 && (bi < 0 || g_ds_n[i] > g_ds_n[bi])) bi = i; if (bi < 0) break;
    uint32_t k = g_ds_rt[bi]; if (o < (int)sizeof b - 50) o += snprintf(b + o, sizeof b - o, " %ux%u/%u ps%u vs%u: %.1f", k >> 18, (k >> 4) & 0x3fff, k & 15, g_ds_sh[bi] >> 16, g_ds_sh[bi] & 0xffff, (double)g_ds_n[bi] / fr); g_ds_n[bi] = -1; }
  fprintf(stderr, "%s\n", b); memset(g_ds_n, 0, sizeof g_ds_n); g_dt_lastp = presents;
}
static void tx_line(long presents) {
  static int on = -1; if (on < 0) { const char *e = getenv("BSI_B1_TXSTAT"); on = !(e && atoi(e) == 0); } if (!on) return;
  dt_line(presents);
  { uint64_t al = (uint64_t)R32(0x13a17e0u) | ((uint64_t)R32(0x13a17e4u) << 32), pool = (uint64_t)R32(0x13a17e8u) | ((uint64_t)R32(0x13a17ecu) << 32);
    fprintf(stderr, "[b1tx] game texture memory: allocated %.1f MB of pool %.1f MB (streaming drops mips when this runs out)\n", al / 1048576.0, pool / 1048576.0); }
  fprintf(stderr, "[b1tx] %ld presents, %s | textures %ld (%.0f Mpix, avg mips %.2f, full chain %ld; width <=128 %ld, 256 %ld, 512 %ld, 1024 %ld, 2048 %ld, more %ld) | 2D SRVs %ld (from mip 0 %ld) | "
          "samplers created %ld (MinLOD>0 %ld avg %.2f, bias!=0 %ld) | updates sub0 %ld sub1-3 %ld sub4+ %ld, copies %ld\n",
          presents, native() ? "native" : "shadow", g_tx_n, g_tx_mpix, g_tx_n ? (double)g_tx_mips / g_tx_n : 0.0, g_tx_full, g_tx_w[0], g_tx_w[1], g_tx_w[2], g_tx_w[3], g_tx_w[4], g_tx_w[5],
          g_srv_n, g_srv_mip0, g_ss_n, g_ss_minlod, g_ss_minlod ? g_ss_minlod_sum / g_ss_minlod : 0.0, g_ss_bias, g_up_tex[0], g_up_tex[1], g_up_tex[2], g_cp_tex);
}

void b1_inv_count(CPU *c, int gi);   /* rhi_b1.c: BSI_B1_INV slot -> COM call inventory */
/* b80 (chat 5, C6): the hitch logger (linux/hitchlog.c, BSI_HITCHLOG=<csv>) hooked bridge_pre/bridge_post only, which the
   native renderer never calls (chat 3: hitch.csv stayed empty with BSI_RHI=b1n). Wrap the native COM entry instead: same
   hl_pre/hl_post(name) calls, so the per-frame CSV (Create*, Map, GetData time on the render thread, per-thread CPU and
   run-queue wait, Present duration) works for b1n. Cost when BSI_HITCHLOG is unset: one static load + branch per COM call. */
void hl_pre(const char *name); void hl_post(const char *name);
static int rhi_b1_com_inner(CPU *c, int gi);
int rhi_b1_com(CPU *c, int gi) {
  static int hl = -1; if (__builtin_expect(hl < 0, 0)) { const char *e = getenv("BSI_HITCHLOG"); hl = e && *e; }
  if (!hl || gi < 0 || gi >= g_com_nmethods) return rhi_b1_com_inner(c, gi);
  /* b83 (chat 5): b81 called hl_pre/hl_post for EVERY COM call; hitchlog.c classifies the method by strncmp/strcmp/strstr each
     time, which cost ~5 ms of render-thread CPU per frame at the Fair (j0076 perf: __strncmp 1.3 ms, hl_kind_of 1.2, clock
     0.8, strcmp 0.5). Classify each method index once (same rules as hl_kind_of: Create*, GetData, Map, UpdateSubresource,
     Present) and skip the hooks for every other method. */
  static uint8_t hk[8192];   /* 0 unknown, 1 hooked, 2 skip */
  uint8_t k = hk[gi];
  if (__builtin_expect(k == 0, 0)) {
    const char *n = g_com_methods[gi].name; k = 2;
    if (!strncmp(n, "ID3D11Device::Create", 20)) k = 1;
    else if (!strncmp(n, "ID3D11DeviceContext::", 21)) { const char *s = n + 21; if (!strcmp(s, "GetData") || !strcmp(s, "Map") || !strcmp(s, "UpdateSubresource")) k = 1; }
    else if (n[0] == 'I' && n[1] == 'D' && n[2] == 'X' && strstr(n, "::Present") && !strstr(n, "LastPresent")) k = 1;
    hk[gi] = k; }
  if (k == 2) return rhi_b1_com_inner(c, gi);
  const char *n = g_com_methods[gi].name; hl_pre(n); int r = rhi_b1_com_inner(c, gi); hl_post(n); return r;
}
static int ctx_method(CPU *c, int idx, int gi);
static int rhi_b1_com_inner(CPU *c, int gi) {
  { static int inv = -1; if (inv) { b1_inv_count(c, gi); if (inv < 0) inv = getenv("BSI_B1_INV") != 0; } }   /* perf73: no call per COM call unless BSI_B1_INV */
  if (gi < 0 || gi >= 8192 || !on()) return 0;
  if (__builtin_expect(g_db_on < 0, 0)) db_init();   /* BSI_B1_DRAWBATCH */
  tx_stat(c, gi);
  uint16_t k = g_cls[gi];
  if (!k) {
    const char *n = gi < g_com_nmethods ? g_com_methods[gi].name : "";
    int idx = gi < g_com_nmethods ? g_com_methods[gi].idx : 0;
    k = !strncmp(n, "ID3D11DeviceContext::", 21) && idx < 256 ? (uint16_t)(0x100 | idx) : !strncmp(n, "IDXGISwapChain::", 16) && idx < 256 ? (uint16_t)(0x200 | idx) : 1;
    if (!strncmp(n, "ID3D11Device::Create", 20) || !strncmp(n, "ID3D11Device::Check", 19) || strstr(n, "::CreateSwapChain") || !strcmp(n, "IDXGISwapChain::GetBuffer") || !strcmp(n, "IDXGISwapChain::ResizeBuffers"))
      g_cre[gi] = 1;
    g_sck[gi] = !strncmp(n, "ID3D11Device::Create", 20) ? 0xff : 0;
    if (!strncmp(n, "IDXGIAdapter", 12) && (strstr(n, "::GetDesc1") ? 2 : strstr(n, "::GetDesc") ? 1 : 0)) g_adesc[gi] = strstr(n, "::GetDesc1") ? 2 : 1;
    g_cls[gi] = k;
  }
  /* DRAWBATCH: anything that is not a deferrable context Set sees the renderer in the state the immediate calls would have left */
  if (g_db_on > 0 && g_db_n && !(k >= 0x100 && k < 0x200 && g_db_defer[k & 0xff])) db_flush();
  if (g_cre[gi] && native()) {   /* b1n: no bridge_pre */
    if (g_sck[gi] && sc_pre(c, gi)) return 1;   /* state object with a known description: answered from the cache */
    b1com_create_pre(g_com_methods[gi].name, c); }
  if (g_adesc[gi] && native()) {   /* adapter description from our Vulkan device (as DXVK's DXGI under the bridge) */
    char nm[256]; uint32_t ven, dev; uint64_t vm, sh; uint32_t g = A(0);
    if (g && b1dx_adapter_desc(nm, sizeof nm, &ven, &dev, &vm, &sh)) {
      int one = g_adesc[gi] == 2; memset(MEMP(g), 0, one ? 296 : 292);
      for (int i = 0; i < 127 && nm[i]; i++) W16(g + 2 * i, (uint8_t)nm[i]);
      W32(g + 256, ven); W32(g + 260, dev); W32(g + 272, (uint32_t)vm); W32(g + 280, (uint32_t)sh);   /* already clamped to 0xC0000000 like DXVK */
      { static int lg; if (!lg++) fprintf(stderr, "[b1] adapter desc: %s vendor %04x device %04x vram %llu MB shared %llu MB\n", nm, ven, dev, (unsigned long long)(vm >> 20), (unsigned long long)(sh >> 20)); }
      c->eax = 0; c->edx = 0; c->esp += 4 + (uint32_t)g_com_methods[gi].argbytes; return 1; } }
  if (k == 1) return 0;
  if (k >= 0x200) {
    if ((k & 0xff) == 8 && native()) {   /* Present (b1n): harness bookkeeping, then our own present */
      char path[512]; if (br_native_present(path, sizeof path)) b1dx_dump_path(path);
      g_npresent++; b1dx_present(A(0));
      if (g_npresent % 600 == 0) tx_line(g_npresent);
      if (g_npresent % 600 == 0) { char a[400], b[800]; b1com_stats(a, sizeof a); b1dx_com_stats(b, sizeof b); fprintf(stderr, "[b1] %s | %s\n", a, b); b1_perf_line(600);
        { extern int com_fullpath_line(char *, int, long) __attribute__((weak)); char f[700]; int o = 0;   /* c5b141: what still takes the full path */
          static long mh0, mm0; long mh = g_idm_hit, mm = g_idm_miss;
          if (com_fullpath_line) o = com_fullpath_line(f, sizeof f, 600);
          if (o >= 0) fprintf(stderr, "[b1] COM full path: %s | id memo hits %.0f misses %.0f per frame (IDMEMO=%d, verify mismatches %ld) | pend cache (PDFAST=%d) verify mismatches %ld\n",
                              o > 0 ? f : "(counter off)", (mh - mh0) / 600.0, (mm - mm0) / 600.0, g_idm, g_idm_bad, g_pdfast, g_pd_bad);
          mh0 = mh; mm0 = mm; } }
      return 0; }
    if ((k & 0xff) == 8) {   /* Present: if the bridge dumps this frame's back buffer, dump ours with the same frame number */
      extern int br_will_dump(void) __attribute__((weak)); int f = br_will_dump ? br_will_dump() : -1;
      if (f >= 0) b1dx_dump_request(f);
      g_npresent++; b1dx_end_frame(); if (g_npresent % 600 == 0) tx_line(g_npresent); }
    return 0;
  }
  g_nctx++; t_cur = k & 0xff;
  return ctx_method(c, k & 0xff, gi);
}
/* BSI_B1_COMFAST (com.c g_comfast): the whole of rhi_b1_com -> rhi_b1_com_inner for an armed method. Everything skipped is a
   no-op for these methods (see com.c comfast_arm); what remains is what the full path does for them, in the same order. */
/* c5b141: returns what rhi_b1_com_inner returns (1 = answered here: GetData with a known query; com.c then skips the null
   completion, as com_dispatch_inner does). Also replays tx_stat for the methods it counts (UpdateSubresource,
   CopySubresourceRegion: g_txk 5/6, classified by the first full call) and serves the non-context AddRef/Release (g_cls 1:
   rhi_b1_com_inner only runs the DRAWBATCH flush for them, then returns 0). */
int b1com_fast(CPU *c, int gi) {
  uint16_t k = g_cls[gi];
  if (__builtin_expect(g_txk[gi] > 1, 0)) tx_stat(c, gi);   /* c5b141: same call, same place (before the flush) as rhi_b1_com_inner */
  if (g_db_on > 0 && g_db_n && !(k >= 0x100 && k < 0x200 && g_db_defer[k & 0xff])) db_flush();   /* DRAWBATCH flush point, as in rhi_b1_com_inner */
  if (k < 0x100) return 0;   /* c5b141: k == 1 (armed AddRef/Release only) */
  int idx = k & 0xff;
  g_nctx++; t_cur = idx;
  return ctx_method(c, idx, gi);
}
/* 1 = arm the fast entry for gi, 0 = never, -1 = not decidable yet (b1n not up / method not classified by a full call) */
int b1com_fast_ok(int gi) {
  static int env = -1, env2 = 0;
  if (env < 0) {
    const char *nf = getenv("BSI_NULLFAST");
    env = !(getenv("BSI_BRSTAT") || getenv("BSI_COMSTAT") || getenv("BSI_BRDBG") || getenv("BSI_EMU_RANGE") || getenv("BSI_EMUALL") || getenv("BSI_B1_INV") || (nf && atoi(nf) == 0));
    /* c5b141 BSI_B1_COMFAST2 (default 1; =0: the b109 list only): also Map, Unmap, GetData, Begin, End, UpdateSubresource,
       the Copy and Clear methods, GenerateMips and the other void context methods with no com.c work, and AddRef/Release of
       every non-swap-chain interface. Off while the hitch logger is on (BSI_HITCHLOG: rhi_b1_com hooks Map/GetData/Update). */
    { const char *c2 = getenv("BSI_B1_COMFAST2"), *hl = getenv("BSI_HITCHLOG"); env2 = env && !(c2 && atoi(c2) == 0) && !(hl && *hl); }
    if (!env) fprintf(stderr, "[b1] COMFAST: a com.c / b1 debug switch is set, every COM call takes the full path\n");
    else fprintf(stderr, "[b1] COMFAST=1: hot context methods bypass com_dispatch after their first call%s\n", env2 ? " (COMFAST2: + Map/Unmap/GetData/Begin/End/Update/Copy/Clear, AddRef/Release)" : "");
  }
  if (!env || g_bridge_on || gi < 0 || gi >= 8192 || gi >= g_com_nmethods) return 0;
  if (!native() || g_ok != 1) return g_ok == -1 ? 0 : -1;   /* on() has not succeeded yet: the first full call may precede the device */
  uint16_t k = g_cls[gi]; if (!k) return -1;   /* classified by rhi_b1_com_inner (runs before com.c's completion on the same call) */
  if (g_cre[gi] || g_adesc[gi]) return 0;
  const ComMethod *m = &g_com_methods[gi];
  /* c5b141: IUnknown AddRef / Release (null-device kinds 5 / 6). rhi_b1_com_inner does nothing for them but the DRAWBATCH flush
     (k == 1) or the context switch's empty default (context idx 1 / 2), both replayed by b1com_fast. Swap-chain class excluded. */
  if (env2 && k < 0x200 && m->outiface < 0 && (m->idx == 1 || m->idx == 2) && (k == 1 || (k & 0xff) == m->idx)) {
    const char *s = strstr(m->name, "::"); if (s && (!strcmp(s + 2, m->idx == 1 ? "AddRef" : "Release"))) return 1; }
  if (k < 0x100 || k >= 0x200) return 0;
  int i = k & 0xff;
  if (env2 && m->outiface < 0 && m->idx == i) switch (i) {   /* c5b141: no com.c work but the completion (g_nfast 1 / 2 Map / 7 Unmap / 8 GetData) */
  case 14: case 29: return m->retkind == 0;   /* Map, GetData (HRESULT) */
  case 15: case 27: case 28: case 30: case 34: case 37: case 38: case 39: case 40: case 42: case 46: case 47: case 48: case 49:
  case 50: case 51: case 52: case 53: case 54: case 55: case 57: case 110: case 111:
    return m->retkind == 1;
  default: break;
  }
  if (m->retkind != 1 || m->outiface >= 0 || m->idx != i) return 0;
  /* the hot plain methods: switch case + null-device completion only (no out-params, no rhi_b1_com_post work).
     Excluded on purpose: Map/Unmap/GetData (post/out-params), 95/96 and 29 (answered by us), UpdateSubresource, Copy.., Clear..
     (hitch logger / rare), 34 OMSetRenderTargetsAndUAVs (rare). 33 OMSetRenderTargets completes via null_omsetrt (g_nfast 3). */
  switch (i) {
  case 7: case 8: case 9: case 10: case 11: case 12: case 13: case 16: case 17: case 18: case 19: case 20: case 21: case 22: case 23: case 24:
  case 25: case 26: case 31: case 32: case 33: case 35: case 36: case 41: case 43: case 44: case 45:
  case 59: case 60: case 61: case 62: case 63: case 64: case 65: case 66: case 67: case 68: case 69: case 70: case 71:
    return 1;
  default: return 0;
  }
}
/* COMFAST exact win inside RSSetViewports / RSSetScissorRects: the C++ entry is a no-op when the rects equal what it holds
   (bind cache on: returns before touching anything; off: rewrites the same bytes and sets vpdirty, already set by the previous
   call of the same data... NOT exact when off, so only with the bind cache on). g_vp/g_scr mirror g_cs.vp/sc exactly as long as
   every call that changes them also reached the C++ entry (g_vp_sync / g_scr_sync; pp76 restores what it borrows). */
static int g_vpskip = -1; static int g_vp_sync, g_scr_sync; static uint32_t g_vp_rawn, g_scr_rawn; static long g_vp_skipped;
static int vpskip(void) { if (g_vpskip < 0) { const char *e = getenv("BSI_B1_COMFAST"), *b = getenv("BSI_B1_NOBINDCACHE"); g_vpskip = (!e || atoi(e) > 0) && !(b && atoi(b) > 0);   /* b109: default on with COMFAST */ } return g_vpskip; }
static int ctx_method(CPU *c, int idx, int gi) {
  uint16_t k = (uint16_t)(0x100 | idx);
  uint32_t ids[128];
  switch (k & 0xff) {
  case 7: case 16: case 22: case 62: case 66: {   /* {VS,PS,GS,HS,DS}SetConstantBuffers(Start, Num, pp) */
    int st = (k & 0xff) == 7 ? 0 : (k & 0xff) == 16 ? 1 : (k & 0xff) == 22 ? 2 : (k & 0xff) == 62 ? 3 : 4;
    uint32_t a0 = A(0), a1 = A(1), n = a1 > 14 ? 14 : a1; ids_of(A(2), n, K_RES, ids, g_mm_cb[st], 14, a0); x_cbs(st, a0, n, ids);   /* c5b141: memo, args read once */
    if (st == 0) for (uint32_t i = 0; i < n && a0 + i < 14; i++) g_vscb[a0 + i] = ids[i];
    break; }
  case 8: case 25: case 31: case 59: case 63: {   /* {PS,VS,GS,HS,DS}SetShaderResources */
    int st = (k & 0xff) == 8 ? 1 : (k & 0xff) == 25 ? 0 : (k & 0xff) == 31 ? 2 : (k & 0xff) == 59 ? 3 : 4;
    uint32_t a0 = A(0), a1 = A(1), n = a1 > 128 ? 128 : a1; ids_of(A(2), n, K_VIEW, ids, g_mm_srv[st], 128, a0); x_srvs(st, a0, n, ids); break; }   /* c5b141 */
  case 10: case 26: case 32: case 61: case 65: {   /* {PS,VS,GS,HS,DS}SetSamplers */
    int st = (k & 0xff) == 10 ? 1 : (k & 0xff) == 26 ? 0 : (k & 0xff) == 32 ? 2 : (k & 0xff) == 61 ? 3 : 4;
    uint32_t a0 = A(0), a1 = A(1), n = a1 > 16 ? 16 : a1; ids_of(A(2), n, K_SS, ids, g_mm_ss[st], 16, a0); x_samplers(st, a0, n, ids); break; }   /* c5b141 */
  case 9: case 11: case 23: case 60: case 64: {   /* {PS,VS,GS,HS,DS}SetShader(p, ppClassInstances, Num) */
    int st = (k & 0xff) == 9 ? 1 : (k & 0xff) == 11 ? 0 : (k & 0xff) == 23 ? 2 : (k & 0xff) == 60 ? 3 : 4;
    { uint32_t sid = co_get_m(A(0), K_SH, &g_mm_sh[st]); if (st == 1) g_cur_ps = sid; else if (st == 0) g_cur_vs = sid; x_shader(st, sid); } break; }
  /* c5b141: the draw arguments are read once (nothing writes the guest's argument area during the call) */
  case 12: { uint32_t a0 = A(0), a1 = A(1), a2 = A(2); dt_count(); pre_draw(0, 0, 1, a0, a1, (int32_t)a2, 0); b1dx_c_draw_indexed(a0, 1, a1, (int32_t)a2, 0); draw_dump("DrawIndexed"); break; }   /* DrawIndexed(Count, StartIndex, BaseVertex) */
  case 13: { uint32_t a0 = A(0), a1 = A(1); dt_count(); pre_draw(a0, a1, 0, 0, 0, 0, 0); b1dx_c_draw(a0, 1, a1, 0); draw_dump("Draw"); break; }                                  /* Draw(Count, Start) */
  case 20: { uint32_t a0 = A(0), a1 = A(1), a2 = A(2), a3 = A(3), a4 = A(4); dt_count(); pre_draw(0, 0, 1, a0, a2, (int32_t)a3, 1); b1dx_c_draw_indexed(a0, a1, a2, (int32_t)a3, a4); draw_dump("DrawIndexedInstanced"); break; }   /* DrawIndexedInstanced */
  case 21: { uint32_t a0 = A(0), a1 = A(1), a2 = A(2), a3 = A(3); dt_count(); pre_draw(a0, a2, 0, 0, 0, 0, 1); b1dx_c_draw(a0, a1, a2, a3); draw_dump("DrawInstanced"); break; }                            /* DrawInstanced */
  case 14: {   /* Map(pRes, Sub, MapType, MapFlags, pMapped): remember; the data is read at Unmap */
    if (A(2) == 1 || A(2) == 3) {   /* READ / READ_WRITE: GPU readback (diagnostic, build 60) */
      static long nrd; static uint32_t seen[32]; static int ns; nrd++;
      int k2; for (k2 = 0; k2 < ns; k2++) if (seen[k2] == A(0)) break;
      if (k2 == ns && ns < 32) { seen[ns++] = A(0); int sb = 0; uint32_t id = co_get(A(0), K_RES, &sb);
        fprintf(stderr, "[b1rb] Map READ%s res %08x (shadow id %u kind %d) sub %u - %ld read maps so far\n", A(2) == 3 ? "_WRITE" : "", A(0), id, sb, A(1), nrd); }
      if (A(2) == 1) break; }
    { static int lg = -1; if (lg < 0) { const char *e = getenv("BSI_B1_TEXLOG"); lg = e && atoi(e) > 0; }
      if (lg) { static long nmap, nalias; nmap++; for (int i = 0; i < 8; i++) if (t_map[i].res == A(0) && t_map[i].sub != A(1)) { nalias++;
          if (nalias <= 10 || nalias % 1000 == 0) fprintf(stderr, "[b1tex] Map res %08x sub %u while sub %u is still mapped (type %u) - %ld of %ld maps\n", A(0), A(1), t_map[i].sub, A(2), nalias, nmap); break; } } }
    for (int i = 0; i < 8; i++) if (!t_map[i].res || (t_map[i].res == A(0) && t_map[i].sub == A(1))) {
      t_map[i].res = A(0); t_map[i].sub = A(1); t_map[i].pm = A(4); t_map[i].type = A(2); t_map[i].pd = 0;
      if (t_map_post < 0) __atomic_add_fetch(&g_map_inflight, 1, __ATOMIC_RELAXED); t_map_post = i; break; }
    break; }
  case 15:     /* Unmap(pRes, Sub) */
    for (int i = 0; i < 8; i++) if (t_map[i].res == A(0) && t_map[i].sub == A(1)) {
      uint32_t pm = t_map[i].pm, pd = t_map[i].pd; int sub = 0;
      uint32_t id = co_get(A(0), K_RES, &sub);
      int hv = 0; uint32_t bsz = (id && sub == 1) ? b1dx_buf_info(id, &hv) : 0;
      static int dynfull = -1; if (dynfull < 0) dynfull = getenv("BSI_B1_DYNFULL") != 0;
      if (dynfull) hv = 0;
      if (id && pd && bsz && hv) { if (t_map[i].type == 4) b1dx_c_map_discard(id); pend_set(id, (const uint8_t *)MEMP(pd), bsz); }   /* dynamic buffer: copied per draw */
      else if (id && pd) b1dx_c_unmap(id, A(1), MEMP(pd), t_map[i].rp, t_map[i].dp, t_map[i].type == 4);
      t_map[i].res = 0; break; }
    break;
  /* compute (build 60) */
  case 69: x_shader(5, co_get_m(A(0), K_SH, &g_mm_sh[5])); break;                                                       /* CSSetShader(p, ppCI, Num) */
  case 67: { uint32_t n = A(1) > 128 ? 128 : A(1); ids_of(A(2), n, K_VIEW, ids, g_mm_srv[5], 128, A(0)); x_srvs(5, A(0), n, ids); break; }    /* CSSetShaderResources */
  case 70: { uint32_t n = A(1) > 16 ? 16 : A(1); ids_of(A(2), n, K_SS, ids, g_mm_ss[5], 16, A(0)); x_samplers(5, A(0), n, ids); break; }    /* CSSetSamplers */
  case 71: { uint32_t n = A(1) > 14 ? 14 : A(1); ids_of(A(2), n, K_RES, ids, g_mm_cb[5], 14, A(0)); x_cbs(5, A(0), n, ids); break; }        /* CSSetConstantBuffers */
  case 68: { uint32_t n = A(1) > 64 ? 64 : A(1); ids_of(A(2), n, K_VIEW, ids, g_mm_uav, 64, A(0)); x_uavs(A(0), n, ids); break; }         /* CSSetUnorderedAccessViews(Start, Num, pp, pCounts) */
  case 41: b1dx_c_dispatch(A(0), A(1), A(2)); break;                                      /* Dispatch(X, Y, Z) */
  case 42: { static int nl; if (nl++ < 4) fprintf(stderr, "[b1] DispatchIndirect not supported (skipped)\n"); break; }
  case 51: case 52: if (A(1)) b1dx_c_clear_uav(co_get(A(0), K_VIEW, 0), (const uint32_t *)MEMP(A(1)), (k & 0xff) == 52); break;   /* ClearUnorderedAccessView{Uint,Float} */
  case 54: b1dx_c_genmips(co_get(A(0), K_VIEW, 0)); break;                                                       /* GenerateMips(pSRV) */
  case 34: {   /* OMSetRenderTargetsAndUnorderedAccessViews(NumRTV, ppRTV, pDSV, UAVStart, NumUAV, ppUAV, pCounts): the RT part */
    if (A(0) != 0xffffffffu) { uint32_t n = A(0) > 8 ? 8 : A(0); ids_of(A(1), n, K_VIEW, ids, g_mm_rtv, 8, 0); g_cur_dsv = co_get_m(A(2), K_VIEW, &g_mm_dsv); g_cur_rtv0 = n ? ids[0] : 0; b1dx_c_rts(n, ids, g_cur_dsv); }
    if (A(4) && A(4) != 0xffffffffu) { static int nl; if (nl++ < 4) fprintf(stderr, "[b1] pixel-shader UAVs (%u) not supported (ignored)\n", A(4)); }
    break; }
  case 17: x_il(co_get_m(A(0), K_IL, &g_mm_il)); break;
  case 18: {   /* IASetVertexBuffers(Start, Num, ppVB, pStrides, pOffsets) */
    uint32_t n = A(1) > 32 ? 32 : A(1), st[32], of[32], a0 = A(0), a3 = A(3), a4 = A(4); ids_of(A(2), n, K_RES, ids, g_mm_vb, 32, a0);
    for (uint32_t i = 0; i < n; i++) { st[i] = a3 ? R32(a3 + 4 * i) : 0; of[i] = a4 ? R32(a4 + 4 * i) : 0;
      if (a0 + i < 32) { g_ia.vb[a0 + i] = ids[i]; g_ia.stride[a0 + i] = st[i]; g_ia.off[a0 + i] = of[i];
                         if (ids[i]) g_vbmask |= 1u << (a0 + i); else g_vbmask &= ~(1u << (a0 + i)); } }   /* c5b141: g_vbmask mirrors g_ia.vb[s] != 0 */
    x_vbs(a0, n, ids, st, of); break; }
  case 19: { uint32_t a1 = A(1), a2 = A(2); g_ia.ib = co_get_m(A(0), K_RES, &g_mm_ib); g_ia.ibfmt = a1; g_ia.iboff = a2; x_ib(g_ia.ib, a1, a2); break; }   /* c5b141 */
  case 24: x_topology(A(0)); break;
  case 33: {   /* OMSetRenderTargets(Num, ppRTV, pDSV) */
    uint32_t n = A(0) > 8 ? 8 : A(0); ids_of(A(1), n, K_VIEW, ids, g_mm_rtv, 8, 0); g_cur_dsv = co_get_m(A(2), K_VIEW, &g_mm_dsv); g_cur_rtv0 = n ? ids[0] : 0; b1dx_c_rts(n, ids, g_cur_dsv);   /* c5b141 */
    { uint32_t r, w = 0, h = 0, f, m, l, nr = 0; for (uint32_t i = 0; i < n; i++) if (ids[i]) nr++;
      if (g_cur_rtv0) b1dx_view_info(g_cur_rtv0, &r, &w, &h, &f, &m, &l); else if (g_cur_dsv) b1dx_view_info(g_cur_dsv, &r, &w, &h, &f, &m, &l);
      g_cur_rtkey = ((w & 0x3fff) << 18) | ((h & 0x3fff) << 4) | (nr & 15); }
    { static int nl; if (nl < 40 && g_cur_dsv) { uint32_t r, w, h, f, m, l; b1dx_view_info(g_cur_dsv, &r, &w, &h, &f, &m, &l); nl++; fprintf(stderr, "[b1q] OMSetRenderTargets n %u rtv0 %u dsv view %u res %u %ux%u\n", n, g_cur_rtv0, g_cur_dsv, r, w, h); } }
    break; }
  case 35: { uint32_t a1 = A(1); x_blend(co_get_m(A(0), K_BS, &g_mm_bs), a1 ? (const float *)MEMP(a1) : 0, A(2)); break; }   /* c5b141 */
  case 36: { uint32_t a0 = A(0), a1 = A(1); g_cur_dss_proxy = a0; g_cur_ref = a1; x_dss(co_get_m(a0, K_DSS, &g_mm_dss), a1); break; }
  case 43: x_rs(co_get_m(A(0), K_RS, &g_mm_rs)); break;
  case 44: { uint32_t raw = A(0), p = A(1), n = raw > 16 ? 16 : raw;   /* RSSetViewports(Num, pViewports) */
    if (!p) { g_nvp = 0; g_vp_sync = 0; break; }   /* no call to the renderer: the mirror no longer matches what it holds */
    int same = vpskip() && g_vp_sync && g_vp_rawn == raw && !memcmp(g_vp, MEMP(p), 24 * n);   /* COMFAST: the C++ entry would return at its own memcmp */
    g_nvp = n; if (!same) memcpy(g_vp, MEMP(p), 24 * n);   /* kept for RSGetViewports (b1n) */
    if (same) { g_vp_skipped++; break; }
    g_vp_rawn = raw; g_vp_sync = vpskip();
    x_viewports(raw, (const float *)MEMP(p)); break; }
  case 45: { uint32_t raw = A(0), p = A(1), n = raw > 16 ? 16 : raw;   /* RSSetScissorRects(Num, pRects) */
    if (!p && raw) { g_nscr = 0; g_scr_sync = 0; break; }   /* no call (as before) */
    int same = vpskip() && g_scr_sync && g_scr_rawn == raw && (!p || !memcmp(g_scr, MEMP(p), 16 * n));
    g_nscr = p ? n : 0; if (p && !same) memcpy(g_scr, MEMP(p), 16 * n);
    if (same) { g_vp_skipped++; break; }
    g_scr_rawn = raw; g_scr_sync = vpskip();
    x_scissors(raw, p ? (const int32_t *)MEMP(p) : 0); break; }
  case 95: case 96: if (native()) {   /* RSGetViewports / RSGetScissorRects(pNum, pRects): build 60. The null device answered
      "0 viewports", so the UE3 RHI's Clear never saw a full-target viewport and drew clear quads instead of ClearRenderTargetView,
      and any code that saves/restores the viewport restored nothing. Same semantics as DXVK's D3D11 context. */
    uint32_t pn = A(0), pr = A(1); int vp = (k & 0xff) == 95; uint32_t have = vp ? g_nvp : g_nscr, sz = vp ? 24 : 16;
    if (pn) { if (!pr) W32(pn, have); else { uint32_t want = R32(pn); for (uint32_t i = 0; i < want; i++) {
          if (i < have) memcpy(MEMP(pr + sz * i), vp ? (const void *)g_vp[i] : (const void *)g_scr[i], sz); else memset(MEMP(pr + sz * i), 0, sz); } } }
    c->eax = 0; c->esp += 4 + (uint32_t)g_com_methods[gi].argbytes; return 1; }
    break;
  case 46: {   /* CopySubresourceRegion(pDst, DstSub, X, Y, Z, pSrc, SrcSub, pBox) */
    uint32_t d = co_get(A(0), K_RES, 0), s = co_get(A(5), K_RES, 0);
    if (d && s) b1dx_c_copy_region(d, A(1), A(2), A(3), A(4), s, A(6), A(7) ? (const uint32_t *)MEMP(A(7)) : 0);
    break; }
  case 47: {   /* CopyResource(pDst, pSrc): subresource 0 only for now (UE3 copies single-mip targets) */
    uint32_t d = co_get(A(0), K_RES, 0), s = co_get(A(1), K_RES, 0);
    if (d && s) b1dx_c_copy_region(d, 0, 0, 0, 0, s, 0, 0);
    break; }
  case 48: {   /* UpdateSubresource(pDst, Sub, pBox, pSrc, RowPitch, DepthPitch) */
    uint32_t id = co_get(A(0), K_RES, 0);
    if (id && A(3)) b1dx_c_update(id, A(1), A(2) ? (const uint32_t *)MEMP(A(2)) : 0, MEMP(A(3)), A(4), A(5));
    break; }
  case 50: if (A(1)) b1dx_c_clear_rtv(co_get(A(0), K_VIEW, 0), (const float *)MEMP(A(1))); break;
  case 53: { float d; uint32_t u = A(2); memcpy(&d, &u, 4); b1dx_c_clear_dsv(co_get(A(0), K_VIEW, 0), A(1), d, A(3) & 0xff); break; }
  case 27: if (native() || qcmp()) { g_inq_id = co_get(A(0), K_Q, 0); b1dx_c_query_begin(g_inq_id); g_inq = 1; } break;
  case 28: if (native() || qcmp()) { b1dx_c_query_end(co_get(A(0), K_Q, 0)); g_inq = 0; } break;
  case 29: if (native()) {   /* GetData(pAsync, pData, Size, Flags): ours answers, the null device does not run */
    uint32_t pa = A(0), pd = A(1), sz = A(2), fl = A(3);
    { extern void com_arm_getdata(int) __attribute__((weak)); static uint8_t tried[8192];   /* c5b141: COMFAST2 may arm GetData even if */
      if (__builtin_expect(!tried[gi & 8191], 0)) { tried[gi & 8191] = 1; if (com_arm_getdata) com_arm_getdata(gi); } }   /* every call is answered here */
    uint32_t q = co_get(pa, K_Q, 0); int r = q ? b1dx_c_query_data(q, pd ? MEMP(pd) : 0, sz, fl) : -1;
    { static int on = -1; if (on < 0) on = getenv("BSI_B1_GDCALLER") != 0;   /* diagnostics: who reads which query results */
      if (on && pd) { static uint32_t seen[32]; static int ns; uint32_t ra = R32(c->esp), key = ra ^ (sz << 24); int k;
        for (k = 0; k < ns; k++) if (seen[k] == key) break;
        if (k == ns && ns < 32) { seen[ns++] = key; fprintf(stderr, "[b1gd] GetData caller %08x size %u flags %u r %d data %08x %08x\n", ra, sz, fl, r, R32(pd), sz >= 8 ? R32(pd + 4) : 0); } } }
    if (r == 1) g_gd[2]++; else if (r == 0 && sz == 8 && pd) { if (R32(pd) | R32(pd + 4)) g_gd[1]++; else g_gd[0]++; } else if (r == 0) g_gd[3]++; else g_gd[3 + 0]++, g_gdnull++;
    if (((g_gd[0] + g_gd[1] + g_gd[2] + g_gd[3]) % 20000) == 0) fprintf(stderr, "[b1cap] GetData: 8-byte zero %ld nonzero %ld, S_FALSE %ld, other %ld (null device answered %ld)\n", g_gd[0], g_gd[1], g_gd[2], g_gd[3], g_gdnull);
    if (r >= 0) { c->eax = (uint32_t)r; c->edx = 0; c->esp += 4 + (uint32_t)g_com_methods[gi].argbytes; return 1; } }
    break;
  default: break;
  }
  return 0;
}

/* build 62 perf diagnostics (b1n, every 600 presents): wall ms per frame, ms per frame the render thread was blocked in our
   query / present / frame-latency waits, and the busiest threads' CPU ms per frame (from /proc/self/task) */
#include <dirent.h>
#include <time.h>
#include <unistd.h>
void b1dx_wait_stats(uint64_t *ev, uint64_t *oq, uint64_t *ts, uint64_t *pres, uint64_t *lat, uint64_t *cs); void b1dx_ts_stats(uint64_t *reads, uint64_t *sfalse); void b1dx_oq_stats(uint64_t *vis, uint64_t *zero);
static void b1_perf_line(int frames) {
  static double t_prev; static uint64_t w_prev[6], tr_prev, tf_prev, ov_prev, oz_prev; enum { NT = 256 }; static int tid_prev[NT]; static long cpu_prev[NT]; static int n_prev;
  struct timespec now; clock_gettime(CLOCK_MONOTONIC, &now); double t = now.tv_sec + now.tv_nsec * 1e-9;
  uint64_t w[6]; b1dx_wait_stats(&w[0], &w[1], &w[2], &w[3], &w[4], &w[5]); uint64_t tr, tf, ov, oz; b1dx_ts_stats(&tr, &tf); b1dx_oq_stats(&ov, &oz);
  int tid[NT]; long cpu[NT]; char nm[NT][20]; int n = 0; long hz = sysconf(_SC_CLK_TCK); if (hz <= 0) hz = 100;
  DIR *d = opendir("/proc/self/task");
  if (d) { struct dirent *e; while ((e = readdir(d)) && n < NT) { if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
      char path[64], buf[512]; snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name); FILE *f = fopen(path, "r"); if (!f) continue;
      size_t k = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[k] = 0;
      char *lp = strchr(buf, '('), *rp = strrchr(buf, ')'); if (!lp || !rp) continue;
      int L = (int)(rp - lp - 1); if (L > 19) L = 19; memcpy(nm[n], lp + 1, L); nm[n][L] = 0;
      unsigned long ut = 0, st = 0; if (sscanf(rp + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) != 2) continue;
      tid[n] = atoi(e->d_name); cpu[n] = (long)(ut + st); n++; }
    closedir(d); }
  if (t_prev > 0) {
    double fr = frames; char line[900]; int o = snprintf(line, sizeof line, "[b1perf] %.1f ms/frame | blocked ms/frame: events %.1f, occlusion %.1f, timestamps %.1f, present %.1f (frame latency %.1f), cs %.1f | timestamps read %.1f, not ready %.1f per frame | occlusion visible %.0f, occluded %.0f per frame | CPU ms/frame:",
      (t - t_prev) * 1000.0 / fr, (w[0] - w_prev[0]) / 1000.0 / fr, (w[1] - w_prev[1]) / 1000.0 / fr, (w[2] - w_prev[2]) / 1000.0 / fr, (w[3] - w_prev[3]) / 1000.0 / fr, (w[4] - w_prev[4]) / 1000.0 / fr, (w[5] - w_prev[5]) / 1000.0 / fr, (tr - tr_prev) / fr, (tf - tf_prev) / fr, (ov - ov_prev) / fr, (oz - oz_prev) / fr);
    long dl[NT]; double tot = 0;
    for (int i = 0; i < n; i++) { dl[i] = cpu[i]; for (int j = 0; j < n_prev; j++) if (tid_prev[j] == tid[i]) { dl[i] = cpu[i] - cpu_prev[j]; break; } tot += dl[i]; }
    for (int r = 0; r < 8; r++) { int b = -1; for (int i = 0; i < n; i++) if (dl[i] > 0 && (b < 0 || dl[i] > dl[b])) b = i; if (b < 0) break;
      if (o < (int)sizeof line - 40) o += snprintf(line + o, sizeof line - o, " %s %.1f", nm[b], dl[b] * 1000.0 / hz / fr); dl[b] = 0; }
    if (o < (int)sizeof line - 30) snprintf(line + o, sizeof line - o, " | all threads %.1f", tot * 1000.0 / hz / fr);
    fprintf(stderr, "%s\n", line);
  }
  t_prev = t; tr_prev = tr; tf_prev = tf; ov_prev = ov; oz_prev = oz; memcpy(w_prev, w, sizeof w); memcpy(tid_prev, tid, sizeof(int) * n); memcpy(cpu_prev, cpu, sizeof(long) * n); n_prev = n;
}

void b1com_stats(char *out, int n) {
  int o = snprintf(out, n, "COM: %ld context calls, %ld presents, dynamic bytes %.1f MB, unknown object refs %ld (by method:", g_nctx, g_npresent, g_dyn_bytes / 1048576.0, g_unknown_refs);
  for (int k = 0; k < 256 && o < n - 24; k++) if (g_unk_by[k]) o += snprintf(out + o, n - o, " %d:%ld", k, g_unk_by[k]);
  if (o < n - 24) o += snprintf(out + o, n - o, ")%s", g_co_full ? " PROXY TABLE FULL" : "");
  if (o < n - 64) o += snprintf(out + o, n - o, ", state dedup hits %ld misses %ld cached %u (samplers %u)", g_sc_hits, g_sc_miss, g_sc_used, g_sc_ss);
  if (g_px == 2 && o < n - 48) o += snprintf(out + o, n - o, ", proxy cache verify mismatches %ld", g_px_bad);   /* perf73 */
  if (g_db_on > 0 && o < n - 96) o += snprintf(out + o, n - o, ", drawbatch sets %ld merged %ld dropped %ld bursts %ld (by a release %ld)", g_db_calls, g_db_merged, g_db_dropped, g_db_flushes, g_db_xflush);
  if (g_vpskip > 0 && o < n - 40) o += snprintf(out + o, n - o, ", viewport/scissor sets skipped %ld", g_vp_skipped);
  { long d = 0, cl = 0, gm = 0; b1dx_compute_stats(&d, &cl, &gm); extern long b1dx_compute_alt_count(void);
    if (o < n - 80) snprintf(out + o, n - o, ", dispatches %ld (AO switch %ld), UAV clears %ld, mip generations %ld", d, b1dx_compute_alt_count(), cl, gm); }
}

/* com.c, after every COM call (args still in guest memory at e0): b1n creations bind their proxies here */
void rhi_b1_com_post(CPU *c, int gi, uint32_t e0) {
  (void)e0;
  if (__atomic_load_n(&g_map_inflight, __ATOMIC_RELAXED) && t_map_post >= 0) {   /* the Map just returned: keep what it handed out */
    int i = t_map_post; uint32_t pm = t_map[i].pm; t_map_post = -1; __atomic_sub_fetch(&g_map_inflight, 1, __ATOMIC_RELAXED);
    if ((int32_t)c->eax >= 0 && pm) { t_map[i].pd = R32(pm); t_map[i].rp = R32(pm + 4); t_map[i].dp = R32(pm + 8); }
    else t_map[i].res = 0;
  }
  if (gi < 0 || gi >= 8192 || !g_cre[gi] || !native()) return;
  int have = t_kind && t_id;   /* before create_post clears them */
  b1com_create_post(g_com_methods[gi].name, c);
  if (t_sc_kind) sc_post(c, have);
}

/* ---- b1n RHI natives (linux/rhi_native_b1n.c, BSI_RHI=b1n + BSI_RHI_NAT=1) ----
   The natives replace the translated bodies of the hot FD3D11DynamicRHI slots and issue the same D3D11 methods on the
   same guest COM proxies. b1com_dispatch completes ONE such call without call_indirect/com_dispatch: the guest stack at
   c->esp holds [return][this][args] exactly as the translated body's `call [vtbl+4*idx]` leaves it, and gi is the method
   the vtable entry names (COM_BASE + 4*gi). It does what com_dispatch (com.c) does for that call in b1n, in the same order:
   rhi_b1_com (hitch-logger wrapper, tx_stat, BSI_B1_INV count, the b1dx_c_* work) -> the null device's completion
   (ret: eax = 0, esp += 4 + argbytes) -> rhi_b1_com_post. It only accepts methods whose null-device handling is exactly
   that plain `ret 0` (the void ID3D11DeviceContext Set.., Draw.. and UpdateSubresource methods the natives issue: com.c reaches
   its default branch for them, nothing else happens), and only while the bridge is off and none of com.c's per-call debug
   switches is set (BSI_BRSTAT, BSI_COMSTAT, BSI_BRDBG, BSI_NULLFAST=0, BSI_EMU_RANGE, BSI_EMUALL: those count or trace
   every call inside com_dispatch, so every call must still go through it). Returns 0 = not handled, nothing touched: the
   caller issues the call with call_indirect on the vtable entry, as the body does (Map, Unmap, GetDesc, Release,
   CreateBuffer and anything unexpected go that way). The one difference from com_dispatch on the accepted methods: com.c's
   first-call memo (g_nfast/g_nbr) is not written by a call that comes through here, and the context proxy's state block
   (proxy+20, used by the OMGet.. queries and OMSetRenderTargets only) is not allocated by it; both are unobservable to the game. */
extern int g_bridge_on;
static uint8_t g_b1n_plain[8192];   /* per method: 0 unclassified, 1 plain ret-0 context method, 2 anything else */
static int g_b1n_fast = -1;
int b1com_native_ok(void) { return native() && on(); }
int b1com_dispatch(CPU *c, int gi) {
  if (g_b1n_fast < 0) {
    const char *nf = getenv("BSI_NULLFAST");
    g_b1n_fast = !(getenv("BSI_BRSTAT") || getenv("BSI_COMSTAT") || getenv("BSI_BRDBG") || getenv("BSI_EMU_RANGE") || getenv("BSI_EMUALL") || (nf && atoi(nf) == 0));
    if (!g_b1n_fast) fprintf(stderr, "[rhinat] b1n: a com.c debug switch is set, the natives' COM calls go through com_dispatch\n");
  }
  if (!g_b1n_fast || g_bridge_on || gi < 0 || gi >= g_com_nmethods || gi >= 8192 || !native()) return 0;
  uint8_t pl = g_b1n_plain[gi];
  if (!pl) {
    const ComMethod *m = &g_com_methods[gi]; int i = m->idx;
    int ok = !strncmp(m->name, "ID3D11DeviceContext::", 21) && m->retkind == 1 && m->outiface < 0 &&
             (i == 7 || i == 9 || i == 11 || i == 12 || i == 13 || i == 16 || i == 17 || i == 18 || i == 19 || i == 20 || i == 21 ||
              i == 22 || i == 23 || i == 24 || i == 48 || i == 60 || i == 62 || i == 64 || i == 66);
    pl = g_b1n_plain[gi] = ok ? 1 : 2;
  }
  if (pl != 1) return 0;
  uint32_t e0 = c->esp;
  if (!rhi_b1_com(c, gi)) { c->eax = 0; c->esp += 4 + (uint32_t)g_com_methods[gi].argbytes; }   /* com.c ret(c,m,0) */
  rhi_b1_com_post(c, gi, e0);
  return 1;
}
