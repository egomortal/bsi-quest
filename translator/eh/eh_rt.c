/* MSVC x86 C++ exception handling for the recompiled game (Linux/Android host runtime).
   _CxxThrowException walks the guest fs:[0] registration chain, finds a matching catch via FuncInfo
   (handler stubs: [cookie check] mov eax,FuncInfo; jmp __CxxFrameHandler3), runs unwind actions,
   calls the catch funclet, then longjmps to the host frame of the catching function (EH_FRAME)
   which resumes its recompiled body at the continuation address. */
#include "cpu.h"
#include <stdio.h>
#include <stdlib.h>
__thread EHF *t_ehtop; __thread uint32_t t_eh_cont;
static __thread uint32_t t_last_obj, t_last_ti;
void call_indirect(CPU*c, uint32_t t);
static void guest_call(CPU*c, uint32_t fn){ PUSH32(c,0xDEADBEEFu); call_indirect(c,fn); }
static void wstr(uint32_t p, char *o, int max){ int k=0; if(p) for(;k<max-1;k++){ uint16_t ch=R16(p+2*k); if(!ch) break; o[k]=ch<128?(char)ch:'?'; } o[k]=0; }
#define ARG(k) R32(c->esp+4+4*(k))

static uint32_t funcinfo_of(uint32_t h){
  for(uint32_t a=h;a<h+0x40;a++){
    if(R8(a)==0xB8 && R8(a+5)==0xE9){ uint32_t fi=R32(a+1); if(fi>=0x401000u && fi<0x1700000u){ uint32_t m=R32(fi); if(m>=0x19930520u && m<=0x19930522u) return fi; } }
    if(R8(a)==0xC3 || R8(a)==0xCC) break;
  }
  return 0;
}
static int eh_log=-1;
static void call_frame(CPU*c, uint32_t node, uint32_t fn){   /* call a funclet with ebp = frame of node */
  uint32_t save_ebp=c->ebp; c->ebp=node+12; guest_call(c,fn); c->ebp=save_ebp; }
static void unwind_to(CPU*c, uint32_t node, uint32_t fi, int target){
  uint32_t umap=R32(fi+8); int maxs=(int)R32(fi+4); int st=(int)R32(node+8);
  while(st!=target && st>=0 && st<maxs){ int to=(int)R32(umap+8*st); uint32_t act=R32(umap+8*st+4);
    W32(node+8,(uint32_t)to);
    if(act){ if(eh_log) fprintf(stderr,"[eh]   unwind state %d action %08x\n",st,act); call_frame(c,node,act); }
    st=to; }
}
static int type_match(uint32_t ti, uint32_t htype, uint32_t *ct_out){
  uint32_t cta=R32(ti+12); int n=(int)R32(cta);
  for(int k=0;k<n;k++){ uint32_t ct=R32(cta+4+4*k); uint32_t td=R32(ct+4);
    if(!htype){ *ct_out=ct; return 1; }
    if(td==htype || !strcmp((char*)MEMP(td+8),(char*)MEMP(htype+8))){ *ct_out=ct; return 1; } }
  return 0;
}
void eh_throw(CPU*c){
  if(eh_log<0) eh_log=getenv("BSI_EHLOG")!=0;
  uint32_t obj=ARG(0), ti=ARG(1);
  if(!obj){ obj=t_last_obj; ti=t_last_ti; }        /* rethrow */
  t_last_obj=obj; t_last_ti=ti;
  c->esp+=4+8;   /* drop our frame: the throw site never continues */
  char txt[512]={0}; if(obj && R32(obj)>=0x10000 && R32(obj)<0x80000000u) wstr(R32(obj),txt,sizeof txt);
  if(eh_log) fprintf(stderr,"[eh] throw obj %08x ti %08x '%s'\n",obj,ti,txt);
  uint32_t fs=c->fs_base;
  for(uint32_t node=R32(fs); node && node!=0xffffffffu; node=R32(node)){
    uint32_t fi=funcinfo_of(R32(node+4)); if(!fi) continue;
    int st=(int)R32(node+8); uint32_t ntry=R32(fi+12), tmap=R32(fi+16);
    for(uint32_t t=0;t<ntry;t++){ uint32_t e=tmap+20*t; int lo=(int)R32(e), hi=(int)R32(e+4);
      if(st<lo || st>hi) continue;
      uint32_t ncat=R32(e+12), harr=R32(e+16);
      for(uint32_t h=0;h<ncat;h++){ uint32_t ht=harr+16*h, adj=R32(ht), htype=R32(ht+4), ct=0; int disp=(int)R32(ht+8); uint32_t fn=R32(ht+12);
        if(!type_match(ti,htype,&ct)) continue;
        if(eh_log) fprintf(stderr,"[eh] caught by frame %08x (funcinfo %08x) try %d state %d funclet %08x\n",node,fi,t,st,fn);
        /* 1. unwind all frames above the target */
        for(uint32_t n2=R32(fs); n2!=node; n2=R32(n2)){ uint32_t f2=funcinfo_of(R32(n2+4)); if(f2) unwind_to(c,n2,f2,-1); W32(fs,R32(n2)); }
        W32(fs,node);
        /* 2. unwind the target frame to the try level */
        unwind_to(c,node,fi,lo); W32(node+8,(uint32_t)(hi+1));
        /* 3. catch object */
        if(htype && disp){ uint32_t dst=node+12+disp;
          if(adj&8) W32(dst,obj+R32(ct+8));
          else if(R32(ct)&1) memcpy(MEMP(dst),MEMP(obj+R32(ct+8)),R32(ct+20));
          else if(R32(ct+24)){ uint32_t se=c->ecx; c->ecx=dst; PUSH32(c,obj+R32(ct+8)); guest_call(c,R32(ct+24)); c->ecx=se; }
          else memcpy(MEMP(dst),MEMP(obj),R32(ct+20)); }
        /* 4. run the catch block */
        call_frame(c,node,fn); uint32_t cont=c->eax;
        /* 5. destroy the exception object */
        if(R32(ti+4)){ uint32_t se=c->ecx; c->ecx=obj; guest_call(c,R32(ti+4)); c->ecx=se; }
        /* 6. resume: esp = saved esp of the frame, ebp = frame */
        c->esp=R32(node-4); c->ebp=node+12; W32(fs,node);
        for(EHF *f=t_ehtop; f; f=f->prev) if(f->entry_esp>node){
          if(eh_log) fprintf(stderr,"[eh] resume at %08x (esp %08x)\n",cont,c->esp);
          t_ehtop=f; t_eh_cont=cont; EH_LONGJMP(f->jb); }
        fprintf(stderr,"[eh] no host frame for continuation %08x (node %08x) - cannot resume\n",cont,node); exit(15);
      }
    }
  }
  fprintf(stderr,"[C++ throw] uncaught: '%s'\n",txt); fflush(0); exit(14);
}
