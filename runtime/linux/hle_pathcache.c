/* BSI_PATHCACHE (notes/HLE_HOT.md, item H3; ROADMAP Phase 2b item 16): memoised file-existence / attribute probes for
   the read-only game tree C:\BSI\ (never C:\BSIUSER\).  #included by hle_fast.c and by regress/hle/hle_fast_test.c.

   Evidence: fstatat64 is 0.34-2.32 % of the GameThread in every Quest in-app profile (v0.8.2a 2.0 %, v0.8.4c 1.74 %,
   profit2 2.32 %, crash3 1.34 %, run2 0.34 %) and nowhere else.  Each probe goes through g2h() in hle_file.c, which
   stat()s every path component again (C:\BSI\XGame\CookedPCConsole\X.upk = 4 component stats + the final stat).
   Callers in the exe: GetFileAttributesW (0x49E910 FFileManagerWindows, 0x4B4E20), _wstat64i32 (0x482340, 0x482480,
   0x49EC80), CreateFileW (7 sites; a failed open of a missing file is also a probe).

   How: the HLE handlers are wrapped (function-pointer override, hle_fast.c); a hit answers from the cache, a miss
   calls the original handler and records its result:
     GetFileAttributesW   -> eax (attribute or INVALID_FILE_ATTRIBUTES; failure also sets last error 2 as before)
     GetFileAttributesExW -> eax + the 36-byte WIN32_FILE_ATTRIBUTE_DATA
     _wstat64i32 / _stat64i32 -> eax + the 48-byte struct
     CreateFileW / CreateFileA -> only FAILED read-only OPEN_EXISTING opens with last error 2 (negative cache)
   Key = the exact guest string (UTF-16 code units, no case folding, so a hit is always the answer the original gave
   for that exact string) + the API.  Only absolute "X:\..." paths under C:\BSI\ without ".." are cached.
   Invalidation: any CreateFileW/A that can write or create, DeleteFileW, CreateDirectoryW flushes the whole cache
   (rare: config/log writes) and adds that file's directory to a never-cache list, so files the game writes (e.g.
   C:\BSI\XGame\Logs\Launch.log, whose size changes with every WriteFile) are never answered from the cache.
   BSI_PATHCACHE=0: off.  BSI_PATHCACHE_STAT=1: hit/miss counts and the 12 most repeated keys every report. */
#include <stdint.h>
#include <pthread.h>

#define PC_N 4096                    /* entries (power of 2); full -> flush */
#define PC_KEYMAX 300                /* UTF-16 units */
typedef struct { uint32_t h; uint8_t api, used; uint16_t klen; uint16_t *key; uint32_t eax; uint8_t out[48]; uint32_t hits; } PcEnt;
static PcEnt g_pc[PC_N]; static int g_pc_n; static pthread_mutex_t g_pc_mu=PTHREAD_MUTEX_INITIALIZER;
static int g_pc_stat; static uint64_t g_pc_hit, g_pc_miss, g_pc_flush, g_pc_skip;
static uint16_t *g_pc_nocache[64]; static int g_pc_nnc, g_pc_off_writes;   /* directory prefixes written by the game (UTF-16, lower-cased) */

enum { PC_ATTR=1, PC_ATTREX=2, PC_WSTAT=3, PC_STATA=4, PC_OPENW=5, PC_OPENA=6 };
static const uint8_t g_pc_outlen[7]={0,0,36,48,48,0,0};

static uint16_t pc_low(uint16_t ch){ return (ch>='A'&&ch<='Z')?ch+32:(ch=='/'?'\\':ch); }
/* read the guest string (wide or narrow) into k; returns the length, or -1 if it must not be cached */
static int pc_key(uint32_t p, int wide, uint16_t *k){
  if(!p) return -1;
  int n=0; for(;n<PC_KEYMAX;n++){ uint16_t ch=wide?R16(p+2*n):R8(p+n); if(!ch) break; k[n]=ch; }
  if(n>=PC_KEYMAX || n<7 || g_pc_off_writes) return -1;
  /* absolute drive path under C:\BSI\ but not C:\BSIUSER */
  static const char pre[]="c:\\bsi\\";
  for(int i=0;i<7;i++) if(pc_low(k[i])!=(uint16_t)pre[i]) return -1;
  for(int i=2;i+1<n;i++) if(k[i]=='.' && k[i+1]=='.') return -1;
  for(int d=0;d<g_pc_nnc;d++){ const uint16_t *q=g_pc_nocache[d]; int i=0; while(q[i] && i<n && pc_low(k[i])==q[i]) i++; if(!q[i]) return -1; }
  /* (keys are not normalised: ".." was rejected above and ".\\" segments only make a key miss, never a wrong hit) */
  return n;
}
static uint32_t pc_hash(const uint16_t *k, int n, int api){ uint32_t h=2166136261u^(uint32_t)api; for(int i=0;i<n;i++){ h^=k[i]; h*=16777619u; } return h|1; }
static void pc_flush_locked(void){ for(int i=0;i<PC_N;i++){ free(g_pc[i].key); g_pc[i].key=0; g_pc[i].used=0; } g_pc_n=0; g_pc_flush++; }
static PcEnt *pc_find_locked(uint32_t h, const uint16_t *k, int n, int api){
  for(uint32_t i=h&(PC_N-1),j=0;j<PC_N;j++,i=(i+1)&(PC_N-1)){ PcEnt *e=&g_pc[i]; if(!e->used) return 0;
    if(e->h==h && e->api==api && e->klen==n && !memcmp(e->key,k,2*n)) return e; }
  return 0; }
static void pc_put_locked(uint32_t h, const uint16_t *k, int n, int api, uint32_t eax, const void *out){
  if(g_pc_n>=PC_N*3/4) pc_flush_locked();
  for(uint32_t i=h&(PC_N-1);;i=(i+1)&(PC_N-1)){ PcEnt *e=&g_pc[i]; if(e->used){ if(e->h==h && e->api==api && e->klen==n && !memcmp(e->key,k,2*n)) return; continue; }
    e->key=malloc(2*n); if(!e->key) return; memcpy(e->key,k,2*n); e->h=h; e->api=(uint8_t)api; e->klen=(uint16_t)n; e->eax=eax; e->hits=0;
    if(g_pc_outlen[api]) memcpy(e->out,out,g_pc_outlen[api]);
    e->used=1; g_pc_n++; return; }
}
/* guest cwd as the game set it (hle_file.c resolves relative paths against the same string); only used to turn a
   relative write path into the absolute directory that must not be cached */
static uint16_t g_pc_cwd[PC_KEYMAX]={'c',':','\\','b','s','i','\\','b','i','n','a','r','i','e','s','\\','w','i','n','3','2',0};
/* absolute, lower-case, '\' separated, "." and ".." collapsed; returns length or -1 */
static int pc_abs(uint32_t p, int wide, uint16_t *o){
  uint16_t t[2*PC_KEYMAX]; int n=0, m=0;
  if(!p) return -1;
  for(;n<PC_KEYMAX-1;n++){ uint16_t ch=wide?R16(p+2*n):R8(p+n); if(!ch) break; t[n]=pc_low(ch); }
  if(n>=PC_KEYMAX-1) return -1;
  uint16_t s[2*PC_KEYMAX]; int sl=0;
  if(n>=2 && t[1]==':'){ memcpy(s,t,2*n); sl=n; }
  else if(n>=1 && t[0]=='\\'){ s[0]='c'; s[1]=':'; memcpy(s+2,t,2*n); sl=n+2; }
  else { int cl=0; while(g_pc_cwd[cl]) cl++; memcpy(s,g_pc_cwd,2*cl); s[cl]='\\'; memcpy(s+cl+1,t,2*n); sl=cl+1+n; }
  /* collapse into o: keep "c:" then components */
  o[m++]=s[0]; o[m++]=s[1]; int i=2;
  while(i<sl){ while(i<sl && s[i]=='\\') i++; int b=i; while(i<sl && s[i]!='\\') i++; int L=i-b; if(!L) break;
    if(L==1 && s[b]=='.') continue;
    if(L==2 && s[b]=='.' && s[b+1]=='.'){ while(m>2 && o[m-1]!='\\') m--; if(m>2) m--; continue; }
    if(m+1+L>=PC_KEYMAX) return -1;
    o[m++]='\\'; memcpy(o+m,s+b,2*L); m+=L; }
  o[m]=0; return m;
}
/* the game is about to write/create/delete: if the target is (or may be) under C:\BSI, flush, and never cache that
   directory again.  C:\BSIUSER targets (saves, user config) do not touch the cache. */
static void pc_invalidate(uint32_t p, int wide){
  uint16_t k[PC_KEYMAX]; int n=pc_abs(p,wide,k);
  if(n>=10){ static const char u[]="c:\\bsiuser"; int i=0; while(i<10 && k[i]==(uint16_t)u[i]) i++; if(i==10 && (n==10||k[10]=='\\')) return; }
  pthread_mutex_lock(&g_pc_mu);
  pc_flush_locked();
  if(n>0){
    int d=n; while(d>0 && k[d-1]!='\\') d--; if(d>0) d--;   /* directory without the trailing separator */
    k[d]='\\'; d++;                                          /* store "dir\" */
    int known=0; for(int i=0;i<g_pc_nnc;i++){ const uint16_t *q=g_pc_nocache[i]; int j=0; while(q[j] && j<d && q[j]==k[j]) j++; if(!q[j]) known=1; }
    if(!known && g_pc_nnc<64){ uint16_t *q=malloc(2*(d+1)); if(q){ memcpy(q,k,2*d); q[d]=0; g_pc_nocache[g_pc_nnc++]=q; } }
  } else g_pc_off_writes=1;   /* unparsable write path: stop caching for the rest of the session */
  pthread_mutex_unlock(&g_pc_mu);
}
static void pc_setcwd(uint32_t p){ uint16_t k[PC_KEYMAX]; int n=pc_abs(p,1,k); if(n>0){ pthread_mutex_lock(&g_pc_mu); memcpy(g_pc_cwd,k,2*(n+1)); pthread_mutex_unlock(&g_pc_mu); } }

typedef void (*pc_hfn)(CPU*);
static pc_hfn pc_old_attr, pc_old_attrex, pc_old_wstat, pc_old_stata, pc_old_openw, pc_old_opena, pc_old_del, pc_old_mkdir, pc_old_cwd;
/* generic memoising wrapper: path at ARG(pidx), output buffer at ARG(oidx), stdcall pop nstd (or cdecl if nstd<0) */
static void pc_wrap(CPU*c, int api, int wide, int pidx, int oidx, int nstd, pc_hfn old){
  uint16_t k[PC_KEYMAX]; int n=pc_key(ARG(pidx),wide,k);
  if(n<0){ if(g_pc_stat) __atomic_fetch_add(&g_pc_skip,1,__ATOMIC_RELAXED); old(c); return; }
  uint32_t h=pc_hash(k,n,api), outp= oidx>=0 ? ARG(oidx) : 0; int ol=g_pc_outlen[api];
  pthread_mutex_lock(&g_pc_mu);
  PcEnt *e=pc_find_locked(h,k,n,api);
  if(e && (!ol || outp)){
    uint32_t eax=e->eax; uint8_t out[48]; if(ol) memcpy(out,e->out,ol); e->hits++;
    pthread_mutex_unlock(&g_pc_mu); g_pc_hit++;
    if(ol && (api==PC_ATTREX ? eax!=0 : eax==0)) memcpy(MEMP(outp),out,ol);
    if((api==PC_ATTR && eax==0xffffffffu) || (api==PC_ATTREX && eax==0) || (api==PC_STATA && eax==0xffffffffu)) g_last_error=2;   /* as the old handlers (fill_stat sets it, _wstat64i32 does not) */
    if(api==PC_OPENW || api==PC_OPENA) g_last_error=2;
    c->eax=eax; c->esp+=4+4*(nstd>0?nstd:0); return;
  }
  pthread_mutex_unlock(&g_pc_mu); g_pc_miss++;
  old(c);
  uint32_t eax=c->eax; uint8_t out[48]={0};
  if(api==PC_OPENW || api==PC_OPENA){ if(eax!=0xffffffffu || g_last_error!=2) return; }   /* negative opens only */
  if(ol && (api==PC_ATTREX ? eax!=0 : eax==0)) memcpy(out,MEMP(outp),ol);
  pthread_mutex_lock(&g_pc_mu); pc_put_locked(h,k,n,api,eax,out); pthread_mutex_unlock(&g_pc_mu);
}
static void hf_GetFileAttributesW(CPU*c){ pc_wrap(c,PC_ATTR,1,0,-1,1,pc_old_attr); }
static void hf_GetFileAttributesExW(CPU*c){ pc_wrap(c,PC_ATTREX,1,0,2,3,pc_old_attrex); }
static void hf__wstat64i32(CPU*c){ pc_wrap(c,PC_WSTAT,1,0,1,-1,pc_old_wstat); }
static void hf__stat64i32(CPU*c){ pc_wrap(c,PC_STATA,0,0,1,-1,pc_old_stata); }
/* CreateFile(name, access, share, sa, disposition, flags, template): read-only OPEN_EXISTING may be answered negatively */
static void hf_CreateFileW(CPU*c){ uint32_t acc=ARG(1), disp=ARG(4);
  if((acc&0x40000000u) || disp!=3){ pc_invalidate(ARG(0),1); pc_old_openw(c); return; }
  pc_wrap(c,PC_OPENW,1,0,-1,7,pc_old_openw); }
static void hf_CreateFileA(CPU*c){ uint32_t acc=ARG(1), disp=ARG(4);
  if((acc&0x40000000u) || disp!=3){ pc_invalidate(ARG(0),0); pc_old_opena(c); return; }
  pc_wrap(c,PC_OPENA,0,0,-1,7,pc_old_opena); }
static void hf_DeleteFileW(CPU*c){ pc_invalidate(ARG(0),1); pc_old_del(c); }
static void hf_CreateDirectoryW(CPU*c){ pc_invalidate(ARG(0),1); pc_old_mkdir(c); }
static void hf_SetCurrentDirectoryW(CPU*c){ pc_setcwd(ARG(0)); pc_old_cwd(c); }

static void pc_config(void){ const char *s=getenv("BSI_PATHCACHE_STAT"); g_pc_stat= s&&*s&&*s!='0'; }
static void pc_report(FILE *f){
  if(!g_pc_stat) return;
  fprintf(f,"[pathcache] hits %llu, misses %llu, not cacheable %llu, flushes %llu, entries %d, no-cache dirs %d\n",
    (unsigned long long)g_pc_hit,(unsigned long long)g_pc_miss,(unsigned long long)g_pc_skip,(unsigned long long)g_pc_flush,g_pc_n,g_pc_nnc);
  pthread_mutex_lock(&g_pc_mu);
  static const char *an[7]={"","attr","attrEx","wstat","statA","openW-","openA-"};
  for(int r=0;r<12;r++){ int best=-1; for(int i=0;i<PC_N;i++) if(g_pc[i].used && (int)g_pc[i].hits>0 && (best<0||g_pc[i].hits>g_pc[best].hits)) best=i; if(best<0) break;
    PcEnt *e=&g_pc[best]; char t[PC_KEYMAX+1]; int j=0; for(;j<e->klen;j++) t[j]=e->key[j]<128?(char)e->key[j]:'?'; t[j]=0;
    fprintf(f,"[pathcache]   %8u x %-6s %s\n",e->hits,an[e->api],t); e->hits|=0x80000000u; }
  for(int i=0;i<PC_N;i++) g_pc[i].hits&=0x7fffffffu;
  pthread_mutex_unlock(&g_pc_mu);
}
