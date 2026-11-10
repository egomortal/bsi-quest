/* Guest (32-bit, recompiled) Direct3D 11 / DXGI  ->  host DXVK (64-bit, Vulkan).
   Guest COM objects are the same 64-byte wrappers the null renderer uses (vtable of magic addresses);
   a wrapper that fronts a DXVK object keeps the host pointer at +32 and the tag 'BRDG' at +40.
   Enabled with BSI_DXVK=1 (libdxvk_dxgi.so / libdxvk_d3d11.so from BSI_DXVK_DIR, WSI via SDL2). */
#define _GNU_SOURCE
#include <unistd.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <errno.h>
#include <time.h>
#include "bridge.h"
#include "com.h"
#include "dxbc_info.h"
#include <dlfcn.h>
#include <signal.h>
#include <math.h>
#include <time.h>
#include <pthread.h>
#ifndef BSI_ANDROID
#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#else
#include "android_plat.h"
typedef void SDL_Window;
#endif

uint32_t com_newobj_named(const char *n);
int com_iface_idx(const char *n);
int g_bridge_on;
static void brq_init(void*);
static void *lib_dxgi, *lib_d3d11;
static pthread_mutex_t br_mu = PTHREAD_MUTEX_INITIALIZER;
#define TAG 0x47445242u

/* ---- host <-> guest object map */
#define NMAP (1<<18)
static void *mh[NMAP]; static uint32_t mg[NMAP];
static uint32_t hslot(void *h){ uint64_t x=(uint64_t)(uintptr_t)h; x^=x>>29; x*=0x9E3779B97F4A7C15ull; return (uint32_t)(x>>46)&(NMAP-1); }
void *br_host(uint32_t g){ if(!g || R32(g+40)!=TAG) return 0; uint64_t h; memcpy(&h,MEMP(g+32),8); return (void*)(uintptr_t)h; }
#define TOMB ((void*)1)
/* generic interface names (IUnknown, ID3D11Resource, ...) are refined to the concrete type so the guest vtable is complete */
static const char *refine(void *host, const char *iface){
  static const char *cand[]={"ID3D11Texture2D","ID3D11Buffer","ID3D11Texture3D","ID3D11Texture1D","ID3D11ShaderResourceView","ID3D11RenderTargetView",
    "ID3D11DepthStencilView","ID3D11UnorderedAccessView","IDXGISwapChain","IDXGIAdapter1","IDXGIOutput","IDXGIFactory1","IDXGISurface1",0};
  if(strcmp(iface,"IUnknown") && strcmp(iface,"ID3D11Resource") && strcmp(iface,"ID3D11View") && strcmp(iface,"ID3D11DeviceChild")
     && strcmp(iface,"IDXGIObject") && strcmp(iface,"IDXGIResource") && strcmp(iface,"IDXGIDeviceSubObject")) return iface;
  for(int k=0;cand[k];k++){ int i=com_iface_idx(cand[k]); if(i<0) continue; void *o=0;
    int32_t hr=((int32_t(*)(void*,const void*,void**))((*(void***)host)[0]))(host,&g_com_ifaces[i].iid,&o);
    if(hr>=0 && o){ ((uint32_t(*)(void*))((*(void***)o)[2]))(o); if(o==host) return cand[k]; } }
  return iface;
}
uint32_t br_wrap(void *host, const char *iface){
  if(!host) return 0;
  pthread_mutex_lock(&br_mu);
  uint32_t k=hslot(host); int64_t free_at=-1;
  for(int n=0;n<NMAP;n++,k=(k+1)&(NMAP-1)){
    if(mh[k]==host){ uint32_t g=mg[k]; pthread_mutex_unlock(&br_mu); return g; }
    if(mh[k]==TOMB){ if(free_at<0) free_at=k; continue; }
    if(!mh[k]) break; }
  if(free_at>=0) k=(uint32_t)free_at;
  iface=refine(host,iface);
  int i=com_iface_idx(iface); if(i<0) i=com_iface_idx("IUnknown");
  uint32_t g=com_newobj_named(i>=0?g_com_ifaces[i].name:"IUnknown");
  uint64_t hv=(uint64_t)(uintptr_t)host; memcpy(MEMP(g+32),&hv,8); W32(g+40,TAG);
  mh[k]=host; mg[k]=g;
  pthread_mutex_unlock(&br_mu);
  return g;
}
/* host object destroyed: drop the mapping so a new object at the same address gets a fresh wrapper of its own type */
/* LEAK.md fix: (1) the slot is deleted by backward shift (linear probing), so no tombstones pile up and lookups of new
   host pointers stay short; (2) the dead 64-byte guest wrapper is recycled (gfree -> reused by the next newobj of the same
   size class) after a quarantine of BR_QUAR later forgets, instead of leaking. UE3 creates and releases ~52 sampler
   states per frame on average (0-470 depending on the scene), each of which used to leave one 128-byte guest block
   behind (~14 MB/min at 36 fps). The quarantine keeps a stale guest pointer harmless for a long time (as in D3D11, a
   released object may stay alive while it is bound). A dead wrapper falls through to the null renderer (tag cleared), so
   its null-path refcount (+4) is set to a sentinel that stray AddRef/Release pairs cannot bring to 0 (com_release would
   gfree it); the quarantine frees it only if the sentinel is still there (never a double free). */
#define BR_QUAR 16384
#define BR_DEADREF 0x40000000u
static uint32_t br_quar[BR_QUAR]; static uint32_t br_quar_n;
static uint64_t br_n_forget, br_n_recycled;
void gfree(uint32_t p);
static void br_del_slot(uint32_t i){   /* br_mu held */
  uint32_t j=i;
  for(;;){ j=(j+1)&(NMAP-1); if(!mh[j]) break;
    if(mh[j]==TOMB){ mh[i]=TOMB; mg[i]=0; i=j; continue; }   /* (no tombstones are created any more; kept for safety) */
    uint32_t h=hslot(mh[j]);
    if(((j-h)&(NMAP-1)) >= ((j-i)&(NMAP-1))){ mh[i]=mh[j]; mg[i]=mg[j]; i=j; } }   /* hole i lies on j's probe path */
  mh[i]=0; mg[i]=0; }
void br_forget(void *host){
  pthread_mutex_lock(&br_mu);
  uint32_t k=hslot(host);
  for(int n=0;n<NMAP;n++,k=(k+1)&(NMAP-1)){ if(mh[k]==host){ uint32_t g=mg[k]; W32(g+40,0); br_del_slot(k); br_n_forget++;
      static int rec=-1; if(rec<0){ const char *e=getenv("BSI_BR_RECYCLE"); rec=e?atoi(e)>0:1; }
      if(rec){ W32(g+4,BR_DEADREF); uint32_t old=br_quar[br_quar_n%BR_QUAR]; br_quar[br_quar_n%BR_QUAR]=g; br_quar_n++;
        if(old && R32(old+40)!=TAG && R32(old+4)-(BR_DEADREF-0x10000u)<0x20000u){ gfree(old); br_n_recycled++; } }
      break; }
    if(!mh[k]) break; }
  pthread_mutex_unlock(&br_mu);
}
void *br_hostc(uint32_t g){ void *h=br_host(g); if(h||!g) return h;
  static uint32_t seen[256]; static int ns; for(int k=0;k<ns;k++) if(seen[k]==g) return 0; if(ns<256) seen[ns++]=g;
  uint32_t vt=R32(g), m0=vt?R32(vt+8):0; const char *nm= (m0>=0xF0000000u && m0<0xF0000000u+4*8192)? g_com_methods[(m0-0xF0000000u)/4].name : "?";
  fprintf(stderr,"[bridge] non-bridged object %08x (%s) passed to DXVK\n",g,nm); return 0; }
const char *br_iid_name(void *iid){
  if(!iid) return "IUnknown";
  for(int i=0;i<g_com_nifaces;i++) if(g_com_ifaces[i].iid.d1 && !memcmp(iid,&g_com_ifaces[i].iid,16)) return g_com_ifaces[i].name;
  return "IUnknown";
}
void br_null(CPU*c, const char *what, int argbytes){ static int n; if(n++<20) fprintf(stderr,"[bridge] %s on a non-bridged object\n",what); c->eax=0x80004005u; c->esp+=4+argbytes; }

/* ---- windows: guest HWND -> SDL window */
static uint32_t wg[64]; static SDL_Window *wh[64]; static int nw;
void *br_handle(uint32_t g){
  if(!g) return 0;
  for(int k=0;k<nw;k++) if(wg[k]==g) return wh[k];
  int w=1280,h=720; if(getenv("BSI_WIN_W")) w=atoi(getenv("BSI_WIN_W")); if(getenv("BSI_WIN_H")) h=atoi(getenv("BSI_WIN_H"));
#ifdef BSI_ANDROID
  SDL_Window *s=android_window();   /* the activity's ANativeWindow; DXVK's Android WSI takes it as HWND */
  if(!s){ fprintf(stderr,"[bridge] no Android window\n"); return 0; }
#else
  SDL_Window *s=SDL_CreateWindow("BioShock Infinite (recompiled)",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,w,h,SDL_WINDOW_VULKAN|SDL_WINDOW_SHOWN);
  if(!s){ fprintf(stderr,"[bridge] SDL_CreateWindow failed: %s\n",SDL_GetError()); return 0; }
#endif
  fprintf(stderr,"[bridge] window %08x -> SDL window %dx%d\n",g,w,h);
  if(nw<64){ wg[nw]=g; wh[nw]=s; nw++; }
  return s;
}
uint32_t br_handle_to_guest(void *h){ for(int k=0;k<nw;k++) if(wh[k]==h) return wg[k]; return 0; }

/* BSI_ZC (default on, =0 disables): zero-copy Map. DXVK's mapped memory lives inside the guest window (see bsi_gva_alloc in rt_linux.c), so Map
   returns the guest address of DXVK's own pointer: no shadow, no copy at Unmap, no write protection. Maps whose pointer is
   outside the window (allocator refused, driver without placed maps / host import) take the shadow path as before. */
static int br_zc_on=-1;
/* default ON since step 3 (validated in the cloud); BSI_ZC=0 = shadow + KEEP path. With BSI_BRIDGE_THREAD=1 the default is
   OFF: a zero-copy Map needs DXVK's new slice address at once, so every Map drains the queue (measured: 2.7 ms/frame of
   bridge stalls, 1%-low 44 vs 60); BSI_ZC=1 still forces it on. */
static int br_zc(void){ if(br_zc_on<0){ const char *e=getenv("BSI_ZC"), *q=getenv("BSI_BRIDGE_THREAD");
    br_zc_on=e?atoi(e)>0:!(q&&atoi(q)>0); } return br_zc_on; }
static uint64_t st_zc;
/* ---- init / exports */
typedef int32_t (*PFN_CreateDXGIFactory)(const void *riid, void **pp);
typedef int32_t (*PFN_D3D11CreateDevice)(void*,uint32_t,void*,uint32_t,const uint32_t*,uint32_t,uint32_t,void**,uint32_t*,void**);
int bridge_init(void){
  if(!getenv("BSI_DXVK")) return 0;
  const char *dir=getenv("BSI_DXVK_DIR"); char p[1024];
#ifndef BSI_ANDROID
  if(!getenv("DXVK_WSI_DRIVER")) setenv("DXVK_WSI_DRIVER","SDL2",1);
  if(SDL_Init(SDL_INIT_VIDEO)){ fprintf(stderr,"[bridge] SDL_Init: %s\n",SDL_GetError()); return 0; }
#else
  if(!getenv("DXVK_WSI_DRIVER")) setenv("DXVK_WSI_DRIVER","Android",1);
#endif
  { const char *r=getenv("BSI_RHI"); if(r && strstr(r,"b1n")){ fprintf(stderr,"[bridge] BSI_RHI=b1n: bridge off, D3D11 objects come from the null device, b1 renders and presents\n"); return 0; } }
  snprintf(p,sizeof p,"%s/libdxvk_dxgi.so",dir?dir:"."); lib_dxgi=dlopen(p,RTLD_NOW);
  snprintf(p,sizeof p,"%s/libdxvk_d3d11.so",dir?dir:"."); lib_d3d11=dlopen(p,RTLD_NOW);
  if(!lib_dxgi||!lib_d3d11){ fprintf(stderr,"[bridge] cannot load DXVK: %s\n",dlerror()); return 0; }
  g_bridge_on=1; fprintf(stderr,"[bridge] DXVK loaded, D3D11 goes to Vulkan\n");
  if(br_zc()){   /* hand DXVK (local patch) the guest-window range allocator before the device exists */
    void *bsi_gva_alloc(size_t,size_t); void bsi_gva_free(void*,size_t); char hv[64];
    snprintf(hv,sizeof hv,"%lx,%lx",(unsigned long)(uintptr_t)bsi_gva_alloc,(unsigned long)(uintptr_t)bsi_gva_free); setenv("BSI_GVA_HOOK",hv,1);
    fprintf(stderr,"[bridge] BSI_ZC: zero-copy Map requested (DXVK places its mapped memory in the guest window)\n"); }
  return 1;
}
#define ARGG(k) R32(c->esp+4+4*(k))
int bridge_dyn(CPU*c, const char *n){
  if(!g_bridge_on) return 0;
  if(!strcmp(n,"CreateDXGIFactory")||!strcmp(n,"CreateDXGIFactory1")){
    PFN_CreateDXGIFactory f=(PFN_CreateDXGIFactory)dlsym(lib_dxgi,n); void *pp=0;
    int32_t hr=f(GP(ARGG(0)),&pp); W32(ARGG(1),pp?br_wrap(pp,br_iid_name(GP(ARGG(0)))):0);
    if(hr<0){ fprintf(stderr,"[bridge] FATAL: DXVK found no usable Vulkan GPU (see the dxvk logs). Stopping.\n"); fflush(stderr); _exit(3); }
    fprintf(stderr,"[bridge] %s -> %08x\n",n,hr); c->eax=hr; c->esp+=4+8; return 1; }
  if(!strcmp(n,"D3D11CreateDevice")){
    PFN_D3D11CreateDevice f=(PFN_D3D11CreateDevice)dlsym(lib_d3d11,"D3D11CreateDevice");
    void *dev=0,*ctx=0; uint32_t fl=0;
    int32_t hr=f(br_host(ARGG(0)),ARGG(1),0,ARGG(3),(const uint32_t*)GP(ARGG(4)),ARGG(5),ARGG(6),ARGG(7)?&dev:0,&fl,ARGG(9)?&ctx:0);
    if(ARGG(7)) W32(ARGG(7),br_wrap(dev,"ID3D11Device"));
    if(ARGG(8)) W32(ARGG(8),fl);
    if(ARGG(9)) W32(ARGG(9),br_wrap(ctx,"ID3D11DeviceContext"));
    if(hr>=0 && ctx) brq_init(ctx);
    { void bf_new_device(void); bf_new_device(); }   /* BSI_BRFAST: forget the bind shadow */
    if(hr>=0 && dev){ void pw_start_(void*); pw_start_(dev); }   /* BSI_PREWARM: replay the recorded shaders (Fable #29) */
    fprintf(stderr,"[bridge] D3D11CreateDevice -> %08x (feature level %x)\n",hr,fl); c->eax=hr; c->esp+=4+40; return 1; }
  return 0;
}

/* ---- hand-written methods (structs that contain pointers / host-sized fields) */
#define SELF void *self=br_host(ARGG(0)); if(!self){ br_null(c,__func__,0); return; }
#define VT(i) ((*(void***)self)[i])
typedef struct { void *pSysMem; uint32_t SysMemPitch, SysMemSlicePitch; } HSUB;
static HSUB *conv_sub(uint32_t g, uint32_t n){ if(!g||!n) return 0; HSUB *h=calloc(n,sizeof *h);
  for(uint32_t k=0;k<n;k++){ h[k].pSysMem=GP(R32(g+12*k)); h[k].SysMemPitch=R32(g+12*k+4); h[k].SysMemSlicePitch=R32(g+12*k+8); } return h; }
static uint32_t nsub(uint32_t desc, int dims){ uint32_t mips, arr=1;
  if(dims==1){ mips=R32(desc+4); arr=R32(desc+8); } else if(dims==2){ mips=R32(desc+8); arr=R32(desc+12); } else { mips=R32(desc+12); }
  if(!mips){ uint32_t w=R32(desc), h=dims>=2?R32(desc+4):1, m=1; while(w>1||h>1){ w>>=1; h>>=1; m++; } mips=m; }
  return mips*(arr?arr:1); }
static void create_res(CPU*c, int vt, int dims, const char *iface){ SELF
  uint32_t desc=ARGG(1), init=ARGG(2), pp=ARGG(3);
  HSUB *h = conv_sub(init, dims==0?1:nsub(desc,dims)); void *out=0;
  int32_t hr=((int32_t(*)(void*,void*,void*,void**))VT(vt))(self,GP(desc),h,pp?&out:0); free(h); if(hr<0) br_fail(iface,hr);
  if(pp) W32(pp,br_wrap(out,iface)); c->eax=hr; c->esp+=4+12+4; }
void bridge_ID3D11Device_CreateBuffer(CPU*c){ create_res(c,3,0,"ID3D11Buffer"); }
void bridge_ID3D11Device_CreateTexture1D(CPU*c){ create_res(c,4,1,"ID3D11Texture1D"); }
void bridge_ID3D11Device_CreateTexture2D(CPU*c){ create_res(c,5,2,"ID3D11Texture2D"); }
void bridge_ID3D11Device_CreateTexture3D(CPU*c){ create_res(c,6,3,"ID3D11Texture3D"); }
typedef struct { const char *SemanticName; uint32_t SemanticIndex, Format, InputSlot, AlignedByteOffset, InputSlotClass, InstanceDataStepRate; } HIED;
void bridge_ID3D11Device_CreateInputLayout(CPU*c){ SELF
  uint32_t g=ARGG(1), n=ARGG(2); HIED *e=calloc(n?n:1,sizeof *e);
  for(uint32_t k=0;k<n;k++){ uint32_t b=g+28*k; e[k].SemanticName=GP(R32(b)); e[k].SemanticIndex=R32(b+4); e[k].Format=R32(b+8); e[k].InputSlot=R32(b+12); e[k].AlignedByteOffset=R32(b+16); e[k].InputSlotClass=R32(b+20); e[k].InstanceDataStepRate=R32(b+24); }
  void *out=0; int32_t hr=((int32_t(*)(void*,void*,uint32_t,void*,size_t,void**))VT(11))(self,e,n,GP(ARGG(3)),ARGG(4),ARGG(5)?&out:0); free(e);
  if(ARGG(5)) W32(ARGG(5),br_wrap(out,"ID3D11InputLayout")); c->eax=hr; c->esp+=4+20+4; }
/* ---- shader dump / override at creation (header: bridge.h, DXBC rules: dxbc_info.h). The six ID3D11Device::Create*Shader wrappers in
   bridge_gen.c (gen_bridge.py SHADER_HOOK) call br_shader_create() in place of the vtable slot; the stream-output GS below uses
   shk_swap() directly. The hash is taken here, on the calling thread, over the bytes the game passed (guest memory, before the DXVK
   call; Create* are never queued by bridge_q.c). No env = the plain vtable call, nothing else runs.
   BSI_SHADER_DUMP_DIR=<dir> (or BSI_SHADER_DUMP=1: /sdcard/BSI/shaders/dump on Android, /tmp/bsishaders elsewhere): every distinct
     shader once as <dir>/<stage>_<hash16>.dxbc (O_EXCL: files from earlier runs stay, the line then says "have") plus one line in
     <dir>/shaders.txt: creation seq, seconds since the first create, stage, hash, bytes, profile from the version token, chunk list,
     cs thread group, groupshared bytes, status (+",ovr" when an override was applied to it).
   BSI_SHADER_OVERRIDE_DIR=<dir>: <dir>/<stage>_<hash16>.dxbc replaces the game's bytecode in the DXVK call. The guest pointer/length
     stay as they are (UE3 reuses the VS blob for CreateInputLayout: with an identical ISGN that is fine); the replacement is host
     memory kept for the rest of the process (cheap, reused if the game recreates the shader). DXVK consumes the bytecode inside
     Create*Shader (D3D11CommonShader ctor: DxbcModule parses + compiles to SPIR-V, keeps no pointer to the input), so the buffer only
     has to outlive the call. Bad files (no 'DXBC' magic, < 32 bytes, no program chunk, program type != stage) are ignored and logged
     once; a Create that fails with an override (DXVK: E_INVALIDARG) is logged and redone with the game's bytecode. */
static int shk_mode=-1; static const char *shk_dump_dir, *shk_ovr_dir; static pthread_mutex_t shk_mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t *shk_seen; static unsigned shk_nseen, shk_ncreate, shk_ndist; static double shk_t0;
#define SHK_NOVR 1024
static struct { uint64_t key; uint8_t *buf; uint32_t len; } shk_ovr[SHK_NOVR]; static int shk_novr;   /* buf 0 = file known bad */
static void shk_mkdir(const char *d){ char t[600]; snprintf(t,sizeof t,"%s",d); for(char *p=t+1;*p;p++) if(*p=='/'){ *p=0; mkdir(t,0777); *p='/'; } mkdir(t,0777); }
static int shk_init(void){
  pthread_mutex_lock(&shk_mu);
  if(shk_mode<0){ int m=0; const char *dd=getenv("BSI_SHADER_DUMP_DIR"), *od=getenv("BSI_SHADER_OVERRIDE_DIR"), *sw=getenv("BSI_SHADER_DUMP");
    if((!dd||!*dd) && sw && atoi(sw)>0){   /* BSI_SHADER_DUMP=1 without a dir: the default below */
#ifdef BSI_ANDROID
      dd="/sdcard/BSI/shaders/dump";
#else
      dd="/tmp/bsishaders";
#endif
    }
    if(dd&&*dd){ shk_dump_dir=dd; shk_mkdir(dd); shk_seen=calloc(1u<<16,8); if(shk_seen) m|=1; fprintf(stderr,"[shader] dumping every distinct shader to %s (index: shaders.txt)\n",dd); }
    if(od&&*od){ shk_ovr_dir=od; m|=2; fprintf(stderr,"[shader] overrides: %s/<stage>_<hash16>.dxbc\n",od); }
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); shk_t0=ts.tv_sec+ts.tv_nsec*1e-9;
    __atomic_store_n(&shk_mode,m,__ATOMIC_RELEASE); }
  pthread_mutex_unlock(&shk_mu); return shk_mode; }
/* override lookup, cached per key (shk_mu held) */
static const uint8_t *shk_override(const char *st, const char *hx, uint64_t h, uint32_t *olen){
  for(int i=0;i<shk_novr;i++) if(shk_ovr[i].key==h){ *olen=shk_ovr[i].len; return shk_ovr[i].buf; }
  char fn[640]; snprintf(fn,sizeof fn,"%s/%s_%s.dxbc",shk_ovr_dir,st,hx);
  int fd=open(fn,O_RDONLY|O_CLOEXEC); if(fd<0) return 0;
  struct stat sb; uint8_t *buf=0; size_t n=0; const char *bad=0; DxbcInfo si;
  if(fstat(fd,&sb) || sb.st_size<32) bad="shorter than 32 bytes"; else if(sb.st_size>(16<<20)) bad="larger than 16 MB";
  else { n=(size_t)sb.st_size; buf=malloc(n); size_t r=0; while(buf && r<n){ ssize_t x=read(fd,buf+r,n-r); if(x<=0) break; r+=x; }
    if(!buf||r<n) bad="read failed"; else if(memcmp(buf,"DXBC",4)) bad="no 'DXBC' magic";
    else { dxbc_info(buf,n,&si); if(!si.ok||!si.shex) bad="no SHEX/SHDR program chunk (bad container?)";
      else if(strcmp(dxbc_stage_name(si.type),st)) bad="program type does not match the stage in the file name";
      else if(dxbc_r32(buf+24)!=n) fprintf(stderr,"[shader] override %s: container size %u != file size %zu (truncated/appended?)\n",fn,dxbc_r32(buf+24),n); } }
  close(fd);
  if(bad){ fprintf(stderr,"[shader] override %s ignored: %s\n",fn,bad); free(buf); buf=0; n=0; }
  if(shk_novr<SHK_NOVR){ shk_ovr[shk_novr].key=h; shk_ovr[shk_novr].buf=buf; shk_ovr[shk_novr].len=(uint32_t)n; shk_novr++; }
  else { static int once; if(!once++) fprintf(stderr,"[shader] more than %d override files: the rest is ignored\n",SHK_NOVR); free(buf); buf=0; n=0; }
  *olen=(uint32_t)n; return buf; }
/* dump once per distinct key: file + index line (shk_mu held) */
static void shk_dump(const char *st, const char *hx, uint64_t h, const uint8_t *code, size_t len, int ovr){
  uint32_t k=(uint32_t)((h*0x9E3779B97F4A7C15ull)>>48);   /* open addressing, 65536 slots, key 0 unused (treated as unseen) */
  if(shk_nseen>=(1u<<16)*3/4) return;
  for(;;k=(k+1)&0xffff){ if(shk_seen[k]==h) return; if(!shk_seen[k]){ shk_seen[k]=h; shk_nseen++; break; } }
  shk_ndist++;
  char fn[640]; snprintf(fn,sizeof fn,"%s/%s_%s.dxbc",shk_dump_dir,st,hx); const char *status="new";
  int fd=open(fn,O_WRONLY|O_CREAT|O_EXCL|O_CLOEXEC,0644);
  if(fd>=0){ size_t w=0; while(w<len){ ssize_t r=write(fd,code+w,len-w); if(r<=0) break; w+=r; } close(fd); if(w<len){ status="short"; unlink(fn); } }
  else if(errno==EEXIST) status="have";
  else { status="err"; static int once; if(!once++) fprintf(stderr,"[shader] cannot write %s: %s\n",fn,strerror(errno)); }
  DxbcInfo si; dxbc_info(code,len,&si); char tg[40], prof[16];
  if(si.tg[0]||si.tg[1]||si.tg[2]) snprintf(tg,sizeof tg,"%ux%ux%u",si.tg[0],si.tg[1],si.tg[2]); else strcpy(tg,"-");
  if(si.shex) snprintf(prof,sizeof prof,"%s_%u_%u",dxbc_stage_name(si.type),si.major,si.minor); else strcpy(prof,"?");
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts);
  snprintf(fn,sizeof fn,"%s/shaders.txt",shk_dump_dir); struct stat sb; int hdr=stat(fn,&sb)!=0||sb.st_size==0; FILE *f=fopen(fn,"a");
  if(f){ if(hdr) fputs("# seq t_s stage hash16 bytes profile chunks numthreads tgsm_bytes status\n",f);
    fprintf(f,"%u %.1f %s %s %zu %s %s %s %u %s%s\n",shk_ncreate,ts.tv_sec+ts.tv_nsec*1e-9-shk_t0,st,hx,len,prof,si.chunks,tg,si.tgsm,status,ovr?",ovr":""); fclose(f); }
  if(shk_ndist<=3 || shk_ndist%250==0) fprintf(stderr,"[shader] %u distinct shaders dumped (%u creates so far)\n",shk_ndist,shk_ncreate); }
/* hash (+dump) the blob at (*code, *len); with an override file, point them at it and return 1 (hx = the 16-digit key either way) */
static int shk_swap(const char *st, const void **code, size_t *len, char hx[20]){
  int m=__atomic_load_n(&shk_mode,__ATOMIC_ACQUIRE); if(m<0) m=shk_init();
  if(!m || !*code || !*len) return 0;
  uint64_t h=dxbc_key(*code,*len); snprintf(hx,20,"%016llx",(unsigned long long)h);
  pthread_mutex_lock(&shk_mu);
  shk_ncreate++;
  const uint8_t *rep=0; uint32_t rlen=0; if(m&2) rep=shk_override(st,hx,h,&rlen);
  if(m&1) shk_dump(st,hx,h,(const uint8_t*)*code,*len,rep!=0);
  pthread_mutex_unlock(&shk_mu);
  if(!rep) return 0;
  fprintf(stderr,"[shader] override %s_%s applied (%zu -> %u bytes)\n",st,hx,*len,rlen);
  *code=rep; *len=rlen; return 1; }
static void pw_record(int slot, const void *code, size_t len);
int32_t br_shader_create(void *dev, int slot, const char *st, const void *code, size_t len, void *linkage, void **out){
  typedef int32_t (*F)(void*,const void*,size_t,void*,void**); F f=(F)((*(void***)dev)[slot]);
  const void *use=code; size_t ulen=len; char hx[20];
  if(!shk_swap(st,&use,&ulen,hx)){ int32_t hr=f(dev,code,len,linkage,out); if(hr>=0) pw_record(slot,code,len); return hr; }
  int32_t hr=f(dev,use,ulen,linkage,out);
  if(hr<0){ fprintf(stderr,"[shader] override %s_%s FAILED %08x (dxvk log: container/signature/program type), creating the game's shader instead\n",st,hx,(uint32_t)hr); hr=f(dev,code,len,linkage,out); use=code; ulen=len; }
  if(hr>=0) pw_record(slot,use,ulen);
  return hr; }

/* ---- BSI_PREWARM: shader pre-creation replay (hitches note: 87 % of the >100 ms frames are pipeline-library builds at area
   changes, and every session rebuilds the libraries of every area it visits, lazily, during the burst).
   DXVK keys its pipeline libraries by shader *object*, so the only way to have a library ready before the game asks is to
   create the game's own shader objects early: the D3D11 module set dedups Create*Shader by DXBC hash, so a shader created
   here and the game's later Create*Shader of the same bytecode share one DxvkShader and one library. Mechanism:
     record: every successful Create*Shader appends (slot, len, bytes) once per distinct bytecode to the record file
             (buffered, flushed by the background thread; the bytes are the ones DXVK received, after any override);
     replay: right after D3D11CreateDevice a thread at idle priority replays the file through the device's own
             Create*Shader slots (12/13/15/16/17/18, no class linkage, ppShader = NULL: the module is created and kept,
             S_FALSE is returned and no COM object exists -- the documented D3D11 'validate only' form).
             DXVK then queues one pipeline-library build per shader on its compiler threads (normal priority, as for the
             game's own creates) -- from the Mesa disk cache when warm -- while the player is still in the menus.
   Cost: the libraries of every shader seen in earlier sessions are resident from the start (the same set a session that
   visits those areas holds at its end; trackPipelineLifetime=False never frees them anyway) and a few seconds of
   idle-priority CPU at boot. Exactness: public D3D11 calls only; a Create that fails is skipped and counted.
   BSI_PREWARM=1 record + replay, =2 record only (fill the file in one session, replay next time), unset/0 = off.
   BSI_PREWARM_FILE=<path> (default /sdcard/BSI/prewarm_shaders.bin). Log lines: [prewarm] ... */
#include <sys/resource.h>
#define PW_MAGIC 0x31575053u   /* 'SPW1' */
static int pw_mode=-1; static const char *pw_path; static pthread_mutex_t pw_mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t *pw_seen; static unsigned pw_nseen, pw_cap;            /* hashes of every record in the file + recorded this session */
static uint8_t *pw_q; static size_t pw_qn, pw_qcap;                    /* records waiting to be appended */
static void *pw_dev; static unsigned pw_created, pw_failed, pw_records;
static int pw_init(void){
  int m=__atomic_load_n(&pw_mode,__ATOMIC_ACQUIRE); if(m>=0) return m;
  const char *e=getenv("BSI_PREWARM"); m=e?atoi(e):0; if(m<0||m>2) m=0;
  pw_path=getenv("BSI_PREWARM_FILE"); if(!pw_path||!*pw_path) pw_path="/sdcard/BSI/prewarm_shaders.bin";
  __atomic_store_n(&pw_mode,m,__ATOMIC_RELEASE); return m; }
static int pw_seen_add(uint64_t h){   /* caller holds pw_mu; 1 if new */
  for(unsigned k=0;k<pw_nseen;k++) if(pw_seen[k]==h) return 0;
  if(pw_nseen==pw_cap){ pw_cap=pw_cap?pw_cap*2:4096; pw_seen=realloc(pw_seen,pw_cap*sizeof *pw_seen); }
  pw_seen[pw_nseen++]=h; return 1; }
static void pw_record(int slot, const void *code, size_t len){
  if(pw_init()==0 || !code || len<32 || len>(16u<<20)) return;
  uint64_t h=dxbc_key(code,len);
  pthread_mutex_lock(&pw_mu);
  if(pw_seen_add(h)){ size_t need=pw_qn+16+len; if(need>pw_qcap){ pw_qcap=need*2; pw_q=realloc(pw_q,pw_qcap); }
    uint32_t sl=(uint32_t)slot, ln=(uint32_t)len; memcpy(pw_q+pw_qn,&sl,4); memcpy(pw_q+pw_qn+4,&ln,4); memcpy(pw_q+pw_qn+8,&h,8); memcpy(pw_q+pw_qn+16,code,len); pw_qn=need; }
  pthread_mutex_unlock(&pw_mu); }
static void pw_flush(void){   /* append the queued records (one write per batch; the file only ever grows) */
  pthread_mutex_lock(&pw_mu); uint8_t *q=pw_q; size_t n=pw_qn; pw_q=0; pw_qn=pw_qcap=0; pthread_mutex_unlock(&pw_mu);
  if(!n) return;
  int fd=open(pw_path,O_WRONLY|O_CREAT|O_APPEND,0644);
  if(fd>=0){ struct stat st; if(fstat(fd,&st)==0 && st.st_size==0){ uint32_t mg=PW_MAGIC; if(write(fd,&mg,4)!=4){} } size_t off=0; while(off<n){ ssize_t w=write(fd,q+off,n-off); if(w<=0) break; off+=w; } close(fd); }
  else fprintf(stderr,"[prewarm] cannot append to %s (errno %d)\n",pw_path,errno);
  free(q); }
static void *pw_thread(void *arg){
  (void)arg; setpriority(PRIO_PROCESS,0,19); pthread_setname_np(pthread_self(),"BSI-prewarm");
  struct timespec t0; clock_gettime(CLOCK_MONOTONIC,&t0);
  int fd=open(pw_path,O_RDONLY); size_t sz=0; uint8_t *buf=0;
  if(fd>=0){ struct stat st; if(fstat(fd,&st)==0 && st.st_size>4){ sz=st.st_size; buf=malloc(sz); size_t off=0; while(off<sz){ ssize_t r=read(fd,buf+off,sz-off); if(r<=0) break; off+=r; } if(off!=sz){ free(buf); buf=0; } } close(fd); }
  int bad=0;
  if(buf){ uint32_t mg; memcpy(&mg,buf,4); if(mg!=PW_MAGIC){ fprintf(stderr,"[prewarm] %s: bad magic, ignored (delete the file to start over)\n",pw_path); free(buf); buf=0; bad=1; } }
  if(buf){
    size_t off=4; void ***vt=(void***)pw_dev;
    while(off+16<=sz){
      uint32_t slot, len; uint64_t h; memcpy(&slot,buf+off,4); memcpy(&len,buf+off+4,4); memcpy(&h,buf+off+8,8); off+=16;
      if(len<32 || off+len>sz || !(slot==12||slot==13||slot==15||slot==16||slot==17||slot==18)) break;   /* truncated or corrupt tail: stop */
      pw_records++;
      pthread_mutex_lock(&pw_mu); int fresh=pw_seen_add(h); pthread_mutex_unlock(&pw_mu);
      if(fresh && pw_mode==1){   /* fresh: not created by the game yet in this session */
        typedef int32_t (*F)(void*,const void*,size_t,void*,void**); F f=(F)((*vt)[slot]);
        int32_t hr=f(pw_dev,buf+off,len,0,0);   /* ppShader = NULL: D3D11 (and DXVK) create and keep the module and return S_FALSE, no COM object */
        if(hr>=0) pw_created++; else pw_failed++; }
      off+=len; }
    free(buf);
    struct timespec t1; clock_gettime(CLOCK_MONOTONIC,&t1); double ms=(t1.tv_sec-t0.tv_sec)*1e3+(t1.tv_nsec-t0.tv_nsec)*1e-6;
    fprintf(stderr,"[prewarm] %s: %u records, %u shaders created, %u failed, %.0f ms (library builds continue on the DXVK compiler threads)\n",pw_path,pw_records,pw_created,pw_failed,ms); }
  else if(!bad) fprintf(stderr,"[prewarm] %s: no records yet (recording this session)\n",pw_path);
  if(bad) return 0;                       /* do not append to a file we do not understand */
  for(;;){ sleep(2); if(!pw_dev) return 0; pw_flush(); }
  return 0; }
static void pw_start(void *dev){
  if(pw_init()==0 || !dev || pw_dev) return;
  pw_dev=dev; ((uint32_t(*)(void*))((*(void***)dev)[1]))(dev);   /* AddRef: the device outlives the thread's use of it */
  pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at,PTHREAD_CREATE_DETACHED);
  if(pthread_create(&th,&at,pw_thread,0)) fprintf(stderr,"[prewarm] thread failed\n"); pthread_attr_destroy(&at); }
void pw_start_(void *dev){ pw_start(dev); }

typedef struct { uint32_t Stream; const char *SemanticName; uint32_t SemanticIndex; uint8_t StartComponent, ComponentCount, OutputSlot; } HSO;
void bridge_ID3D11Device_CreateGeometryShaderWithStreamOutput(CPU*c){ SELF
  /* (bytecode, len, pSODecl, NumEntries, pBufferStrides, NumStrides, RasterizedStream, pClassLinkage, ppGS) */
  uint32_t g=ARGG(3), n=ARGG(4); HSO *e=calloc(n?n:1,sizeof *e);
  for(uint32_t k=0;k<n;k++){ uint32_t b=g+16*k; e[k].Stream=R32(b); e[k].SemanticName=GP(R32(b+4)); e[k].SemanticIndex=R32(b+8); e[k].StartComponent=R8(b+12); e[k].ComponentCount=R8(b+13); e[k].OutputSlot=R8(b+14); }
  typedef int32_t (*F)(void*,const void*,size_t,void*,uint32_t,void*,uint32_t,uint32_t,void*,void**); F f=(F)VT(14);
  const void *code=GP(ARGG(1)); size_t len=ARGG(2); char hx[20]; int ov=shk_swap("gs",&code,&len,hx);   /* shader dump / override hook */
  void *out=0; int32_t hr=f(self,code,len,e,n,GP(ARGG(5)),ARGG(6),ARGG(7),br_host(ARGG(8)),ARGG(9)?&out:0);
  if(hr<0 && ov){ fprintf(stderr,"[shader] override gs_%s (stream output) FAILED %08x, creating the game's shader instead\n",hx,(uint32_t)hr);
    hr=f(self,GP(ARGG(1)),ARGG(2),e,n,GP(ARGG(5)),ARGG(6),ARGG(7),br_host(ARGG(8)),ARGG(9)?&out:0); }
  free(e);
  if(ARGG(9)) W32(ARGG(9),br_wrap(out,"ID3D11GeometryShader")); c->eax=hr; c->esp+=4+36+4; }

/* Map/Unmap: the guest can only touch guest memory -> shadow buffer per (resource, subresource) */
static void *lut_cb; volatile uint32_t g_trap_addr; volatile uint64_t g_trap_host;
typedef struct { void *res; uint32_t sub; uint32_t guest; uint32_t size; void *host; uint32_t type, cur; int tracked; uint8_t *dirty; uint32_t npg;
  uint32_t gobj, rp, dp, mflags; int rp_known, deferred; uint64_t stamp; uint32_t dt_last, dt_maps; int32_t keep, lastf; } Shadow;   /* gobj..stamp: BSI_BRIDGE_THREAD (bridge_q.c) */
extern int g_brq_on; static void brq_init(void*); static void q_wait(uint64_t s); static inline uint64_t q_done(void); void brq_drain(void); static uint64_t qs_fault_waits;
static Shadow sh[4096]; static int nsh, nsh_max; static pthread_mutex_t sh_mu=PTHREAD_MUTEX_INITIALIZER;
/* Write-mapped shadows of 64 KB or more are dirty-tracked: while mapped, their guest pages are read-only; the first
   write to a page faults (bridge_segv), marks it dirty and unlocks it. Unmap then copies only the dirty pages to the
   GPU memory instead of the whole resource (UE3 maps big dynamic buffers many times per frame and writes a few KB). */
uint32_t gpages(uint32_t n); void gpages_free(uint32_t a);
#define TRACK_MIN (64u<<10)
#define NARM 1024
/* Tracked shadows stay read-only for their whole life ("armed"): the first write to a page faults once, marks it dirty
   and unlocks it; Unmap copies the dirty runs to the GPU memory and re-locks just those pages. Cost per map is
   proportional to the pages the game touched, not to the buffer size. */
static long g_pgsz; static Shadow *volatile armed[NARM];
static uint64_t st_maps, st_full, st_bytes, st_saved, st_faults, st_relocks;
/* BSI_DT_STAT=1: write pattern of tracked (dirty-page) shadows at Unmap, summarised every 10 s ("[dtstat]") */
static int dt_on=-1; static uint64_t dt_n, dt_nd[6], dt_nr[4], dt_dl[5], dt_pg;
static void dt_stat(Shadow *s){
  if(dt_on<0) dt_on=getenv("BSI_DT_STAT")!=0; if(!dt_on) return;
  uint32_t nd=0, nr=0, first=~0u, last=0;
  for(uint32_t p=0;p<s->npg;p++) if(s->dirty[p]){ nd++; if(!p||!s->dirty[p-1]) nr++; if(first==~0u) first=p; last=p; }
  dt_n++; dt_pg+=nd; s->dt_maps++;
  dt_nd[nd==0?0:nd==1?1:nd==2?2:nd<=4?3:nd<=8?4:5]++; dt_nr[nr>2?3:nr]++;
  if(nd){ int64_t d=(int64_t)first-(int64_t)s->dt_last; dt_dl[d==0?0:d==1?1:(d>1&&d<=4)?2:d<0?3:4]++; s->dt_last=last; }
  static time_t lt; time_t t=time(0); if(!lt) lt=t; if(t-lt<10) return; lt=t;
  int ns=0; uint32_t big[5]={0}; int bi[5]={-1,-1,-1,-1,-1};
  for(int k=0;k<nsh_max;k++) if(sh[k].tracked && sh[k].dt_maps){ ns++; for(int j=0;j<5;j++) if(sh[k].dt_maps>big[j]){ for(int m=4;m>j;m--){ big[m]=big[m-1]; bi[m]=bi[m-1]; } big[j]=sh[k].dt_maps; bi[j]=k; break; } }
  fprintf(stderr,"[dtstat] tracked unmaps %llu, dirty pages/unmap %.2f | ndirty 0:%llu 1:%llu 2:%llu 3-4:%llu 5-8:%llu >8:%llu | runs 0:%llu 1:%llu 2:%llu >2:%llu | first-vs-prev-last +0:%llu +1:%llu +2..4:%llu back:%llu far:%llu | shadows %d, top:",
    (unsigned long long)dt_n,dt_n?(double)dt_pg/dt_n:0,(unsigned long long)dt_nd[0],(unsigned long long)dt_nd[1],(unsigned long long)dt_nd[2],(unsigned long long)dt_nd[3],(unsigned long long)dt_nd[4],(unsigned long long)dt_nd[5],
    (unsigned long long)dt_nr[0],(unsigned long long)dt_nr[1],(unsigned long long)dt_nr[2],(unsigned long long)dt_nr[3],(unsigned long long)dt_dl[0],(unsigned long long)dt_dl[1],(unsigned long long)dt_dl[2],(unsigned long long)dt_dl[3],(unsigned long long)dt_dl[4],ns);
  for(int j=0;j<5;j++) if(bi[j]>=0) fprintf(stderr," %uKB x%u",sh[bi[j]].size>>10,big[j]); fputc('\n',stderr);
  for(int k=0;k<nsh_max;k++) sh[k].dt_maps=0; dt_n=dt_pg=0; memset(dt_nd,0,sizeof dt_nd); memset(dt_nr,0,sizeof dt_nr); memset(dt_dl,0,sizeof dt_dl); }
int bridge_segv(void *addr){
  uintptr_t a=(uintptr_t)addr;
  for(int i=0;i<NARM;i++){ Shadow *s=armed[i]; if(!s) continue; uintptr_t lo=(uintptr_t)MEMP(s->guest);
    if(a>=lo && a<lo+(uintptr_t)s->npg*g_pgsz){ uint32_t pg=(uint32_t)((a-lo)/g_pgsz);
      if(g_brq_on && s->stamp){ uint64_t st=s->stamp; if(q_done()<st){ qs_fault_waits++; q_wait(st); } }
      s->dirty[pg]=1; s->lastf=(int32_t)pg; st_faults++;
      mprotect((void*)(lo+(uintptr_t)pg*g_pgsz),g_pgsz,PROT_READ|PROT_WRITE); return 1; } }
  return 0; }
/* ---- BSI_PASSTIME: per-pass GPU time from the game's own command stream (D3D11 timestamp queries on the immediate
   context), so the 60 % "shading fragments" of the Ultra profile can be split by pass on the Adreno itself.
   A boundary = every OMSetRenderTargets / OMSetRenderTargetsAndUnorderedAccessViews, the first Dispatch after draws, the
   first Draw after dispatches, and Present. At each boundary one TIMESTAMP query is End()ed (DXVK: vkCmdWriteTimestamp at
   the bottom of the pipe, i.e. "everything before this point is finished"), so the segment between two boundaries is the
   GPU time of the pass that was bound in between. Each segment carries a signature of what was bound (render-target formats
   and size, read back with OMGetRenderTargets after the bind; "compute" for dispatch bursts) plus its draw / dispatch /
   clear / copy counts. Frames are read back two Presents later with D3D11_ASYNC_GETDATA_DONOTFLUSH (never a stall; a frame
   whose queries are not ready after four Presents is dropped and counted), converted with the frame's
   TIMESTAMP_DISJOINT frequency, and every BSI_PASSTIME_INTERVAL seconds (default 10) the per-frame averages are printed:
     [passtime] t=.. frames=N gpu/frame=.. ms  dropped=.. overflow=..
     [passtime]  avg_ms   %  segs/f draws/f   signature            (top BSI_PASSTIME_TOP rows, default 24)
     [passtime] sequence of one frame: #k ms draws signature | ...   (the pass order, to map rows to UE3 passes)
   Cost: ~60 End() calls and ~60 GetData() per frame, one OMGetRenderTargets per bind. Nothing is changed in what the game
   draws; the queries live on the game's device and are released with it. BSI_PASSTIME=1 on, unset/0 = nothing happens
   (not even a CreateQuery). Works with the synchronous bridge (hooks in bridge_pre/bridge_post) and with
   BSI_BRIDGE_THREAD (hooks in the consumer, bridge_q.c q_exec). Log lines: [passtime] ... */
#define PT_NF 4            /* frames in flight (ring) */
#define PT_MAXQ 192        /* boundaries per frame */
#define PT_NSIG 128        /* distinct signatures kept per interval */
#define PT_CACHE 64        /* (bound views) -> signature cache */
typedef struct { void *ts[PT_MAXQ]; void *dj; int n; int used; uint64_t frame_no, present_no;
  unsigned sig[PT_MAXQ]; uint16_t draws[PT_MAXQ], disp[PT_MAXQ], ops[PT_MAXQ]; char txt[PT_MAXQ][40]; } PTFrame;
typedef struct { char text[48]; unsigned hash; double ms; unsigned segs, draws, disp, ops; } PTSig;
static int pt_mode=-1, pt_top=24; static double pt_interval=10.0; static pthread_mutex_t pt_mu=PTHREAD_MUTEX_INITIALIZER;
static void *pt_ctx, *pt_dev; static PTFrame pt_fr[PT_NF]; static int pt_cur=-1; static uint64_t pt_frames;
static int pt_in_compute; static unsigned pt_cursig; static char pt_cursig_text[48];
static struct { void *v[9]; unsigned hash; char text[48]; } pt_cache[PT_CACHE];
static PTSig pt_sigs[PT_NSIG]; static int pt_nsigs; static double pt_t0, pt_gpu_ms; static unsigned pt_frames_done, pt_dropped, pt_overflow, pt_notready;
static char pt_seq[1200]; static int pt_seq_frame_done;
static int pt_init(void){
  int m=__atomic_load_n(&pt_mode,__ATOMIC_ACQUIRE); if(m>=0) return m;
  const char *e=getenv("BSI_PASSTIME"); m=(e&&atoi(e)>0)?1:0;
  if(m){ e=getenv("BSI_PASSTIME_TOP"); if(e&&atoi(e)>0) pt_top=atoi(e); e=getenv("BSI_PASSTIME_INTERVAL"); if(e&&atof(e)>0) pt_interval=atof(e); }
  __atomic_store_n(&pt_mode,m,__ATOMIC_RELEASE); return m; }
static double pt_now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }
static uint32_t pt_release(void *o){ return o?((uint32_t(*)(void*))((*(void***)o)[2]))(o):0; }
static const char *pt_fmt(uint32_t f){
  switch(f){ case 2: return "RGBA32F"; case 10: return "RGBA16F"; case 11: return "RGBA16"; case 16: return "RG32F"; case 24: return "RGB10A2"; case 26: return "R11G11B10F";
    case 27: return "RGBA8t"; case 28: return "RGBA8"; case 29: return "RGBA8s"; case 34: return "RG16F"; case 39: return "R32t"; case 40: return "D32F"; case 41: return "R32F";
    case 44: return "R24G8t"; case 45: return "D24S8"; case 53: return "R16t"; case 54: return "R16F"; case 55: return "D16"; case 56: return "R16"; case 61: return "R8"; case 87: return "BGRA8"; case 91: return "BGRA8s"; default: return 0; } }
static void pt_fmtname(uint32_t f, char *o, size_t n){ const char *s=pt_fmt(f); if(s) snprintf(o,n,"%s",s); else snprintf(o,n,"f%u",f); }
static void pt_viewinfo(void *view, uint32_t *fmt, uint32_t *w, uint32_t *h){   /* RTV / DSV -> view format + texture size */
  *fmt=0; *w=*h=0; if(!view) return;
  uint32_t vd[12]={0}; ((void(*)(void*,uint32_t*))((*(void***)view)[8]))(view,vd); *fmt=vd[0];   /* D3D11_*_VIEW_DESC.Format */
  void *res=0; ((void(*)(void*,void**))((*(void***)view)[7]))(view,&res); if(!res) return;
  uint32_t dim=0; ((void(*)(void*,uint32_t*))((*(void***)res)[7]))(res,&dim);
  if(dim==3){ uint32_t d[11]={0}; ((void(*)(void*,uint32_t*))((*(void***)res)[10]))(res,d); *w=d[0]; *h=d[1]; if(!*fmt) *fmt=d[4]; }
  pt_release(res); }
static unsigned pt_hash(const char *s){ unsigned h=2166136261u; for(;*s;s++){ h^=(uint8_t)*s; h*=16777619u; } return h?h:1; }
/* signature of the currently bound targets: "fmt[xN]+..+Dfmt WxH" (views are grouped by equal format) */
static void pt_signature(void *ctx){
  void *rtv[8]={0}; void *dsv=0;
  ((void(*)(void*,uint32_t,void**,void**))((*(void***)ctx)[89]))(ctx,8,rtv,&dsv);
  int k; for(k=0;k<PT_CACHE;k++){ if(!pt_cache[k].hash) break; int same=1; for(int j=0;j<8;j++) if(pt_cache[k].v[j]!=rtv[j]){ same=0; break; } if(same && pt_cache[k].v[8]==dsv) break; }
  if(k<PT_CACHE && pt_cache[k].hash){ pt_cursig=pt_cache[k].hash; snprintf(pt_cursig_text,sizeof pt_cursig_text,"%s",pt_cache[k].text); }
  else {
    char t[48]=""; size_t p=0; uint32_t W=0,H=0, lastf=~0u; int run=0, nr=0;
    for(int j=0;j<8 && p<sizeof t-12;j++){ if(!rtv[j]) continue; uint32_t f,w,h; pt_viewinfo(rtv[j],&f,&w,&h); if(!W){ W=w; H=h; } nr++;
      if(f==lastf){ run++; continue; }
      if(run>1) p+=snprintf(t+p,sizeof t-p,"x%d",run);
      char fn[16]; pt_fmtname(f,fn,sizeof fn); p+=snprintf(t+p,sizeof t-p,"%s%s",p?"+":"",fn); lastf=f; run=1; }
    if(run>1 && p<sizeof t-4) p+=snprintf(t+p,sizeof t-p,"x%d",run);
    if(dsv && p<sizeof t-12){ uint32_t f,w,h; pt_viewinfo(dsv,&f,&w,&h); if(!W){ W=w; H=h; } char fn[16]; pt_fmtname(f,fn,sizeof fn); p+=snprintf(t+p,sizeof t-p,"%s%s",p?"+":"",fn); }
    if(!nr && !dsv) p+=snprintf(t+p,sizeof t-p,"(no targets)");
    if(W && p<sizeof t-10) snprintf(t+p,sizeof t-p," %ux%u",W,H);
    static unsigned next; k=(next++)%PT_CACHE; for(int j=0;j<8;j++) pt_cache[k].v[j]=rtv[j]; pt_cache[k].v[8]=dsv; pt_cache[k].hash=pt_hash(t); snprintf(pt_cache[k].text,sizeof pt_cache[k].text,"%s",t);
    pt_cursig=pt_cache[k].hash; snprintf(pt_cursig_text,sizeof pt_cursig_text,"%s",t); }
  for(int j=0;j<8;j++) pt_release(rtv[j]); pt_release(dsv); }
static void *pt_create_query(void *dev, uint32_t type){ uint32_t desc[2]={type,0}; void *q=0;
  int32_t hr=((int32_t(*)(void*,const uint32_t*,void**))((*(void***)dev)[24]))(dev,desc,&q); return hr>=0?q:0; }
static void pt_reset_ctx(void *ctx){   /* new immediate context (the game creates several devices at start): drop everything */
  for(int f=0;f<PT_NF;f++){ for(int i=0;i<pt_fr[f].n;i++) pt_release(pt_fr[f].ts[i]); pt_release(pt_fr[f].dj); memset(&pt_fr[f],0,sizeof pt_fr[f]); }
  if(pt_dev) pt_release(pt_dev); pt_dev=0; pt_ctx=ctx; pt_cur=-1; pt_in_compute=0; pt_cursig=0; pt_cursig_text[0]=0; memset(pt_cache,0,sizeof pt_cache);
  if(ctx){ ((void(*)(void*,void**))((*(void***)ctx)[3]))(ctx,&pt_dev); } }
static PTFrame *pt_frame(void){   /* the frame being recorded; opened lazily at the first boundary after a Present */
  if(pt_cur<0){ pt_cur=(int)(pt_frames%PT_NF); PTFrame *F=&pt_fr[pt_cur];
    if(F->used){ pt_dropped++; F->used=0; }   /* its queries were never read back: give up on that frame */
    F->n=0; F->frame_no=pt_frames; if(!F->dj) F->dj=pt_create_query(pt_dev,3);   /* D3D11_QUERY_TIMESTAMP_DISJOINT */
    if(F->dj) ((void(*)(void*,void*))((*(void***)pt_ctx)[27]))(pt_ctx,F->dj);   /* Begin */
    pt_seq[0]=0; }
  return &pt_fr[pt_cur]; }
static void pt_stamp(unsigned sig, const char *text){   /* close the running segment (timestamp) and open one with `sig` */
  PTFrame *F=pt_frame();
  if(F->n>=PT_MAXQ){ pt_overflow++; return; }   /* more boundaries than queries: the rest of the frame merges into the last segment */
  if(!F->ts[F->n]) F->ts[F->n]=pt_create_query(pt_dev,2);   /* D3D11_QUERY_TIMESTAMP */
  if(!F->ts[F->n]) return;
  ((void(*)(void*,void*))((*(void***)pt_ctx)[28]))(pt_ctx,F->ts[F->n]);   /* End: "all work before this point" */
  F->sig[F->n]=sig; F->draws[F->n]=F->disp[F->n]=F->ops[F->n]=0; snprintf(F->txt[F->n],sizeof F->txt[F->n],"%s",text?text:(F->n?F->txt[F->n-1]:"")); F->n++;
  if(text && F->n<=40){ size_t l=strlen(pt_seq); if(l<sizeof pt_seq-80) snprintf(pt_seq+l,sizeof pt_seq-l,"%s#%d %s",l?" | ":"",F->n-1,text); } }
static void pt_count(int what){ if(pt_cur<0) return; PTFrame *F=&pt_fr[pt_cur]; int i=F->n-1; if(i<0) return;
  if(what==0){ if(F->draws[i]<65535) F->draws[i]++; } else if(what==1){ if(F->disp[i]<65535) F->disp[i]++; } else if(F->ops[i]<65535) F->ops[i]++; }
static PTSig *pt_sig_row(unsigned h, const char *text){
  for(int i=0;i<pt_nsigs;i++) if(pt_sigs[i].hash==h) return &pt_sigs[i];
  if(pt_nsigs>=PT_NSIG) return &pt_sigs[PT_NSIG-1];
  PTSig *s=&pt_sigs[pt_nsigs++]; memset(s,0,sizeof *s); s->hash=h; snprintf(s->text,sizeof s->text,"%s",text); return s; }
static int pt_cmp_ms(const void *a, const void *b){ double x=((const PTSig*)a)->ms, y=((const PTSig*)b)->ms; return x<y?1:x>y?-1:0; }
/* read back one frame: all its timestamps must be ready (GetData S_OK) -- returns 0 = not yet, 1 = consumed */
static int pt_readback(PTFrame *F){
  if(!F->used) return 1;
  uint64_t dj[2]={0,0};
  int32_t hr=((int32_t(*)(void*,void*,void*,uint32_t,uint32_t))((*(void***)pt_ctx)[29]))(pt_ctx,F->dj,dj,16,1);   /* DONOTFLUSH */
  if(hr!=0){ if(hr<0){ F->used=0; pt_dropped++; return 1; } return 0; }   /* S_FALSE: not ready; error: give the frame up */
  uint64_t t[PT_MAXQ]; for(int i=0;i<F->n;i++){ hr=((int32_t(*)(void*,void*,void*,uint32_t,uint32_t))((*(void***)pt_ctx)[29]))(pt_ctx,F->ts[i],&t[i],8,1); if(hr!=0){ if(hr<0){ F->used=0; pt_dropped++; return 1; } return 0; } }
  F->used=0;
  if(dj[1]&1 || !dj[0] || F->n<2) return 1;   /* disjoint (clock changed) or empty: skip */
  double per_tick_ms=1000.0/(double)dj[0];
  for(int i=1;i<F->n;i++){ double ms=(double)(t[i]-t[i-1])*per_tick_ms; if(ms<0||ms>10000) continue;
    PTSig *s=pt_sig_row(F->sig[i-1],F->txt[i-1]);
    s->ms+=ms; s->segs++; s->draws+=F->draws[i-1]; s->disp+=F->disp[i-1]; s->ops+=F->ops[i-1]; }
  pt_gpu_ms+=(double)(t[F->n-1]-t[0])*per_tick_ms; pt_frames_done++;
  return 1; }
static void pt_report(void){
  double now=pt_now(); if(pt_t0==0){ pt_t0=now; return; } if(now-pt_t0<pt_interval) return;
  unsigned n=pt_frames_done?pt_frames_done:1;
  fprintf(stderr,"[passtime] t=%.0f frames=%u gpu/frame=%.2f ms (first to last boundary)  dropped=%u notready=%u overflow=%u\n",now,pt_frames_done,pt_gpu_ms/n,pt_dropped,pt_notready,pt_overflow);
  qsort(pt_sigs,pt_nsigs,sizeof pt_sigs[0],pt_cmp_ms);
  double tot=pt_gpu_ms>0?pt_gpu_ms:1;
  fprintf(stderr,"[passtime]   avg_ms    %%  segs/f  draws/f  disp/f  ops/f  signature\n");
  for(int i=0;i<pt_nsigs && i<pt_top;i++){ PTSig *s=&pt_sigs[i];
    fprintf(stderr,"[passtime]  %7.3f %5.1f  %6.2f  %7.1f  %6.2f  %5.1f  %s\n",s->ms/n,100.0*s->ms/tot,(double)s->segs/n,(double)s->draws/n,(double)s->disp/n,(double)s->ops/n,s->text); }
  if(pt_seq_frame_done && pt_seq[0]) fprintf(stderr,"[passtime] sequence of one frame: %s\n",pt_seq);
  pt_nsigs=0; pt_gpu_ms=0; pt_frames_done=0; pt_dropped=0; pt_notready=0; pt_overflow=0; pt_t0=now; pt_seq_frame_done=0; }
/* ---- hooks. slot = ID3D11DeviceContext vtable index of the call about to run (pre) / that just ran (post). */
static void pt_slot_pre(void *ctx, int slot){
  if(pt_init()<=0 || !ctx) return;
  pthread_mutex_lock(&pt_mu);
  if(ctx!=pt_ctx) pt_reset_ctx(ctx);
  if(pt_dev){
    switch(slot){
    case 33: case 34: break;   /* bind: the boundary is stamped in post, when the new targets are known */
    case 12: case 13: case 20: case 21: case 38: case 39: case 40:   /* draws */
      if(pt_in_compute || pt_cur<0 || pt_fr[pt_cur].n==0){ pt_in_compute=0; pt_stamp(pt_cursig,pt_cursig_text[0]?pt_cursig_text:"(unbound)"); } pt_count(0); break;
    case 41: case 42:   /* dispatches */
      if(!pt_in_compute || pt_cur<0 || pt_fr[pt_cur].n==0){ pt_in_compute=1; pt_stamp(pt_hash("compute"),"compute"); } pt_count(1); break;
    case 46: case 47: case 50: case 51: case 52: case 53: case 54: case 57: case 58:   /* copies, clears, resolves, mips, command lists */
      if(pt_cur<0 || pt_fr[pt_cur].n==0) pt_stamp(pt_cursig,pt_cursig_text[0]?pt_cursig_text:"(unbound)"); pt_count(2); break;
    default: break; } }
  pthread_mutex_unlock(&pt_mu); }
static void pt_slot_post(void *ctx, int slot){
  if(pt_init()<=0 || !ctx) return;
  if(slot!=33 && slot!=34) return;
  pthread_mutex_lock(&pt_mu);
  if(ctx==pt_ctx && pt_dev){ pt_signature(ctx); pt_in_compute=0; pt_stamp(pt_cursig,pt_cursig_text); }
  pthread_mutex_unlock(&pt_mu); }
static uint64_t pt_presents;
static void pt_present(void *ctx){   /* end of frame: close the last segment, End the disjoint query, read back older frames, report */
  if(pt_init()<=0) return;
  pthread_mutex_lock(&pt_mu);
  if(ctx && ctx!=pt_ctx) pt_reset_ctx(ctx);
  if(pt_dev){
    pt_presents++;
    if(pt_cur>=0){ PTFrame *F=&pt_fr[pt_cur];
      if(F->n>0 && F->n<PT_MAXQ) pt_stamp(F->sig[F->n-1],0);   /* final boundary (same signature: a zero-draw tail segment) */
      if(F->dj) ((void(*)(void*,void*))((*(void***)pt_ctx)[28]))(pt_ctx,F->dj);
      F->used=F->n>=2; F->present_no=pt_presents; pt_cur=-1; pt_frames++; if(!pt_seq_frame_done && pt_seq[0]) pt_seq_frame_done=1; }
    /* read back every frame that is at least two Presents old (never a stall: DONOTFLUSH); a slot still pending when its
       ring position comes around again is dropped in pt_frame() */
    for(int f=0;f<PT_NF;f++){ PTFrame *G=&pt_fr[f]; if(!G->used || G->present_no+2>pt_presents) continue; if(!pt_readback(G)) pt_notready++; }
    pt_report(); }
  pthread_mutex_unlock(&pt_mu); }
/* name-based entry for the synchronous bridge path (bridge_pre / bridge_post get the method name, not the slot) */
static int pt_slot_of(const char *name){
  if(strncmp(name,"ID3D11DeviceContext::",21)) return -1; const char *m=name+21;
  switch(m[0]){
  case 'O': if(!strcmp(m,"OMSetRenderTargets")) return 33; if(!strcmp(m,"OMSetRenderTargetsAndUnorderedAccessViews")) return 34; return -1;
  case 'D': if(!strcmp(m,"DrawIndexed")) return 12; if(!strcmp(m,"Draw")) return 13; if(!strcmp(m,"DrawIndexedInstanced")) return 20; if(!strcmp(m,"DrawInstanced")) return 21;
            if(!strcmp(m,"Dispatch")) return 41; if(!strcmp(m,"DispatchIndirect")) return 42; if(!strcmp(m,"DrawAuto")) return 38; if(!strcmp(m,"DrawIndexedInstancedIndirect")) return 39; if(!strcmp(m,"DrawInstancedIndirect")) return 40; return -1;
  case 'C': if(!strcmp(m,"CopySubresourceRegion")) return 46; if(!strcmp(m,"CopyResource")) return 47; if(!strcmp(m,"ClearRenderTargetView")) return 50; if(!strcmp(m,"ClearUnorderedAccessViewUint")) return 51; if(!strcmp(m,"ClearUnorderedAccessViewFloat")) return 52; if(!strcmp(m,"ClearDepthStencilView")) return 53; return -1;
  case 'G': if(!strcmp(m,"GenerateMips")) return 54; return -1;
  case 'R': if(!strcmp(m,"ResolveSubresource")) return 57; return -1;
  case 'E': if(!strcmp(m,"ExecuteCommandList")) return 58; return -1;
  default: return -1; } }
static void pt_pre(const char *name, void *self){ if(__atomic_load_n(&pt_mode,__ATOMIC_RELAXED)==0) return; int s=pt_slot_of(name); if(s>=0) pt_slot_pre(self,s); }
static void pt_post(const char *name, void *self){ if(__atomic_load_n(&pt_mode,__ATOMIC_RELAXED)==0) return; int s=pt_slot_of(name); if(s==33||s==34) pt_slot_post(self,s); }
/* ---- end BSI_PASSTIME */

void bridge_prefault(uint32_t g, uint32_t n){
  if(!n) return; uintptr_t a=(uintptr_t)MEMP(g), e=a+n;
  for(int i=0;i<NARM;i++){ Shadow *s=armed[i]; if(!s) continue; uintptr_t lo=(uintptr_t)MEMP(s->guest), hi=lo+(uintptr_t)s->npg*g_pgsz;
    if(e<=lo || a>=hi) continue; uintptr_t x=a<lo?lo:a, y=e>hi?hi:e;
    if(g_brq_on && s->stamp) q_wait(s->stamp);
    for(uintptr_t p=(x-lo)/g_pgsz; p<=(y-1-lo)/g_pgsz; p++) if(!s->dirty[p]){ s->dirty[p]=1; mprotect((void*)(lo+p*g_pgsz),g_pgsz,PROT_READ|PROT_WRITE); } } }
/* BSI_DT_KEEP (default on, =0 disables): UE3 fills its two 4 MB dynamic VB/IB rings with ~270 small sequential writes per frame, and 99.7% of
   the Maps continue in the page where the previous one stopped (BSI_DT_STAT). Re-locking that page at every Unmap and
   faulting on it again at the next write costs 2 mprotects + a SIGSEGV per Map (24 us each on the cloud VM, where every
   mprotect is a TLB-shootdown IPI). With KEEP, the most recently faulted page of a tracked shadow (the ring cursor page)
   stays unlocked across Unmaps and is simply copied again at every Unmap (4 KB memcpy); only pages behind the cursor are
   re-locked. Invariant kept: dirty[p]==1 <=> page p is writable, so an open page is always copied. Not used where the
   copy happens later from guest memory (BSI_BRQ_ZC / oversize queued Unmaps): there the page must fault. */
static int dt_keep_on=-1;
static int dt_keep(void){ if(dt_keep_on<0){ const char *e=getenv("BSI_DT_KEEP"); dt_keep_on=e?atoi(e)>0:1; } return dt_keep_on; }   /* default ON since step 2 (validated); BSI_DT_KEEP=0 = old behaviour */
/* at Unmap, before scanning: the last page faulted since the previous Unmap becomes the kept page */
static void dt_pick_keep(Shadow *s, int allow){
  if(!allow || !dt_keep()){ s->keep=-1; s->lastf=-1; return; }
  if(s->lastf>=0 && (uint32_t)s->lastf<s->npg && s->dirty[s->lastf]) s->keep=s->lastf;
  s->lastf=-1; if(s->keep>=0 && !s->dirty[s->keep]) s->keep=-1; }
/* re-lock the dirty run [p,q) except the kept page (which stays dirty and writable) */
static void dt_relock(Shadow *s, uint32_t p, uint32_t q){
  int64_t k=s->keep;
  if(k>=(int64_t)p && k<(int64_t)q){
    if(k>(int64_t)p){ memset(s->dirty+p,0,(uint32_t)k-p); mprotect(MEMP(s->guest+p*(uint32_t)g_pgsz),(size_t)((uint32_t)k-p)*g_pgsz,PROT_READ); st_relocks++; }
    if(k+1<(int64_t)q){ memset(s->dirty+k+1,0,q-(uint32_t)k-1); mprotect(MEMP(s->guest+((uint32_t)k+1)*(uint32_t)g_pgsz),(size_t)(q-(uint32_t)k-1)*g_pgsz,PROT_READ); st_relocks++; }
    return; }
  memset(s->dirty+p,0,q-p); mprotect(MEMP(s->guest+p*(uint32_t)g_pgsz),(size_t)(q-p)*g_pgsz,PROT_READ); st_relocks++; }
static int arm_slot(Shadow *s){ for(int i=0;i<NARM;i++) if(!armed[i]){ armed[i]=s; return 1; } return 0; }
static void unarm(Shadow *s){ for(int i=0;i<NARM;i++) if(armed[i]==s){ armed[i]=0; break; } }
static void sh_alloc(Shadow *s, uint32_t size){
  if(!g_pgsz) g_pgsz=sysconf(_SC_PAGESIZE);
  static int notrack=-1; if(notrack<0) notrack=getenv("BSI_NO_DIRTYTRACK")!=0;
  s->size=size; s->tracked=0; s->dirty=0; s->npg=0; s->keep=s->lastf=-1;
  if(size>=TRACK_MIN && !notrack){ s->guest=gpages(size); s->npg=(size+g_pgsz-1)/g_pgsz; s->dirty=calloc(s->npg,1);
    if(arm_slot(s)){ s->tracked=1; mprotect(MEMP(s->guest),(size_t)s->npg*g_pgsz,PROT_READ); } }
  else s->guest=gmalloc(size); }
static void sh_free(Shadow *s){
  if(g_brq_on && s->stamp) q_wait(s->stamp);   /* a queued Unmap may still copy from this shadow */
  if(s->npg){ if(s->tracked){ unarm(s); mprotect(MEMP(s->guest),(size_t)s->npg*g_pgsz,PROT_READ|PROT_WRITE); } gpages_free(s->guest); free(s->dirty); s->dirty=0; s->npg=0; }
  else gfree(s->guest); s->tracked=0; }
static int16_t sh_hint[8192];
static inline uint32_t sh_h(void *res, uint32_t sub){ uint64_t x=(uint64_t)(uintptr_t)res^((uint64_t)sub<<40); x*=0x9E3779B97F4A7C15ull; return (uint32_t)(x>>51); }
static Shadow *shadow_find(void *res, uint32_t sub){
  uint32_t h=sh_h(res,sub); int k=sh_hint[h]; if(k<nsh_max && sh[k].res==res && sh[k].sub==sub) return &sh[k];
  for(k=0;k<nsh_max;k++) if(sh[k].res==res && sh[k].sub==sub){ sh_hint[h]=(int16_t)k; return &sh[k]; } return 0; }
static Shadow *shadow_for(void *res, uint32_t sub, uint32_t size){
  { Shadow *f=shadow_find(res,sub); if(f){ int k=(int)(f-sh); if(sh[k].size<size){ sh_free(&sh[k]); sh_alloc(&sh[k],size); } return &sh[k]; } }
  if(nsh>=4096) nsh=0;
  Shadow *s=&sh[nsh++]; if(nsh>nsh_max) nsh_max=nsh; if(s->res && s->guest) sh_free(s); s->res=res; s->sub=sub; s->gobj=0; s->rp_known=0; s->deferred=0; s->host=0; s->stamp=0; sh_alloc(s,size); return s; }
/* copy host -> tracked shadow (READ / WRITE maps keep current contents): unlock, copy, re-lock */
static void sh_fill(Shadow *s, const void *src, uint32_t n){
  if(s->tracked){ mprotect(MEMP(s->guest),(size_t)s->npg*g_pgsz,PROT_READ|PROT_WRITE); memcpy(MEMP(s->guest),src,n);
    mprotect(MEMP(s->guest),(size_t)s->npg*g_pgsz,PROT_READ); memset(s->dirty,0,s->npg); s->keep=s->lastf=-1; }
  else memcpy(MEMP(s->guest),src,n); }
typedef struct { void *pData; uint32_t RowPitch, DepthPitch; } HMAP;
void bridge_ID3D11DeviceContext_Map(CPU*c){ SELF
  void *res=br_host(ARGG(1)); uint32_t sub=ARGG(2), type=ARGG(3), flags=ARGG(4), out=ARGG(5); HMAP m={0};
  int32_t hr=((int32_t(*)(void*,void*,uint32_t,uint32_t,uint32_t,HMAP*))VT(14))(self,res,sub,type,flags,&m);
  if(hr<0 || !m.pData){ c->eax=hr; c->esp+=4+20+4; return; }
  if(br_zc() && (uintptr_t)m.pData-(uintptr_t)M < 0x100000000ull){   /* zero-copy: DXVK's memory is guest memory */
    __atomic_add_fetch(&st_zc,1,__ATOMIC_RELAXED);
    W32(out,(uint32_t)((uint8_t*)m.pData-M)); W32(out+4,m.RowPitch); W32(out+8,m.DepthPitch); c->eax=hr; c->esp+=4+20+4; return; }
  uint32_t dim=0; ((void(*)(void*,uint32_t*))((*(void***)res)[7]))(res,&dim);   /* ID3D11Resource::GetType */
  uint32_t size=m.DepthPitch?m.DepthPitch:m.RowPitch;
  if(dim==4){ uint32_t d[9]; ((void(*)(void*,uint32_t*))((*(void***)res)[10]))(res,d); uint32_t mip=sub%(d[3]?d[3]:1), dep=d[2]>>mip; size=m.DepthPitch*(dep?dep:1); }
  if(dim==1){ uint32_t d[6]; ((void(*)(void*,uint32_t*))((*(void***)res)[10]))(res,d); size=d[0]; }
  pthread_mutex_lock(&sh_mu); Shadow *s=shadow_for(res,sub,size); s->host=m.pData; s->type=type; s->cur=size; uint32_t g=s->guest;
  s->gobj=ARGG(1); s->rp=m.RowPitch; s->dp=m.DepthPitch; s->rp_known=1; s->deferred=0; s->mflags=flags;
  if(type==1||type==3||type==2) sh_fill(s,m.pData,size);   /* READ / READ_WRITE / WRITE keep the current contents */
  st_maps++;
  pthread_mutex_unlock(&sh_mu);
  W32(out,g); W32(out+4,m.RowPitch); W32(out+8,m.DepthPitch);
  if(lut_cb && res==lut_cb){ static int nt; if(nt++==3){ g_trap_addr=g; fprintf(stderr,"[trap] LUT cb shadow at guest %08x host %p\n",g,MEMP(g+64)); fflush(stderr); raise(SIGTRAP); } }
  c->eax=hr; c->esp+=4+20+4; }
static void unmap_stat(void){ static time_t last; time_t t=time(0); if(t-last>=10){ last=t; fprintf(stderr,"[bridge] zero-copy maps %llu, shadow maps %llu, uploaded %llu MB, skipped (clean) %llu MB, untracked full copies %llu, write faults %llu, re-lock mprotects %llu\n",
      (unsigned long long)st_zc,(unsigned long long)st_maps,(unsigned long long)(st_bytes>>20),(unsigned long long)(st_saved>>20),(unsigned long long)st_full,(unsigned long long)st_faults,(unsigned long long)st_relocks); } }
void bridge_ID3D11DeviceContext_Unmap(CPU*c){ SELF
  void *res=br_host(ARGG(1)); uint32_t sub=ARGG(2);
  pthread_mutex_lock(&sh_mu);
  { Shadow *s=shadow_find(res,sub); if(s && s->host){
    if(s->tracked){ uint32_t n=0; dt_stat(s); dt_pick_keep(s,1);
      for(uint32_t p=0;p<s->npg;){ if(!s->dirty[p]){ p++; continue; } uint32_t q=p; while(q<s->npg && s->dirty[q]) q++;
        uint32_t off=p*(uint32_t)g_pgsz, end=q*(uint32_t)g_pgsz; if(end>s->cur) end=s->cur;
        if(off<end && s->type!=1){ memcpy((uint8_t*)s->host+off,MEMP(s->guest+off),end-off); n+=end-off; }
        dt_relock(s,p,q); p=q; }
      st_bytes+=n; if(s->type!=1) st_saved+=s->cur-n; }
    else if(s->type!=1){ memcpy(s->host,MEMP(s->guest),s->cur); st_bytes+=s->cur; st_full++; }
    s->host=0; } }
  pthread_mutex_unlock(&sh_mu);
  unmap_stat();
  ((void(*)(void*,void*,uint32_t))VT(15))(self,res,sub); c->esp+=4+8+4; }

/* DXGI */
typedef struct { uint32_t W,H,RN,RD,Fmt,SO,Sc; } HMODE;
typedef struct { HMODE BufferDesc; uint32_t SC,SQ,Usage,Count; void *Wnd; int32_t Windowed; uint32_t Effect, Flags; } HSCD;
static void scd_g2h(uint32_t g, HSCD *h){ memcpy(&h->BufferDesc,MEMP(g),28); h->SC=R32(g+28); h->SQ=R32(g+32); h->Usage=R32(g+36); h->Count=R32(g+40);
  if(!getenv("BSI_WIN_W") && h->BufferDesc.W){ char t[16]; snprintf(t,16,"%u",h->BufferDesc.W); setenv("BSI_WIN_W",t,1); snprintf(t,16,"%u",h->BufferDesc.H); setenv("BSI_WIN_H",t,1); }
  h->Wnd=br_handle(R32(g+44)); h->Windowed=1; h->Effect=R32(g+52); h->Flags=R32(g+56); }
static void scd_h2g(const HSCD *h, uint32_t g){ memcpy(MEMP(g),&h->BufferDesc,28); W32(g+28,h->SC); W32(g+32,h->SQ); W32(g+36,h->Usage); W32(g+40,h->Count);
  W32(g+44,br_handle_to_guest(h->Wnd)); W32(g+48,h->Windowed); W32(g+52,h->Effect); W32(g+56,h->Flags); }
static void create_sc(CPU*c){ SELF
  HSCD d; scd_g2h(ARGG(2),&d); void *sc=0;
  int32_t hr=((int32_t(*)(void*,void*,HSCD*,void**))VT(10))(self,br_host(ARGG(1)),&d,&sc);
  fprintf(stderr,"[bridge] CreateSwapChain %ux%u fmt %u -> %08x\n",d.BufferDesc.W,d.BufferDesc.H,d.BufferDesc.Fmt,hr);
  if(ARGG(3)) W32(ARGG(3),br_wrap(sc,"IDXGISwapChain")); c->eax=hr; c->esp+=4+12+4; }
void bridge_IDXGIFactory_CreateSwapChain(CPU*c){ create_sc(c); }
void bridge_IDXGIFactory1_CreateSwapChain(CPU*c){ create_sc(c); }
void bridge_IDXGIFactory2_CreateSwapChain(CPU*c){ create_sc(c); }
static void sc_getdesc(CPU*c){ SELF HSCD d; memset(&d,0,sizeof d); int32_t hr=((int32_t(*)(void*,HSCD*))VT(12))(self,&d); if(hr>=0) scd_h2g(&d,ARGG(1)); c->eax=hr; c->esp+=4+4+4; }
void bridge_IDXGISwapChain_GetDesc(CPU*c){ sc_getdesc(c); }
void bridge_IDXGISwapChain1_GetDesc(CPU*c){ sc_getdesc(c); }
void bridge_IDXGISwapChain_GetContainingOutput(CPU*c){ SELF void *o=0; int32_t hr=((int32_t(*)(void*,void**))VT(15))(self,&o); if(ARGG(1)) W32(ARGG(1),br_wrap(o,"IDXGIOutput")); c->eax=hr; c->esp+=4+4+4; }
typedef struct { wchar_t Desc[128]; uint32_t Ven,Dev,Sub,Rev; size_t VM,SM,Shared; uint32_t LuidLo; int32_t LuidHi; uint32_t Flags; } HAD;
static void ad_getdesc(CPU*c, int slot, int one){ SELF HAD d; memset(&d,0,sizeof d); int32_t hr=((int32_t(*)(void*,HAD*))VT(slot))(self,&d);
  uint32_t g=ARGG(1); memset(MEMP(g),0,one?296:292); for(int k=0;k<127&&d.Desc[k];k++) W16(g+2*k,(uint16_t)d.Desc[k]);
  W32(g+256,d.Ven); W32(g+260,d.Dev); W32(g+264,d.Sub); W32(g+268,d.Rev); W32(g+272,(uint32_t)(d.VM>0x7fffffff?0x7fffffff:d.VM)); W32(g+276,(uint32_t)d.SM); W32(g+280,(uint32_t)(d.Shared>0x7fffffff?0x7fffffff:d.Shared));
  W32(g+284,d.LuidLo); W32(g+288,d.LuidHi); if(one) W32(g+292,d.Flags); c->eax=hr; c->esp+=4+4+4; }
void bridge_IDXGIAdapter_GetDesc(CPU*c){ ad_getdesc(c,8,0); }
void bridge_IDXGIAdapter1_GetDesc(CPU*c){ ad_getdesc(c,8,0); }
void bridge_IDXGIAdapter1_GetDesc1(CPU*c){ ad_getdesc(c,10,1); }
void bridge_IDXGIAdapter2_GetDesc(CPU*c){ ad_getdesc(c,8,0); }
void bridge_IDXGIAdapter2_GetDesc1(CPU*c){ ad_getdesc(c,10,1); }
typedef struct { wchar_t Name[32]; int32_t L,T,R,B; int32_t Attached; uint32_t Rot; void *Mon; } HOD;
static void out_getdesc(CPU*c){ SELF HOD d; memset(&d,0,sizeof d); int32_t hr=((int32_t(*)(void*,HOD*))VT(7))(self,&d);
  uint32_t g=ARGG(1); memset(MEMP(g),0,92); for(int k=0;k<31&&d.Name[k];k++) W16(g+2*k,(uint16_t)d.Name[k]);
  W32(g+64,d.L); W32(g+68,d.T); W32(g+72,d.R); W32(g+76,d.B); W32(g+80,d.Attached); W32(g+84,d.Rot); W32(g+88,0x10001); c->eax=hr; c->esp+=4+4+4; }
void bridge_IDXGIOutput_GetDesc(CPU*c){ out_getdesc(c); }
void bridge_IDXGIOutput1_GetDesc(CPU*c){ out_getdesc(c); }
void bridge_IDXGIFactory_MakeWindowAssociation(CPU*c){ c->eax=0; c->esp+=4+8+4; }
void bridge_IDXGIFactory1_MakeWindowAssociation(CPU*c){ c->eax=0; c->esp+=4+8+4; }
void bridge_IDXGIFactory_GetWindowAssociation(CPU*c){ if(ARGG(1)) W32(ARGG(1),0); c->eax=0; c->esp+=4+4+4; }

/* ---- render-target dumps for debugging: BSI_DUMPFRAME=N writes the bound RTV after every draw of frame N
   (and the back buffer before its Present) as PPM files into BSI_DUMPDIR (default /tmp/bsidump) */
static int dump_frame=-2, frame_no, dump_seq;
int br_will_dump(void){ return (dump_frame>=0 && frame_no==dump_frame) ? frame_no : -1; }   /* rhi_b1_com.c: dump the shadow frame too */
static const char *dump_dir;
static float h2f(uint16_t h){ uint32_t s=(h>>15)&1, e=(h>>10)&31, m=h&1023; uint32_t f;
  if(!e){ if(!m) f=s<<31; else { e=127-15+1; while(!(m&1024)){ m<<=1; e--; } m&=1023; f=(s<<31)|(e<<23)|(m<<13); } }
  else if(e==31) f=(s<<31)|0x7f800000|(m<<13); else f=(s<<31)|((e+112)<<23)|(m<<13); float r; memcpy(&r,&f,4); return r; }
static uint8_t tob(float v){ if(!(v>0)) return 0; v=v/(1+v)*1.0f; v=powf(v,1/2.2f); return v>=1?255:(uint8_t)(v*255); }
static int g_last_black;
static void dump_tex(void *ctx, void *tex, const char *tag){
  uint32_t d[11]; ((void(*)(void*,uint32_t*))((*(void***)tex)[10]))(tex,d);   /* D3D11_TEXTURE2D_DESC */
  uint32_t W=d[0],H=d[1],fmt=d[4],ms=d[5];
  if(ms>1){ fprintf(stderr,"[dump] %s: MSAA x%u fmt %u skipped\n",tag,ms,fmt); return; }
  void *dev=0; ((void(*)(void*,void**))((*(void***)ctx)[3]))(ctx,&dev);
  uint32_t sd[11]={W,H,1,1,fmt,1,0,3,0,0x20000,0}; void *st=0;
  int32_t hr=((int32_t(*)(void*,uint32_t*,void*,void**))((*(void***)dev)[5]))(dev,sd,0,&st);
  ((uint32_t(*)(void*))((*(void***)dev)[2]))(dev);
  if(hr<0||!st){ fprintf(stderr,"[dump] %s: staging fmt %u failed %08x\n",tag,fmt,hr); return; }
  ((void(*)(void*,void*,uint32_t,uint32_t,uint32_t,uint32_t,void*,uint32_t,void*))((*(void***)ctx)[46]))(ctx,st,0,0,0,0,tex,0,0);
  HMAP m={0}; hr=((int32_t(*)(void*,void*,uint32_t,uint32_t,uint32_t,HMAP*))((*(void***)ctx)[14]))(ctx,st,0,1,0,&m);
  if(hr>=0 && m.pData){
    char fn[512]; snprintf(fn,sizeof fn,"%s/f%d_%04d_%s_fmt%u.ppm",dump_dir,frame_no,dump_seq++,tag,fmt);
    FILE *f=fopen(fn,"wb"); if(f){ fprintf(f,"P6\n%u %u\n255\n",W,H); uint8_t *row=malloc(W*3);
      for(uint32_t y=0;y<H;y++){ uint8_t *p=(uint8_t*)m.pData+(size_t)y*m.RowPitch;
        for(uint32_t x=0;x<W;x++){ uint8_t *o=row+3*x;
          switch(fmt){
            case 27: case 28: case 29: case 30: o[0]=p[4*x]; o[1]=p[4*x+1]; o[2]=p[4*x+2]; break;
            case 87: case 88: case 90: case 91: o[0]=p[4*x+2]; o[1]=p[4*x+1]; o[2]=p[4*x]; break;
            case 9: case 10: { uint16_t *q=(uint16_t*)p+4*x; o[0]=tob(h2f(q[0])); o[1]=tob(h2f(q[1])); o[2]=tob(h2f(q[2])); break; }
            case 2: { float *q=(float*)p+4*x; o[0]=tob(q[0]); o[1]=tob(q[1]); o[2]=tob(q[2]); break; }
            case 24: { uint32_t v=((uint32_t*)p)[x]; o[0]=(v&1023)>>2; o[1]=((v>>10)&1023)>>2; o[2]=((v>>20)&1023)>>2; break; }
            case 26: { uint32_t v=((uint32_t*)p)[x]; uint32_t r=v&2047,g=(v>>11)&2047,b=v>>22;
              float fr=h2f((uint16_t)(r<<4)), fg=h2f((uint16_t)(g<<4)), fb=h2f((uint16_t)(b<<5)); o[0]=tob(fr); o[1]=tob(fg); o[2]=tob(fb); break; }
            case 41: case 43: case 34: case 35: { float v=((float*)p)[x*(fmt==41?1:(fmt==34?2:1))]; if(fmt==35||fmt==34) v=h2f(((uint16_t*)p)[x*(fmt==34?2:1)]); o[0]=o[1]=o[2]=tob(v); break; }
            case 61: case 65: o[0]=o[1]=o[2]=p[x]; break;
            default: o[0]=o[1]=o[2]=p[(size_t)x*4%m.RowPitch]; }
        }
        fwrite(row,1,W*3,f); }
      { int z=1; for(uint32_t y=0;y<H&&z;y+=7){ uint8_t *p=(uint8_t*)m.pData+(size_t)y*m.RowPitch; for(uint32_t x=0;x<W*4;x++) if(p[x]&&(x&3)!=3){ z=0; break; } } g_last_black=z; }
      free(row); fclose(f); fprintf(stderr,"[dump] %s\n",fn); }
    ((void(*)(void*,void*,uint32_t))((*(void***)ctx)[15]))(ctx,st,0);
  }
  ((uint32_t(*)(void*))((*(void***)st)[2]))(st);
}
static void *g_lastctx;
static struct { void *h; uint8_t *code; uint32_t len; } shd[8192]; static int nshd; static uint32_t pend_code, pend_len, pend_out;
static void save_shader(void *h, const char *tag){ for(int k=0;k<nshd;k++) if(shd[k].h==h){ char fn[512]; snprintf(fn,sizeof fn,"%s/f%d_%04d_%s.dxbc",dump_dir,frame_no,dump_seq++,tag);
    FILE *f=fopen(fn,"wb"); if(f){ fwrite(shd[k].code,1,shd[k].len,f); fclose(f); fprintf(stderr,"[dump]   shader %s\n",fn); } return; } }

/* BSI_FORCE_FADE=1: debug override of GammaColorScale (PS cb0 floats 16..18) in the color-grading LUT pass
   (render target 256x16) so a faded-out frame becomes visible */
static void *force_cb, *force_saved;
static void force_fade_pre(void *ctx){
  void *rtv[1]={0}; void *dsv=0; ((void(*)(void*,uint32_t,void**,void**))((*(void***)ctx)[89]))(ctx,1,rtv,&dsv);
  if(dsv) ((uint32_t(*)(void*))((*(void***)dsv)[2]))(dsv); if(!rtv[0]) return;
  void *res=0; ((void(*)(void*,void**))((*(void***)rtv[0])[7]))(rtv[0],&res); ((uint32_t(*)(void*))((*(void***)rtv[0])[2]))(rtv[0]); if(!res) return;
  uint32_t dim=0, d[11]={0}; ((void(*)(void*,uint32_t*))((*(void***)res)[7]))(res,&dim); if(dim==3) ((void(*)(void*,uint32_t*))((*(void***)res)[10]))(res,d);
  ((uint32_t(*)(void*))((*(void***)res)[2]))(res); if(dim!=3||d[0]!=256||d[1]!=16) return;
  void *cb=0; ((void(*)(void*,uint32_t,uint32_t,void**))((*(void***)ctx)[77]))(ctx,0,1,&cb); if(!cb) return;
  lut_cb=cb; { const char *te=getenv("BSI_TRAPCB"); static int nd; if(te && *te=='2' && nd++==2) raise(SIGTRAP); }
  { static int tr=-1; if(tr<0) tr=getenv("BSI_TRAPCB")!=0; if(tr){ ((uint32_t(*)(void*))((*(void***)cb)[2]))(cb); return; } }
  uint32_t bd[6]; ((void(*)(void*,uint32_t*))((*(void***)cb)[10]))(cb,bd);
  void *dev=0; ((void(*)(void*,void**))((*(void***)ctx)[3]))(ctx,&dev);
  uint32_t sd[6]={bd[0],3,0,0x20000,0,0}; void *st=0; ((int32_t(*)(void*,uint32_t*,void*,void**))((*(void***)dev)[3]))(dev,sd,0,&st);
  if(st){ ((void(*)(void*,void*,void*))((*(void***)ctx)[47]))(ctx,st,cb); HMAP m={0};
    if(((int32_t(*)(void*,void*,uint32_t,uint32_t,uint32_t,HMAP*))((*(void***)ctx)[14]))(ctx,st,0,1,0,&m)>=0 && m.pData){
      float *f=malloc(bd[0]); memcpy(f,m.pData,bd[0]); ((void(*)(void*,void*,uint32_t))((*(void***)ctx)[15]))(ctx,st,0);
      { static int ll=-1; if(ll<0) ll=getenv("BSI_LUTLOG")!=0;
        if(ll){ if(frame_no==dump_frame){ fprintf(stderr,"[lut] f%d cb(%u):",frame_no,bd[0]); for(uint32_t k=0;k<bd[0]/4&&k<40;k++) fprintf(stderr," %g",f[k]); fprintf(stderr,"\n"); }
          free(f); ((uint32_t(*)(void*))((*(void***)st)[2]))(st); ((uint32_t(*)(void*))((*(void***)dev)[2]))(dev); ((uint32_t(*)(void*))((*(void***)cb)[2]))(cb); return; } }
      if(f[16]==0&&f[17]==0&&f[18]==0){ f[16]=f[17]=f[18]=1; }
      uint32_t nd[6]={bd[0],0,4,0,0,0}; struct { const void *p; uint32_t a,b; } init={f,0,0};
      if(!force_cb || 1){ if(force_cb) ((uint32_t(*)(void*))((*(void***)force_cb)[2]))(force_cb); force_cb=0;
        ((int32_t(*)(void*,uint32_t*,void*,void**))((*(void***)dev)[3]))(dev,nd,&init,&force_cb); }
      free(f);
      if(force_cb){ force_saved=cb; ((void(*)(void*,uint32_t,uint32_t,void**))((*(void***)ctx)[16]))(ctx,0,1,&force_cb); cb=0; } }
    ((uint32_t(*)(void*))((*(void***)st)[2]))(st); }
  ((uint32_t(*)(void*))((*(void***)dev)[2]))(dev);
  if(cb) ((uint32_t(*)(void*))((*(void***)cb)[2]))(cb);
}
static void force_fade_post(void *ctx){ if(!force_saved) return; ((void(*)(void*,uint32_t,uint32_t,void**))((*(void***)ctx)[16]))(ctx,0,1,&force_saved);
  ((uint32_t(*)(void*))((*(void***)force_saved)[2]))(force_saved); force_saved=0; }
static void bench_frame(void);
static void tl_pre(const char *name);
void hl_pre(const char *name); void hl_post(const char *name);   /* linux/hitchlog.c (BSI_HITCHLOG, off by default) */
volatile long g_br_presents;   /* swapchain Present count (BSI_BRSTAT=2 per-frame stats) */
/* BSI_BENCH_FT=<file>: per-frame log for dip analysis (regress/analyze.py). Producer side, one line per Present:
   "t_s frame_ms other_wait_ms present_wait_ms drains relwaits rt_cpu_ms rt_rqwait_ms gt_cpu_ms gt_rqwait_ms" (the waits are BSI_BRIDGE_THREAD stalls of the
   RenderingThread inside that frame: present_wait = one-frame bound, other = drains / last-ref releases / ring full).
   With the bridge thread on, the consumer writes "<file>.c": "t_s frame_ms busy_ms" per executed Present. */
static uint64_t g_ftw_tot_ns, g_ftw_present_ns, g_ftw_drains, g_ftw_relwait;
/* run time / run-queue wait (ns) of a thread from /proc schedstat; *fd caches the open file (tid 0 = calling thread) */
static int ft_sched(int *fd, int *fdtid, int tid, uint64_t *run, uint64_t *wq){
  int me=tid?tid:(int)syscall(SYS_gettid);
  if(*fd<0 || *fdtid!=me){ if(*fd>=0) close(*fd); char p[64]; snprintf(p,sizeof p,"/proc/self/task/%d/schedstat",me); *fd=open(p,O_RDONLY|O_CLOEXEC); *fdtid=me; if(*fd<0) return 0; }
  char b[96]; ssize_t n=pread(*fd,b,sizeof b-1,0); if(n<=0) return 0; b[n]=0;
  unsigned long long r=0,w=0; if(sscanf(b,"%llu %llu",&r,&w)!=2) return 0; *run=r; *wq=w; return 1; }   /* swapchain Present count (BSI_BRSTAT=2 per-frame stats) */
extern int g_brq_on;   /* bridge_q.c (included below) */
void bridge_pre(CPU *c, const char *name, void *self){
  { extern void rhi_hook_bridge_call(void) __attribute__((weak)); if(rhi_hook_bridge_call) rhi_hook_bridge_call(); }   /* BSI_RHI=b1: early install (rhi_hook.c) */
  { extern void rhi_b1_capture(const char*,CPU*) __attribute__((weak)); if(rhi_b1_capture) rhi_b1_capture(name,c); }   /* BSI_RHI=b1: shadow resource creation (rhi_b1.c) */
  hl_pre(name);
  tl_pre(name);
  pt_pre(name,self);   /* BSI_PASSTIME: pass boundaries / draw counts (synchronous bridge path) */
  if(lut_cb && !strcmp(name,"ID3D11DeviceContext::UpdateSubresource") && br_host(R32(c->esp+8))==lut_cb){ static int nt; uint32_t src=R32(c->esp+20);
    float *f=(float*)MEMP(src); if(nt<3) fprintf(stderr,"[trap] LUT cb update src %08x f16..19 %g %g %g %g\n",src,f[16],f[17],f[18],f[19]);
    if(nt++==3 && getenv("BSI_TRAPCB") && *getenv("BSI_TRAPCB")=='1'){ g_trap_addr=src; g_trap_host=(uint64_t)(uintptr_t)MEMP(src+64); fflush(stderr); raise(SIGTRAP); } }
  { static int ff=-1; if(ff<0) ff=getenv("BSI_FORCE_FADE")||getenv("BSI_TRAPCB")||getenv("BSI_LUTLOG"); if(ff && !strncmp(name,"ID3D11DeviceContext::DrawIndexed",32)) force_fade_pre(self); }
  if(dump_frame==-2){ const char *e=getenv("BSI_DUMPFRAME"); dump_frame=e?atoi(e):(getenv("BSI_DUMPAT")?1000000000:-1); dump_dir=getenv("BSI_DUMPDIR")?getenv("BSI_DUMPDIR"):"/tmp/bsidump";
    if(dump_frame>=0){ char cmd[600]; snprintf(cmd,sizeof cmd,"mkdir -p '%s'",dump_dir); if(system(cmd)){} } }
  if(name[0]=='I' && name[1]=='D' && name[2]=='X' && strstr(name,"::Present") && !strstr(name,"LastPresent")){ g_br_presents++; bench_frame(); if(!g_brq_on) pt_present(0); }   /* BSI_PASSTIME: end of frame (queued path: q_exec) */
  if(dump_frame<0) return;
  if(!strcmp(name,"ID3D11Device::CreatePixelShader")||!strcmp(name,"ID3D11Device::CreateVertexShader")){ pend_code=R32(c->esp+8); pend_len=R32(c->esp+12); pend_out=R32(c->esp+20); }
  if(strstr(name,"::Present")){
    { extern volatile int g_dump_req; if(g_dump_req){ g_dump_req=0; dump_frame=frame_no+2; fprintf(stderr,"[dump] requested, frame %d\n",dump_frame); } }
    { static double at=-2, t0; static const char *rest; if(at==-2){ rest=getenv("BSI_DUMPAT"); at=rest?atof(rest):-1; struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); t0=ts.tv_sec+ts.tv_nsec*1e-9; }
      if(at>0){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); if(ts.tv_sec+ts.tv_nsec*1e-9-t0>=at){ dump_frame=frame_no+12; fprintf(stderr,"[dump] arming frame %d\n",dump_frame);
          rest=rest?strchr(rest,','):0; if(rest){ rest++; at=atof(rest); } else at=-1; } } }
    if(frame_no==dump_frame && g_lastctx){ void *bb=0; static const uint8_t iid_t2d[16]={0xf2,0xaa,0x15,0x6f,0x08,0xd2,0x89,0x4e,0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c};
      int32_t hr=((int32_t(*)(void*,uint32_t,const void*,void**))((*(void***)self)[9]))(self,0,iid_t2d,&bb);
      if(hr>=0&&bb){ dump_tex(g_lastctx,bb,"backbuffer"); if(getenv("BSI_MEMDUMP_ON_BLACK")&&g_last_black){ static int nb; extern void memdump_snap(int); memdump_snap(90+nb++); } ((uint32_t(*)(void*))((*(void***)bb)[2]))(bb); } }
    { static double cap=-1, nextt; if(cap<0){ const char*e=getenv("BSI_FPSCAP"); cap=e?atof(e):0; } if(cap>0){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); double t=ts.tv_sec+ts.tv_nsec*1e-9;
        if(nextt>t){ double d=nextt-t; usleep((useconds_t)(d*1e6)); nextt+=1.0/cap; } else nextt=t+1.0/cap; } }
    frame_no++; }
}
/* frame statistics for driver/setting A/B tests: every 10 s log avg fps, 1%-low fps, worst frame, hitches >100 ms;
   also appended to $BSI_BENCH (session summary written at each interval) */
static int cmpd(const void *a, const void *b){ double x=*(const double*)a, y=*(const double*)b; return x<y?-1:x>y; }
static void bench_frame(void){
  { extern void rhi_hook_present(void) __attribute__((weak)); if(rhi_hook_present) rhi_hook_present(); }   /* BSI_RHI=census (linux/rhi_hook.c, off unless set) */
  static double last=-1, t0, ft[4096]; static int n; static long tot_frames, tot_hitch; static double tot_time, worst_all;
  struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); double t=ts.tv_sec+ts.tv_nsec*1e-9;
  if(last<0){ last=t0=t; return; }
  double d=t-last; last=t; if(n<4096) ft[n++]=d; tot_frames++; tot_time+=d; if(d>0.1) tot_hitch++; if(d>worst_all) worst_all=d;
  { static FILE *ff; static int tr; static uint64_t w0,p0,d0,r0; static double lf;
    if(!tr){ tr=1; const char *fp=getenv("BSI_BENCH_FT"); if(fp&&*fp) ff=fopen(fp,"w"); }
    if(ff){ uint64_t w=__atomic_load_n(&g_ftw_tot_ns,__ATOMIC_RELAXED), p=g_ftw_present_ns, dr=g_ftw_drains, rw=g_ftw_relwait;
      /* + RenderingThread (caller) and GameThread (process main thread) CPU run time and run-queue wait in this frame */
      static int rfd=-1, rft, gfd=-1, gft; static uint64_t rr0, rq0, gr0, gq0; uint64_t rr=rr0, rq=rq0, gr=gr0, gq=gq0;
      ft_sched(&rfd,&rft,0,&rr,&rq); ft_sched(&gfd,&gft,(int)getpid(),&gr,&gq);
      struct timespec rt; clock_gettime(CLOCK_REALTIME,&rt);
      fprintf(ff,"%.6f %.3f %.3f %.3f %u %u %.3f %.3f %.3f %.3f\n",rt.tv_sec+rt.tv_nsec*1e-9,d*1000,((w-w0)-(p-p0))/1e6,(p-p0)/1e6,(unsigned)(dr-d0),(unsigned)(rw-r0),
        (rr-rr0)/1e6,(rq-rq0)/1e6,(gr-gr0)/1e6,(gq-gq0)/1e6); w0=w; p0=p; d0=dr; r0=rw; rr0=rr; rq0=rq; gr0=gr; gq0=gq;
      if(t-lf>=2.0){ fflush(ff); lf=t; } } }
  if(t-t0>=10.0 && n>10){
    double s=0, w=0; for(int i=0;i<n;i++){ s+=ft[i]; if(ft[i]>w) w=ft[i]; } int h=0; for(int i=0;i<n;i++) if(ft[i]>0.1) h++;
    qsort(ft,n,sizeof(double),cmpd); double p99=ft[(int)(n*0.99)>=n?n-1:(int)(n*0.99)];
    char line[300]; snprintf(line,sizeof line,"[bench] %.1f fps avg, %.1f fps 1%%-low, worst %.0f ms, hitches>100ms %d | session: %.1f fps avg, %ld hitches, worst %.0f ms\n",
      n/s, 1.0/p99, w*1000, h, tot_frames/tot_time, tot_hitch, worst_all*1000);
    /* v0.8.4e: frame-time histogram in 120 Hz refresh units (8.33 ms) to show quantisation, plus memory pressure */
    int hb[8]={0}; for(int i=0;i<n;i++){ int r=(int)(ft[i]/(1.0/120)+0.5); if(r<1) r=1; hb[r>=8?7:r-1]++; }
    long rss=0, swp=0, avail=0; { char b[256]; FILE *f=fopen("/proc/self/status","r"); if(f){ while(fgets(b,sizeof b,f)){ sscanf(b,"VmRSS: %ld",&rss); sscanf(b,"VmSwap: %ld",&swp); } fclose(f); }
      f=fopen("/proc/meminfo","r"); if(f){ while(fgets(b,sizeof b,f)) sscanf(b,"MemAvailable: %ld",&avail); fclose(f); } }
    char l2[300]; snprintf(l2,sizeof l2,"[hist] refreshes/frame 1:%d 2:%d 3:%d 4:%d 5:%d 6:%d 7:%d 8+:%d | [mem] rss %ld MB swap %ld MB avail %ld MB\n",
      hb[0],hb[1],hb[2],hb[3],hb[4],hb[5],hb[6],hb[7],rss/1024,swp/1024,avail/1024);
    fputs(line,stderr); fputs(l2,stderr);
    static FILE *bf; static int tried; if(!tried){ tried=1; const char *bp=getenv("BSI_BENCH"); if(bp) bf=fopen(bp,"a"); }
    if(bf){ fputs(line,bf); fputs(l2,bf); fflush(bf); }
    n=0; t0=t; }
}
/* optional frame timeline (GitHub PR "frame-timeline logger"): enabled by /sdcard/BSI/timeline.txt. Per game frame: first bridge call
   (render-thread frame start), first draw/dispatch (render submit), Present, plus the XR thread's xrEndFrame (tl_xr_end, xr_glue.c).
   Every 10 s the new frames are appended to /sdcard/BSI/timeline.csv (ms since the first frame). */
#define TL_N 8192
static int tl_on=-1;
static double tl_gs[TL_N], tl_sub[TL_N], tl_pr[TL_N], tl_xr[TL_N]; static volatile long tl_nf, tl_nx;
static double tl_now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }
static int tl_enabled(void){ if(tl_on<0) tl_on=access("/sdcard/BSI/timeline.txt",F_OK)==0; return tl_on; }
void tl_xr_end(void){ if(!tl_enabled()) return; long i=tl_nx; tl_xr[i%TL_N]=tl_now(); __atomic_store_n(&tl_nx,i+1,__ATOMIC_RELEASE); }
static void tl_dump(void){
  static FILE *f; static int tried; static long done; static double t0=-1, last=0;
  long nf=tl_nf, nx=__atomic_load_n(&tl_nx,__ATOMIC_ACQUIRE); double now=tl_now(); if(last==0) last=now; if(now-last<10.0) return; last=now;
  if(!tried){ tried=1; f=fopen("/sdcard/BSI/timeline.csv","a"); if(f) fputs("frame,game_start_ms,render_submit_ms,present_ms,xr_end_ms\n",f); }
  if(!f) return;
  if(t0<0) t0=tl_gs[0];
  if(nf-done>TL_N) done=nf-TL_N; long x=nx>TL_N?nx-TL_N:0;
  for(long i=done;i<nf;i++){ double pr=tl_pr[i%TL_N];
    while(x<nx && tl_xr[x%TL_N]<pr) x++;
    if(nx>0 && x>=nx) break;
    fprintf(f,"%ld,%.3f,%.3f,%.3f,",i,(tl_gs[i%TL_N]-t0)*1e3,(tl_sub[i%TL_N]-t0)*1e3,(pr-t0)*1e3);
    if(x<nx) fprintf(f,"%.3f",(tl_xr[x%TL_N]-t0)*1e3);
    fputc('\n',f); done=i+1; }
  fflush(f);
}
static void tl_pre(const char *name){
  if(!tl_enabled()) return;
  static int armed=1, subbed; long i=tl_nf; int s=i%TL_N;
  if(armed){ armed=0; subbed=0; tl_gs[s]=tl_now(); tl_sub[s]=tl_pr[s]=0; }
  if(!subbed && (!strncmp(name,"ID3D11DeviceContext::Draw",25) || !strncmp(name,"ID3D11DeviceContext::Dispatch",29) || !strcmp(name,"ID3D11DeviceContext::ExecuteCommandList"))){ subbed=1; tl_sub[s]=tl_now(); }
  if(name[0]=='I' && name[1]=='D' && name[2]=='X' && strstr(name,"::Present") && !strstr(name,"LastPresent")){
    tl_pr[s]=tl_now(); if(!subbed) tl_sub[s]=tl_pr[s]; tl_nf=i+1; armed=1; tl_dump(); }
}
void bridge_post(CPU *c, const char *name, void *self){
  { extern void b1com_create_post(const char*,CPU*) __attribute__((weak)); if(b1com_create_post) b1com_create_post(name,c); }   /* BSI_RHI=b1 COM seam (rhi_b1_com.c) */
  hl_post(name);
  pt_post(name,self);   /* BSI_PASSTIME: the new render targets are bound now */
  if(force_saved && !strncmp(name,"ID3D11DeviceContext::DrawIndexed",32)) force_fade_post(self);
  if(dump_frame<0) return;
  if(pend_out && (!strcmp(name,"ID3D11Device::CreatePixelShader")||!strcmp(name,"ID3D11Device::CreateVertexShader"))){
    void *h=br_host(R32(pend_out)); if(h && nshd<8192){ shd[nshd].h=h; shd[nshd].len=pend_len; shd[nshd].code=malloc(pend_len); memcpy(shd[nshd].code,MEMP(pend_code),pend_len); nshd++; } pend_out=0; return; }
  if(!strncmp(name,"ID3D11DeviceContext::",21)) g_lastctx=self;
  if(frame_no!=dump_frame || strncmp(name,"ID3D11DeviceContext::Draw",25)) return;
  { static int bbo=-1; if(bbo<0) bbo=getenv("BSI_BBONLY")!=0; if(bbo) return; }
  void *rtv[2]={0}; void *dsv=0;
  ((void(*)(void*,uint32_t,void**,void**))((*(void***)self)[89]))(self,1,rtv,&dsv);
  if(dsv) ((uint32_t(*)(void*))((*(void***)dsv)[2]))(dsv);
  if(!rtv[0]) return;
  void *res=0; ((void(*)(void*,void**))((*(void***)rtv[0])[7]))(rtv[0],&res);
  uint32_t dim=0; if(res) ((void(*)(void*,uint32_t*))((*(void***)res)[7]))(res,&dim);
  char tag[64]; snprintf(tag,sizeof tag,"%s",name+21);
  if(res && dim==3) dump_tex(self,res,tag);
  static int dst=-1; if(dst<0) dst=getenv("BSI_DUMPSTATE")!=0;
  if(dst){ void *ps=0; uint32_t nci=0; ((void(*)(void*,void**,void**,uint32_t*))((*(void***)self)[74]))(self,&ps,0,&nci); if(ps){ save_shader(ps,"ps"); ((uint32_t(*)(void*))((*(void***)ps)[2]))(ps); }
    float vp[6*4]={0}; uint32_t nvp=4; ((void(*)(void*,uint32_t*,float*))((*(void***)self)[95]))(self,&nvp,vp);
    fprintf(stderr,"[dump]   vp n=%u %.0f,%.0f %.0fx%.0f z%.1f-%.1f\n",nvp,vp[0],vp[1],vp[2],vp[3],vp[4],vp[5]);
    void *bs=0; float bf[4]; uint32_t mask=0; ((void(*)(void*,void**,float*,uint32_t*))((*(void***)self)[91]))(self,&bs,bf,&mask);
    if(bs){ uint32_t bd[2+8*8]; ((void(*)(void*,uint32_t*))((*(void***)bs)[7]))(bs,bd);
      fprintf(stderr,"[dump]   blend a2c=%u indep=%u rt0 en=%u src=%u dst=%u op=%u srcA=%u dstA=%u opA=%u wmask=%x bf=%.2f,%.2f,%.2f,%.2f mask=%x\n",bd[0],bd[1],bd[2],bd[3],bd[4],bd[5],bd[6],bd[7],bd[8],bd[9],bf[0],bf[1],bf[2],bf[3],mask);
      ((uint32_t(*)(void*))((*(void***)bs)[2]))(bs); } else fprintf(stderr,"[dump]   blend default\n");
    void *ds=0; uint32_t sref=0; ((void(*)(void*,void**,uint32_t*))((*(void***)self)[92]))(self,&ds,&sref);
    if(ds){ uint32_t dd[13]; ((void(*)(void*,uint32_t*))((*(void***)ds)[7]))(ds,dd); fprintf(stderr,"[dump]   depth en=%u write=%u func=%u stencil=%u\n",dd[0],dd[1],dd[2],dd[3]); ((uint32_t(*)(void*))((*(void***)ds)[2]))(ds); }
    void *rs=0; ((void(*)(void*,void**))((*(void***)self)[94]))(self,&rs);
    if(rs){ uint32_t rd[10]; ((void(*)(void*,uint32_t*))((*(void***)rs)[7]))(rs,rd); fprintf(stderr,"[dump]   rast fill=%u cull=%u ccw=%u clip=%u scissor=%u\n",rd[0],rd[1],rd[2],rd[6],rd[7]); ((uint32_t(*)(void*))((*(void***)rs)[2]))(rs); }
    void *cb[2]={0}; ((void(*)(void*,uint32_t,uint32_t,void**))((*(void***)self)[77]))(self,0,2,cb);
    for(int k=0;k<2;k++) if(cb[k]){ uint32_t bd[6]; ((void(*)(void*,uint32_t*))((*(void***)cb[k])[10]))(cb[k],bd);
      void *dev=0; ((void(*)(void*,void**))((*(void***)self)[3]))(self,&dev);
      uint32_t sd[6]={bd[0],3,0,0x20000,0,0}; void *st=0; ((int32_t(*)(void*,uint32_t*,void*,void**))((*(void***)dev)[3]))(dev,sd,0,&st);
      ((uint32_t(*)(void*))((*(void***)dev)[2]))(dev);
      if(st){ ((void(*)(void*,void*,void*))((*(void***)self)[47]))(self,st,cb[k]); HMAP m={0};
        if(((int32_t(*)(void*,void*,uint32_t,uint32_t,uint32_t,HMAP*))((*(void***)self)[14]))(self,st,0,1,0,&m)>=0 && m.pData){
          float *f=m.pData; fprintf(stderr,"[dump]   pscb%d size %u:",k,bd[0]); for(uint32_t i=0;i<bd[0]/4 && i<32;i++) fprintf(stderr," %g",f[i]); fprintf(stderr,"\n");
          ((void(*)(void*,void*,uint32_t))((*(void***)self)[15]))(self,st,0); }
        ((uint32_t(*)(void*))((*(void***)st)[2]))(st); }
      ((uint32_t(*)(void*))((*(void***)cb[k])[2]))(cb[k]); }
  }
  static int dsrv=-1; if(dsrv<0) dsrv=getenv("BSI_DUMPSRV")!=0;
  if(dsrv){ void *srv[8]={0}; ((void(*)(void*,uint32_t,uint32_t,void**))((*(void***)self)[73]))(self,0,8,srv);
    for(int k=0;k<8;k++) if(srv[k]){ void *r=0; ((void(*)(void*,void**))((*(void***)srv[k])[7]))(srv[k],&r); uint32_t dm=0;
      if(r){ ((void(*)(void*,uint32_t*))((*(void***)r)[7]))(r,&dm);
        if(dm==3){ uint32_t d[11]; ((void(*)(void*,uint32_t*))((*(void***)r)[10]))(r,d); fprintf(stderr,"[dump]   srv%d tex2d %ux%u mips %u fmt %u\n",k,d[0],d[1],d[2],d[4]);
          char t2[80]; snprintf(t2,sizeof t2,"%s_srv%d",tag,k); if(d[4]!=71&&d[4]!=74&&d[4]!=77&&d[4]!=80&&d[4]!=83&&d[4]!=72&&d[4]!=75&&d[4]!=78) dump_tex(self,r,t2); }
        else fprintf(stderr,"[dump]   srv%d dim %u\n",k,dm);
        ((uint32_t(*)(void*))((*(void***)r)[2]))(r); }
      ((uint32_t(*)(void*))((*(void***)srv[k])[2]))(srv[k]); } }
  if(res) ((uint32_t(*)(void*))((*(void***)res)[2]))(res);
  ((uint32_t(*)(void*))((*(void***)rtv[0])[2]))(rtv[0]);
}
void br_fail(const char *what, int32_t hr){ static int n; if(n++<200) fprintf(stderr,"[bridge] %s failed %08x\n",what,(uint32_t)hr); }

/* BSI_FASTDRAW=1 (cloud testing on the CPU rasterizer): skip draw calls except in frames being dumped, so game
   logic runs at full speed (avoids timing races that only exist at 2-6 fps) */
int bridge_skip(const char *name){
  static int fd=-1; if(fd<0) fd=getenv("BSI_FASTDRAW")!=0; if(!fd) return 0;
  if(strncmp(name,"ID3D11DeviceContext::Draw",25) && strncmp(name,"ID3D11DeviceContext::Dispatch",29)) return 0;
  if(dump_frame>=0 && frame_no>=dump_frame-12 && frame_no<=dump_frame) return 0;   /* render the dumped frame and two before it */
  return 1;
}

/* debug paths that read back from the context (frame dumps near a dump frame, FORCE_FADE / LUT traps, BSI_DUMPFRAME)
   need every earlier call executed: with the bridge thread on, they run synchronously */
static int br_dbg_sync(void){
  static int ff=-1; if(ff<0) ff=getenv("BSI_FORCE_FADE")||getenv("BSI_TRAPCB")||getenv("BSI_LUTLOG")||getenv("BSI_DUMPFRAME")||getenv("BSI_DUMPSTATE")||getenv("BSI_DUMPSRV");
  if(ff) return 1;
  if(dump_frame==-2) return 1;
  if(dump_frame<0) return 0;
  { static int bbo=-1; if(bbo<0) bbo=getenv("BSI_BBONLY")!=0; if(bbo) return 0; }   /* back-buffer-only dumps: the frame runs queued, Present drains (br_dump_drain) */
  return frame_no>=dump_frame-13 && frame_no<=dump_frame;
}
/* the back-buffer dump in bridge_pre (Present of frame dump_frame) reads back through the context: drain the queue first */
static int br_dump_due(void){ return dump_frame>=0 && frame_no==dump_frame; }
#include "bridge_q.c"
#include "bridge_fast.c"
#include "getdata_wait.c"   /* BSI_GETDATA_WAIT: bridge_ID3D11DeviceContext_GetData (block instead of spinning on GPU queries) */

/* HITCHES.md H1: per-frame hitch attribution (BSI_HITCHLOG=<file>; off by default) */
#include "hitchlog.c"   /* copy of regress/hitch/hitchlog.c */

/* b1n (rhi_b1_com.c): the bridge is off, so its Present bookkeeping runs from here: frame stats, and the harness's back-buffer
   dumps (g_dump_req / BSI_DUMPAT / BSI_DUMPFRAME). Returns 1 and the file name when this frame must be dumped. */
int br_native_present(char *path, int n){
  g_br_presents++; bench_frame();
  if(dump_frame==-2){ const char *e=getenv("BSI_DUMPFRAME"); dump_frame=e?atoi(e):(getenv("BSI_DUMPAT")?1000000000:-1); dump_dir=getenv("BSI_DUMPDIR")?getenv("BSI_DUMPDIR"):"/tmp/bsidump";
    if(dump_frame>=0){ char cmd[600]; snprintf(cmd,sizeof cmd,"mkdir -p '%s'",dump_dir); if(system(cmd)){} } }
  if(dump_frame<0) return 0;
  { extern volatile int g_dump_req; if(g_dump_req){ g_dump_req=0; dump_frame=frame_no+2; fprintf(stderr,"[dump] requested, frame %d\n",dump_frame); } }
  { static double at=-2, t0; static const char *rest; if(at==-2){ rest=getenv("BSI_DUMPAT"); at=rest?atof(rest):-1; struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); t0=ts.tv_sec+ts.tv_nsec*1e-9; }
    if(at>0){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); if(ts.tv_sec+ts.tv_nsec*1e-9-t0>=at){ dump_frame=frame_no+12; fprintf(stderr,"[dump] arming frame %d\n",dump_frame);
        rest=rest?strchr(rest,','):0; if(rest){ rest++; at=atof(rest); } else at=-1; } } }
  int r=0; if(frame_no==dump_frame){ snprintf(path,n,"%s/f%d_%04d_backbuffer_fmt28.ppm",dump_dir,frame_no,dump_seq++); r=1; }
  frame_no++; return r; }
