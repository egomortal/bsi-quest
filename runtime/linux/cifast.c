/* Lane B step E (OVERNIGHT.md): indirect-call dispatch fast paths, #included by rt_linux.c before call_indirect_slow.
   BSI_CIFAST (default 1; 0 = the previous dispatch, unchanged):
   - Flat call table with byte-granular entries (ft_init, g_ft_sh = 0 when the host text spans < 256 MB). The old
     entries stored (fn - base) >> 2 and so skipped every host function not 4-byte aligned: on the x86-64 -O1 cloud
     build that was 85,691 of 113,148 functions (76%), ~21,000 binary-search lookups per frame in the boat scene.
     (aarch64 functions are always 4-byte aligned: the Quest build already had them all in the table.)
   - COM fast path: a guest call to a COM vtable magic address (COM_BASE + 4*method, see com.c) goes straight from
     call_indirect to com_dispatch, instead of call_indirect -> call_indirect_slow (emu-range / import / magic checks)
     -> magic_dispatch -> com_dispatch. Exact: those earlier checks cannot match a COM address (import range 0xE0..,
     xaudio 0xE9.., dyn 0xE8..), and the path is not taken if BSI_EMU_RANGE overlaps the COM range.
   - Import fast path: an indirect call to an import magic value (IMP_BASE + 4*idx, e.g. `mov esi,[IAT]; call esi`)
     goes straight to dispatch(), as the first non-emu check of call_indirect_slow does.
   - Granule-duplicate table: the flat call table (ft_init) holds one function per 8-byte guest granule; the other
     function starts of a shared granule (tiny functions, often empty virtuals) went to lookup() (binary search over
     ~100k entries). They now sit in a small immutable open-addressing hash built by ft_init, with the same exclusions
     as the flat table (BSI_EMU_RANGE, patch_call targets, BSI_EMUALL/BSI_NOFT).
   - Thunk table: an indirect call to an import thunk (`jmp [IAT]` = FF 25 imm32 inside the exe, not a function start)
     used to do lookup() (miss) -> bad-target range checks -> call_import. The thunk's IAT address is cached in a
     direct-mapped table (entries written once per address, 64-bit atomic stores; the IAT itself is read each call,
     so lazy DLL binding still works).
   BSI_CISTAT=1 (debug, off): counts call_indirect_slow targets and every 10 s prints the top 30 per frame with
   their class and last return address ("[cistat]" lines). */
#define CI_COM_BASE 0xF0000000u
extern const int g_com_nmethods;
void com_dispatch(CPU*c, uint32_t pc);
static int g_cifast = -1;                /* resolved in ci_init (ft_init) */
static uint32_t g_ci_com_n, g_ci_imp_n;  /* byte sizes of the COM / import magic ranges served by the fast path, 0 = off */
typedef struct { uint32_t a; gfn f; } CiEnt;
#define CI_DUP_N 8192u
static CiEnt *g_ci_dup;                  /* immutable after ft_init; a==0: empty */
static uint32_t g_ci_ndup;
static inline uint32_t ci_hash(uint32_t a){ return (a*0x9E3779B1u)>>19; }   /* 13 bits = CI_DUP_N */
static inline gfn ci_dup_find(uint32_t a){
  const CiEnt *d=g_ci_dup; for(uint32_t h=ci_hash(a);;h=(h+1)&(CI_DUP_N-1)){ uint32_t x=d[h].a; if(x==a) return d[h].f; if(!x) return 0; } }
static void ci_dup_add(uint32_t a, gfn f){
  if(!g_ci_dup || g_ci_ndup>=CI_DUP_N/2) return;   /* table full: that function keeps using the slow path */
  uint32_t h=ci_hash(a); while(g_ci_dup[h].a && g_ci_dup[h].a!=a) h=(h+1)&(CI_DUP_N-1);
  if(!g_ci_dup[h].a){ g_ci_dup[h].a=a; g_ci_dup[h].f=f; g_ci_ndup++; } }
/* thunk cache: (thunk address << 32) | IAT address; direct mapped, 1024 entries */
#define CI_TH_N 1024u
static uint64_t g_ci_th[CI_TH_N];
static void ci_init(uint32_t emu_lo, uint32_t emu_hi){
  const char *e=getenv("BSI_CIFAST"); g_cifast=!(e && *e=='0');
  if(!g_cifast){ fprintf(stderr,"[ci] BSI_CIFAST=0: previous indirect-call dispatch\n"); return; }
  uint32_t cn=4u*(uint32_t)g_com_nmethods, in=4u*(uint32_t)imps_total();
  int emu_com = emu_lo<emu_hi && emu_lo<CI_COM_BASE+cn && emu_hi>CI_COM_BASE;
  int emu_imp = emu_lo<emu_hi && emu_lo<IMP_BASE+in && emu_hi>IMP_BASE;
  g_ci_com_n = emu_com?0:cn; g_ci_imp_n = emu_imp?0:in;
  g_ci_dup = calloc(CI_DUP_N,sizeof(CiEnt));
}
static void ci_report(void){
  if(g_cifast>0) { LOG("[ci] fast paths: COM %u methods, imports %u, granule-duplicate table %u functions\n",g_ci_com_n/4,g_ci_imp_n/4,g_ci_ndup);
    fprintf(stderr,"[ci] fast paths: COM %u methods, imports %u, granule-duplicate table %u functions\n",g_ci_com_n/4,g_ci_imp_n/4,g_ci_ndup); } }
/* returns 1 and runs the thunk's import if t is a cached thunk */
static inline int ci_thunk(CPU*c, uint32_t t){
  uint64_t v=__atomic_load_n(&g_ci_th[(t>>1)&(CI_TH_N-1)],__ATOMIC_RELAXED);
  if((uint32_t)(v>>32)!=t || !t) return 0;
  call_import(c,(uint32_t)v); return 1; }
static void ci_thunk_put(uint32_t t, uint32_t iat){ __atomic_store_n(&g_ci_th[(t>>1)&(CI_TH_N-1)],((uint64_t)t<<32)|iat,__ATOMIC_RELAXED); }

/* ---- BSI_CISTAT=1 */
static int g_cistat=-1;
#define CIS_N 16384u
static uint32_t cis_a[CIS_N], cis_n[CIS_N], cis_ra[CIS_N];
extern volatile long g_br_presents;
static const char *ci_class(uint32_t t);
static void cistat_report(void){
  static double t0; static long f0; static int busy; struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); double now=ts.tv_sec+ts.tv_nsec*1e-9;
  if(t0==0){ t0=now; f0=g_br_presents; return; }
  if(now-t0<10.0 || __atomic_exchange_n(&busy,1,__ATOMIC_ACQUIRE)) return;
  long fr=g_br_presents-f0; double fd=fr>0?(double)fr:1.0; static uint32_t snap[CIS_N]; unsigned long tot=0;
  for(uint32_t k=0;k<CIS_N;k++){ snap[k]=__atomic_exchange_n(&cis_n[k],0,__ATOMIC_RELAXED); tot+=snap[k]; }
  fprintf(stderr,"[cistat] dt=%.1f frames=%ld slow-path calls/frame=%.1f (total %lu)\n",now-t0,fr,tot/fd,tot);
  for(int r=0;r<30;r++){ uint32_t b=CIS_N, bv=0; for(uint32_t k=0;k<CIS_N;k++) if(snap[k]>bv){ bv=snap[k]; b=k; } if(b==CIS_N) break;
    fprintf(stderr,"[cistat]  %9.2f %08x %-14s ra %08x\n",bv/fd,cis_a[b],ci_class(cis_a[b]),cis_ra[b]); snap[b]=0; }
  t0=now; f0=g_br_presents; __atomic_store_n(&busy,0,__ATOMIC_RELEASE); }
static void cistat(CPU*c, uint32_t t){
  uint32_t h=(t*0x9E3779B1u)>>18;
  for(int k=0;k<64;k++,h=(h+1)&(CIS_N-1)){
    uint32_t a=__atomic_load_n(&cis_a[h],__ATOMIC_RELAXED);
    if(!a){ uint32_t z=0; if(__atomic_compare_exchange_n(&cis_a[h],&z,t,0,__ATOMIC_RELAXED,__ATOMIC_RELAXED)) a=t; else a=z; }
    if(a==t){ __atomic_add_fetch(&cis_n[h],1,__ATOMIC_RELAXED); cis_ra[h]=R32(c->esp); break; } }
  static unsigned tick; if(!(__atomic_add_fetch(&tick,1,__ATOMIC_RELAXED)&1023)) cistat_report(); }
static const char *ci_class(uint32_t t){
  if(t>=g_emu_lo && t<g_emu_hi) return "emu-range";
  if(t>=IMP_BASE && t<IMP_BASE+4u*imps_total()) return "import";
  if(t>=0xE9000000u && t<0xE9010000u) return "xaudio";
  if(t>=CI_COM_BASE && t<CI_COM_BASE+4u*g_com_nmethods) return "com";
  if(t>=0xE8000000u && t<0xE8000040u) return "dyn";
  if(t==0xaf2950u||t==0xaf12b0u||t==0xaf3480u||t==0xaf1240u) return "patched";
  if(lookup(t)) return (t>=0x00400000u && t<0x10700000u)?"ft-miss":"functab-high";
  if(t>=0x401000u && t<0x10d4000u && R8(t)==0xFF && R8(t+1)==0x25) return "thunk";
  if(dll_contains(t)) return "dll-emu";
  return "other"; }
