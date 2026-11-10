/* Optional behaviour patches for the recompiled game (env BSI_PATCHES=1, default on in the Linux runtime).
   loadclose: UXGameViewportClient::ShouldTickLoadingCountdown (0xaf12b0) only lets CloseLoadingMovieCountdown
   run while the loading movie is still playing. If rendering is slow (CPU rasterizer, first-frame shader
   compiles) the startup movie can finish before the 10-frame countdown starts, leaving the loading screen
   "open" forever (camera fade stays black). Keep the countdown running while it is > 0. */
#include "hle.h"
/* loadclose2: the script can request the loading screen to close (vf 0xc0) before the post-load countdown
   (vf 0xbc) has started. The close then runs immediately, the countdown starts afterwards, finishes, marks
   "close pending" and fades the camera to black, and nothing ever closes it again (black screen with the
   level running). Remember such an early request and re-queue it (flag 8) when the countdown starts. */
static int g_close_early;
static int loadlog(void){ static int l=-1; if(l<0) l=getenv("BSI_LOADLOG")!=0; return l; }
#include <time.h>
void call_indirect(CPU*c, uint32_t t);
static double now_s(void){ struct timespec ts; clock_gettime(CLOCK_MONOTONIC,&ts); return ts.tv_sec+ts.tv_nsec*1e-9; }
/* BSI_LCSTAT=1 (diagnostic, notes/GAMETHREAD_HOT.md): count calls into the actor-collision interface (vtable 0x114ef68:
   0x6443c0 line check -> f_0083f8b0 -> f_0083d6d0, and its three siblings) with the caller's return address and the next
   call-return address up the guest stack; every 10 s print calls/s per entry and the top callers.  rt_linux.c ft_init keeps
   these four addresses off the flat call table when BSI_LCSTAT is set, so their calls reach patch_call. */
#include <stdatomic.h>
extern __thread uint32_t t_tid;
#define LC_N 4096u
static struct { _Atomic uint64_t key; _Atomic uint32_t n; } g_lc[LC_N]; static _Atomic uint32_t g_lc_slot[4], g_lc_gt[4];
static int lc_iscall(uint32_t v){ return v>0x401005u && v<0x1074000u && (R8(v-5)==0xE8 || R8(v-6)==0xFF || R8(v-2)==0xFF || R8(v-3)==0xFF); }
static void lc_note(CPU *c, uint32_t t){
  int k = t==0x6443c0u?0 : t==0x6444a0u?1 : t==0x6444f0u?2 : 3; atomic_fetch_add(&g_lc_slot[k],1); if(t_tid==0x1004u) atomic_fetch_add(&g_lc_gt[k],1);
  uint32_t ra=R32(c->esp), ra2=0; for(int i=1;i<48 && !ra2;i++){ uint32_t v=R32(c->esp+4*i); if(lc_iscall(v)) ra2=v; }
  uint64_t key=((uint64_t)k<<62)|((uint64_t)ra<<31)|ra2;
  for(uint32_t h=(uint32_t)((key*0x9E3779B97F4A7C15ull)>>52)&(LC_N-1), i=0;i<64;i++,h=(h+1)&(LC_N-1)){
    uint64_t cur=atomic_load(&g_lc[h].key);
    if(cur==key || (cur==0 && atomic_compare_exchange_strong(&g_lc[h].key,&cur,key)) || cur==key){ atomic_fetch_add(&g_lc[h].n,1); break; } }
  static double last; double n=now_s(); if(last==0) last=n;
  if(n-last>=10.0){ double dt=n-last; last=n;
    fprintf(stderr,"[lcstat] per s: line %.0f (GT %.0f) | e1 %.0f (GT %.0f) | e2 %.0f (GT %.0f) | e3 %.0f (GT %.0f)\n",
      atomic_exchange(&g_lc_slot[0],0)/dt,atomic_exchange(&g_lc_gt[0],0)/dt,atomic_exchange(&g_lc_slot[1],0)/dt,atomic_exchange(&g_lc_gt[1],0)/dt,
      atomic_exchange(&g_lc_slot[2],0)/dt,atomic_exchange(&g_lc_gt[2],0)/dt,atomic_exchange(&g_lc_slot[3],0)/dt,atomic_exchange(&g_lc_gt[3],0)/dt);
    for(int r=0;r<15;r++){ uint32_t best=0, bi=0; for(uint32_t i=0;i<LC_N;i++){ uint32_t v=atomic_load(&g_lc[i].n); if(v>best){ best=v; bi=i; } }
      if(!best) break; uint64_t kk=atomic_load(&g_lc[bi].key);
      fprintf(stderr,"[lcstat]   %7.0f/s entry %d ra %08x ra2 %08x\n",best/dt,(int)(kk>>62),(uint32_t)(kk>>31)&0x7fffffffu,(uint32_t)kk&0x7fffffffu);
      atomic_store(&g_lc[bi].n,0); }
    for(uint32_t i=0;i<LC_N;i++) atomic_store(&g_lc[i].n,0); } }
/* apexfetch (BSI_APEXFIX, default 1; 0 = original instruction order): APEX ApexScene::FetchResults task release()
   (ApexFramework 0x1040f492, vtable 0x10474b58 slot 5) is
       PxTask::release(); SetEvent(fetchResultsReady); if(mFollowingTask){ mFollowingTask->removeReference(); mFollowingTask=0; }
   removeReference() can submit the following task (the engine's physics completion task, vtable 0x115b354). Another worker can
   run it to the end before this worker returns; the GameThread then wakes (done event, f_006b0c50), starts the next
   ApexScene::simulate and re-arms this task with setFollowingTask(completionTask) ([this+0x1c]). The late
   "mFollowingTask=0" here then wipes the NEW pointer: next frame release() skips removeReference, the completion task is
   never submitted and the GameThread waits forever in f_006b0c50 (the startup/title freeze, hang_fix/README.md).
   Fix: take and clear the pointer before removeReference(), so no write to the task can follow the submit.
   BSI_APEX_RELDELAY_US=N (debug): sleep N us right after removeReference() to widen the window (stress test). */
#include <unistd.h>
static int apex_fetch_release(CPU *c){
  static int mode=-1, dly; if(mode<0){ const char*e=getenv("BSI_APEXFIX"); dly=getenv("BSI_APEX_RELDELAY_US")?atoi(getenv("BSI_APEX_RELDELAY_US")):0; mode=e?atoi(e):1;
    fprintf(stderr,"[apexfix] FetchResults task release: %s%s\n",mode?"fixed order (clear mFollowingTask before removeReference)":"ORIGINAL order (race possible), races are logged",dly?" + debug delay":""); }
  uint32_t self=c->ecx, ebx=c->ebx, esi=c->esi, edi=c->edi, ebp=c->ebp, sp0=c->esp;
  uint32_t tm=R32(self+8); W32(self+8,0);                                   /* PxTask::release(): mTm=0; tm->taskCompleted(*this) */
  c->esp-=4; W32(c->esp,self); c->esp-=4; W32(c->esp,0x1040f4a2u); c->ecx=tm; call_indirect(c,R32(R32(tm)+0x28));
  uint32_t ev=R32(R32(R32(self+0x18)+0xf0));                                /* SetEvent(scene->fetchResultsReady) through the DLL's IAT */
  c->esp-=4; W32(c->esp,ev); c->esp-=4; W32(c->esp,0x1040f4b3u); call_import(c,0x1046b018u);
  uint32_t f = mode ? __atomic_exchange_n((uint32_t*)MEMP(self+0x1c),0u,__ATOMIC_SEQ_CST) : R32(self+0x1c);
  static int lg=-1; if(lg<0) lg=getenv("BSI_APEX_RELLOG")?atoi(getenv("BSI_APEX_RELLOG")):0;
  if(lg>0){ static int n; if(n++<lg) fprintf(stderr,"[apexfix] release task %08x tid %04x following %08x\n",self,t_tid,f); }
  if(f){ c->esp-=4; W32(c->esp,0x1040f4bfu); c->ecx=f; call_indirect(c,R32(R32(f)+0x10));   /* mFollowingTask->removeReference() */
    if(dly) usleep(dly);
    if(!mode){ uint32_t now=R32(self+0x1c); if(now!=f){ static int n; if(n++<50) fprintf(stderr,"[apexfix] RACE: FetchResults task %08x re-armed (%08x -> %08x) during removeReference; original code clears it now -> completion task lost\n",self,f,now); }
      W32(self+0x1c,0); } }
  c->esp=sp0+4; c->ebx=ebx; c->esi=esi; c->edi=edi; c->ebp=ebp; c->ecx=f;   /* ret (thiscall, no args) */
  return 1; }
int patch_call(CPU *c, uint32_t t){
  if(t==0x1040f492u) return apex_fetch_release(c);   /* APEX FetchResults release race (apexfetch, above) */
  /* build 68 opt-in gameplay mods (gear_mod.c, weapon_perks.c, weapon_wheel.c, economy.c): every hook below is a weak symbol and
     each mod stays idle unless its config file exists (/sdcard/BSI/<mod>.txt). Order as in economy/build_econ.sh:
     economy first (its 0xaf0480 tick observer returns 0), then the tick (weapon perks wrap the gear mod), then the damage
     handler (weapon perks wrap the gear mod's wrapper). The weapon wheel hooks call_indirect_slow in rt_linux.c instead. */
  { extern int economy_call(CPU*,uint32_t) __attribute__((weak)); if(economy_call && economy_call(c,t)) return 1; }   /* economy.c (economy/src): tick observer for 0xaf0480 (returns 0) + loot roll/table/money pickup hooks */
  if(t==0xaf0480u){ extern void gear_mod_tick(CPU*) __attribute__((weak)); extern void weapon_perks_tick(CPU*) __attribute__((weak));
    { extern void legendary_tick(CPU*) __attribute__((weak)); if(legendary_tick) legendary_tick(c); }   /* ext72: legendary drops (linux/legendary.c); observe only */
    { extern void ext_menu_tick(void) __attribute__((weak)); if(ext_menu_tick) ext_menu_tick(); }   /* ext72: in-game mod menu (linux/ext_menu.c) */
    { extern void autopilot_game_tick(void) __attribute__((weak)); if(autopilot_game_tick) autopilot_game_tick(); }   /* b78: lab autopilot map tracking (linux/autopilot.c); observe only */
    if(weapon_perks_tick) weapon_perks_tick(c); else if(gear_mod_tick) gear_mod_tick(c);   /* weapon perks (weaponperks/src/weapon_perks.c) wrap the gear mod tick; observe only */
    extern int local_engine_tick_call(CPU*) __attribute__((weak)); return local_engine_tick_call?local_engine_tick_call(c):0; }   /* UXGameEngine::Tick: save list (local_profile.c) */
  if(t==0x971fa0u){ extern int legendary_call(CPU*,uint32_t) __attribute__((weak)); if(legendary_call && legendary_call(c,t)) return 1; }   /* ext72: The Chosen One procs; runs the perks / gear chain itself */
  if(t==0x971fa0u){ extern int weapon_perks_call(CPU*,uint32_t) __attribute__((weak)); if(weapon_perks_call && weapon_perks_call(c,t)) return 1; }   /* weapon perks: wraps the gear mod's wrapper */
  if(t==0x971fa0u){ extern int gear_mod_call(CPU*,uint32_t) __attribute__((weak)); return gear_mod_call?gear_mod_call(c,t):0; }   /* AXPawn damage handler (gear mod events) */
  if(t>=0x1600000u&&t<0x1602000u){   /* local_profile.c: RemoteStorage014 0x1600000.., SteamUser020 0x1601000.. */
    extern int local_profile_call(CPU*,uint32_t) __attribute__((weak));
    return local_profile_call?local_profile_call(c,t):0;
  }
  /* Wwise setting switches (linux/wwise_tune.c, BSI_AK_*): UXAKAudioDevice::Init 0xC22160 + CAkRoomVerbFX::Init 0xF6A4D0.
     rt_linux.c ft_init keeps them off the flat call table only while wwise_tune_enabled() says a switch is set. */
  if(t==0xc22160u||t==0xf6a4d0u){ extern int wwise_tune_call(CPU*,uint32_t) __attribute__((weak)); return wwise_tune_call?wwise_tune_call(c,t):0; }
  if(t==0x5cc660u||t==0x5cf730u||t==0x5d02e0u||t==0x51dd10u){ extern int mb_off_call(CPU*,uint32_t) __attribute__((weak)); return mb_off_call?mb_off_call(c,t):0; }   /* motion blur + bloom switches (linux/mb_off.c) */
  if(t-0x6443c0u<=0x1a0u && (t==0x6443c0u||t==0x6444a0u||t==0x6444f0u||t==0x644560u)){ static int on=-1; if(on<0) on=getenv("BSI_LCSTAT")!=0; if(on) lc_note(c,t); return 0; }
  /* loadclose3: the front-end Kismet "hide loading screen" (XSeqAct_ShowLoadingScreen input 1) sometimes never
     fires, leaving the viewport in "close loading movie pending" with the camera faded to black forever.
     If that state persists for BSI_LOADCLOSE_SEC (default 4) seconds with no loading screen or movie up,
     request the close ourselves. */
  if(t==0xaf2950u){ static __thread int in; static double since; if(in) return 0;
    uint32_t self=c->ecx; in=1; call_indirect(c,t); in=0;
    uint32_t fl=R32(self+0x12c), mp=R32(0x13a17f8);
    int stuck=(fl&0x40)&&!(fl&8)&&R32(self+0x144)==0&&!R32(self+0x13c)&&!(mp&&R32(mp+0x9c)!=0xffffffffu);
    if(!stuck) since=0; else { double n=now_s(); if(since==0) since=n;
      static double lim=-1; if(lim<0){ const char*e=getenv("BSI_LOADCLOSE_SEC"); lim=e?atof(e):4; }
      if(lim>0 && n-since>lim){ since=0; fprintf(stderr,"[load] loading screen close never requested; closing it (flags %x)\n",fl);
        c->ecx=self; guest_call(c,0xaf3430u);
        /* the skipped Kismet sequence would also fade the camera in: do it (1 s fade from black) */
        uint32_t eng=R32(self+0x14), gp=eng?R32(eng+0x1b0):0, lp=gp?R32(gp):0, pc=lp?R32(lp+0x2c):0, cam=pc?R32(pc+0x240):0;
        if(cam && (R32(cam+0x20c)&4)){ float one=1.0f, zero=0.0f; uint32_t o,z; memcpy(&o,&one,4); memcpy(&z,&zero,4);
          W32(cam+0x4bc,o); W32(cam+0x4c0,z); W32(cam+0x4c4,o); W32(cam+0x4c8,o); fprintf(stderr,"[load] camera %x fading in\n",cam); } } }
    return 1; }
  if(loadlog() && (t==0xaf2950u||t==0xaf12b0u||t==0xaf3480u||t==0xaf1240u)){ static int n[4]; int k=t==0xaf2950u?0:t==0xaf12b0u?1:t==0xaf3480u?2:3;
    if(n[k]++<20 || (n[k]%2000)==0) fprintf(stderr,"[load] call %x #%d flags %x cd %d lsi %x\n",t,n[k],R32(c->ecx+0x12c),(int)R32(c->ecx+0x144),R32(c->ecx+0x13c)); }
  if(t==0xaf12b0u){
    uint32_t self=c->ecx; uint32_t r=0;
    if(R32(self+0x13c)) r=1;
    else { uint32_t mp=R32(0x13a17f8); if(mp && R32(mp+0x9c)!=0xffffffffu) r=1; }
    if(!r && (int32_t)R32(self+0x144)>0) r=1;
    c->eax=r; c->esp+=4; return 1;
  }
  if(t==0xaf3480u){   /* RequestCloseLoadingScreen (vf 0xc0) */
    uint32_t self=c->ecx, fl=R32(self+0x12c), cd=R32(self+0x144);
    if(cd==0 && !(fl&0x40)) g_close_early=1;
    if(loadlog()){ fprintf(stderr,"[load] RequestClose flags %x countdown %d lsi %x -> early=%d ret %x stack:",fl,(int)cd,R32(self+0x13c),g_close_early,R32(c->esp)); for(int k=1;k<64;k++){ uint32_t v=R32(c->esp+4*k); if(v>0x401000&&v<0x1000000) fprintf(stderr," %x",v); } fprintf(stderr,"\n"); }
    return 0;
  }
  if(t==0xaf1240u){   /* StartLoadingCountdown (vf 0xbc) */
    uint32_t self=c->ecx;
    if(g_close_early){ W32(self+0x12c,R32(self+0x12c)|8); g_close_early=0; if(loadlog()) fprintf(stderr,"[load] StartCountdown after early close: queue close (flags %x)\n",R32(self+0x12c)); }
    return 0;
  }
  return 0;
}
