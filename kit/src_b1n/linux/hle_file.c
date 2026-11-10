#include <sys/syscall.h>
#include <time.h>
#define _GNU_SOURCE
/* HLE: files, directories, resources, shell/user32 bits. Guest path mapping:
   C:\BSI\...        -> $BSI_ROOT (game root, default /home/claude/bsigame)
   C:\BSIUSER\...    -> $BSI_USER (documents, default /home/claude/bsiuser)
   relative paths resolve against the guest cwd (initially C:\BSI\Binaries\Win32). Lookup is case-insensitive. */
#include "hle.h"
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <strings.h>
#include <fnmatch.h>
#include <ctype.h>
#include "hle_dircache.h"   /* BSI_DIRCACHE (default off): directory-table cache under g2h(); notes/FABLE_REVIEW.md */

const char *g_exe_path; static const char *ROOT, *USERD;
static char g_cwd[1024]="C:\\BSI\\Binaries\\Win32";
void hle_init(int argc, char **argv){
  ROOT=getenv("BSI_ROOT")?getenv("BSI_ROOT"):"/home/claude/bsigame";
  USERD=getenv("BSI_USER")?getenv("BSI_USER"):"/home/claude/bsiuser";
  mkdir(USERD,0755);
  dc_init(USERD);   /* BSI_DIRCACHE: reads the switch; tables under the user dir (the only tree written) expire after 2 s */
  static char exe[1024]; snprintf(exe,sizeof exe,"%s/Binaries/Win32/BioShockInfinite.exe",ROOT);
  g_exe_path = argc>1?argv[1]:exe;
}
/* normalise a guest path (host string) into an absolute guest path with backslashes, resolving . and .. */
static void gnorm(const char *in, char *out){
  char tmp[2048];
  if(in[0] && in[1]==':') snprintf(tmp,sizeof tmp,"%s",in);
  else if(in[0]=='\\' || in[0]=='/') snprintf(tmp,sizeof tmp,"C:%s",in);
  else snprintf(tmp,sizeof tmp,"%s\\%s",g_cwd,in);
  for(char*p=tmp;*p;p++) if(*p=='/') *p='\\';
  if(!strncmp(tmp,"\\\\?\\",4)) memmove(tmp,tmp+4,strlen(tmp+4)+1);
  char *parts[256]; int n=0; char *save=0;
  for(char *t=strtok_r(tmp+2,"\\",&save); t; t=strtok_r(0,"\\",&save)){
    if(!strcmp(t,".")) continue;
    if(!strcmp(t,"..")){ if(n) n--; continue; }
    parts[n++]=t;
  }
  strcpy(out,"C:"); for(int k=0;k<n;k++){ strcat(out,"\\"); strcat(out,parts[k]); }
  if(!n) strcat(out,"\\");
}
/* guest absolute path -> host path (case-insensitive component match). returns 0 if prefix unknown */
static int g2h(const char *gpath, char *host, int mustexist){
  char g[2048]; gnorm(gpath,g);
  const char *rest; const char *base;
  if(!strncasecmp(g,"C:\\BSIUSER",10) && (g[10]==0||g[10]=='\\')){ base=USERD; rest=g+10; }
  else if(!strncasecmp(g,"C:\\BSI",6) && (g[6]==0||g[6]=='\\')){ base=ROOT; rest=g+6; }
  else { snprintf(host,1024,"/nonexistent%s",g+2); return 0; }
  /* v0.8.8 DLC folder alias (only with BSI_DLC=1, hle_crypt.c): the game scans ..\..\XGAME\DLC (0x535e00 -> 0x560c30),
     but Steam installs the DLC at the top level (<game>\DLC, sibling of XGame), which is how the Quest copy is laid out.
     If <root>/XGame/DLC does not exist and <root>/DLC does, serve XGame\DLC\... from DLC\... . No file is changed and the
     signature check still runs on every bundle. */
  char dlc_alias[2048];
  if(base==ROOT && !strncasecmp(rest,"\\XGame\\DLC",10) && (rest[10]==0||rest[10]=='\\')){
    extern int dlc_crypt_enabled(void) __attribute__((weak));
    if(dlc_crypt_enabled && dlc_crypt_enabled()){
      char xd[1100], td[1100]; struct stat st0;
      snprintf(xd,sizeof xd,"%s/XGame/DLC",ROOT); snprintf(td,sizeof td,"%s/DLC",ROOT);
      if(stat(xd,&st0)!=0 && stat(td,&st0)==0 && S_ISDIR(st0.st_mode)){
        static int said; if(!said){ said=1; fprintf(stderr,"[dlc] XGame\\DLC not present; using %s (Steam layout)\n",td); }
        snprintf(dlc_alias,sizeof dlc_alias,"\\DLC%s",rest+10); rest=dlc_alias; } } }
  if(dc_ready()){ dc_resolve(base,rest,host,1024); return 1; }   /* BSI_DIRCACHE=1 (hle_dircache.h); off = the loop below, unchanged */
  strcpy(host,base);
  char comp[1024]; const char *p=rest;
  while(*p){
    while(*p=='\\') p++; if(!*p) break;
    int k=0; while(*p && *p!='\\') comp[k++]=*p++; comp[k]=0;
    char cand[2048]; snprintf(cand,sizeof cand,"%s/%s",host,comp);
    struct stat st;
    if(stat(cand,&st)==0){ strcpy(host,cand); continue; }
    DIR *d=opendir(host); int found=0;
    if(d){ struct dirent *e; while((e=readdir(d))){ if(!strcasecmp(e->d_name,comp)){ snprintf(cand,sizeof cand,"%s/%s",host,e->d_name); found=1; break; } } closedir(d); }
    strcpy(host,cand);
  }
  return 1;
}
static void gstrw(uint32_t p, char *out){ g2host_w(p,out,2048); }

/* handle table */
#define NH 4096
static struct { int used, fd, vo, vfind, cat, rm; DIR *dir; char pattern[512], dirpath[1024]; } H[NH];   /* vfind: BSI_DIRCACHE exact-name FindFirstFileW (no DIR*) */
/* voice-stream read log (diagnosing doubled dialogue): reads of the streamed VO pack go to <BSI_VOLOG> */
static FILE *g_volog; static int g_volog_n;
static void volog_read(uint64_t off, uint32_t n, uint32_t dst){ if(!g_volog){ const char *p=getenv("BSI_VOLOG"); if(!p) return; g_volog=fopen(p,"w"); if(!g_volog) return; }
  if(g_volog_n++ < 20000){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); fprintf(g_volog,"%ld.%03ld tid %ld off %llu n %u dst %08x\n",(long)ts.tv_sec,ts.tv_nsec/1000000,(long)syscall(SYS_gettid),(unsigned long long)off,n,dst); fflush(g_volog); } }
static int rm_open(const char *h, int fd);   /* build 144: BSI_READMAP, defined above ReadFile */
static uint32_t hnew(void){ for(int k=1;k<NH;k++) if(!H[k].used){ H[k].used=1; H[k].fd=-1; H[k].dir=0; H[k].vo=0; H[k].vfind=0; H[k].cat=0; H[k].rm=-1; return 0x1000u+4*k; } return 0xffffffffu; }
static int hidx(uint32_t h){ if(h<0x1000u || h>=0x1000u+4*NH) return -1; int k=(h-0x1000u)/4; return H[k].used?k:-1; }
int hle_file_close(uint32_t h){ int k=hidx(h); if(k<0) return 0; if(H[k].fd>=0) close(H[k].fd); if(H[k].dir) closedir(H[k].dir); H[k].used=0; return 1; }

/* Step B (ROADMAP item 13): skip the Bink startup logo movies.  BSI_SKIP_INTRO=1 skips the startup movies listed
   in [FullScreenMovie] StartupMovies (2K, Irrational, UE3 and AMD logos); BSI_SKIP_INTRO=<name,name,...> names the movies
   (base names) instead.  Other movies (credits, in-game) are untouched.  Unset/0 = off.
   v0.8.7 (notes/MOVIESKIP.md): failing ALL the logo opens is not safe.  The game's InitiateStartupSequence (0x55a280)
   then starts LoopingMovieUntilStartupFinishes directly with mode 1 (play once, from disk); the loop flag (0x80) is only
   set when the render thread advances to the last StartupMovies entry (0x55ecd0).  It plays its 3 s once and the startup
   sequence ends (index 0x9c = -1).  The title only comes up from UXGameViewportClient 0xaf3480 -> 0xaf3430 -> 0xaf33b0,
   which runs only while the startup sequence is still active (0xaf12b0 -> 0xaf12d0 -> movie slot 0x54); if the startup
   packages finish loading after those 3 s (Quest), the screen stays black forever.
   Stand-in (default): the FIRST skipped movie the game looks for (2KLogoSweep720p2997 with the stock list) is opened as
   LoopingMovieUntilStartupFinishes.bik from the same folder instead of failing, so the sequence starts normally, the other
   logos fail and are dropped from the preload, and the real looping movie is reached through 0x55ecd0 with the loop flag.
   Cost: one extra 3 s pass of the loading loop.  BSI_SKIP_INTRO_STANDIN=0 = v0.8.6 behaviour (all fail);
   BSI_SKIP_INTRO_STANDIN=<base name> = another stand-in movie.  The stand-in movie itself is never skipped. */
static int g2h(const char *gpath, char *host, int mustexist);
static int skip_intro_movie(const char *g, char *h){
  static int mode=-1; static char list[512], standin[128], chosen[300]; static int nlog;
  if(mode<0){ const char *e=getenv("BSI_SKIP_INTRO"); mode=e&&*e&&strcmp(e,"0");
    snprintf(list,sizeof list,",%s,",mode&&strcmp(e,"1")?e:"2KLogoSweep720p2997,IrrationalLogoTest,UE3AnimatedLogo_ProRes,AMDLogo720p");
    const char *s=getenv("BSI_SKIP_INTRO_STANDIN");
    snprintf(standin,sizeof standin,"%s",!s||!*s||!strcmp(s,"1")?"LoopingMovieUntilStartupFinishes":strcmp(s,"0")?s:"");
    if(mode) fprintf(stderr,"[movie] BSI_SKIP_INTRO on, stand-in: %s\n",*standin?standin:"none (all skipped movies fail)"); }
  if(!mode) return 0;
  const char *b=strrchr(h,'/'); b=b?b+1:h; size_t n=strlen(b);
  if(n<5 || strcasecmp(b+n-4,".bik")) return 0;
  if(n-4>=sizeof chosen-2) return 0;
  char k[300]; snprintf(k,sizeof k,",%.*s,",(int)(n-4),b);
  size_t kl=strlen(k); int listed=0;
  for(const char *p=list; *p; p++) if(!strncasecmp(p,k,kl)){ listed=1; break; }
  if(!listed) return 0;
  if(*standin && (size_t)(n-4)==strlen(standin) && !strncasecmp(b,standin,n-4)){
    if(nlog++<4) fprintf(stderr,"[movie] %s is the stand-in movie, not skipped\n",b);
    return 0; }
  if(*standin && (!*chosen || !strcasecmp(chosen,k))){
    /* same guest folder, stand-in base name; resolved case-insensitively like any other open */
    char g2[2048]; const char *gs=strrchr(g,'\\'), *gs2=strrchr(g,'/'); if(gs2>gs) gs=gs2;
    int dl=gs?(int)(gs-g)+1:0; snprintf(g2,sizeof g2,"%.*s%s.bik",dl,g,standin);
    char h2[1024]; struct stat st;
    if(g2h(g2,h2,0) && !stat(h2,&st) && S_ISREG(st.st_mode)){
      if(!*chosen){ snprintf(chosen,sizeof chosen,"%s",k); fprintf(stderr,"[movie] startup movie %s -> stand-in %s (BSI_SKIP_INTRO)\n",b,h2); }
      strcpy(h,h2); return 0; }
  }
  if(nlog++<16) fprintf(stderr,"[movie] skipping startup movie %s (BSI_SKIP_INTRO)\n",b);
  return 1;
}
HLE(kernel32,CreateFileW){
  char g[2048], h[1024]; gstrw(ARG(0),g);
  uint32_t acc=ARG(1), disp=ARG(4);
  if(!strncmp(g,"\\\\.\\",4)){ g_last_error=2; RET_STD(7,0xffffffffu); }   /* pipes/devices */
  g2h(g,h,0);
  if(skip_intro_movie(g,h)){ g_last_error=2; RET_STD(7,0xffffffffu); }
  int fl=0; int wr=(acc&0x40000000u)!=0, rd=(acc&0x80000000u)!=0||!wr;
  fl = wr ? (rd?O_RDWR:O_WRONLY) : O_RDONLY;
  switch(disp){ case 1: fl|=O_CREAT|O_EXCL; break; case 2: fl|=O_CREAT|O_TRUNC; break; case 4: fl|=O_CREAT; break; case 5: fl|=O_TRUNC; break; }
  { struct stat st; if(!stat(h,&st) && S_ISDIR(st.st_mode) && !(ARG(5)&0x02000000u)){ g_last_error=5; RET_STD(7,0xffffffffu); } }
  int fd=open(h,fl,0644);
  static int flog=-1; if(flog<0) flog=getenv("BSI_FILELOG")!=0;
  if(flog) fprintf(stderr,"[file] open '%s' = %s -> %s\n",g,h,fd<0?"FAIL":"ok");
  if(fd<0){ g_last_error = errno==ENOENT?2:(errno==EEXIST?80:5); RET_STD(7,0xffffffffu); }
  if(wr || (fl&(O_CREAT|O_TRUNC))) dc_invalidate(h);   /* BSI_DIRCACHE: the listing may have changed (no-op when off) */
  uint32_t hd=hnew(); H[(hd-0x1000)/4].fd=fd; H[(hd-0x1000)/4].vo=strstr(h,"CombinedStreaming_")!=0; H[(hd-0x1000)/4].rm=rm_open(h,fd);
  { size_t hl=strlen(h); H[(hd-0x1000)/4].cat=(hl>4&&!strcasecmp(h+hl-4,".tfc"))?1:(hl>4&&!strcasecmp(h+hl-4,".xxx"))?2:(hl>4&&!strcasecmp(h+hl-4,".pck"))?3:0; } g_last_error=0; if(flog) fprintf(stderr,"[file]   handle %08x\n",hd); RET_STD(7,hd);
}
HLE(kernel32,CreateFileA){ char g[2048], h[1024]; g2host_a(ARG(0),g,2048); g2h(g,h,0);
  if(skip_intro_movie(g,h)){ g_last_error=2; RET_STD(7,0xffffffffu); }
  uint32_t acc=ARG(1), disp=ARG(4); int wr=(acc&0x40000000u)!=0; int fl=wr?O_RDWR:O_RDONLY;
  switch(disp){ case 1: fl|=O_CREAT|O_EXCL; break; case 2: fl|=O_CREAT|O_TRUNC; break; case 4: fl|=O_CREAT; break; case 5: fl|=O_TRUNC; break; }
  int fd=open(h,fl,0644); if(fd<0){ g_last_error=2; RET_STD(7,0xffffffffu); } if(wr || (fl&(O_CREAT|O_TRUNC))) dc_invalidate(h); uint32_t hd=hnew(); H[(hd-0x1000)/4].fd=fd; H[(hd-0x1000)/4].rm=rm_open(h,fd); RET_STD(7,hd); }
/* build 144: BSI_READMAP (default 0 = off, nothing below runs). =1: read map of the game files, to find data that is never
   read (installer "trim" option). For every file opened under the game root, one bit per 64 KiB block that ReadFile
   returned data from. The map is merged with the one from earlier sessions and written to /sdcard/BSI/readmap.txt
   (BSI_READMAP=/some/path picks another file) at most every 60 s while reads happen. Text format:
     # BSI-Quest read map v1 block=65536
     F <file size> <blocks read> <blocks total> <path relative to the game root>
     R <first>-<last> <first>-<last> ...        (block ranges of the F line above; omitted when nothing was read)
   I/O bookkeeping only: the bytes the game gets, and everything it draws, are unchanged. */
#include <pthread.h>
#define RM_BLOCK 65536u
#define RM_MAX 8192
static struct { char *path; uint64_t size; uint32_t nblk, nset; uint8_t *bits; } RM[RM_MAX];
static int rm_n, rm_on=-1, rm_dirty; static char rm_file[512]; static uint64_t rm_last; static pthread_mutex_t rm_mu=PTHREAD_MUTEX_INITIALIZER;
static uint64_t rm_now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec; }
static int rm_find(const char *rel, uint64_t size){   /* rm_mu held; returns the entry (new or existing), -1 when the table is full */
  for(int i=0;i<rm_n;i++) if(!strcasecmp(RM[i].path,rel)){
    if(RM[i].size!=size){ uint32_t nb=(uint32_t)((size+RM_BLOCK-1)/RM_BLOCK); free(RM[i].bits); RM[i].bits=calloc((nb+7)/8+1,1); RM[i].size=size; RM[i].nblk=nb; RM[i].nset=0; }   /* file changed: start over */
    return i; }
  if(rm_n>=RM_MAX) return -1;
  uint32_t nb=(uint32_t)((size+RM_BLOCK-1)/RM_BLOCK);
  RM[rm_n].path=strdup(rel); RM[rm_n].size=size; RM[rm_n].nblk=nb; RM[rm_n].nset=0; RM[rm_n].bits=calloc((nb+7)/8+1,1);
  return rm_n++;
}
static void rm_set(int i, uint32_t a, uint32_t b){   /* rm_mu held; blocks a..b inclusive */
  if(i<0 || !RM[i].bits) return; if(b>=RM[i].nblk) b=RM[i].nblk?RM[i].nblk-1:0; if(!RM[i].nblk || a>b) return;
  for(uint32_t k=a;k<=b;k++) if(!(RM[i].bits[k>>3]&(1u<<(k&7)))){ RM[i].bits[k>>3]|=(uint8_t)(1u<<(k&7)); RM[i].nset++; rm_dirty=1; }
}
static void rm_load(void){   /* rm_mu held */
  FILE *f=fopen(rm_file,"r"); if(!f) return;
  char *ln=malloc(1<<20); int cur=-1; if(!ln){ fclose(f); return; }
  while(fgets(ln,1<<20,f)){
    if(ln[0]=='F'){ unsigned long long sz; unsigned a,b; int off=0; cur=-1;
      if(sscanf(ln+1," %llu %u %u %n",&sz,&a,&b,&off)>=3 && off>0){ char *nm=ln+1+off; nm[strcspn(nm,"\r\n")]=0; if(*nm) cur=rm_find(nm,(uint64_t)sz); } }
    else if(ln[0]=='R' && cur>=0){ char *q=ln+1; for(;;){ char *e; unsigned long a=strtoul(q,&e,10); if(e==q) break; unsigned long b=a; if(*e=='-'){ q=e+1; b=strtoul(q,&e,10); if(e==q) break; } rm_set(cur,(uint32_t)a,(uint32_t)b); q=e; } }
  }
  free(ln); fclose(f); rm_dirty=0;
  fprintf(stderr,"[readmap] loaded %d files from %s\n",rm_n,rm_file);
}
static void rm_dump(void){   /* rm_mu held */
  char tmp[540]; snprintf(tmp,sizeof tmp,"%s.tmp",rm_file); FILE *f=fopen(tmp,"w"); if(!f) return;
  uint64_t tot=0, got=0;
  fprintf(f,"# BSI-Quest read map v1 block=%u\n",RM_BLOCK);
  for(int i=0;i<rm_n;i++){
    fprintf(f,"F %llu %u %u %s\n",(unsigned long long)RM[i].size,RM[i].nset,RM[i].nblk,RM[i].path); tot+=RM[i].nblk; got+=RM[i].nset;
    if(!RM[i].nset) continue;
    fputs("R",f);
    for(uint32_t k=0;k<RM[i].nblk;){ if(!(RM[i].bits[k>>3]&(1u<<(k&7)))){ k++; continue; }
      uint32_t e=k; while(e+1<RM[i].nblk && (RM[i].bits[(e+1)>>3]&(1u<<((e+1)&7)))) e++;
      if(e==k) fprintf(f," %u",k); else fprintf(f," %u-%u",k,e); k=e+1; }
    fputs("\n",f);
  }
  fclose(f); rename(tmp,rm_file); rm_dirty=0;
  fprintf(stderr,"[readmap] %d files, %.1f of %.1f MB read so far (%.1f %%) -> %s\n",rm_n,got*(RM_BLOCK/1048576.0),tot*(RM_BLOCK/1048576.0),tot?100.0*got/tot:0.0,rm_file);
}
static int rm_enabled(void){
  if(rm_on>=0) return rm_on;
  pthread_mutex_lock(&rm_mu);
  if(rm_on<0){ const char *e=getenv("BSI_READMAP"); int on=e&&*e&&strcmp(e,"0");
    if(on){ snprintf(rm_file,sizeof rm_file,"%s",e[0]=='/'?e:"/sdcard/BSI/readmap.txt"); rm_load(); rm_last=rm_now();
      fprintf(stderr,"[readmap] on (BSI_READMAP): %u KiB blocks, file %s\n",RM_BLOCK/1024,rm_file); }
    rm_on=on; }
  pthread_mutex_unlock(&rm_mu); return rm_on;
}
/* called after a successful open: host path h, descriptor fd. Returns the map entry for the handle, -1 = not tracked. */
static int rm_open(const char *h, int fd){
  if(!rm_enabled() || !ROOT) return -1;
  size_t rl=strlen(ROOT); if(strncmp(h,ROOT,rl) || (h[rl]!='/' && h[rl]!=0)) return -1;   /* game files only (not saves/settings) */
  struct stat st; if(fstat(fd,&st) || !S_ISREG(st.st_mode)) return -1;
  const char *rel=h+rl; while(*rel=='/') rel++;
  pthread_mutex_lock(&rm_mu); int i=rm_find(rel,(uint64_t)st.st_size); if(i>=0) rm_dirty=1; pthread_mutex_unlock(&rm_mu); return i;
}
static void rm_read(int i, uint64_t off, ssize_t r){
  if(i<0 || r<=0) return;
  pthread_mutex_lock(&rm_mu);
  rm_set(i,(uint32_t)(off/RM_BLOCK),(uint32_t)((off+(uint64_t)r-1)/RM_BLOCK));
  if(rm_dirty){ uint64_t t=rm_now(); if(t-rm_last>=60000000000ull){ rm_last=t; rm_dump(); } }
  pthread_mutex_unlock(&rm_mu);
}
void bridge_prefault(uint32_t g, uint32_t n) __attribute__((weak));
/* test hook (notes/MOVIESKIP.md): BSI_TEST_READ_DELAY_US=<us> sleeps before every ReadFile, to make startup loading
   Quest-slow in the cloud (the startup-movie black screen only shows when loading outlasts the movies). Unset = off. */
static int g_test_read_delay=-1;
/* build 65: "[io]" every 30 s when the game read files: MB and total read time by kind (texture caches .tfc, packages .xxx,
   audio .pck, other), to see what texture loading costs on the device. BSI_IOSTAT=0: off. */
static uint64_t io_b[4], io_ns[4], io_n[4]; static int io_on=-1;
static inline uint64_t io_now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return (uint64_t)ts.tv_sec*1000000000ull+ts.tv_nsec; }
static void io_note(int cat, ssize_t r, uint64_t ns){
  if(cat<0||cat>3) cat=0; __atomic_add_fetch(&io_n[cat],1,__ATOMIC_RELAXED); if(r>0) __atomic_add_fetch(&io_b[cat],(uint64_t)r,__ATOMIC_RELAXED); __atomic_add_fetch(&io_ns[cat],ns,__ATOMIC_RELAXED);
  static uint64_t last; static int lk; uint64_t t=io_now(); if(!last){ last=t; return; }
  if(t-last<30000000000ull || __atomic_exchange_n(&lk,1,__ATOMIC_ACQUIRE)) return;
  if(t-last>=30000000000ull){ last=t; static const char *nm[4]={"other","tfc","xxx","pck"};
    char b[512]; int o=snprintf(b,sizeof b,"[io] reads so far:");
    for(int k=0;k<4;k++) if(io_n[k]) o+=snprintf(b+o,sizeof b-o," %s %llu x %.1f MB in %.0f ms (%.0f MB/s)",nm[k],(unsigned long long)io_n[k],io_b[k]/1048576.0,io_ns[k]*1e-6,io_ns[k]?io_b[k]/1048576.0/(io_ns[k]*1e-9):0.0);
    fprintf(stderr,"%s\n",b); }
  __atomic_store_n(&lk,0,__ATOMIC_RELEASE);
}
HLE(kernel32,ReadFile){ int k=hidx(ARG(0)); if(k<0){ g_last_error=6; RET_STD(5,0);} uint32_t ov=ARG(4); ssize_t r; if(bridge_prefault) bridge_prefault(ARG(1),ARG(2));
  if(g_test_read_delay){ if(g_test_read_delay<0){ const char *e=getenv("BSI_TEST_READ_DELAY_US"); g_test_read_delay=e?atoi(e):0; } if(g_test_read_delay>0) usleep(g_test_read_delay); }
  if(io_on<0){ const char *e=getenv("BSI_IOSTAT"); io_on=!(e&&atoi(e)==0); } uint64_t t0=io_on?io_now():0;
  if(ov){ uint64_t off=(uint64_t)R32(ov+8)|((uint64_t)R32(ov+12)<<32); if(H[k].vo) volog_read(off,ARG(2),ARG(1)); r=pread(H[k].fd,MEMP(ARG(1)),ARG(2),off); if(H[k].rm>=0) rm_read(H[k].rm,off,r); W32(ov,0); W32(ov+4,r<0?0:(uint32_t)r); }
  else { if(H[k].vo) volog_read((uint64_t)lseek(H[k].fd,0,SEEK_CUR),ARG(2),ARG(1)); uint64_t rmo=H[k].rm>=0?(uint64_t)lseek(H[k].fd,0,SEEK_CUR):0; r=read(H[k].fd,MEMP(ARG(1)),ARG(2)); if(H[k].rm>=0) rm_read(H[k].rm,rmo,r); }
  if(io_on) io_note(H[k].cat,r,io_now()-t0); if(ARG(3)) W32(ARG(3),r<0?0:(uint32_t)r); RET_STD(5,r>=0); }
HLE(kernel32,WriteFile){ int k=hidx(ARG(0)); if(k<0){ g_last_error=6; RET_STD(5,0);} ssize_t r=write(H[k].fd,MEMP(ARG(1)),ARG(2)); if(ARG(3)) W32(ARG(3),r<0?0:(uint32_t)r); RET_STD(5,r>=0); }
HLE(kernel32,GetFileSize){ int k=hidx(ARG(0)); if(k<0) RET_STD(2,0xffffffffu); struct stat st; fstat(H[k].fd,&st); if(ARG(1)) W32(ARG(1),(uint32_t)((uint64_t)st.st_size>>32)); RET_STD(2,(uint32_t)st.st_size); }
HLE(kernel32,GetFileSizeEx){ int k=hidx(ARG(0)); struct stat st; fstat(H[k].fd,&st); W64(ARG(1),(uint64_t)st.st_size); RET_STD(2,1); }
HLE(kernel32,SetFilePointer){ int k=hidx(ARG(0)); int64_t off=(int32_t)ARG(1); if(ARG(2)) off=((int64_t)(int32_t)R32(ARG(2))<<32)|(uint32_t)ARG(1);
  off_t r=lseek(H[k].fd,off,ARG(3)==0?SEEK_SET:ARG(3)==1?SEEK_CUR:SEEK_END); if(ARG(2)) W32(ARG(2),(uint32_t)((uint64_t)r>>32)); RET_STD(4,(uint32_t)r); }
HLE(kernel32,SetEndOfFile){ int k=hidx(ARG(0)); off_t p=lseek(H[k].fd,0,SEEK_CUR); ftruncate(H[k].fd,p); RET_STD(1,1); }
HLE(kernel32,FlushFileBuffers){ RET_STD(1,1); }
static void fill_find(uint32_t fd, const char *dirpath, const char *name){
  memset(MEMP(fd),0,592); char p[2048]; snprintf(p,sizeof p,"%s/%s",dirpath,name); struct stat st; stat(p,&st);
  W32(fd,S_ISDIR(st.st_mode)?0x10:0x80);
  uint64_t ft=((uint64_t)st.st_mtime+11644473600ull)*10000000ull; W64(fd+4,ft); W64(fd+12,ft); W64(fd+20,ft);
  W32(fd+28,(uint32_t)((uint64_t)st.st_size>>32)); W32(fd+32,(uint32_t)st.st_size);
  for(int k=0;name[k]&&k<259;k++) W16(fd+44+2*k,(uint8_t)name[k]);
}
static int find_next(int k, uint32_t fd){
  struct dirent *e;
  while((e=readdir(H[k].dir))){
    if(!fnmatch(H[k].pattern,e->d_name,FNM_CASEFOLD)){ fill_find(fd,H[k].dirpath,e->d_name); return 1; }
  }
  return 0;
}
HLE(kernel32,FindFirstFileW){
  char g[2048], hp[1024]; gstrw(ARG(0),g);
  char *bs=strrchr(g,'\\'); char pat[512]; if(bs){ strcpy(pat,bs+1); *bs=0; } else { strcpy(pat,g); strcpy(g,"."); }
  int known=g2h(g,hp,0);
  if(known && !dc_haswild(pat)){   /* BSI_DIRCACHE: exact name (UE3's existence probes) answered from the directory table */
    char real[512]; int r=dc_dir_lookup(hp,pat,real,sizeof real,0);
    if(r==-1){ g_last_error=3; RET_STD(2,0xffffffffu); }
    if(r==0){ g_last_error=2; RET_STD(2,0xffffffffu); }
    if(r==1){ uint32_t hd=hnew(); int k=(hd-0x1000)/4; H[k].vfind=1; strcpy(H[k].dirpath,hp); fill_find(ARG(1),hp,real); RET_STD(2,hd); }
  }                                /* r==-2 (cache off / unreadable dir): the scan below, unchanged */
  DIR *d=opendir(hp); if(!d){ g_last_error=3; RET_STD(2,0xffffffffu); }
  uint32_t hd=hnew(); int k=(hd-0x1000)/4; H[k].dir=d; strcpy(H[k].pattern,pat); strcpy(H[k].dirpath,hp);
  if(!strcmp(pat,"*.*")) strcpy(H[k].pattern,"*");
  if(!find_next(k,ARG(1))){ hle_file_close(hd); g_last_error=2; RET_STD(2,0xffffffffu); }
  RET_STD(2,hd);
}
HLE(kernel32,FindNextFileW){ int k=hidx(ARG(0)); if(k<0||(!H[k].dir&&!H[k].vfind)){ g_last_error=6; RET_STD(2,0);} if(!H[k].vfind && find_next(k,ARG(1))) RET_STD(2,1); g_last_error=18; RET_STD(2,0); }
HLE(kernel32,FindClose){ hle_file_close(ARG(0)); RET_STD(1,1); }
HLE(kernel32,GetFileAttributesW){ char g[2048],h[1024]; gstrw(ARG(0),g); g2h(g,h,0); struct stat st; if(stat(h,&st)){ g_last_error=2; RET_STD(1,0xffffffffu);} RET_STD(1,S_ISDIR(st.st_mode)?0x10:0x80); }
HLE(kernel32,GetFileAttributesExW){ char g[2048],h[1024]; gstrw(ARG(0),g); g2h(g,h,0); struct stat st; if(stat(h,&st)){ g_last_error=2; RET_STD(3,0);} uint32_t p=ARG(2); memset(MEMP(p),0,36); W32(p,S_ISDIR(st.st_mode)?0x10:0x80); uint64_t ft=((uint64_t)st.st_mtime+11644473600ull)*10000000ull; W64(p+4,ft); W64(p+12,ft); W64(p+20,ft); W32(p+28,(uint32_t)((uint64_t)st.st_size>>32)); W32(p+32,(uint32_t)st.st_size); RET_STD(3,1); }
HLE(kernel32,CreateDirectoryW){ char g[2048],h[1024]; gstrw(ARG(0),g); g2h(g,h,0); if(mkdir(h,0755)){ g_last_error= errno==EEXIST?183:3; RET_STD(2,0);} dc_invalidate(h); RET_STD(2,1); }
HLE(kernel32,DeleteFileW){ char g[2048],h[1024]; gstrw(ARG(0),g); g2h(g,h,0); int ok=unlink(h)==0; if(ok) dc_invalidate(h); RET_STD(1,ok); }
/* BSI_DIRCACHE: MoveFileW / CopyFileW / RemoveDirectoryW are still auto-stubs (imps.c); if they get implemented, call
   dc_invalidate() on every host path they create, remove or rename (both ends of a move) after the operation. */
HLE(kernel32,SetCurrentDirectoryW){ char g[2048]; gstrw(ARG(0),g); gnorm(g,g_cwd); RET_STD(1,1); }
HLE(kernel32,GetCurrentDirectoryW){ uint32_t n=ARG(0), d=ARG(1); uint32_t l=strlen(g_cwd); if(n<=l) RET_STD(2,l+1); for(uint32_t k=0;k<=l;k++) W16(d+2*k,(uint8_t)g_cwd[k]); RET_STD(2,l); }
HLE(msvcr90,_wstat64i32){ char g[2048],h[1024]; gstrw(ARG(0),g); g2h(g,h,0); struct stat st; if(stat(h,&st)) RET_C(0xffffffffu);
  uint32_t b=ARG(1); memset(MEMP(b),0,48); W16(b+6,S_ISDIR(st.st_mode)?0x41ff:0x81b6); W32(b+20,(uint32_t)st.st_size); W64(b+24,(uint64_t)st.st_atime); W64(b+32,(uint64_t)st.st_mtime); W64(b+40,(uint64_t)st.st_ctime); RET_C(0); }
HLE(shell32,SHGetFolderPathW){ const char *s="C:\\BSIUSER"; uint32_t d=ARG(4); for(int k=0;;k++){ W16(d+2*k,(uint8_t)s[k]); if(!s[k]) break; } RET_STD(5,0); }
HLE(shlwapi,PathRemoveBlanksW){ uint32_t p=ARG(0); uint32_t n=gwcslen(p), a=0; while(a<n && R16(p+2*a)==' ') a++; uint32_t b=n; while(b>a && R16(p+2*(b-1))==' ') b--; for(uint32_t k=a;k<b;k++) W16(p+2*(k-a),R16(p+2*k)); W16(p+2*(b-a),0); RET_STD(1,0); }
HLE(shlwapi,PathFindFileNameW){ uint32_t p=ARG(0), r=p; for(uint32_t k=0;R16(p+2*k);k++){ uint16_t ch=R16(p+2*k); if((ch=='\\'||ch=='/'||ch==':') && R16(p+2*k+2)) r=p+2*k+2; } RET_STD(1,r); }
HLE(shlwapi,PathMatchSpecW){ char a[2048], s[512]; gstrw(ARG(0),a); gstrw(ARG(1),s); char *nm=strrchr(a,'\\'); RET_STD(2, fnmatch(s,nm?nm+1:a,FNM_CASEFOLD)==0); }

/* resources: walk .rsrc of the guest image */
static uint32_t rsrc_base(void){ uint32_t pe=R32(0x400000+0x3c); uint32_t dd=0x400000+pe+24+96+2*8; return 0x400000+R32(dd); }
static int rsrc_match(uint32_t dir, uint32_t id, uint32_t *out){
  uint32_t base=rsrc_base(); uint16_t nn=R16(dir+12), ni=R16(dir+14);
  for(uint32_t k=0;k<(uint32_t)nn+ni;k++){ uint32_t e=dir+16+8*k, name=R32(e), off=R32(e+4);
    int ok=0;
    if(id<0x10000){ ok = !(name&0x80000000u) && (name&0xffff)==id; }
    else if(name&0x80000000u){ uint32_t s=base+(name&0x7fffffffu); uint16_t l=R16(s); ok=1; for(uint32_t j=0;j<l;j++){ if(wlower(R16(s+2+2*j))!=wlower(R16(id+2*j))){ ok=0; break;} } if(ok && R16(id+2*l)) ok=0; }
    if(ok){ *out = base+(off&0x7fffffffu) | (off&0x80000000u ? 0x80000000u:0); return 1; }
  }
  return 0;
}
HLE(kernel32,FindResourceW){ uint32_t base=rsrc_base(), d1, d2, d3; uint32_t name=ARG(1), type=ARG(2);
  if(!rsrc_match(base,type,&d1) || !rsrc_match(d1&0x7fffffffu,name,&d2)){ g_last_error=1813; RET_STD(3,0); }
  uint32_t lang=(d2&0x7fffffffu); uint32_t first=lang+16; d3=R32(first+4); RET_STD(3, base+(d3&0x7fffffffu)); }
HLE(kernel32,LoadResource){ RET_STD(2,ARG(1)); }
HLE(kernel32,SizeofResource){ RET_STD(2,R32(ARG(1)+4)); }
HLE(kernel32,LockResource){ RET_STD(1,0x400000+R32(ARG(0))); }

/* user32 basics */
HLE(user32,GetSystemMetrics){ uint32_t i=ARG(0); RET_STD(1, i==0?1920: i==1?1080: i==78?1920: i==79?1080: 0); }
HLE(user32,SystemParametersInfoW){ RET_STD(4,1); }
/* _stat64i32 / _wstat64i32: dev 0, ino 4, mode 6, nlink 8, uid 10, gid 12, rdev 16, size 20, atime 24, mtime 32, ctime 40 */
static uint32_t fill_stat(const char *h, uint32_t out){ struct stat st; if(stat(h,&st)) { g_last_error=2; return 0xffffffffu; }
  memset(MEMP(out),0,48); W16(out+6,(uint16_t)(S_ISDIR(st.st_mode)?0x4000|0x1ff:0x8000|0x1b6)); W16(out+8,1); W32(out+20,(uint32_t)st.st_size);
  W64(out+24,(uint64_t)st.st_atime); W64(out+32,(uint64_t)st.st_mtime); W64(out+40,(uint64_t)st.st_ctime); return 0; }
HLE(msvcr90,_stat64i32){ char g[2048],h[1024]; g2host_a(ARG(0),g,2048); g2h(g,h,0); RET_C(fill_stat(h,ARG(1))); }
