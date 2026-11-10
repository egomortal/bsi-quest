/* Loader for the game's own x86 DLLs (PhysX3, APEX, Bink, nvtt): mapped into guest memory at fixed bases,
   relocated, imports bound to HLE handlers (extended import table), code run by the Unicorn fallback until
   these DLLs are recompiled too. */
#define _GNU_SOURCE
#include "hle.h"
#include <dlfcn.h>
#include <dirent.h>
#include <strings.h>
#include <ctype.h>
#define IMP_BASE 0xE0000000u
typedef void (*hle_fn)(CPU*c);
typedef struct { uint32_t iat; const char *name; hle_fn fn; } Imp;
extern Imp g_imps[]; extern int g_nimps;
Imp g_ximps[4096]; int g_nximps;
void guest_map(uint32_t a, uint32_t n); uint32_t gmalloc(uint32_t n);
void emu_add_region(uint32_t a, uint32_t n);
void guest_call(CPU*c, uint32_t fn);
extern __thread CPU *t_cpu;

typedef struct { const char *name; uint32_t base, size; int loaded; } Dll;
static Dll g_dlls[]={
  {"physx3_x86.dll",0x10000000u,0,0},
  {"apexframeworkshipping_x86.dll",0x10400000u,0,0},
  {"apex_clothingshipping_x86.dll",0x10500000u,0,0},
  {"binkw32.dll",0x10600000u,0,0},
  {"nvtt.dll",0x10680000u,0,0},
  {0,0,0,0}};

int dll_contains(uint32_t a){ for(Dll*d=g_dlls;d->name;d++) if(d->loaded && a>=d->base && a<d->base+d->size) return 1; return 0; }
uint32_t sysmod_handle(const char*t);
uint32_t dll_handle(const char *n){ const char *s=strrchr(n,'\\'); if(!s) s=strrchr(n,'/'); s=s?s+1:n;
  for(Dll*d=g_dlls;d->name;d++) if(d->loaded && !strcasecmp(d->name,s)) return d->base; return sysmod_handle(n); }

static void unimpl_dll_import(CPU*c){ fprintf(stderr,"DLL import not implemented (see bsi_linux.log)\n"); exit(12); }
static void mk_unimpl_name(const char*n){ fprintf(stderr,"[dll] unresolved import %s\n",n); }

static hle_fn resolve(const char *dll, const char *fn){
  if(!strncasecmp(dll,"MSVCR80",7)) dll="MSVCR90.dll";
  char full[256]; snprintf(full,sizeof full,"%s!%s",dll,fn);
  for(int k=0;k<g_nimps;k++) if(!strcasecmp(g_imps[k].name,full)) return g_imps[k].fn;
  char sym[256]; int j=snprintf(sym,sizeof sym,"hle_"); for(const char*p=dll;*p && *p!='.';p++) sym[j++]=tolower((unsigned char)*p);
  sym[j++]='_'; for(const char*p=fn;*p;p++) sym[j++]=isalnum((unsigned char)*p)?*p:'_'; sym[j]=0;
  hle_fn f=(hle_fn)dlsym(RTLD_DEFAULT,sym);
  if(!f) mk_unimpl_name(full);
  return f;
}

static int find_file(const char *want, char *out, int max){
  DIR *dd=opendir("."); if(!dd) return 0; struct dirent *e; int ok=0;
  while((e=readdir(dd))) if(!strcasecmp(e->d_name,want)){ snprintf(out,max,"%s",e->d_name); ok=1; break; }
  closedir(dd); return ok;
}

static uint32_t load_one(CPU *c, Dll *d){
  char path[512]; if(!find_file(d->name,path,sizeof path)) return 0;
  FILE *f=fopen(path,"rb"); fseek(f,0,SEEK_END); long n=ftell(f); fseek(f,0,SEEK_SET); uint8_t *b=malloc(n); fread(b,1,n,f); fclose(f);
  uint32_t pe=*(uint32_t*)(b+0x3c); uint16_t nsec=*(uint16_t*)(b+pe+6); uint16_t optsz=*(uint16_t*)(b+pe+20);
  uint8_t *opt=b+pe+24; uint32_t pref=*(uint32_t*)(opt+28), imgsz=*(uint32_t*)(opt+56), hdrsz=*(uint32_t*)(opt+60);
  uint32_t base=d->base; d->size=(imgsz+0xffff)&~0xffffu;
  guest_map(base,d->size); memset(MEMP(base),0,d->size); memcpy(MEMP(base),b,hdrsz<n?hdrsz:n);
  uint8_t *sec=opt+optsz;
  for(int i=0;i<nsec;i++,sec+=40){ uint32_t vsz=*(uint32_t*)(sec+8), va=*(uint32_t*)(sec+12), rsz=*(uint32_t*)(sec+16), raw=*(uint32_t*)(sec+20);
    uint32_t c=rsz<vsz||!vsz?rsz:vsz; if(raw+c>(uint32_t)n) c=n>raw?n-raw:0; memcpy(MEMP(base+va),b+raw,c); }
  free(b);
  /* data directories (in guest copy of the headers) */
  uint32_t dirs=base+pe+24+96;
  uint32_t reloc_rva=R32(dirs+5*8), reloc_sz=R32(dirs+5*8+4), imp_rva=R32(dirs+1*8);
  int32_t delta=(int32_t)(base-pref);
  if(delta && reloc_rva){ uint32_t p=base+reloc_rva, end=p+reloc_sz; int cnt=0;
    while(p<end){ uint32_t page=R32(p), bs=R32(p+4); if(!bs) break;
      for(uint32_t q=p+8;q<p+bs;q+=2){ uint16_t e=R16(q); if((e>>12)==3){ uint32_t a=base+page+(e&0xfff); W32(a,R32(a)+delta); cnt++; } }
      p+=bs; }
    fprintf(stderr,"[dll] %s: %d relocations applied (delta %08x)\n",d->name,cnt,delta); }
  W32(base+pe+24+28,base);   /* ImageBase = actual */
  if(imp_rva){ for(uint32_t id=base+imp_rva; R32(id+12); id+=20){
      char dn[128]; g2host_a(base+R32(id+12),dn,sizeof dn);
      uint32_t ilt=R32(id)?R32(id):R32(id+16), iat=R32(id+16);
      for(uint32_t k=0;;k++){ uint32_t e=R32(base+ilt+4*k); if(!e) break;
        char fn[160]; if(e&0x80000000u) snprintf(fn,sizeof fn,"ord%u",e&0xffff); else g2host_a(base+e+2,fn,sizeof fn);
        int x=g_nximps++; Imp *im=&g_ximps[x]; char *full=malloc(strlen(dn)+strlen(fn)+2); sprintf(full,"%s!%s",dn,fn);
        im->name=full; im->iat=base+iat+4*k; im->fn=resolve(dn,fn);
        if(!strcmp(fn,"_adjust_fdiv")){ uint32_t v=gmalloc(4); W32(v,0); W32(im->iat,v); continue; }
        W32(im->iat, IMP_BASE+4*(g_nimps+x)); } } }
  d->loaded=1;
  emu_add_region(base,d->size);
  fprintf(stderr,"[dll] loaded %s at %08x (%u KB)\n",d->name,base,d->size>>10);
  /* DllMain(hinst, DLL_PROCESS_ATTACH, 0) */
  uint32_t ep=R32(base+pe+24+16);
  if(ep){ CPU save=*c;
    PUSH32(c,0); PUSH32(c,1); PUSH32(c,base); guest_call(c,base+ep);
    uint32_t r=c->eax; *c=save; fprintf(stderr,"[dll] %s DllMain -> %u\n",d->name,r); if(!r) return 0; }
  return base;
}
uint32_t dll_load(CPU *c, const char *n){
  const char *s=strrchr(n,'\\'); if(!s) s=strrchr(n,'/'); s=s?s+1:n;
  for(Dll*d=g_dlls;d->name;d++) if(!strcasecmp(d->name,s)){ if(d->loaded) return d->base; return load_one(c,d); }
  return 0;
}
uint32_t dll_export(uint32_t base, const char *name, uint32_t ord){
  Dll *d=0; for(Dll*x=g_dlls;x->name;x++) if(x->loaded && x->base==base) d=x; if(!d) return 0;
  uint32_t pe=R32(base+0x3c), exp=R32(base+pe+24+96); if(!exp) return 0; uint32_t ed=base+exp;
  uint32_t nfun=R32(ed+20), nnames=R32(ed+24), funcs=base+R32(ed+28), names=base+R32(ed+32), ords=base+R32(ed+36), obase=R32(ed+16);
  if(!name){ uint32_t i=ord-obase; return i<nfun? base+R32(funcs+4*i):0; }
  for(uint32_t k=0;k<nnames;k++){ char t[256]; g2host_a(base+R32(names+4*k),t,sizeof t); if(!strcmp(t,name)) return base+R32(funcs+4*R16(ords+2*k)); }
  return 0;
}
/* GetProcAddress on a system module: bind to the HLE handler through a new extended import slot */
uint32_t proc_for(const char*dll,const char*name){
  static char *names[512]; static uint32_t addrs[512]; static int n;
  for(int k=0;k<n;k++) if(!strcmp(names[k],name)) return addrs[k];
  hle_fn f=resolve(dll,name); if(!f) return 0;
  int x=g_nximps++; Imp *im=&g_ximps[x]; char *full=malloc(strlen(dll)+strlen(name)+2); sprintf(full,"%s!%s",dll,name);
  im->name=full; im->iat=0; im->fn=f; uint32_t a=IMP_BASE+4*(g_nimps+x);
  if(n<512){ names[n]=strdup(name); addrs[n]=a; n++; } return a;
}
/* extended import table accessors used by the dispatcher */
Imp *imp_at(int idx){ return idx<g_nimps? &g_imps[idx] : &g_ximps[idx-g_nimps]; }
int imps_total(void){ return g_nimps+g_nximps; }

/* exe imports that live in one of these DLLs: resolved lazily on first call */
int dll_owns(const char *full){ const char *b=strchr(full,'!'); if(!b) return 0; char dn[128]; int n=b-full; if(n>=128) return 0; memcpy(dn,full,n); dn[n]=0;
  for(Dll*d=g_dlls;d->name;d++) if(!strcasecmp(d->name,dn)) return 1; return 0; }
uint32_t dll_resolve(CPU *c, const char *full){ const char *b=strchr(full,'!'); char dn[128]; int n=b-full; memcpy(dn,full,n); dn[n]=0;
  uint32_t h=dll_load(c,dn); if(!h){ fprintf(stderr,"[dll] cannot load %s\n",dn); return 0; }
  const char *fn=b+1; uint32_t a = !strncmp(fn,"ord",3) && fn[3]>='0' && fn[3]<='9' ? dll_export(h,0,atoi(fn+3)) : dll_export(h,fn,0);
  if(!a) fprintf(stderr,"[dll] %s: export not found\n",full); return a; }
