/* HLE: MSVCR90 subset + printf engine */
#include "hle.h"
uint32_t gmalloc_c(uint32_t n, uint32_t caller);
#include <math.h>
#include <ctype.h>
#include <stdarg.h>
#include <time.h>

/* -------- printf with guest varargs. fmt/out in guest memory; wide selects 16-bit chars -------- */
typedef struct { char *b; int n, cap; } SB;
static void sb_put(SB*s, int ch){ if(s->n+1>=s->cap){ s->cap=s->cap?s->cap*2:256; s->b=realloc(s->b,s->cap);} s->b[s->n++]=(char)ch; s->b[s->n]=0; }
static void sb_puts(SB*s, const char*t){ while(*t) sb_put(s,*t++); }
/* returns host buffer (UTF-8/latin1 bytes, one byte per char) */
char* gformat(uint32_t fmt, int wide, uint32_t va){
  SB s={0}; sb_put(&s,0); s.n=0;
  uint32_t i=0;
  #define FCH(k) (wide? R16(fmt+2*(k)) : R8(fmt+(k)))
  for(;;){
    uint32_t ch=FCH(i++); if(!ch) break;
    if(ch!='%'){ sb_put(&s,ch<256?ch:'?'); continue; }
    char spec[32]; int sp=0; spec[sp++]='%';
    uint32_t c2;
    while((c2=FCH(i)) && strchr("-+ #0",c2)){ spec[sp++]=c2; i++; }
    if(FCH(i)=='*'){ int w=(int)R32(va); va+=4; sp+=sprintf(spec+sp,"%d",w); i++; } else while(isdigit(FCH(i))) spec[sp++]=FCH(i++);
    if(FCH(i)=='.'){ spec[sp++]='.'; i++; if(FCH(i)=='*'){ int p=(int)R32(va); va+=4; sp+=sprintf(spec+sp,"%d",p); i++; } else while(isdigit(FCH(i))) spec[sp++]=FCH(i++); }
    int lng=0, h=0, i64=0;
    for(;;){ c2=FCH(i);
      if(c2=='l'){ lng++; i++; } else if(c2=='h'){ h=1; i++; }
      else if(c2=='I' && FCH(i+1)=='6' && FCH(i+2)=='4'){ i64=1; i+=3; }
      else if(c2=='I' && FCH(i+1)=='3' && FCH(i+2)=='2'){ i+=3; }
      else if(c2=='L'||c2=='q'||c2=='z'||c2=='t'){ if(c2=='q') i64=1; i++; }
      else break; }
    if(lng>=2) i64=1;
    uint32_t conv=FCH(i++); char tmp[512];
    switch(conv){
      case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
        if(i64){ uint64_t v=R64(va); va+=8; spec[sp++]='l'; spec[sp++]='l'; spec[sp++]=conv; spec[sp]=0; snprintf(tmp,sizeof tmp,spec,v); }
        else { uint32_t v=R32(va); va+=4; if(h) v=(conv=='d'||conv=='i')?(uint32_t)(int32_t)(int16_t)v:(uint16_t)v; spec[sp++]=conv; spec[sp]=0; snprintf(tmp,sizeof tmp,spec,v); }
        sb_puts(&s,tmp); break; }
      case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': {
        double v; memcpy(&v,MEMP(va),8); va+=8; spec[sp++]=conv; spec[sp]=0; snprintf(tmp,sizeof tmp,spec,v); sb_puts(&s,tmp); break; }
      case 'c': case 'C': { uint32_t v=R32(va); va+=4; sb_put(&s,(v&0xffff)<256?v&0xff:'?'); break; }
      case 'p': { uint32_t v=R32(va); va+=4; snprintf(tmp,sizeof tmp,"%08X",v); sb_puts(&s,tmp); break; }
      case 's': case 'S': {
        uint32_t p=R32(va); va+=4;
        int widestr = (conv=='s') ? (wide ? !h : lng>0) : (wide ? 0 : !h);
        if(conv=='s' && wide && h) widestr=0;
        char *hs; char buf[8192];
        if(!p){ hs="(null)"; } else if(widestr) { g2host_w(p,buf,sizeof buf); hs=buf; } else { g2host_a(p,buf,sizeof buf); hs=buf; }
        spec[sp++]='s'; spec[sp]=0;
        char *o=malloc(strlen(hs)+600); sprintf(o,spec,hs); sb_puts(&s,o); free(o); break; }
      case '%': sb_put(&s,'%'); break;
      case 'n': va+=4; break;
      default: sb_put(&s,'%'); if(conv) sb_put(&s,conv); break;
    }
  }
  if(!s.b){ s.b=calloc(1,1); }
  return s.b;
}
static uint32_t put_out(uint32_t dst, uint32_t cap, int wide, const char *s){ /* returns chars written (excl NUL) or -1 if truncated */
  uint32_t n=strlen(s);
  if(cap==0) return n;
  uint32_t m = n < cap-1 ? n : cap-1;
  for(uint32_t k=0;k<m;k++){ if(wide) W16(dst+2*k,(uint8_t)s[k]); else W8(dst+k,s[k]); }
  if(wide) W16(dst+2*m,0); else W8(dst+m,0);
  return n<cap ? n : (uint32_t)-1;
}
/* _vsnwprintf_s(buf, sizeInWords, count, fmt, va) */
HLE(msvcr90,_vsnwprintf_s){ char*s=gformat(ARG(3),1,ARG(4)); uint32_t cap=ARG(1); uint32_t cnt=ARG(2); if(cnt!=0xffffffffu && cnt+1<cap) cap=cnt+1; uint32_t r=put_out(ARG(0),cap,1,s); free(s); RET_C(r); }
HLE(msvcr90,_vscwprintf_p){ char*s=gformat(ARG(0),1,ARG(1)); uint32_t n=strlen(s); free(s); RET_C(n); }
HLE(msvcr90,_vscwprintf){ char*s=gformat(ARG(0),1,ARG(1)); uint32_t n=strlen(s); free(s); RET_C(n); }
HLE(msvcr90,vswprintf_s){ char*s=gformat(ARG(2),1,ARG(3)); uint32_t r=put_out(ARG(0),ARG(1),1,s); free(s); RET_C(r); }
HLE(msvcr90,_vsnprintf_s){ char*s=gformat(ARG(3),0,ARG(4)); uint32_t cap=ARG(1); uint32_t cnt=ARG(2); if(cnt!=0xffffffffu && cnt+1<cap) cap=cnt+1; uint32_t r=put_out(ARG(0),cap,0,s); free(s); RET_C(r); }
HLE(msvcr90,vsprintf_s){ char*s=gformat(ARG(2),0,ARG(3)); uint32_t r=put_out(ARG(0),ARG(1),0,s); free(s); RET_C(r); }
HLE(msvcr90,_vsnprintf){ char*s=gformat(ARG(2),0,ARG(3)); uint32_t r=put_out(ARG(0),ARG(1)+1,0,s); free(s); RET_C(r); }
HLE(msvcr90,_vscprintf){ char*s=gformat(ARG(0),0,ARG(1)); uint32_t n=strlen(s); free(s); RET_C(n); }

/* -------- CRT startup -------- */
static uint32_t g_fmode, g_commode, g_onexit[4096]; static int g_nonexit;
HLE(msvcr90,__set_app_type){ RET_C(0); }
HLE(msvcr90,_encode_pointer){ RET_C(ARG(0)); }
HLE(msvcr90,_decode_pointer){ RET_C(ARG(0)); }
HLE(msvcr90,__p__fmode){ if(!g_fmode) g_fmode=gmalloc(4); RET_C(g_fmode); }
HLE(msvcr90,__p__commode){ if(!g_commode) g_commode=gmalloc(4); RET_C(g_commode); }
HLE(msvcr90,_controlfp_s){ if(ARG(0)) W32(ARG(0),0x9001f); RET_C(0); }
HLE(msvcr90,_onexit){ if(g_nonexit<4096) g_onexit[g_nonexit++]=ARG(0); RET_C(ARG(0)); }
HLE(msvcr90,__dllonexit){ if(g_nonexit<4096) g_onexit[g_nonexit++]=ARG(0); RET_C(ARG(0)); }
HLE(msvcr90,_initterm){
  uint32_t a=ARG(0), e=ARG(1), ret=R32(c->esp); c->esp+=4;
  for(;a<e;a+=4){ uint32_t f=R32(a); if(f){ guest_call(c,f); } }
  c->esp-=4; W32(c->esp,ret); RET_C(0);
}
HLE(msvcr90,_initterm_e){
  uint32_t a=ARG(0), e=ARG(1), ret=R32(c->esp), rc=0; c->esp+=4;
  for(;a<e && !rc;a+=4){ uint32_t f=R32(a); if(f){ guest_call(c,f); rc=c->eax; } }
  c->esp-=4; W32(c->esp,ret); RET_C(rc);
}
extern const char *g_guest_cmdline;
static uint32_t g_argv, g_envp;
HLE(msvcr90,__getmainargs){
  if(!g_argv){ g_argv=gmalloc(8); W32(g_argv,galloc_str("BioShockInfinite.exe")); W32(g_argv+4,0); g_envp=gmalloc(4); W32(g_envp,0); }
  W32(ARG(0),1); W32(ARG(1),g_argv); W32(ARG(2),g_envp); RET_C(0);
}
HLE(msvcr90,_set_SSE2_enable){ RET_C(1); }
HLE(msvcr90,_set_invalid_parameter_handler){ RET_C(0); }
HLE(msvcr90,_set_purecall_handler){ RET_C(0); }
HLE(msvcr90,_configthreadlocale){ RET_C(0); }
HLE(msvcr90,_ismbblead){ RET_C(0); }
HLE(msvcr90,_amsg_exit){ fprintf(stderr,"_amsg_exit(%u)\n",ARG(0)); exit(7); }
void guest_stackscan(CPU*c, const char*why);
HLE(msvcr90,exit){ fprintf(stderr,"guest exit(%d)\n",(int)ARG(0)); if(getenv("BSI_EXITSCAN")) guest_stackscan(c,"exit"); fflush(0); exit((int)ARG(0)); }
HLE(msvcr90,_exit){ fprintf(stderr,"guest _exit(%d)\n",(int)ARG(0)); exit((int)ARG(0)); }
HLE(msvcr90,_cexit){ RET_C(0); }

/* -------- memory -------- */
HLE(msvcr90,malloc){ RET_C(gmalloc_c(ARG(0),R32(c->esp))); }
HLE(msvcr90,free){ gfree(ARG(0)); RET_C(0); }
HLE(msvcr90,calloc){ uint32_t n=ARG(0)*ARG(1); uint32_t p=gmalloc_c(n,R32(c->esp)); memset(MEMP(p),0,n); RET_C(p); }
HLE(msvcr90,realloc){ uint32_t p=ARG(0), n=ARG(1); if(!p) RET_C(gmalloc_c(n,R32(c->esp))); if(!n){ gfree(p); RET_C(0);} uint32_t o=gsize(p); if(o>=n) RET_C(p); uint32_t q=gmalloc_c(n,R32(c->esp)); memcpy(MEMP(q),MEMP(p),o); gfree(p); RET_C(q); }
HLE(msvcr90,_msize){ RET_C(gsize(ARG(0))); }
HLE(msvcr90,memcpy){ memmove(MEMP(ARG(0)),MEMP(ARG(1)),ARG(2)); RET_C(ARG(0)); }
HLE(msvcr90,memmove){ memmove(MEMP(ARG(0)),MEMP(ARG(1)),ARG(2)); RET_C(ARG(0)); }
HLE(msvcr90,memset){ memset(MEMP(ARG(0)),(int)ARG(1),ARG(2)); RET_C(ARG(0)); }
HLE(msvcr90,memcmp){ RET_C((uint32_t)memcmp(MEMP(ARG(0)),MEMP(ARG(1)),ARG(2))); }
HLE(msvcr90,memmove_s){ memmove(MEMP(ARG(0)),MEMP(ARG(2)),ARG(3)); RET_C(0); }
HLE(msvcr90,memcpy_s){ memmove(MEMP(ARG(0)),MEMP(ARG(2)),ARG(3)); RET_C(0); }

/* -------- narrow strings -------- */
HLE(msvcr90,strlen){ RET_C(gstrlen(ARG(0))); }
HLE(msvcr90,strcpy_s){ uint32_t d=ARG(0),n=ARG(1),s=ARG(2),k=0; for(;k<n;k++){ uint8_t ch=R8(s+k); W8(d+k,ch); if(!ch) break; } if(k==n&&n) W8(d,0); RET_C(k==n?34:0); }
HLE(msvcr90,strncpy_s){ uint32_t d=ARG(0),n=ARG(1),s=ARG(2),cnt=ARG(3),k=0; for(;k<n-1 && k<cnt;k++){ uint8_t ch=R8(s+k); if(!ch) break; W8(d+k,ch);} W8(d+k,0); RET_C(0); }
HLE(msvcr90,strncpy){ uint32_t d=ARG(0),s=ARG(1),n=ARG(2),k=0; for(;k<n;k++){ uint8_t ch=R8(s+k); W8(d+k,ch); if(!ch){ for(k++;k<n;k++) W8(d+k,0); break; } } RET_C(d); }
HLE(msvcr90,strcat_s){ uint32_t d=ARG(0),n=ARG(1),s=ARG(2),l=gstrlen(d),k=0; for(;l+k<n;k++){ uint8_t ch=R8(s+k); W8(d+l+k,ch); if(!ch) break; } RET_C(0); }
HLE(msvcr90,_stricmp){ uint32_t a=ARG(0),b=ARG(1); for(uint32_t k=0;;k++){ int x=tolower(R8(a+k)), y=tolower(R8(b+k)); if(x!=y) RET_C((uint32_t)(x-y)); if(!x) RET_C(0);} }
HLE(msvcr90,_strnicmp){ uint32_t a=ARG(0),b=ARG(1),n=ARG(2); for(uint32_t k=0;k<n;k++){ int x=tolower(R8(a+k)), y=tolower(R8(b+k)); if(x!=y) RET_C((uint32_t)(x-y)); if(!x) break;} RET_C(0); }
HLE(msvcr90,strcmp){ uint32_t a=ARG(0),b=ARG(1); for(uint32_t k=0;;k++){ int x=R8(a+k), y=R8(b+k); if(x!=y) RET_C((uint32_t)(x-y)); if(!x) RET_C(0);} }
HLE(msvcr90,strncmp){ uint32_t a=ARG(0),b=ARG(1),n=ARG(2); for(uint32_t k=0;k<n;k++){ int x=R8(a+k), y=R8(b+k); if(x!=y) RET_C((uint32_t)(x-y)); if(!x) break;} RET_C(0); }
HLE(msvcr90,tolower){ RET_C((uint32_t)tolower((int)ARG(0))); }
HLE(msvcr90,toupper){ RET_C((uint32_t)toupper((int)ARG(0))); }
HLE(msvcr90,strchr){ uint32_t a=ARG(0); int ch=(uint8_t)ARG(1); for(;;a++){ int x=R8(a); if(x==ch) RET_C(a); if(!x) RET_C(0);} }
HLE(msvcr90,strrchr){ uint32_t a=ARG(0),r=0; int ch=(uint8_t)ARG(1); for(;;a++){ int x=R8(a); if(x==ch) r=a; if(!x) break;} RET_C(ch?r:a); }
HLE(msvcr90,strstr){ uint32_t a=ARG(0),b=ARG(1),lb=gstrlen(b); for(;;a++){ if(!memcmp(MEMP(a),MEMP(b),lb)) RET_C(a); if(!R8(a)) RET_C(0);} }
HLE(msvcr90,atoi){ char t[64]; g2host_a(ARG(0),t,64); RET_C((uint32_t)atoi(t)); }
HLE(msvcr90,atof){ char t[128]; g2host_a(ARG(0),t,128); c->top=(c->top-1)&7; c->st[c->top]=atof(t); c->esp+=4; }

/* -------- wide strings -------- */
HLE(msvcr90,wcscpy_s){ uint32_t d=ARG(0),n=ARG(1),s=ARG(2),k=0; for(;k<n;k++){ uint16_t ch=R16(s+2*k); W16(d+2*k,ch); if(!ch) break; } if(k==n&&n) W16(d,0); RET_C(k==n?34:0); }
HLE(msvcr90,wcsncpy_s){ uint32_t d=ARG(0),n=ARG(1),s=ARG(2),cnt=ARG(3),k=0; for(;k<n-1 && k<cnt;k++){ uint16_t ch=R16(s+2*k); if(!ch) break; W16(d+2*k,ch);} W16(d+2*k,0); RET_C(0); }
HLE(msvcr90,wcscat_s){ uint32_t d=ARG(0),n=ARG(1),s=ARG(2),l=gwcslen(d),k=0; for(;l+k<n;k++){ uint16_t ch=R16(s+2*k); W16(d+2*(l+k),ch); if(!ch) break; } RET_C(0); }
HLE(msvcr90,wcslen){ RET_C(gwcslen(ARG(0))); }
HLE(msvcr90,_wcsicmp){ uint32_t a=ARG(0),b=ARG(1); for(uint32_t k=0;;k++){ int x=wlower(R16(a+2*k)), y=wlower(R16(b+2*k)); if(x!=y) RET_C((uint32_t)(x-y)); if(!x) RET_C(0);} }
HLE(msvcr90,_wcsnicmp){ uint32_t a=ARG(0),b=ARG(1),n=ARG(2); for(uint32_t k=0;k<n;k++){ int x=wlower(R16(a+2*k)), y=wlower(R16(b+2*k)); if(x!=y) RET_C((uint32_t)(x-y)); if(!x) break;} RET_C(0); }
HLE(msvcr90,wcscmp){ uint32_t a=ARG(0),b=ARG(1); for(uint32_t k=0;;k++){ int x=R16(a+2*k), y=R16(b+2*k); if(x!=y) RET_C((uint32_t)(x-y)); if(!x) RET_C(0);} }
HLE(msvcr90,wcsncmp){ uint32_t a=ARG(0),b=ARG(1),n=ARG(2); for(uint32_t k=0;k<n;k++){ int x=R16(a+2*k), y=R16(b+2*k); if(x!=y) RET_C((uint32_t)(x-y)); if(!x) break;} RET_C(0); }
HLE(msvcr90,wcschr){ uint32_t a=ARG(0); uint16_t ch=(uint16_t)ARG(1); for(;;a+=2){ uint16_t x=R16(a); if(x==ch) RET_C(a); if(!x) RET_C(0);} }
HLE(msvcr90,wcsrchr){ uint32_t a=ARG(0),r=0; uint16_t ch=(uint16_t)ARG(1); for(;;a+=2){ uint16_t x=R16(a); if(x==ch) r=a; if(!x) break;} RET_C(ch?r:a); }
HLE(msvcr90,wcsstr){ uint32_t a=ARG(0),b=ARG(1),lb=gwcslen(b); for(;;a+=2){ if(!memcmp(MEMP(a),MEMP(b),2*lb)) RET_C(a); if(!R16(a)) RET_C(0);} }
HLE(msvcr90,_wtoi){ char t[64]; g2host_w(ARG(0),t,64); RET_C((uint32_t)atoi(t)); }
HLE(msvcr90,_wtof){ char t[128]; g2host_w(ARG(0),t,128); c->top=(c->top-1)&7; c->st[c->top]=atof(t); c->esp+=4; }
HLE(msvcr90,wcstod){ char t[128]; uint32_t p=ARG(0); g2host_w(p,t,128); char*e; double v=strtod(t,&e); if(ARG(1)) W32(ARG(1),p+2*(uint32_t)(e-t)); c->top=(c->top-1)&7; c->st[c->top]=v; c->esp+=4; }
HLE(msvcr90,wcstoul){ char t[128]; uint32_t p=ARG(0); g2host_w(p,t,128); char*e; unsigned long v=strtoul(t,&e,(int)ARG(2)); if(ARG(1)) W32(ARG(1),p+2*(uint32_t)(e-t)); RET_C((uint32_t)v); }
HLE(msvcr90,wcstol){ char t[128]; uint32_t p=ARG(0); g2host_w(p,t,128); char*e; long v=strtol(t,&e,(int)ARG(2)); if(ARG(1)) W32(ARG(1),p+2*(uint32_t)(e-t)); RET_C((uint32_t)v); }
HLE(msvcr90,towlower){ RET_C(wlower((uint16_t)ARG(0))); }
HLE(msvcr90,towupper){ uint16_t ch=(uint16_t)ARG(0); RET_C((ch>='a'&&ch<='z')?ch-32:ch); }
HLE(msvcr90,iswspace){ uint16_t ch=(uint16_t)ARG(0); RET_C(ch==' '||(ch>=9&&ch<=13)); }

/* -------- misc -------- */
HLE(msvcr90,_time64){ int64_t t=(int64_t)time(0); if(ARG(0)){ W64(ARG(0),(uint64_t)t);} c->eax=(uint32_t)t; c->edx=(uint32_t)(t>>32); c->esp+=4; }
HLE(msvcr90,srand){ srand(ARG(0)); RET_C(0); }
HLE(msvcr90,rand){ RET_C((uint32_t)(rand()&0x7fff)); }
HLE(msvcr90,_CIpow){ double y=c->st[c->top], x=c->st[(c->top+1)&7]; c->top=(c->top+1)&7; c->st[c->top]=pow(x,y); c->esp+=4; }
/* qsort (v0.8.4f): a faithful port of the VC9 CRT quicksort (qsort.c: median-of-three, CUTOFF 8 selection "shortsort",
   explicit stack). The old version was an O(n^2) insertion sort that also passed a pointer into ONE static guest scratch
   buffer shared by all threads: two threads sorting at once (render thread + game thread) could overwrite each other's
   element mid-sort, which crashed the comparator f_007f0420 on the RenderingThread (null+0x34) at the "profit" scene.
   This version keeps no shared state, only ever passes pointers into the caller's array (as MSVC does), and produces
   MSVC's exact element order, so draw ordering matches the PC. */
static int q_cmp(CPU *c, uint32_t cmp, uint32_t a, uint32_t b){ PUSH32(c,b); PUSH32(c,a); guest_call(c,cmp); c->esp+=8; return (int32_t)c->eax; }
static void q_swap(uint32_t a, uint32_t b, uint32_t w){ if(a==b) return; uint8_t *p=MEMP(a), *q=MEMP(b); while(w--){ uint8_t t=*p; *p++=*q; *q++=t; } }
HLE(msvcr90,qsort){
  uint32_t base=ARG(0), num=ARG(1), w=ARG(2), cmp=ARG(3), ret=R32(c->esp); c->esp+=4;
  if(num>=2 && w){
    uint32_t lostk[64], histk[64]; int sp=0;
    uint32_t lo=base, hi=base+w*(num-1), mid, loguy, higuy;
    for(;;){
      uint32_t size=(hi-lo)/w+1;
      if(size<=8){                                   /* shortsort: selection sort, largest to the end */
        while(hi>lo){ uint32_t mx=lo; for(uint32_t p=lo+w;p<=hi;p+=w) if(q_cmp(c,cmp,p,mx)>0) mx=p; q_swap(mx,hi,w); hi-=w; }
      } else {
        mid=lo+(size/2)*w;
        if(q_cmp(c,cmp,lo,mid)>0) q_swap(lo,mid,w);
        if(q_cmp(c,cmp,lo,hi)>0) q_swap(lo,hi,w);
        if(q_cmp(c,cmp,mid,hi)>0) q_swap(mid,hi,w);
        loguy=lo; higuy=hi;
        for(;;){
          if(mid>loguy){ do loguy+=w; while(loguy<mid && q_cmp(c,cmp,loguy,mid)<=0); }
          if(mid<=loguy){ do loguy+=w; while(loguy<=hi && q_cmp(c,cmp,loguy,mid)<=0); }
          do higuy-=w; while(higuy>mid && q_cmp(c,cmp,higuy,mid)>0);
          if(higuy<loguy) break;
          q_swap(loguy,higuy,w);
          if(mid==higuy) mid=loguy;
        }
        higuy+=w;
        if(mid<higuy){ do higuy-=w; while(higuy>mid && q_cmp(c,cmp,higuy,mid)==0); }
        if(mid>=higuy){ do higuy-=w; while(higuy>lo && q_cmp(c,cmp,higuy,mid)==0); }
        if(higuy-lo >= hi-loguy){
          if(lo<higuy && sp<64){ lostk[sp]=lo; histk[sp]=higuy; sp++; }
          if(loguy<hi){ lo=loguy; continue; }
        } else {
          if(loguy<hi && sp<64){ lostk[sp]=loguy; histk[sp]=hi; sp++; }
          if(lo<higuy){ hi=higuy; continue; }
        }
      }
      if(--sp<0) break;
      lo=lostk[sp]; hi=histk[sp];
    }
  }
  c->esp-=4; W32(c->esp,ret); RET_C(0);
}
HLE(msvcr90,_vswprintf_c_l){ char*s=gformat(ARG(2),1,ARG(4)); uint32_t r=put_out(ARG(0),ARG(1),1,s); free(s); RET_C(r); }
HLE(msvcr90,_vswprintf_c){ char*s=gformat(ARG(2),1,ARG(3)); uint32_t r=put_out(ARG(0),ARG(1),1,s); free(s); RET_C(r); }
HLE(msvcr90,strcspn){ uint32_t s=ARG(0), r=ARG(1); uint32_t n=0; for(;;n++){ uint8_t ch=R8(s+n); if(!ch) break; int hit=0; for(uint32_t k=0;;k++){ uint8_t d=R8(r+k); if(!d) break; if(d==ch){ hit=1; break; } } if(hit) break; } RET_C(n); }
HLE(msvcr90,strspn){ uint32_t s=ARG(0), r=ARG(1); uint32_t n=0; for(;;n++){ uint8_t ch=R8(s+n); if(!ch) break; int hit=0; for(uint32_t k=0;;k++){ uint8_t d=R8(r+k); if(!d) break; if(d==ch){ hit=1; break; } } if(!hit) break; } RET_C(n); }
/* localeconv: "C" locale */
HLE(msvcr90,localeconv){ static uint32_t lc; if(!lc){ lc=gmalloc(128); memset(MEMP(lc),0,128); uint32_t dot=gmalloc(4), empty=gmalloc(4); W8(dot,'.'); W8(dot+1,0); W32(empty,0);
    W32(lc,dot); for(int k=1;k<10;k++) W32(lc+4*k,empty); for(int k=40;k<48;k++) W8(lc+k,127);
    uint32_t wdot=gmalloc(8); W16(wdot,'.'); W16(wdot+2,0); W32(lc+48,wdot); W32(lc+52,empty); W32(lc+56,empty); W32(lc+60,wdot); W32(lc+64,empty); W32(lc+68,empty); W32(lc+72,empty); W32(lc+76,empty); }
  RET_C(lc); }
