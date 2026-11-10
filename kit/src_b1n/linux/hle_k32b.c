/* kernel32 functions used by the statically linked CRTs inside the game's DLLs (PhysX/APEX/Bink/nvtt) */
#include "hle.h"
#include <ctype.h>
#include <unistd.h>
HLE(kernel32,CreateFileMappingA){ RET_STD(6,0); }
HLE(kernel32,MapViewOfFile){ RET_STD(5,0); }
HLE(kernel32,UnmapViewOfFile){ RET_STD(1,1); }
HLE(kernel32,RtlUnwind){ fprintf(stderr,"[k32] RtlUnwind called (SEH unwind not supported)\n"); RET_STD(4,0); }
HLE(kernel32,SetHandleCount){ RET_STD(1,ARG(0)); }
HLE(kernel32,GetFileType){ RET_STD(1,2); }
static uint32_t envblk(int wide){ static uint32_t a,w; uint32_t *p=wide?&w:&a; if(!*p){ *p=gmalloc(8); memset(MEMP(*p),0,8);} return *p; }
HLE(kernel32,GetEnvironmentStrings){ RET_STD(0,envblk(0)); }
HLE(kernel32,GetEnvironmentStringsA){ RET_STD(0,envblk(0)); }
HLE(kernel32,GetEnvironmentStringsW){ RET_STD(0,envblk(1)); }
HLE(kernel32,FreeEnvironmentStringsA){ RET_STD(1,1); }
HLE(kernel32,FreeEnvironmentStringsW){ RET_STD(1,1); }
static uint32_t next_heap=0x00160000;
HLE(kernel32,HeapCreate){ uint32_t h=next_heap; next_heap+=0x10000; RET_STD(3,h); }
HLE(kernel32,HeapDestroy){ RET_STD(1,1); }
HLE(kernel32,VirtualQuery){ uint32_t a=ARG(0), m=ARG(1); if(!m||ARG(2)<28) RET_STD(3,0); memset(MEMP(m),0,28);
  W32(m,a&~0xfffu); W32(m+4,a&~0xffffu); W32(m+8,4); W32(m+12,0x1000); W32(m+16,0x1000); W32(m+20,4); W32(m+24,0x20000); RET_STD(3,28); }
HLE(kernel32,FatalAppExitA){ char t[512]; g2host_a(ARG(1),t,512); fprintf(stderr,"FatalAppExit: %s\n",t); exit(13); }
HLE(kernel32,GetCPInfo){ uint32_t p=ARG(1); memset(MEMP(p),0,20); W32(p,1); W8(p+4,'?'); RET_STD(2,1); }
HLE(kernel32,GetACP){ RET_STD(0,1252); }
HLE(kernel32,GetOEMCP){ RET_STD(0,437); }
HLE(kernel32,IsValidCodePage){ RET_STD(1,1); }
HLE(kernel32,GetLocaleInfoA){ RET_STD(4,0); }
HLE(kernel32,GetConsoleCP){ RET_STD(0,437); }
HLE(kernel32,GetConsoleOutputCP){ RET_STD(0,437); }
HLE(kernel32,GetConsoleMode){ RET_STD(2,0); }
HLE(kernel32,WriteConsoleA){ uint32_t n=ARG(2); if(n) { ssize_t r=write(2,MEMP(ARG(1)),n); (void)r; } if(ARG(3)) W32(ARG(3),n); RET_STD(5,1); }
HLE(kernel32,WriteConsoleW){ uint32_t n=ARG(2); for(uint32_t k=0;k<n;k++) fputc(R16(ARG(1)+2*k)&0x7f,stderr); if(ARG(3)) W32(ARG(3),n); RET_STD(5,1); }
HLE(kernel32,GetTimeFormatA){ RET_STD(6,0); }
HLE(kernel32,GetDateFormatA){ RET_STD(6,0); }
HLE(kernel32,EnumSystemLocalesA){ RET_STD(2,1); }
HLE(kernel32,IsValidLocale){ RET_STD(2,1); }
static uint16_t ctype1(unsigned ch){ uint16_t t=0; if(ch<128){ if(isupper(ch)) t|=1; if(islower(ch)) t|=2; if(isdigit(ch)) t|=4; if(isspace(ch)) t|=8;
  if(ispunct(ch)) t|=0x10; if(iscntrl(ch)) t|=0x20; if(ch==' '||ch=='\t') t|=0x40; if(isxdigit(ch)) t|=0x80; if(isalpha(ch)) t|=0x100; } else t=0x100|2; return t; }
HLE(kernel32,GetStringTypeA){ uint32_t src=ARG(2), out=ARG(4); int n=(int)ARG(3); if(n<0) n=strlen((char*)MEMP(src))+1; for(int k=0;k<n;k++) W16(out+2*k,ctype1(R8(src+k))); RET_STD(5,1); }
HLE(kernel32,GetStringTypeW){ uint32_t src=ARG(1), out=ARG(3); int n=(int)ARG(2); if(n<0){ n=0; while(R16(src+2*n)) n++; n++; } for(int k=0;k<n;k++) W16(out+2*k,ctype1(R16(src+2*k))); RET_STD(4,1); }
HLE(kernel32,LCMapStringA){ uint32_t fl=ARG(1), src=ARG(2), dst=ARG(4); int n=(int)ARG(3), dn=(int)ARG(5); if(n<0) n=strlen((char*)MEMP(src))+1;
  if(!dn) RET_STD(6,n); if(dn<n) n=dn; for(int k=0;k<n;k++){ int ch=R8(src+k); if(fl&0x100) ch=tolower(ch); if(fl&0x200) ch=toupper(ch); W8(dst+k,ch);} RET_STD(6,n); }
HLE(kernel32,LCMapStringW){ uint32_t fl=ARG(1), src=ARG(2), dst=ARG(4); int n=(int)ARG(3), dn=(int)ARG(5); if(n<0){ n=0; while(R16(src+2*n)) n++; n++; }
  if(!dn) RET_STD(6,n); if(dn<n) n=dn; for(int k=0;k<n;k++){ int ch=R16(src+2*k); if(ch<128){ if(fl&0x100) ch=tolower(ch); if(fl&0x200) ch=toupper(ch);} W16(dst+2*k,ch);} RET_STD(6,n); }
HLE(kernel32,GetTimeZoneInformation){ memset(MEMP(ARG(0)),0,172); RET_STD(1,0); }
static int cmpgen(uint32_t a,int na,uint32_t b,int nb,int w,int ic){
  if(na<0){ na=0; while(w?R16(a+2*na):R8(a+na)) na++; } if(nb<0){ nb=0; while(w?R16(b+2*nb):R8(b+nb)) nb++; }
  for(int k=0;;k++){ if(k==na||k==nb) return na==nb?2:(k==na?1:3); int x=w?R16(a+2*k):R8(a+k), y=w?R16(b+2*k):R8(b+k);
    if(ic && x<128 && y<128){ x=tolower(x); y=tolower(y);} if(x!=y) return x<y?1:3; } }
HLE(kernel32,CompareStringA){ RET_STD(6,cmpgen(ARG(2),(int)ARG(3),ARG(4),(int)ARG(5),0,ARG(1)&1)); }
HLE(kernel32,CompareStringW){ RET_STD(6,cmpgen(ARG(2),(int)ARG(3),ARG(4),(int)ARG(5),1,ARG(1)&1)); }
HLE(kernel32,SetEnvironmentVariableA){ RET_STD(2,1); }
HLE(kernel32,EncodePointer){ RET_STD(1,ARG(0)); }
HLE(kernel32,DecodePointer){ RET_STD(1,ARG(0)); }
HLE(kernel32,IsProcessorFeaturePresent){ uint32_t f=ARG(0); RET_STD(1, (f==6||f==10||f==3||f==7||f==8||f==13)?1:0); }
/* used by binkw32 / nvtt */
HLE(kernel32,DisableThreadLibraryCalls){ RET_STD(1,1); }
static uint32_t put_a(uint32_t d, uint32_t max, const char *s){ uint32_t n=strlen(s); if(n+1>max) return n+1; memcpy(MEMP(d),s,n+1); return n; }
HLE(kernel32,GetSystemDirectoryA){ RET_STD(2,put_a(ARG(0),ARG(1),"C:\\Windows\\System32")); }
HLE(kernel32,GetWindowsDirectoryA){ RET_STD(2,put_a(ARG(0),ARG(1),"C:\\Windows")); }
HLE(gdi32,CreateCompatibleBitmap){ RET_STD(3,0); }
HLE(gdi32,CreateDIBSection){ RET_STD(6,0); }
HLE(gdi32,GetDIBits){ RET_STD(7,0); }
HLE(gdi32,GetPixel){ RET_STD(3,0); }
HLE(gdi32,SetPixel){ RET_STD(4,0); }
HLE(gdi32,SetStretchBltMode){ RET_STD(2,1); }
HLE(user32,ChangeDisplaySettingsA){ RET_STD(2,0); }
HLE(user32,GetClassLongA){ RET_STD(2,0); }
HLE(user32,GetTopWindow){ RET_STD(1,0); }
HLE(user32,GetWindow){ RET_STD(2,0); }
HLE(user32,RegisterClassA){ RET_STD(1,0xc001); }
HLE(winmm,waveOutOpen){ RET_STD(6,6); }   /* MMSYSERR_NODRIVER */
HLE(winmm,waveOutClose){ RET_STD(1,0); }
HLE(winmm,waveOutPause){ RET_STD(1,0); }
HLE(winmm,waveOutPrepareHeader){ RET_STD(3,0); }
HLE(winmm,waveOutReset){ RET_STD(1,0); }
HLE(winmm,waveOutRestart){ RET_STD(1,0); }
HLE(winmm,waveOutSetVolume){ RET_STD(2,0); }
HLE(winmm,waveOutUnprepareHeader){ RET_STD(3,0); }
HLE(winmm,waveOutWrite){ RET_STD(3,0); }
/* ---- MSVCR80 extras (nvtt.dll's CRT; MSVCR80 imports resolve to the MSVCR90 set) */
uint32_t gmalloc(uint32_t n); void gfree(uint32_t p);
uint32_t gmalloc_c(uint32_t n, uint32_t caller);
HLE(msvcr90,_malloc_crt){ RET_C(gmalloc_c(ARG(0),R32(c->esp))); }
HLE(msvcr90,_encoded_null){ RET_C(0); }
HLE(msvcr90,_lock){ RET_C(0); }
HLE(msvcr90,_unlock){ RET_C(0); }
HLE(msvcr90,__CppXcptFilter){ RET_C(0); }
HLE(msvcr90,_except_handler4_common){ RET_C(1); }
HLE(msvcr90,__clean_type_info_names_internal){ RET_C(0); }
HLE(msvcr90,_purecall){ fprintf(stderr,"pure virtual call (guest; called from %08x)\n", R32(c->esp)); { void guest_stackscan(CPU*c, const char*why); guest_stackscan(c,"purecall"); } fflush(0); _exit(16); }   /* b73: guest stack + _exit (no static destructors racing the render/XR threads) */
HLE(msvcr90,_crt_debugger_hook){ RET_C(0); }
void hle_msvcr90___type_info_dtor_internal_method_type_info__QAEXXZ(CPU*c){ c->esp+=4; }
void hle_msvcr90___1exception_std__UAE_XZ(CPU*c){ c->esp+=4; }
void hle_msvcr90___0exception_std__QAE_XZ(CPU*c){ W32(c->ecx,0); W32(c->ecx+4,0); W32(c->ecx+8,0); c->eax=c->ecx; c->esp+=4; }
void hle_msvcr90___0exception_std__QAE_ABV01__Z(CPU*c){ memcpy(MEMP(c->ecx),MEMP(ARG(0)),12); c->eax=c->ecx; c->esp+=8; }
void hle_msvcr90__terminate__YAXXZ(CPU*c){ fprintf(stderr,"std::terminate\n"); exit(17); }
HLE(msvcr90,fopen_s){ if(ARG(0)) W32(ARG(0),0); RET_C(2); }
HLE(msvcr90,fclose){ RET_C(0); }
HLE(msvcr90,fseek){ RET_C(-1); }
HLE(msvcr90,ftell){ RET_C(-1); }
HLE(msvcr90,fwrite){ RET_C(0); }
HLE(msvcr90,ferror){ RET_C(1); }
HLE(msvcr90,feof){ RET_C(1); }
HLE(msvcr90,clearerr){ RET_C(0); }
/* MSVCP80 std::string (VC8 release layout: +4 buf[16]/ptr, +20 size, +24 capacity) */
static void str_assign(uint32_t s, uint32_t src, uint32_t n){ W32(s+24,15); W32(s+20,0);
  if(n<16){ memcpy(MEMP(s+4),MEMP(src),n); W8(s+4+n,0); } else { uint32_t p=gmalloc(n+1); memcpy(MEMP(p),MEMP(src),n); W8(p+n,0); W32(s+4,p); W32(s+24,n); }
  W32(s+20,n); }
void hle_msvcp80___0__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_PBD_Z(CPU*c){ uint32_t src=ARG(0); str_assign(c->ecx,src,strlen((char*)MEMP(src))); c->eax=c->ecx; c->esp+=8; }
void hle_msvcp80___0__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_ABV01__Z(CPU*c){ uint32_t o=ARG(0); uint32_t n=R32(o+20); uint32_t src=R32(o+24)>=16?R32(o+4):o+4; str_assign(c->ecx,src,n); c->eax=c->ecx; c->esp+=8; }
void hle_msvcp80___1__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_XZ(CPU*c){ if(R32(c->ecx+24)>=16) gfree(R32(c->ecx+4)); c->esp+=4; }
/* ---- more imports hit at runtime */
#include <stdlib.h>
HLE(kernel32,InterlockedCompareExchange64){ uint32_t p=ARG(0); uint64_t ex=(uint64_t)ARG(1)|((uint64_t)ARG(2)<<32), cmp=(uint64_t)ARG(3)|((uint64_t)ARG(4)<<32);
  uint64_t exp=cmp; __atomic_compare_exchange_n((uint64_t*)MEMP(p),&exp,ex,0,__ATOMIC_SEQ_CST,__ATOMIC_SEQ_CST);
  c->edx=(uint32_t)(exp>>32); RET_STD(5,(uint32_t)exp); }
HLE(msvcr90,isdigit){ RET_C(ARG(0)<256 && isdigit((int)ARG(0))); }
HLE(msvcr90,isspace){ RET_C(ARG(0)<256 && isspace((int)ARG(0))); }
HLE(msvcr90,isxdigit){ RET_C(ARG(0)<256 && isxdigit((int)ARG(0))); }
HLE(msvcr90,strtol){ char t[128]; int k=0; for(;k<127;k++){ uint8_t ch=R8(ARG(0)+k); t[k]=ch; if(!ch) break; } t[127]=0; char *e; long v=strtol(t,&e,(int)ARG(2)); if(ARG(1)) W32(ARG(1),ARG(0)+(uint32_t)(e-t)); RET_C((uint32_t)v); }
char* gformat(uint32_t fmt, int wide, uint32_t va);
HLE(msvcr90,sprintf_s){ char *s=gformat(ARG(2),0,c->esp+16); uint32_t cap=ARG(1), n=strlen(s); if(n>=cap) n=cap?cap-1:0; memcpy(MEMP(ARG(0)),s,n); if(cap) W8(ARG(0)+n,0); free(s); RET_C(n); }
HLE(msvcr90,__iob_func){ static uint32_t iob; if(!iob){ iob=gmalloc(32*3); memset(MEMP(iob),0,96);} RET_C(iob); }
HLE(kernel32,GetCurrentDirectoryA){ RET_STD(2,put_a(ARG(1),ARG(0),"C:\\BSI\\Binaries\\Win32")); }
HLE(ole32,CoCreateGuid){ for(int k=0;k<16;k++) W8(ARG(0)+k,rand()&0xff); RET_STD(1,0); }
/* D3DXMatrixInverse(D3DXMATRIX *out, float *det, const D3DXMATRIX *m) */
HLE(d3dx9_43,D3DXMatrixInverse){ float m[16], inv[16]; memcpy(m,MEMP(ARG(2)),64);
  inv[0]=m[5]*m[10]*m[15]-m[5]*m[11]*m[14]-m[9]*m[6]*m[15]+m[9]*m[7]*m[14]+m[13]*m[6]*m[11]-m[13]*m[7]*m[10];
  inv[4]=-m[4]*m[10]*m[15]+m[4]*m[11]*m[14]+m[8]*m[6]*m[15]-m[8]*m[7]*m[14]-m[12]*m[6]*m[11]+m[12]*m[7]*m[10];
  inv[8]=m[4]*m[9]*m[15]-m[4]*m[11]*m[13]-m[8]*m[5]*m[15]+m[8]*m[7]*m[13]+m[12]*m[5]*m[11]-m[12]*m[7]*m[9];
  inv[12]=-m[4]*m[9]*m[14]+m[4]*m[10]*m[13]+m[8]*m[5]*m[14]-m[8]*m[6]*m[13]-m[12]*m[5]*m[10]+m[12]*m[6]*m[9];
  inv[1]=-m[1]*m[10]*m[15]+m[1]*m[11]*m[14]+m[9]*m[2]*m[15]-m[9]*m[3]*m[14]-m[13]*m[2]*m[11]+m[13]*m[3]*m[10];
  inv[5]=m[0]*m[10]*m[15]-m[0]*m[11]*m[14]-m[8]*m[2]*m[15]+m[8]*m[3]*m[14]+m[12]*m[2]*m[11]-m[12]*m[3]*m[10];
  inv[9]=-m[0]*m[9]*m[15]+m[0]*m[11]*m[13]+m[8]*m[1]*m[15]-m[8]*m[3]*m[13]-m[12]*m[1]*m[11]+m[12]*m[3]*m[9];
  inv[13]=m[0]*m[9]*m[14]-m[0]*m[10]*m[13]-m[8]*m[1]*m[14]+m[8]*m[2]*m[13]+m[12]*m[1]*m[10]-m[12]*m[2]*m[9];
  inv[2]=m[1]*m[6]*m[15]-m[1]*m[7]*m[14]-m[5]*m[2]*m[15]+m[5]*m[3]*m[14]+m[13]*m[2]*m[7]-m[13]*m[3]*m[6];
  inv[6]=-m[0]*m[6]*m[15]+m[0]*m[7]*m[14]+m[4]*m[2]*m[15]-m[4]*m[3]*m[14]-m[12]*m[2]*m[7]+m[12]*m[3]*m[6];
  inv[10]=m[0]*m[5]*m[15]-m[0]*m[7]*m[13]-m[4]*m[1]*m[15]+m[4]*m[3]*m[13]+m[12]*m[1]*m[7]-m[12]*m[3]*m[5];
  inv[14]=-m[0]*m[5]*m[14]+m[0]*m[6]*m[13]+m[4]*m[1]*m[14]-m[4]*m[2]*m[13]-m[12]*m[1]*m[6]+m[12]*m[2]*m[5];
  inv[3]=-m[1]*m[6]*m[11]+m[1]*m[7]*m[10]+m[5]*m[2]*m[11]-m[5]*m[3]*m[10]-m[9]*m[2]*m[7]+m[9]*m[3]*m[6];
  inv[7]=m[0]*m[6]*m[11]-m[0]*m[7]*m[10]-m[4]*m[2]*m[11]+m[4]*m[3]*m[10]+m[8]*m[2]*m[7]-m[8]*m[3]*m[6];
  inv[11]=-m[0]*m[5]*m[11]+m[0]*m[7]*m[9]+m[4]*m[1]*m[11]-m[4]*m[3]*m[9]-m[8]*m[1]*m[7]+m[8]*m[3]*m[5];
  inv[15]=m[0]*m[5]*m[10]-m[0]*m[6]*m[9]-m[4]*m[1]*m[10]+m[4]*m[2]*m[9]+m[8]*m[1]*m[6]-m[8]*m[2]*m[5];
  float det=m[0]*inv[0]+m[1]*inv[4]+m[2]*inv[8]+m[3]*inv[12];
  if(ARG(1)) memcpy(MEMP(ARG(1)),&det,4);
  if(det==0) RET_STD(3,0);
  for(int k=0;k<16;k++) inv[k]/=det; memcpy(MEMP(ARG(0)),inv,64); RET_STD(3,ARG(0)); }

/* ---- MSVCP90 std::string (same VC8/VC9 release layout as above) */
static uint32_t s_ptr(uint32_t s){ return R32(s+24)>=16? R32(s+4) : s+4; }
static void s_set(uint32_t s, const char *src, uint32_t n){ if(R32(s+24)>=16) gfree(R32(s+4));
  W32(s+24,15); if(n<16){ memcpy(MEMP(s+4),src,n); W8(s+4+n,0); } else { uint32_t p=gmalloc(n+1); memcpy(MEMP(p),src,n); W8(p+n,0); W32(s+4,p); W32(s+24,n); } W32(s+20,n); }
static void s_init(uint32_t s){ W32(s+24,15); W32(s+20,0); W8(s+4,0); }
void hle_msvcp90___0__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_PBD_Z(CPU*c){ uint32_t src=ARG(0); s_init(c->ecx); s_set(c->ecx,(char*)MEMP(src),strlen((char*)MEMP(src))); c->eax=c->ecx; c->esp+=8; }
void hle_msvcp90___0__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_ABV01__Z(CPU*c){ uint32_t o=ARG(0); char *t=strndup((char*)MEMP(s_ptr(o)),R32(o+20)); s_init(c->ecx); s_set(c->ecx,t,R32(o+20)); free(t); c->eax=c->ecx; c->esp+=8; }
void hle_msvcp90___1__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAE_XZ(CPU*c){ if(R32(c->ecx+24)>=16) gfree(R32(c->ecx+4)); s_init(c->ecx); c->esp+=4; }
void hle_msvcp90___4__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAEAAV01_ABV01__Z(CPU*c){ uint32_t o=ARG(0); if(o!=c->ecx){ char *t=strndup((char*)MEMP(s_ptr(o)),R32(o+20)); s_set(c->ecx,t,R32(o+20)); free(t); } c->eax=c->ecx; c->esp+=8; }
void hle_msvcp90___Y__basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAEAAV01_PBD_Z(CPU*c){ uint32_t s=c->ecx; uint32_t n0=R32(s+20); const char *a=(char*)MEMP(ARG(0)); uint32_t na=strlen(a);
  char *t=malloc(n0+na+1); memcpy(t,MEMP(s_ptr(s)),n0); memcpy(t+n0,a,na); s_set(s,t,n0+na); free(t); c->eax=s; c->esp+=8; }
void hle_msvcp90__erase___basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QAEAAV12_II_Z(CPU*c){ uint32_t s=c->ecx, pos=ARG(0), cnt=ARG(1), n=R32(s+20); if(pos>n) pos=n; if(cnt>n-pos) cnt=n-pos;
  char *t=malloc(n+1); memcpy(t,MEMP(s_ptr(s)),n); memmove(t+pos,t+pos+cnt,n-pos-cnt); s_set(s,t,n-cnt); free(t); c->eax=s; c->esp+=12; }
void hle_msvcp90__find___basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QBEIPBDII_Z(CPU*c){ uint32_t s=c->ecx, pat=ARG(0), pos=ARG(1), pn=ARG(2), n=R32(s+20); const char *h=(char*)MEMP(s_ptr(s)); const char *p=(char*)MEMP(pat);
  uint32_t r=0xffffffffu; if(pn==0 && pos<=n) r=pos; else for(uint32_t k=pos;k+pn<=n;k++) if(!memcmp(h+k,p,pn)){ r=k; break; }
  c->eax=r; c->esp+=16; }
void hle_msvcp90__substr___basic_string_DU__char_traits_D_std__V__allocator_D_2__std__QBE_AV12_II_Z(CPU*c){ uint32_t s=c->ecx, ret=ARG(0), pos=ARG(1), cnt=ARG(2), n=R32(s+20); if(pos>n) pos=n; if(cnt>n-pos) cnt=n-pos;
  char *t=strndup((char*)MEMP(s_ptr(s))+pos,cnt); s_init(ret); s_set(ret,t,cnt); free(t); c->eax=ret; c->esp+=16; }

/* CRT time conversion (struct tm = 9 ints) */
#include <time.h>
static void tm_to_guest(uint32_t p, const struct tm *t){ int v[9]={t->tm_sec,t->tm_min,t->tm_hour,t->tm_mday,t->tm_mon,t->tm_year,t->tm_wday,t->tm_yday,t->tm_isdst}; for(int k=0;k<9;k++) W32(p+4*k,(uint32_t)v[k]); }
static void tm_from_guest(uint32_t p, struct tm *t){ memset(t,0,sizeof *t); t->tm_sec=R32(p); t->tm_min=R32(p+4); t->tm_hour=R32(p+8); t->tm_mday=R32(p+12); t->tm_mon=R32(p+16); t->tm_year=R32(p+20); t->tm_wday=R32(p+24); t->tm_yday=R32(p+28); t->tm_isdst=(int32_t)R32(p+32); }
static uint32_t tmbuf(void){ static __thread uint32_t b; if(!b) b=gmalloc(64); return b; }
HLE(msvcr90,_localtime64){ uint32_t pt=ARG(0); if(!pt) RET_C(0); time_t t=(time_t)(int64_t)R64(pt); struct tm tm; if(!localtime_r(&t,&tm)) RET_C(0); uint32_t b=tmbuf(); tm_to_guest(b,&tm); RET_C(b); }
HLE(msvcr90,_gmtime64){ uint32_t pt=ARG(0); if(!pt) RET_C(0); time_t t=(time_t)(int64_t)R64(pt); struct tm tm; if(!gmtime_r(&t,&tm)) RET_C(0); uint32_t b=tmbuf(); tm_to_guest(b,&tm); RET_C(b); }
HLE(msvcr90,_gmtime64_s){ uint32_t out=ARG(0), pt=ARG(1); if(!out||!pt) RET_C(22); time_t t=(time_t)(int64_t)R64(pt); struct tm tm; if(!gmtime_r(&t,&tm)) RET_C(22); tm_to_guest(out,&tm); RET_C(0); }
HLE(msvcr90,_localtime64_s){ uint32_t out=ARG(0), pt=ARG(1); if(!out||!pt) RET_C(22); time_t t=(time_t)(int64_t)R64(pt); struct tm tm; if(!localtime_r(&t,&tm)) RET_C(22); tm_to_guest(out,&tm); RET_C(0); }
HLE(msvcr90,_mktime64){ struct tm tm; tm_from_guest(ARG(0),&tm); int64_t t=(int64_t)mktime(&tm); tm_to_guest(ARG(0),&tm); c->eax=(uint32_t)t; c->edx=(uint32_t)(t>>32); c->esp+=4; }
HLE(msvcr90,strftime){ uint32_t dst=ARG(0), max=ARG(1); char fmt[256]; g2host_a(ARG(2),fmt,sizeof fmt); struct tm tm; tm_from_guest(ARG(3),&tm); char o[1024]; size_t n=strftime(o,sizeof o,fmt,&tm); if(n>=max) n=0; if(dst&&max){ memcpy(MEMP(dst),o,n); W8(dst+n,0); } RET_C((uint32_t)n); }
HLE(msvcr90,_wstrtime_s){ uint32_t dst=ARG(0), n=ARG(1); time_t t=time(0); struct tm tm; localtime_r(&t,&tm); char o[16]; snprintf(o,sizeof o,"%02d:%02d:%02d",tm.tm_hour,tm.tm_min,tm.tm_sec); if(!dst||n<9) RET_C(22); for(int k=0;k<9;k++) W16(dst+2*k,(uint16_t)o[k]); RET_C(0); }
