/* HLE: window classes, a real per-process message queue and keyboard/mouse state.
   Sources: SDL events from the DXVK window (when the bridge is on) and a scripted
   injector for headless tests: BSI_KEYS="<sec>:<vk>[,<sec>:<vk>...]" presses (down+up) a
   virtual key at the given number of seconds after start. */
#include "hle.h"
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#ifndef BSI_ANDROID
#include <SDL2/SDL.h>
#else
#include "android_plat.h"
#endif
extern uint32_t next_hwnd;
static struct { uint32_t atom; uint32_t proc; uint16_t name[64]; } cls[64]; static int ncls;
static struct { uint32_t hwnd, proc, tid; } wins[64]; static int nwins;
extern __thread uint32_t t_tid; static uint32_t cur_tid(void){ return t_tid?t_tid:0x1004; }
static uint32_t win_tid(uint32_t h){ for(int k=0;k<nwins;k++) if(wins[k].hwnd==h) return wins[k].tid; return 0; }
static uint8_t keys[256];
/* DirectInput view of the same input: DIK scan-code state + buffered events, mouse deltas */
static pthread_mutex_t dmu=PTHREAD_MUTEX_INITIALIZER; static uint8_t dik[256]; static struct { uint8_t code, down; } dq[256]; static int dqh, dqt; static int mdx, mdy, mdz; static uint8_t mbtn[8];
static const uint8_t vk2dik[256]={ [8]=0x0E,[9]=0x0F,[13]=0x1C,[16]=0x2A,[17]=0x1D,[18]=0x38,[27]=0x01,[32]=0x39,[37]=0xCB,[38]=0xC8,[39]=0xCD,[40]=0xD0,
  ['0']=0x0B,['1']=0x02,['2']=0x03,['3']=0x04,['4']=0x05,['5']=0x06,['6']=0x07,['7']=0x08,['8']=0x09,['9']=0x0A,
  ['A']=0x1E,['B']=0x30,['C']=0x2E,['D']=0x20,['E']=0x12,['F']=0x21,['G']=0x22,['H']=0x23,['I']=0x17,['J']=0x24,['K']=0x25,['L']=0x26,['M']=0x32,
  ['N']=0x31,['O']=0x18,['P']=0x19,['Q']=0x10,['R']=0x13,['S']=0x1F,['T']=0x14,['U']=0x16,['V']=0x2F,['W']=0x11,['X']=0x2D,['Y']=0x15,['Z']=0x2C,
  [112]=0x3B,[113]=0x3C,[114]=0x3D,[115]=0x3E,[116]=0x3F,[117]=0x40,[118]=0x41,[119]=0x42,[120]=0x43,[121]=0x44,[122]=0x57,[123]=0x58 };
static void dik_event(uint8_t code,int down){ if(!code) return; dik[code]=down?0x80:0; pthread_mutex_lock(&dmu); int n=(dqt+1)&255; if(n!=dqh){ dq[dqt].code=code; dq[dqt].down=down; dqt=n; } pthread_mutex_unlock(&dmu); }
void di_pump(void);
uint8_t *di_keystate(void){ di_pump(); return dik; }
int di_keyevent(uint8_t *code, int *down){ di_pump(); pthread_mutex_lock(&dmu); if(dqh==dqt){ pthread_mutex_unlock(&dmu); return 0; } *code=dq[dqh].code; *down=dq[dqh].down; dqh=(dqh+1)&255; pthread_mutex_unlock(&dmu); return 1; }
void di_mouse(int *x,int *y,int *z,uint8_t *b){ di_pump(); *x=mdx; *y=mdy; *z=mdz; mdx=mdy=mdz=0; memcpy(b,mbtn,8); }
typedef struct { uint32_t hwnd, msg, wp, lp, tid; } Msg;
static Msg q[1024]; static int qh, qt; static pthread_mutex_t qmu=PTHREAD_MUTEX_INITIALIZER;
static int mx=640, my=360;
/* gamepad play: park the (invisible) cursor outside the window so menu hover does not override stick/d-pad navigation */
static void cursor_park(void){ static int done; if(done) return; done=1; if(getenv("BSI_CURSOR_PARK")){ mx=-4000; my=-4000; } }
static uint32_t main_hwnd(void){ return nwins? wins[nwins-1].hwnd : next_hwnd; }   /* focus = most recently created window (the game viewport) */
static void post(uint32_t h, uint32_t m, uint32_t w, uint32_t l){ pthread_mutex_lock(&qmu); int n=(qt+1)&1023; if(n!=qh){ uint32_t hh=h?h:main_hwnd(); q[qt]=(Msg){hh,m,w,l,win_tid(hh)}; qt=n; } pthread_mutex_unlock(&qmu); }
int ww_key_hook(uint32_t vk, int down) __attribute__((weak));   /* weapon_wheel.c: 1 = the wheel consumed the key */
void ww_pad_hook(uint32_t st) __attribute__((weak));            /* weapon_wheel.c: edits the XINPUT_STATE in place */
int ext_menu_key_hook(uint32_t vk, int down) __attribute__((weak));   /* ext72 ext_menu.c: mod menu (F10, arrows/Enter/Esc while open) */
void ext_menu_pad_hook(uint32_t st) __attribute__((weak));            /* ext72 ext_menu.c: mod menu (hold Back), edits the XINPUT_STATE in place */
static void key(uint32_t vk, int down){ if(vk>255) return; if(ext_menu_key_hook && ext_menu_key_hook(vk,down)) return; if(ww_key_hook && ww_key_hook(vk,down)) return; uint32_t sc=0; int rep=down&&keys[vk];
  keys[vk]=down?0x80:0; dik_event(vk2dik[vk],down); uint32_t lp=1|(sc<<16)|(rep?1u<<30:0)|(down?0:(3u<<30));
  post(0, down?0x100:0x101, vk, lp); }
static double t0=-1; static double now(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); double t=ts.tv_sec+ts.tv_nsec*1e-9; if(t0<0) t0=t; return t-t0; }
/* BSI_KEYFILE=<file> (interactive headless driving, default off): lines appended to the file are executed as they arrive:
   "k <vk> [hold]" key press, "p <hex buttons> <lx> <ly> <secs>" virtual pad state for secs, "d" dump the next backbuffer */
volatile int g_dump_req; static double pad_ov_until; static unsigned pad_ov_b; static int pad_ov_lx, pad_ov_ly;
static void keyfile(double t){ static FILE *f; static int init; static double next; static struct { double up; uint32_t vk; } held[16];
  for(int k=0;k<16;k++) if(held[k].vk && t>=held[k].up){ key(held[k].vk,0); held[k].vk=0; }
  if(!init){ init=1; const char*e=getenv("BSI_KEYFILE"); if(e) f=fopen(e,"r"); }
  if(!f || t<next) return; next=t+0.1; char ln[128];
  while(fgets(ln,sizeof ln,f)){ unsigned vk, b; double h=0.15; int lx, ly;
    if(ln[0]=='k' && sscanf(ln+1,"%u %lf",&vk,&h)>=1){ fprintf(stderr,"[input] keyfile key %u\n",vk); key(vk,1); for(int k=0;k<16;k++) if(!held[k].vk){ held[k].vk=vk; held[k].up=t+h; break; } }
    else if(ln[0]=='p' && sscanf(ln+1,"%x %d %d %lf",&b,&lx,&ly,&h)==4){ fprintf(stderr,"[input] keyfile pad %x %d %d %.2f\n",b,lx,ly,h); pad_ov_b=b; pad_ov_lx=lx; pad_ov_ly=ly; pad_ov_until=t+h; }
    else if(ln[0]=='d'){ g_dump_req=1; } }
  clearerr(f); }
static void script(void){ static int init; static struct { double t, hold; uint32_t vk; int st; } ev[64]; static int nev;
  keyfile(now());
  if(!init){ init=1; now(); const char *e=getenv("BSI_KEYS"); while(e&&*e&&nev<64){ double t; unsigned vk; int n;
      if(sscanf(e,"%lf:%u%n",&t,&vk,&n)<2) break; ev[nev].t=t; ev[nev].vk=vk; ev[nev].hold=0.15; e+=n; if(*e==':'){ double h; int m; if(sscanf(e+1,"%lf%n",&h,&m)==1){ ev[nev].hold=h; e+=1+m; } } nev++; if(*e==',') e++; } }
  double t=now(); for(int k=0;k<nev;k++){ if(ev[k].st==0 && t>=ev[k].t){ fprintf(stderr,"[input] scripted key %u down\n",ev[k].vk); key(ev[k].vk,1); ev[k].st=1; }
    else if(ev[k].st==1 && t>=ev[k].t+ev[k].hold){ key(ev[k].vk,0); ev[k].st=2; } } }
/* BSI_MOUSE="<sec>:<dx>:<dy>:<dur>,..." scripted relative mouse motion (pixels spread over dur seconds) */
static void mscript(void){ static int init; static struct { double t, dur, dx, dy, done; } ev[32]; static int nev; static double last=-1;
  if(!init){ init=1; const char *e=getenv("BSI_MOUSE"); while(e&&*e&&nev<32){ double t,dx,dy,d; int n; if(sscanf(e,"%lf:%lf:%lf:%lf%n",&t,&dx,&dy,&d,&n)<4) break;
      ev[nev].t=t; ev[nev].dx=dx; ev[nev].dy=dy; ev[nev].dur=d>0?d:0.01; nev++; e+=n; if(*e==',') e++; } }
  double t=now(); if(last<0){ last=t; return; }
  for(int k=0;k<nev;k++){ double a=ev[k].t, b=ev[k].t+ev[k].dur; double lo=last>a?last:a, hi=t<b?t:b; if(hi<=lo) continue;
    double f=(hi-lo)/ev[k].dur; double want=ev[k].done+f; int ix=(int)(ev[k].dx*want)-(int)(ev[k].dx*ev[k].done), iy=(int)(ev[k].dy*want)-(int)(ev[k].dy*ev[k].done); ev[k].done=want;
    mdx+=ix; mdy+=iy; mx+=ix; my+=iy; if(ix||iy) post(0,0x200,0,(mx&0xffff)|((my&0xffff)<<16)); }
  last=t; }
#ifndef BSI_ANDROID
static uint32_t sdl2vk(SDL_Keycode k){
  if(k>='a'&&k<='z') return k-'a'+'A'; if(k>='0'&&k<='9') return k;
  switch(k){ case SDLK_RETURN: return 13; case SDLK_ESCAPE: return 27; case SDLK_SPACE: return 32; case SDLK_TAB: return 9; case SDLK_BACKSPACE: return 8;
    case SDLK_LEFT: return 37; case SDLK_UP: return 38; case SDLK_RIGHT: return 39; case SDLK_DOWN: return 40;
    case SDLK_LSHIFT: case SDLK_RSHIFT: return 16; case SDLK_LCTRL: case SDLK_RCTRL: return 17; case SDLK_LALT: case SDLK_RALT: return 18; }
  if(k>=SDLK_F1&&k<=SDLK_F12) return 112+(k-SDLK_F1); return 0; }
static void pump(void){ script(); mscript();
  if(!SDL_WasInit(SDL_INIT_VIDEO)) return; SDL_Event e;
  while(SDL_PollEvent(&e)){ switch(e.type){
    case SDL_KEYDOWN: case SDL_KEYUP: { uint32_t vk=sdl2vk(e.key.keysym.sym); if(vk) key(vk,e.type==SDL_KEYDOWN); break; }
    case SDL_MOUSEMOTION: mdx+=e.motion.xrel; mdy+=e.motion.yrel; mx+=e.motion.xrel; my+=e.motion.yrel; post(0,0x200,0,(mx&0xffff)|(my<<16)); break;
    case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP: { int d=e.type==SDL_MOUSEBUTTONDOWN; uint32_t m= e.button.button==1?(d?0x201:0x202): e.button.button==3?(d?0x204:0x205):(d?0x207:0x208);
      keys[e.button.button==1?1:e.button.button==3?2:4]=d?0x80:0; mbtn[e.button.button==1?0:e.button.button==3?1:2]=d?0x80:0; post(0,m,0,(mx&0xffff)|(my<<16)); break; }
    case SDL_MOUSEWHEEL: mdz+=e.wheel.y*120; break;
    case SDL_QUIT: post(0,0x10,0,0); break; } } }
#else
static void pump(void){ script(); mscript();
  int t, a, b; while(android_input_next(&t,&a,&b)){
    switch(t){
    case AIN_KEY: key((uint32_t)a,b); break;
    case AIN_MOUSE: mdx+=a; mdy+=b; mx+=a; my+=b; post(0,0x200,0,(mx&0xffff)|((my&0xffff)<<16)); break;
    case AIN_BUTTON: { int d=b; uint32_t m= a==0?(d?0x201:0x202): a==1?(d?0x204:0x205):(d?0x207:0x208);
      keys[a==0?1:a==1?2:4]=d?0x80:0; mbtn[a]=d?0x80:0; post(0,m,0,(mx&0xffff)|((my&0xffff)<<16)); break; }
    case AIN_WHEEL: mdz+=a; break;
    case AIN_MOUSEABS: { static int gw, gh; if(!gw){ gw = getenv("BSI_GAME_W") ? atoi(getenv("BSI_GAME_W")) : 1280; gh = getenv("BSI_GAME_H") ? atoi(getenv("BSI_GAME_H")) : 720; }
      int nx = (int)((long)a*gw/65536), ny = (int)((long)b*gh/65536); mdx+=nx-mx; mdy+=ny-my; mx=nx; my=ny; post(0,0x200,0,(mx&0xffff)|((my&0xffff)<<16)); break; }
    case AIN_QUIT: post(0,0x10,0,0); break; } } }
#endif
HLE(user32,RegisterClassExW){ uint32_t w=ARG(0); int k=ncls<64?ncls++:63; cls[k].proc=R32(w+8); cls[k].atom=0xc001+k;
  uint32_t nm=R32(w+40); memset(cls[k].name,0,sizeof cls[k].name); if(nm>0xffff) for(int i=0;i<63;i++){ uint16_t ch=R16(nm+2*i); cls[k].name[i]=ch; if(!ch) break; }
  RET_STD(1,cls[k].atom); }
HLE(user32,RegisterClassW){ uint32_t w=ARG(0); int k=ncls<64?ncls++:63; cls[k].proc=R32(w+4); cls[k].atom=0xc001+k;
  uint32_t nm=R32(w+36); memset(cls[k].name,0,sizeof cls[k].name); if(nm>0xffff) for(int i=0;i<63;i++){ uint16_t ch=R16(nm+2*i); cls[k].name[i]=ch; if(!ch) break; }
  RET_STD(1,cls[k].atom); }
uint32_t wndproc_of(uint32_t h){ for(int k=0;k<nwins;k++) if(wins[k].hwnd==h) return wins[k].proc; return 0; }
HLE(user32,CreateWindowExW){ uint32_t cn=ARG(1), proc=0;
  for(int k=0;k<ncls;k++){ if(cn<=0xffff){ if(cls[k].atom==cn) proc=cls[k].proc; } else { int i=0; for(;i<63;i++){ uint16_t a=R16(cn+2*i), b=cls[k].name[i]; if(a!=b) break; if(!a){ i=99; break; } } if(i==99) proc=cls[k].proc; } }
  char t[256]; g2host_w(ARG(2)>0xffff?ARG(2):0,t,256); uint32_t h=next_hwnd+=16;
  if(nwins<64){ wins[nwins].hwnd=h; wins[nwins].proc=proc; wins[nwins].tid=cur_tid(); nwins++; }
  fprintf(stderr,"[ui] CreateWindowExW '%s' %dx%d -> %08x wndproc %08x\n",t,(int)ARG(8),(int)ARG(9),h,proc);
  if(proc && !strstr(t,"Splash")){ post(h,0x1c,1,0); post(h,0x06,1,0); post(h,0x07,0,0); }   /* WM_ACTIVATEAPP, WM_ACTIVATE(WA_ACTIVE), WM_SETFOCUS: the game pauses while unfocused */
  RET_STD(12,h); }
static int getmsg(uint32_t p, int remove){ pump();
  /* per-thread queues: a thread only receives messages for windows it created (or thread messages, tid 0 = any) */
  uint32_t me=cur_tid(); Msg m; int found=0;
  pthread_mutex_lock(&qmu);
  for(int k=qh;k!=qt;k=(k+1)&1023) if(!q[k].tid || q[k].tid==me){ m=q[k]; found=1;
      if(remove){ for(int j=k;j!=qh;j=(j-1)&1023) q[j]=q[(j-1)&1023]; qh=(qh+1)&1023; } break; }
  pthread_mutex_unlock(&qmu);
  if(!found) return 0;
  W32(p,m.hwnd); W32(p+4,m.msg); W32(p+8,m.wp); W32(p+12,m.lp); W32(p+16,(uint32_t)(now()*1000)); W32(p+20,mx); W32(p+24,my); return 1; }
HLE(user32,PeekMessageW){ RET_STD(5,getmsg(ARG(0),ARG(4)&1)); }
HLE(user32,GetMessageW){ uint32_t p=ARG(0); if(getmsg(p,1)) RET_STD(4,R32(p+4)!=0x12); usleep(10000); RET_STD(4,0); }  /* old behaviour when idle: acts like WM_QUIT for helper threads */
HLE(user32,PostMessageW){ post(ARG(0),ARG(1),ARG(2),ARG(3)); RET_STD(4,1); }
HLE(user32,TranslateMessage){ uint32_t p=ARG(0); uint32_t m=R32(p+4), vk=R32(p+8);
  if(m==0x100){ uint32_t ch=0; if(vk>='A'&&vk<='Z') ch=keys[16]?vk:vk+32; else if((vk>='0'&&vk<='9')||vk==32||vk==13||vk==8||vk==27||vk==9) ch=vk;
    if(ch){ post(R32(p),0x102,ch,R32(p+12)); RET_STD(1,1); } }
  RET_STD(1,0); }
HLE(user32,DispatchMessageW){ uint32_t p=ARG(0); uint32_t h=R32(p), m=R32(p+4), w=R32(p+8), l=R32(p+12); uint32_t proc=wndproc_of(h);
  { static int lg=-1; if(lg<0) lg=getenv("BSI_INPUTLOG")!=0; if(lg && m!=0x200) fprintf(stderr,"[input] dispatch hwnd %x msg %x w %x l %x -> proc %x\n",h,m,w,l,proc); }
  c->esp+=4+4; uint32_t ret_esp=c->esp;   /* pop our frame first, then call the window procedure from the caller's frame */
  if(!proc){ c->eax=0; return; }
  PUSH32(c,l); PUSH32(c,w); PUSH32(c,m); PUSH32(c,h); guest_call(c,proc); }
HLE(user32,GetKeyState){ uint32_t vk=ARG(0)&255; RET_STD(1,(uint32_t)(int16_t)(keys[vk]?0x8000:0)); }
HLE(user32,GetAsyncKeyState){ uint32_t vk=ARG(0)&255; pump(); RET_STD(1,(uint32_t)(int16_t)(keys[vk]?0x8000:0)); }
HLE(user32,GetKeyboardState){ memcpy(MEMP(ARG(0)),keys,256); RET_STD(1,1); }

void di_pump(void){ pump(); }

/* window longs: GWL_WNDPROC (-4) subclassing is how the engine attaches its viewport window procedure */
static struct { uint32_t h; int32_t idx; uint32_t v; } wl[256]; static int nwl;
static uint32_t wl_get(uint32_t h,int32_t i){ if(i==-4) return wndproc_of(h); for(int k=0;k<nwl;k++) if(wl[k].h==h&&wl[k].idx==i) return wl[k].v; return 0; }
static uint32_t wl_set(uint32_t h,int32_t i,uint32_t v){ uint32_t old=wl_get(h,i);
  if(i==-4){ for(int k=0;k<nwins;k++) if(wins[k].hwnd==h){ wins[k].proc=v; fprintf(stderr,"[ui] window %08x subclassed: wndproc %08x -> %08x\n",h,old,v); } return old; }
  for(int k=0;k<nwl;k++) if(wl[k].h==h&&wl[k].idx==i){ wl[k].v=v; return old; }
  if(nwl<256){ wl[nwl].h=h; wl[nwl].idx=i; wl[nwl].v=v; nwl++; } return old; }
HLE(user32,SetWindowLongW){ RET_STD(3,wl_set(ARG(0),(int32_t)ARG(1),ARG(2))); }
HLE(user32,SetWindowLongA){ RET_STD(3,wl_set(ARG(0),(int32_t)ARG(1),ARG(2))); }
HLE(user32,GetWindowLongW){ RET_STD(2,wl_get(ARG(0),(int32_t)ARG(1))); }
HLE(user32,GetWindowLongA){ RET_STD(2,wl_get(ARG(0),(int32_t)ARG(1))); }
HLE(user32,CallWindowProcW){ uint32_t proc=ARG(0), h=ARG(1), m=ARG(2), w=ARG(3), l=ARG(4); c->esp+=4+20; if(!proc){ c->eax=0; return; } PUSH32(c,l); PUSH32(c,w); PUSH32(c,m); PUSH32(c,h); guest_call(c,proc); }

#ifndef BSI_ANDROID
/* XInput 1.3 (ordinals 2 = XInputGetState, 3 = XInputSetState, 4 = XInputGetCapabilities) on SDL game controllers */
static SDL_GameController *gc[4];
static SDL_GameController *pad(uint32_t i){
  if(i>3) return 0; static int init; if(!init){ init=1; if(SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER)) return 0; }
  if(gc[i] && !SDL_GameControllerGetAttached(gc[i])){ SDL_GameControllerClose(gc[i]); gc[i]=0; }
  if(!gc[i]){ static double lastscan; double t=now(); if(t-lastscan<1.0) return 0; lastscan=t; SDL_GameControllerUpdate();
    int n=0; for(int j=0;j<SDL_NumJoysticks();j++) if(SDL_IsGameController(j)){ if(n++==(int)i){ gc[i]=SDL_GameControllerOpen(j); if(gc[i]) fprintf(stderr,"[input] XInput pad %u = %s\n",i,SDL_GameControllerName(gc[i])); break; } } }
  return gc[i]; }
/* BSI_PADS="<sec>:<buttons hex>:<lx>:<ly>[,...]" scripted virtual XInput pad for headless tests (each state holds until the next) */
static int padscript(uint32_t st){ static int init, nev; static struct { double t; unsigned b; int lx, ly; } ev[64]; static int on;
  if(!init){ init=1; now(); const char *e=getenv("BSI_PADS"); on = e!=0; while(e&&*e&&nev<64){ double t; unsigned b; int lx, ly, n;
      if(sscanf(e,"%lf:%x:%d:%d%n",&t,&b,&lx,&ly,&n)<4) break; ev[nev].t=t; ev[nev].b=b; ev[nev].lx=lx; ev[nev].ly=ly; nev++; e+=n; if(*e==',') e++; } }
  if(!on) return 0; if(!st) return 1;
  double t=now(); unsigned b=0; int lx=0, ly=0; static int last=-1; for(int k=0;k<nev;k++) if(t>=ev[k].t){ b=ev[k].b; lx=ev[k].lx; ly=ev[k].ly; if(k!=last){ last=k; fprintf(stderr,"[input] scripted pad %x %d %d\n",b,lx,ly); } }
  script(); if(t<pad_ov_until){ b=pad_ov_b; lx=pad_ov_lx; ly=pad_ov_ly; }
  static uint32_t pkt; W32(st,++pkt); W16(st+4,b); W8(st+6,0); W8(st+7,0); W16(st+8,(uint16_t)lx); W16(st+10,(uint16_t)ly); W16(st+12,0); W16(st+14,0); return 1; }
HLE(xinput1_3,ord2){ uint32_t i=ARG(0), st=ARG(1); if(i==0 && padscript(st)){ if(ext_menu_pad_hook) ext_menu_pad_hook(st); if(ww_pad_hook) ww_pad_hook(st); RET_STD(2,0); } SDL_GameController *g=pad(i); if(!g) RET_STD(2,1167);
  SDL_GameControllerUpdate(); static uint32_t pkt; W32(st,++pkt);
  static const int bm[][2]={{SDL_CONTROLLER_BUTTON_DPAD_UP,1},{SDL_CONTROLLER_BUTTON_DPAD_DOWN,2},{SDL_CONTROLLER_BUTTON_DPAD_LEFT,4},{SDL_CONTROLLER_BUTTON_DPAD_RIGHT,8},
    {SDL_CONTROLLER_BUTTON_START,0x10},{SDL_CONTROLLER_BUTTON_BACK,0x20},{SDL_CONTROLLER_BUTTON_LEFTSTICK,0x40},{SDL_CONTROLLER_BUTTON_RIGHTSTICK,0x80},
    {SDL_CONTROLLER_BUTTON_LEFTSHOULDER,0x100},{SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,0x200},{SDL_CONTROLLER_BUTTON_A,0x1000},{SDL_CONTROLLER_BUTTON_B,0x2000},{SDL_CONTROLLER_BUTTON_X,0x4000},{SDL_CONTROLLER_BUTTON_Y,0x8000}};
  uint16_t b=0; for(unsigned k=0;k<sizeof bm/sizeof bm[0];k++) if(SDL_GameControllerGetButton(g,bm[k][0])) b|=bm[k][1];
  int lt=SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_TRIGGERLEFT), rt=SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
  int ly=-SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_LEFTY)-1, ry=-SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_RIGHTY)-1;
  W16(st+4,b); W8(st+6,(uint8_t)(lt*255/32767)); W8(st+7,(uint8_t)(rt*255/32767));
  W16(st+8,(uint16_t)SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_LEFTX)); W16(st+10,(uint16_t)ly);
  W16(st+12,(uint16_t)SDL_GameControllerGetAxis(g,SDL_CONTROLLER_AXIS_RIGHTX)); W16(st+14,(uint16_t)ry);
  if(ext_menu_pad_hook) ext_menu_pad_hook(st); if(ww_pad_hook) ww_pad_hook(st);
  RET_STD(2,0); }
HLE(xinput1_3,ord3){ uint32_t i=ARG(0), v=ARG(1); if(i==0 && padscript(0)) RET_STD(2,0); SDL_GameController *g=pad(i); if(!g) RET_STD(2,1167);
  if(v) SDL_GameControllerRumble(g,R16(v),R16(v+2),200); RET_STD(2,0); }
HLE(xinput1_3,ord4){ uint32_t i=ARG(0), cap=ARG(2); SDL_GameController *g=(i==0 && padscript(0)) ? (SDL_GameController*)1 : pad(i); if(!g) RET_STD(3,1167);
  memset(MEMP(cap),0,20); W8(cap,1); W8(cap+1,1); W16(cap+4,0xf3ff); W8(cap+6,255); W8(cap+7,255); for(int k=0;k<4;k++) W16(cap+8+2*k,0xffc0); W16(cap+16,65535); W16(cap+18,65535); RET_STD(3,0); }

#else
/* XInput 1.3 on Android: gamepad state collected by the activity's input thread (android_glue.c) */
HLE(xinput1_3,ord2){ uint32_t i=ARG(0), st=ARG(1); AndroidPad p; if(i!=0 || !android_pad(&p)) RET_STD(2,1167);
  { static long calls, active; static time_t last; calls++; if(p.buttons||p.lx||p.ly||p.rx||p.ry||p.lt||p.rt) active++;
    time_t t=time(0); if(t-last>=10){ last=t; fprintf(stderr,"[input] XInputGetState calls %ld, with input %ld, now buttons %04x L %d,%d R %d,%d\n",calls,active,p.buttons,p.lx,p.ly,p.rx,p.ry); } }
  static uint32_t pkt; W32(st,++pkt); W16(st+4,p.buttons); W8(st+6,p.lt); W8(st+7,p.rt);
  W16(st+8,(uint16_t)p.lx); W16(st+10,(uint16_t)p.ly); W16(st+12,(uint16_t)p.rx); W16(st+14,(uint16_t)p.ry); if(ext_menu_pad_hook) ext_menu_pad_hook(st); if(ww_pad_hook) ww_pad_hook(st); RET_STD(2,0); }
void android_rumble(float l, float r);
HLE(xinput1_3,ord3){ AndroidPad p; if(ARG(0)!=0 || !android_pad(&p)) RET_STD(2,1167);
  uint32_t v=ARG(1); if(v) android_rumble(R16(v)/65535.0f, R16(v+2)/65535.0f); RET_STD(2,0); }
HLE(xinput1_3,ord4){ uint32_t cap=ARG(2); AndroidPad p; if(ARG(0)!=0 || !android_pad(&p)) RET_STD(3,1167);
  memset(MEMP(cap),0,20); W8(cap,1); W8(cap+1,1); W16(cap+4,0xf3ff); W8(cap+6,255); W8(cap+7,255); for(int k=0;k<4;k++) W16(cap+8+2*k,0xffc0); RET_STD(3,0); }
#endif
/* virtual cursor: relative mouse motion moves it, the game re-centres it with SetCursorPos (UE3 mouse look) */
HLE(user32,GetCursorPos){ cursor_park(); script(); mscript(); W32(ARG(0),mx); W32(ARG(0)+4,my); RET_STD(1,1); }
HLE(user32,SetCursorPos){ cursor_park(); if(!getenv("BSI_CURSOR_PARK")){ mx=(int)ARG(0); my=(int)ARG(1); } RET_STD(2,1); }
/* focus / capture: the game viewport (most recently created window) is always foreground and focused */
static uint32_t capture_hwnd;
HLE(user32,GetForegroundWindow){ RET_STD(0,main_hwnd()); }
HLE(user32,GetActiveWindow){ RET_STD(0,main_hwnd()); }
HLE(user32,GetFocus){ RET_STD(0,main_hwnd()); }
HLE(user32,GetCapture){ RET_STD(0,capture_hwnd); }
HLE(user32,SetCapture){ uint32_t old=capture_hwnd; capture_hwnd=ARG(0); { static int n; if(n++<5) fprintf(stderr,"[input] SetCapture %x\n",ARG(0)); } RET_STD(1,old); }
HLE(user32,ReleaseCapture){ capture_hwnd=0; RET_STD(0,1); }
