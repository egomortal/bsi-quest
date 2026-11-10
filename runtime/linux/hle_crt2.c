/* More CRT / kernel32 / user32 functions reached once the game gets past the main menu. */
#define _GNU_SOURCE
#include "hle.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
uint32_t gmalloc(uint32_t n); void gfree(uint32_t p);
char* gformat(uint32_t fmt, int wide, uint32_t va);
#define VA(n) (c->esp+4+4*(n))   /* address of the first variadic argument after n fixed args */

static int gstr(uint32_t p, char *o, int max){ int k=0; for(;k<max-1;k++){ uint8_t ch=R8(p+k); o[k]=ch; if(!ch) return k; } o[k]=0; return k; }
static int gwstr(uint32_t p, char *o, int max){ int k=0; for(;k<max-1;k++){ uint16_t ch=R16(p+2*k); o[k]=ch<128?(char)ch:'?'; if(!ch) return k; } o[k]=0; return k; }
static uint32_t out_str(uint32_t dst, uint32_t cap, int wide, const char *s){
  uint32_t n=strlen(s); if(!cap) return n; uint32_t m=n<cap-1?n:cap-1;
  for(uint32_t k=0;k<m;k++){ if(wide) W16(dst+2*k,(uint8_t)s[k]); else W8(dst+k,s[k]); }
  if(wide) W16(dst+2*m,0); else W8(dst+m,0); return n<cap?n:(uint32_t)-1; }

/* ---- number parsing ---- */
HLE(msvcr90,strtoul){ char t[128]; gstr(ARG(0),t,128); char *e; unsigned long v=strtoul(t,&e,(int)ARG(2)); if(ARG(1)) W32(ARG(1),ARG(0)+(uint32_t)(e-t)); RET_C((uint32_t)v); }
HLE(msvcr90,_strtoi64){ char t[128]; gstr(ARG(0),t,128); char *e; long long v=strtoll(t,&e,(int)ARG(2)); if(ARG(1)) W32(ARG(1),ARG(0)+(uint32_t)(e-t)); c->eax=(uint32_t)v; c->edx=(uint32_t)((unsigned long long)v>>32); c->esp+=4; }
HLE(msvcr90,_strtoui64){ char t[128]; gstr(ARG(0),t,128); char *e; unsigned long long v=strtoull(t,&e,(int)ARG(2)); if(ARG(1)) W32(ARG(1),ARG(0)+(uint32_t)(e-t)); c->eax=(uint32_t)v; c->edx=(uint32_t)(v>>32); c->esp+=4; }
HLE(msvcr90,_wcstoui64){ char t[128]; gwstr(ARG(0),t,128); char *e; unsigned long long v=strtoull(t,&e,(int)ARG(2)); if(ARG(1)) W32(ARG(1),ARG(0)+2*(uint32_t)(e-t)); c->eax=(uint32_t)v; c->edx=(uint32_t)(v>>32); c->esp+=4; }
HLE(msvcr90,_wtoi64){ char t[128]; gwstr(ARG(0),t,128); long long v=strtoll(t,0,10); c->eax=(uint32_t)v; c->edx=(uint32_t)((unsigned long long)v>>32); c->esp+=4; }
HLE(msvcr90,strtod){ char t[256]; gstr(ARG(0),t,256); char *e; double v=strtod(t,&e); if(ARG(1)) W32(ARG(1),ARG(0)+(uint32_t)(e-t)); c->top=(c->top-1)&7; c->st[c->top]=v; c->esp+=4; }
HLE(msvcr90,abs){ int32_t v=(int32_t)ARG(0); RET_C((uint32_t)(v<0?-v:v)); }
HLE(msvcr90,ldiv){ /* struct return via hidden pointer? MSVC returns 8-byte struct in edx:eax */ int32_t a=(int32_t)ARG(0), b=(int32_t)ARG(1); c->eax=b?(uint32_t)(a/b):0; c->edx=b?(uint32_t)(a%b):0; c->esp+=4; }
HLE(msvcr90,_itoa_s){ char t[40]; int32_t v=(int32_t)ARG(0); int r=(int)ARG(3); if(r==10) snprintf(t,40,"%d",v); else if(r==16) snprintf(t,40,"%x",(uint32_t)v); else if(r==8) snprintf(t,40,"%o",(uint32_t)v); else { uint32_t u=(uint32_t)v; char b[40]; int n=0; do{ b[n++]="0123456789abcdefghijklmnopqrstuvwxyz"[u%r]; u/=r; }while(u&&n<39); for(int k=0;k<n;k++) t[k]=b[n-1-k]; t[n]=0; }
  if(strlen(t)+1>ARG(2)) RET_C(34); out_str(ARG(1),ARG(2),0,t); RET_C(0); }
HLE(msvcr90,_fpclass){ double d; uint64_t u=R64(c->esp+4); memcpy(&d,&u,8); int r; switch(fpclassify(d)){ case FP_NAN: r=2; break; case FP_INFINITE: r=signbit(d)?4:0x200; break; case FP_ZERO: r=signbit(d)?0x20:0x40; break; case FP_SUBNORMAL: r=signbit(d)?0x10:0x80; break; default: r=signbit(d)?8:0x100; } RET_C(r); }

/* ---- strings / chars ---- */
HLE(msvcr90,_strdup){ uint32_t s=ARG(0); if(!s) RET_C(0); uint32_t n=0; while(R8(s+n)) n++; uint32_t d=gmalloc(n+1); memcpy(MEMP(d),MEMP(s),n+1); RET_C(d); }
HLE(msvcr90,memchr){ uint32_t p=ARG(0); uint8_t ch=(uint8_t)ARG(1); uint32_t n=ARG(2); for(uint32_t k=0;k<n;k++) if(R8(p+k)==ch) RET_C(p+k); RET_C(0); }
HLE(msvcr90,strpbrk){ uint32_t s=ARG(0), a=ARG(1); for(uint32_t k=0;;k++){ uint8_t ch=R8(s+k); if(!ch) RET_C(0); for(uint32_t j=0;;j++){ uint8_t d=R8(a+j); if(!d) break; if(d==ch) RET_C(s+k); } } }
HLE(msvcr90,isalnum){ RET_C(ARG(0)<256 && isalnum((int)ARG(0))?4:0); }
HLE(msvcr90,isalpha){ RET_C(ARG(0)<256 && isalpha((int)ARG(0))?2:0); }
HLE(msvcr90,iswpunct){ uint32_t ch=ARG(0)&0xffff; RET_C(ch<128 && ispunct((int)ch)?0x10:0); }
HLE(msvcr90,wctob){ uint32_t ch=ARG(0)&0xffff; RET_C(ch<128?ch:0xffffffffu); }
HLE(msvcr90,wcscoll){ uint32_t a=ARG(0), b=ARG(1); for(uint32_t k=0;;k++){ uint16_t x=R16(a+2*k), y=R16(b+2*k); if(x!=y) RET_C(x<y?(uint32_t)-1:1); if(!x) RET_C(0); } }
HLE(msvcr90,_wcsicoll){ uint32_t a=ARG(0), b=ARG(1); for(uint32_t k=0;;k++){ uint16_t x=R16(a+2*k), y=R16(b+2*k); if(x<128) x=tolower(x); if(y<128) y=tolower(y); if(x!=y) RET_C(x<y?(uint32_t)-1:1); if(!x) RET_C(0); } }
HLE(msvcr90,strerror){ static uint32_t b; if(!b) b=gmalloc(128); out_str(b,128,0,strerror((int)ARG(0))); RET_C(b); }
HLE(msvcr90,getenv){ RET_C(0); }
static __thread uint32_t t_errno_slot;
HLE(msvcr90,_errno){ if(!t_errno_slot){ t_errno_slot=gmalloc(4); W32(t_errno_slot,0); } RET_C(t_errno_slot); }
HLE(msvcr90,_getcwd){ const char *cw="C:\\BSI\\Binaries\\Win32"; uint32_t b=ARG(0), n=ARG(1); if(!b){ n=strlen(cw)+1; b=gmalloc(n); } out_str(b,n,0,cw); RET_C(b); }
HLE(msvcr90,_aligned_malloc){ uint32_t n=ARG(0), al=ARG(1); if(al<8) al=8; uint32_t raw=gmalloc(n+al+4); uint32_t p=(raw+4+al-1)&~(al-1); W32(p-4,raw); RET_C(p); }
HLE(msvcr90,_aligned_free){ uint32_t p=ARG(0); if(p) gfree(R32(p-4)); RET_C(0); }
HLE(msvcr90,abort){ fprintf(stderr,"guest abort()\n"); fflush(0); _exit(3); }

/* ---- printf family ---- */
HLE(msvcr90,_snprintf){ char*s=gformat(ARG(2),0,VA(3)); uint32_t r=out_str(ARG(0),ARG(1)+1,0,s); if(strlen(s)>ARG(1)) r=(uint32_t)-1; free(s); RET_C(r); }
HLE(msvcr90,_snprintf_s){ char*s=gformat(ARG(3),0,VA(4)); uint32_t cap=ARG(1), cnt=ARG(2); if(cnt!=0xffffffffu && cnt+1<cap) cap=cnt+1; uint32_t r=out_str(ARG(0),cap,0,s); free(s); RET_C(r); }
HLE(msvcr90,_snwprintf_s){ char*s=gformat(ARG(3),1,VA(4)); uint32_t cap=ARG(1), cnt=ARG(2); if(cnt!=0xffffffffu && cnt+1<cap) cap=cnt+1; uint32_t r=out_str(ARG(0),cap,1,s); free(s); RET_C(r); }
HLE(msvcr90,sprintf){ char*s=gformat(ARG(1),0,VA(2)); uint32_t r=out_str(ARG(0),0x7fffffff,0,s); free(s); RET_C(r); }
HLE(msvcr90,vsprintf){ char*s=gformat(ARG(1),0,ARG(2)); uint32_t r=out_str(ARG(0),0x7fffffff,0,s); free(s); RET_C(r); }
HLE(msvcr90,printf){ char*s=gformat(ARG(0),0,VA(1)); fputs(s,stderr); uint32_t n=strlen(s); free(s); RET_C(n); }
HLE(msvcr90,printf_s){ char*s=gformat(ARG(0),0,VA(1)); fputs(s,stderr); uint32_t n=strlen(s); free(s); RET_C(n); }
HLE(msvcr90,vprintf){ char*s=gformat(ARG(0),0,ARG(1)); fputs(s,stderr); uint32_t n=strlen(s); free(s); RET_C(n); }
HLE(msvcr90,wprintf){ char*s=gformat(ARG(0),1,VA(1)); fputs(s,stderr); uint32_t n=strlen(s); free(s); RET_C(n); }
HLE(msvcr90,wprintf_s){ char*s=gformat(ARG(0),1,VA(1)); fputs(s,stderr); uint32_t n=strlen(s); free(s); RET_C(n); }

/* ---- scanf (subset: %d %i %u %x %X %f %g %e %s %c %n with h/l/ll/I64 sizes) ---- */
static int gscan(const char *in, uint32_t fmt, int wide, uint32_t va, int secure){
  const char *p=in; int n=0; uint32_t f=fmt;
  #define FCH(i) (wide? (char)R16(f+2*(i)) : (char)R8(f+(i)))
  int i=0;
  for(;;){ char ch=FCH(i); if(!ch) break;
    if(isspace((unsigned char)ch)){ while(isspace((unsigned char)*p)) p++; i++; continue; }
    if(ch!='%'){ if(*p!=ch) break; p++; i++; continue; }
    i++; int skip=0, width=0, sz=0; if(FCH(i)=='*'){ skip=1; i++; }
    while(isdigit((unsigned char)FCH(i))){ width=width*10+(FCH(i)-'0'); i++; }
    for(;;){ char m=FCH(i); if(m=='h'){ sz=-1; i++; } else if(m=='l'){ sz=sz==1?2:1; i++; } else if(m=='I'&&FCH(i+1)=='6'&&FCH(i+2)=='4'){ sz=2; i+=3; } else if(m=='L'){ sz=2; i++; } else break; }
    char cv=FCH(i++); if(!cv) break;
    if(cv=='%'){ while(isspace((unsigned char)*p)) p++; if(*p!='%') break; p++; continue; }
    if(cv=='n'){ if(!skip){ W32(R32(va),(uint32_t)(p-in)); va+=4; } continue; }
    if(cv!='c') while(isspace((unsigned char)*p)) p++;
    if(!*p) break;
    char buf[512]; int bl=width?width:511; if(bl>511) bl=511;
    if(cv=='d'||cv=='i'||cv=='u'||cv=='x'||cv=='X'||cv=='o'){ int base= cv=='x'||cv=='X'?16: cv=='o'?8: cv=='i'?0:10;
      strncpy(buf,p,bl); buf[bl]=0; char *e; long long v= cv=='u'||cv=='x'||cv=='X'||cv=='o' ? (long long)strtoull(buf,&e,base) : strtoll(buf,&e,base);
      if(e==buf) break; p+=e-buf; if(!skip){ uint32_t d=R32(va); va+=4; if(sz==2) W64(d,(uint64_t)v); else if(sz==-1) W16(d,(uint16_t)v); else W32(d,(uint32_t)v); n++; } continue; }
    if(cv=='f'||cv=='g'||cv=='e'||cv=='E'||cv=='G'){ strncpy(buf,p,bl); buf[bl]=0; char *e; double v=strtod(buf,&e); if(e==buf) break; p+=e-buf;
      if(!skip){ uint32_t d=R32(va); va+=4; if(sz>=1){ uint64_t u; memcpy(&u,&v,8); W64(d,u); } else { float fv=(float)v; uint32_t u; memcpy(&u,&fv,4); W32(d,u); } n++; } continue; }
    if(cv=='s'||cv=='c'){ int cnt= cv=='c' ? (width?width:1) : 0; uint32_t d=skip?0:R32(va); uint32_t cap=0xffffffffu; if(!skip){ va+=4; if(secure){ cap=R32(va); va+=4; } }
      int k=0; if(cv=='c'){ for(;k<cnt && *p;k++,p++) if(d && (uint32_t)k<cap){ if(wide) W16(d+2*k,(uint8_t)*p); else W8(d+k,*p); } }
      else { while(*p && !isspace((unsigned char)*p) && (!width||k<width)){ if(d && (uint32_t)k+1<cap){ if(wide) W16(d+2*k,(uint8_t)*p); else W8(d+k,*p); } k++; p++; } if(d){ if(wide) W16(d+2*k,0); else W8(d+k,0); } }
      if(!skip) n++; continue; }
    break; }
  #undef FCH
  return n; }
HLE(msvcr90,sscanf){ char in[2048]; gstr(ARG(0),in,2048); RET_C((uint32_t)gscan(in,ARG(1),0,VA(2),0)); }
HLE(msvcr90,swscanf_s){ char in[2048]; gwstr(ARG(0),in,2048); RET_C((uint32_t)gscan(in,ARG(1),1,VA(2),1)); }

/* ---- kernel32 time ---- */
static void st_from_tm(uint32_t p, const struct tm *t, int ms){ W16(p,t->tm_year+1900); W16(p+2,t->tm_mon+1); W16(p+4,t->tm_wday); W16(p+6,t->tm_mday); W16(p+8,t->tm_hour); W16(p+10,t->tm_min); W16(p+12,t->tm_sec); W16(p+14,ms); }
static uint64_t ft_from_unix(double t){ return (uint64_t)((t+11644473600.0)*1e7); }
static double unix_from_ft(uint64_t ft){ return ft/1e7-11644473600.0; }
HLE(kernel32,GetSystemTime){ struct timespec ts; clock_gettime(CLOCK_REALTIME,&ts); struct tm tm; gmtime_r(&ts.tv_sec,&tm); st_from_tm(ARG(0),&tm,ts.tv_nsec/1000000); RET_STD(1,0); }
HLE(kernel32,FileTimeToSystemTime){ uint64_t ft=R64(ARG(0)); double u=unix_from_ft(ft); time_t s=(time_t)u; struct tm tm; gmtime_r(&s,&tm); st_from_tm(ARG(1),&tm,(int)((u-s)*1000)); RET_STD(2,1); }
HLE(kernel32,SystemTimeToFileTime){ uint32_t p=ARG(0); struct tm tm; memset(&tm,0,sizeof tm); tm.tm_year=R16(p)-1900; tm.tm_mon=R16(p+2)-1; tm.tm_mday=R16(p+6); tm.tm_hour=R16(p+8); tm.tm_min=R16(p+10); tm.tm_sec=R16(p+12); time_t s=timegm(&tm); W64(ARG(1),ft_from_unix((double)s+R16(p+14)/1000.0)); RET_STD(2,1); }
HLE(kernel32,FileTimeToLocalFileTime){ W64(ARG(1),R64(ARG(0))); RET_STD(2,1); }
HLE(kernel32,LocalFileTimeToFileTime){ W64(ARG(1),R64(ARG(0))); RET_STD(2,1); }
HLE(kernel32,SleepEx){ usleep((useconds_t)ARG(0)*1000); RET_STD(2,0); }
HLE(kernel32,SetErrorMode){ RET_STD(1,0); }
HLE(kernel32,VerifyVersionInfoA){ RET_STD(4,1); }
HLE(kernel32,VerifyVersionInfoW){ RET_STD(4,1); }
HLE(kernel32,VerSetConditionMask){ c->eax=R32(c->esp+4); c->edx=R32(c->esp+8); c->esp+=4+16; }
HLE(kernel32,RtlCaptureStackBackTrace){ RET_STD(4,0); }
HLE(kernel32,GlobalAlloc){ uint32_t p=gmalloc(ARG(1)?ARG(1):1); if(ARG(0)&0x40) memset(MEMP(p),0,ARG(1)); RET_STD(2,p); }
HLE(kernel32,GlobalLock){ RET_STD(1,ARG(0)); }
HLE(kernel32,GlobalUnlock){ RET_STD(1,1); }
HLE(kernel32,DebugBreak){ RET_STD(0,0); }
HLE(kernel32,SuspendThread){ RET_STD(1,0); }
HLE(kernel32,GetStdHandle){ RET_STD(1,0xfffffff0u+(ARG(0)&0xf)); }
HLE(kernel32,UnhandledExceptionFilter){ RET_STD(1,1); }
/* ---- user32 odds and ends ---- */
HLE(user32,GetWindowThreadProcessId){ if(ARG(1)) W32(ARG(1),0x1000); RET_STD(2,0x1004); }
HLE(user32,MapWindowPoints){ RET_STD(4,0); }
HLE(user32,GetClipCursor){ uint32_t p=ARG(0); W32(p,0); W32(p+4,0); W32(p+8,1920); W32(p+12,1080); RET_STD(1,1); }
HLE(user32,SendInput){ RET_STD(3,ARG(0)); }
HLE(user32,GetWindowPlacement){ uint32_t p=ARG(1); memset(MEMP(p),0,44); W32(p,44); W32(p+8,1); W32(p+36,1920); W32(p+40,1080); RET_STD(2,1); }
HLE(user32,SetWindowPlacement){ RET_STD(2,1); }
HLE(user32,PostThreadMessageW){ RET_STD(4,1); }
HLE(user32,GetKeyboardLayoutNameW){ uint32_t p=ARG(0); const char *s="00000409"; for(int k=0;k<9;k++) W16(p+2*k,(uint16_t)s[k]); RET_STD(1,1); }
HLE(user32,RegisterDeviceNotificationW){ RET_STD(3,0x5001); }
HLE(user32,UnregisterDeviceNotification){ RET_STD(1,1); }
HLE(user32,IsWindowEnabled){ RET_STD(1,1); }
HLE(user32,EnableWindow){ RET_STD(2,0); }
HLE(user32,ValidateRect){ RET_STD(2,1); }
HLE(user32,GetCursorInfo){ uint32_t p=ARG(0); W32(p+4,1); W32(p+8,0x7e0002); W32(p+12,960); W32(p+16,540); RET_STD(1,1); }
HLE(user32,SetWindowsHookExW){ RET_STD(4,0x6001); }
HLE(user32,UnhookWindowsHookEx){ RET_STD(1,1); }
HLE(user32,CallNextHookEx){ RET_STD(4,0); }
HLE(gdi32,CreateFontIndirectW){ RET_STD(1,0x3101); }
HLE(gdi32,SetBkColor){ RET_STD(2,0); }
