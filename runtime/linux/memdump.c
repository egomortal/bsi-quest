/* BSI_MEMDUMP=<seconds>[,<seconds>...]: write a snapshot of all mapped guest memory to
   BSI_MEMDUMP_DIR (default /tmp/bsimem)/snap<N>.bin as records {u32 guest_addr, u32 len, bytes}. Used for offline
   inspection of engine state (names table, object array, actor properties) with tools/uinspect.py. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
extern uint8_t *M;
void memdump_snap(int n);
static void snap(int n){
  const char *d=getenv("BSI_MEMDUMP_DIR"); if(!d) d="/tmp/bsimem"; char cmd[600]; snprintf(cmd,sizeof cmd,"mkdir -p '%s'",d); if(system(cmd)){}
  char fn[600]; snprintf(fn,sizeof fn,"%s/snap%d.bin",d,n); FILE *o=fopen(fn,"wb"); if(!o) return;
  FILE *m=fopen("/proc/self/maps","r"); char l[512]; uint64_t base=(uint64_t)(uintptr_t)M, tot=0;
  while(fgets(l,sizeof l,m)){ uint64_t a,b; char perm[8]; if(sscanf(l,"%lx-%lx %7s",&a,&b,perm)<3) continue; if(perm[0]!='r') continue;
    if(b<=base||a>=base+0x100000000ull) continue; if(a<base) a=base; if(b>base+0x100000000ull) b=base+0x100000000ull;
    for(uint64_t p=a;p<b;p+=0x1000000){ uint64_t e=p+0x1000000<b?p+0x1000000:b; uint32_t ga=(uint32_t)(p-base), len=(uint32_t)(e-p);
      /* skip all-zero chunks */ int nz=0; for(uint64_t q=p;q<e;q+=4096) if(*(uint64_t*)q||*(uint64_t*)(q+64)){ nz=1; break; } if(!nz) continue;
      fwrite(&ga,4,1,o); fwrite(&len,4,1,o); fwrite((void*)p,1,len,o); tot+=len; } }
  fclose(m); fclose(o); fprintf(stderr,"[memdump] %s: %lu MB\n",fn,(unsigned long)(tot>>20));
}
static void *thr(void *arg){ const char *e=getenv("BSI_MEMDUMP"); int n=0; double prev=0;
  while(e&&*e){ double t=atof(e); usleep((useconds_t)((t-prev)*1e6)); prev=t; snap(n++); e=strchr(e,','); if(e) e++; } return 0; }
void memdump_init(void){ if(!getenv("BSI_MEMDUMP")) return; pthread_t t; pthread_create(&t,0,thr,0); pthread_detach(t); }

void memdump_snap(int n){ snap(n); }
