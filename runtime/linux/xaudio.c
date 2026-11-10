/* XAudio2 2.7 for the Linux runtime (the game's Wwise sound engine uses XAudio2 as its output sink).
   Guest-visible objects have vtables of magic addresses XA_BASE+4*k; the "engine" runs on a guest thread
   (so voice/engine callbacks into Wwise run as ordinary guest calls) and paces itself in 10 ms quanta.
   Output: SDL audio (float32 stereo 48 kHz); BSI_AUDIO_WAV=<file> also writes the mix to a WAV file.
   BSI_AUDIOLOG=1 logs voice creation and buffer traffic. */
#define _GNU_SOURCE
#include "hle.h"
#include <pthread.h>
#include <time.h>
#include <math.h>
#ifndef BSI_ANDROID
#include <SDL2/SDL.h>
#else
#include "android_plat.h"
#include <sys/resource.h>
typedef int SDL_AudioDeviceID;
#endif

#define XA_BASE   0xE9000000u
#define XA_THREAD (XA_BASE+0xF000u)
void guest_call(CPU*c, uint32_t fn);
uint32_t hle_thread_create(uint32_t start, uint32_t param);

enum { NM_ENGINE=16, NM_VOICE=19, NM_SOURCE=29 };
#define K_ENGINE 0
#define K_VOICE  NM_ENGINE                 /* mastering / submix voice methods */
#define K_SOURCE (NM_ENGINE+NM_VOICE)      /* source voice methods */
static uint32_t vt_engine, vt_voice, vt_source;

#define OUT_RATE 48000
#define OUT_CH 2
#define QUANT (OUT_RATE/100)               /* 10 ms */

typedef struct { uint32_t flags, bytes, data, begin, len, lbegin, llen, lcount, ctx; uint32_t pos, end, started, loops; } Buf;
typedef struct Voice {
  int used, kind;                          /* 1 source, 2 submix, 3 mastering */
  uint32_t guest, cb;                      /* guest object, IXAudio2VoiceCallback* */
  int ch, rate, bits, isfloat, align;
  int running, flush; float vol, ratio; double frac;
  Buf q[64]; int nq;
  uint64_t played;
} Voice;
#define NV 512
static Voice V[NV];
static pthread_mutex_t mu=PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t pass_mu=PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;   /* held for a whole engine pass (DestroyVoice waits on it) */
static uint32_t eng_cb[8]; static int n_eng_cb;
static int thread_started, alog=-1;
static SDL_AudioDeviceID sdl_dev; static FILE *wav; static uint32_t wav_bytes;

static int log_on(void){ if(alog<0) alog=getenv("BSI_AUDIOLOG")!=0; return alog; }
static uint32_t mkvt(int first, int n){ uint32_t v=gmalloc(4*n); for(int k=0;k<n;k++) W32(v+4*k,XA_BASE+4*(first+k)); return v; }
static void init_vt(void){ if(vt_engine) return; vt_engine=mkvt(K_ENGINE,NM_ENGINE); vt_voice=mkvt(K_VOICE,NM_VOICE); vt_source=mkvt(K_SOURCE,NM_SOURCE); }
static Voice *voice_of(uint32_t g){ uint32_t i=R32(g+4); return (i<NV && V[i].used && V[i].guest==g)?&V[i]:0; }
static uint32_t new_voice(int kind, uint32_t vt){
  pthread_mutex_lock(&mu); int i=1; while(i<NV && V[i].used) i++;
  if(i>=NV){ pthread_mutex_unlock(&mu); return 0; }
  memset(&V[i],0,sizeof V[i]); V[i].used=1; V[i].kind=kind; V[i].vol=1; V[i].ratio=1;
  uint32_t g=gmalloc(16); W32(g,vt); W32(g+4,i); V[i].guest=g; pthread_mutex_unlock(&mu); return g;
}
uint32_t xaudio_create(void){ init_vt(); uint32_t o=gmalloc(16); W32(o,vt_engine); W32(o+4,1); return o; }

static void out_open(void){
  static int done; if(done) return; done=1;
  const char *w=getenv("BSI_AUDIO_WAV"); if(w){ wav=fopen(w,"wb"); if(wav){ uint8_t h[44]={0}; fwrite(h,1,44,wav); } }
  if(getenv("BSI_NOAUDIO")) return;
#ifdef BSI_ANDROID
  sdl_dev=1; fprintf(stderr,"[xaudio] Android AAudio output\n"); return;
#else
  if(SDL_InitSubSystem(SDL_INIT_AUDIO)){ fprintf(stderr,"[xaudio] SDL audio: %s (continuing silently)\n",SDL_GetError()); return; }
  SDL_AudioSpec want={0}, have; want.freq=OUT_RATE; want.format=AUDIO_F32SYS; want.channels=OUT_CH; want.samples=1024;
  sdl_dev=SDL_OpenAudioDevice(0,0,&want,&have,0);
  if(!sdl_dev) fprintf(stderr,"[xaudio] no audio device: %s (continuing silently)\n",SDL_GetError());
  else { SDL_PauseAudioDevice(sdl_dev,0); fprintf(stderr,"[xaudio] SDL audio out %d Hz %d ch (%s)\n",have.freq,have.channels,SDL_GetCurrentAudioDriver()); }
#endif
}
static void wav_finish(void){
  if(!wav) return; long pos=ftell(wav); uint8_t h[44]; uint32_t sr=OUT_RATE, br=OUT_RATE*8, data=wav_bytes;
  memcpy(h,"RIFF",4); uint32_t rs=36+data; memcpy(h+4,&rs,4); memcpy(h+8,"WAVEfmt ",8); uint32_t fs=16; memcpy(h+16,&fs,4);
  uint16_t tag=3, ch=2, ba=8, bits=32; memcpy(h+20,&tag,2); memcpy(h+22,&ch,2); memcpy(h+24,&sr,4); memcpy(h+28,&br,4); memcpy(h+32,&ba,2); memcpy(h+34,&bits,2);
  memcpy(h+36,"data",4); memcpy(h+40,&data,4); fseek(wav,0,SEEK_SET); fwrite(h,1,44,wav); fseek(wav,pos,SEEK_SET); fflush(wav);
}

/* read one frame (up to 2 output channels) from a source buffer */
static void frame_at(Voice *v, Buf *b, uint32_t f, float *l, float *r){
  uint32_t a=b->data+f*v->align; float s0=0,s1=0;
  if(v->isfloat){ uint32_t x=R32(a); memcpy(&s0,&x,4); if(v->ch>1){ x=R32(a+4); memcpy(&s1,&x,4);} else s1=s0; }
  else if(v->bits==16){ s0=(int16_t)R16(a)/32768.f; s1=v->ch>1?(int16_t)R16(a+2)/32768.f:s0; }
  else if(v->bits==8){ s0=(R8(a)-128)/128.f; s1=v->ch>1?(R8(a+1)-128)/128.f:s0; }
  else if(v->bits==24){ int32_t x=(int32_t)((R8(a)<<8)|(R8(a+1)<<16)|(R8(a+2)<<24)); s0=x/2147483648.f; if(v->ch>1){ x=(int32_t)((R8(a+3)<<8)|(R8(a+4)<<16)|(R8(a+5)<<24)); s1=x/2147483648.f; } else s1=s0; }
  else if(v->bits==32){ s0=(int32_t)R32(a)/2147483648.f; s1=v->ch>1?(int32_t)R32(a+4)/2147483648.f:s0; }
  *l=s0; *r=s1;
}

typedef struct { uint32_t cb; int m; uint32_t a, b; int nargs; } Ev;   /* callback event */

static unsigned long g_starved;
static void engine_pass(CPU *c, float *mix){
  Ev ev[1024]; int ne=0;
  for(int k=0;k<n_eng_cb;k++){ ev[ne++]=(Ev){eng_cb[k],1,0,0,0}; }   /* OnProcessingPassStart */
  /* fire pass-start events first (Wwise fills buffers in OnVoiceProcessingPassStart) */
  pthread_mutex_lock(&mu);
  for(int i=1;i<NV;i++){ Voice *v=&V[i]; if(!v->used||v->kind!=1||!v->running||!v->cb) continue;
    uint64_t have=0; for(int k=0;k<v->nq;k++) have+=v->q[k].end-v->q[k].pos;
    uint32_t need=(uint32_t)ceil(QUANT*(double)v->rate/OUT_RATE*v->ratio)+1;
    uint32_t req=have>=need?0:(uint32_t)(need-have)*v->align;
    /* Never tell Wwise it is about to starve: it answers an urgent BytesRequired by submitting the next buffer of its
       ring even if it has not rendered it yet, and that stale buffer is heard as an echo 21/43 ms later.
       Its normal refill (driven by OnBufferEnd) keeps the queue fed; we wait for it below. */
    req=0;
    if(ne<1000) ev[ne++]=(Ev){v->cb,0,req,0,1}; }
  pthread_mutex_unlock(&mu);
  for(int k=0;k<ne;k++){ Ev *e=&ev[k]; if(e->nargs) PUSH32(c,e->a); PUSH32(c,e->cb); guest_call(c,R32(R32(e->cb)+4*e->m)); }
  ne=0;
  /* Wwise renders on its own thread after OnVoiceProcessingPassStart; give it up to 8 ms to submit before mixing,
     instead of mixing a silent (starved) slice, which is heard as stutter/flutter */
  for(int w=0;w<8;w++){ int ok=1; pthread_mutex_lock(&mu);
    for(int i=1;i<NV;i++){ Voice *v=&V[i]; if(!v->used||v->kind!=1||!v->running||!v->cb) continue;
      uint64_t have=0; for(int k=0;k<v->nq;k++) have+=v->q[k].end-v->q[k].pos;
      if(have < (uint64_t)ceil(QUANT*(double)v->rate/OUT_RATE*v->ratio)+1){ ok=0; break; } }
    pthread_mutex_unlock(&mu); if(ok) break; if(w==7) g_starved++; struct timespec ts={0,1000000}; nanosleep(&ts,0); }
  /* mix */
  memset(mix,0,sizeof(float)*QUANT*OUT_CH);
  pthread_mutex_lock(&mu);
  for(int i=1;i<NV;i++){ Voice *v=&V[i]; if(!v->used||v->kind!=1) continue;
    if(v->flush){ v->flush=0; for(int k=0;k<v->nq;k++) if(v->cb && ne<1000) ev[ne++]=(Ev){v->cb,4,v->q[k].ctx,0,1}; v->nq=0; }
    if(!v->running) continue;
    double step=(double)v->rate/OUT_RATE*v->ratio; int starved=0;
    for(int s=0;s<QUANT;s++){
      if(!v->nq){ starved=1; break; }
      Buf *b=&v->q[0];
      if(!b->started){ b->started=1; if(v->cb && ne<1000) ev[ne++]=(Ev){v->cb,3,b->ctx,0,1}; }
      float l,r; frame_at(v,b,b->pos,&l,&r); mix[2*s]+=l*v->vol; mix[2*s+1]+=r*v->vol;
      v->frac+=step; uint32_t adv=(uint32_t)v->frac; v->frac-=adv; b->pos+=adv; v->played+=adv;
      while(v->nq && v->q[0].pos>=v->q[0].end){ Buf *h=&v->q[0];
        if(h->lcount && h->loops<h->lcount){ h->loops++; uint32_t over=h->pos-h->end; h->pos=h->lbegin+over; h->end=h->llen?h->lbegin+h->llen:h->begin+(h->len?h->len:h->bytes/v->align);
          if(v->cb && ne<1000) ev[ne++]=(Ev){v->cb,5,h->ctx,0,1}; continue; }
        uint32_t ctx=h->ctx, fl=h->flags, over=h->pos-h->end;
        memmove(&v->q[0],&v->q[1],sizeof(Buf)*(v->nq-1)); v->nq--;
        if(v->nq) v->q[0].pos+=over;
        if(v->cb && ne<1000) ev[ne++]=(Ev){v->cb,4,ctx,0,1};
        if((fl&0x40) && v->cb && ne<1000) ev[ne++]=(Ev){v->cb,2,0,0,0};
      }
    }
    (void)starved;
    if(v->cb && ne<1000) ev[ne++]=(Ev){v->cb,1,0,0,0};   /* OnVoiceProcessingPassEnd */
  }
  pthread_mutex_unlock(&mu);
  for(int k=0;k<n_eng_cb;k++) if(ne<1020) ev[ne++]=(Ev){eng_cb[k],0,0,0,0};   /* OnProcessingPassEnd */
  for(int k=0;k<ne;k++){ Ev *e=&ev[k]; if(e->nargs) PUSH32(c,e->a); PUSH32(c,e->cb); guest_call(c,R32(R32(e->cb)+4*e->m)); }
}

static void engine_thread(CPU *c){
#ifdef BSI_ANDROID
  { int r=setpriority(PRIO_PROCESS,0,-16); fprintf(stderr,"[xaudio] engine thread priority -16: %d\n",r); }
#endif
  out_open(); float mix[QUANT*OUT_CH]; struct timespec next; clock_gettime(CLOCK_MONOTONIC,&next); uint64_t passes=0;
  for(;;){
    pthread_mutex_lock(&pass_mu); engine_pass(c,mix); pthread_mutex_unlock(&pass_mu);
    for(int k=0;k<QUANT*OUT_CH;k++){ float x=mix[k]; mix[k]=x>1?1:(x<-1?-1:x); }
#ifdef BSI_ANDROID
    if(sdl_dev) android_audio_write(mix,QUANT);
#else
    if(sdl_dev && SDL_GetQueuedAudioSize(sdl_dev)<OUT_RATE*OUT_CH*4/5) SDL_QueueAudio(sdl_dev,mix,sizeof mix);
#endif
    if(wav){ fwrite(mix,1,sizeof mix,wav); wav_bytes+=sizeof mix; if((passes%100)==0) wav_finish(); }
    if(log_on() && (passes%1000)==0){ int nv=0, nr=0; for(int i=1;i<NV;i++) if(V[i].used&&V[i].kind==1){ nv++; nr+=V[i].running; } float pk=0; for(int k=0;k<QUANT*OUT_CH;k++) pk=fmaxf(pk,fabsf(mix[k])); fprintf(stderr,"[xaudio] pass %lu: %d source voices (%d running), peak %.3f\n",(unsigned long)passes,nv,nr,pk); }
    passes++;
#ifdef BSI_ANDROID
    /* pace by the output buffer: keep ~150 ms queued; produce immediately when below, so a slow pass is caught up */
    /* real-time pacing (one 10 ms pass per 10 ms), steered by the output ring: catch up immediately when it runs low,
       wait when it is above target */
    if(sdl_dev){ int q=android_audio_queued(), tgt=android_audio_target();
      { static time_t lt; time_t t=time(0); if(t-lt>=10){ lt=t; fprintf(stderr,"[xaudio] starved passes %lu, ring %d/%d\n",g_starved,q,tgt); } }
      /* steady 10 ms cadence, gently steered (+-2 ms) by the ring level - no bursts, so Wwise always gets a full
         pass of time to render the next buffer */
      long adj=0; if(q>=0 && q<tgt*3/4) adj=-2000000; else if(q>tgt*5/4) adj=2000000;
      next.tv_nsec+=adj; if(next.tv_nsec<0){ next.tv_nsec+=1000000000; next.tv_sec--; } if(next.tv_nsec>=1000000000){ next.tv_nsec-=1000000000; next.tv_sec++; } }
#endif
    next.tv_nsec+=10000000; if(next.tv_nsec>=1000000000){ next.tv_nsec-=1000000000; next.tv_sec++; }
    struct timespec now; clock_gettime(CLOCK_MONOTONIC,&now);
    if(now.tv_sec>next.tv_sec+1) next=now;   /* fell far behind: resync */
    clock_nanosleep(CLOCK_MONOTONIC,TIMER_ABSTIME,&next,0);
  }
}
static void start_thread(void){ if(__atomic_exchange_n(&thread_started,1,__ATOMIC_SEQ_CST)) return; hle_thread_create(XA_THREAD,0); }

static void parse_fmt(Voice *v, uint32_t f){
  uint16_t tag=R16(f); v->ch=R16(f+2); v->rate=R32(f+4); v->align=R16(f+12); v->bits=R16(f+14);
  if(tag==0xFFFE) tag=(uint16_t)R32(f+24);
  v->isfloat=(tag==3); if(!v->align) v->align=v->ch*v->bits/8; if(!v->align) v->align=4;
  if(log_on()) fprintf(stderr,"[xaudio] source voice fmt tag %u ch %d rate %d bits %d align %d cb %08x\n",R16(f),v->ch,v->rate,v->bits,v->align,v->cb);
}

#define A(k) R32(c->esp+8+4*(k))
#define THIS R32(c->esp+4)
#define RET(nargs,v) do{ c->eax=(uint32_t)(v); c->esp+=8+4*(nargs); return 1; }while(0)
static float AF(CPU*c,int k){ uint32_t x=A(k); float f; memcpy(&f,&x,4); return f; }

static int engine_method(CPU *c, int m){
  uint32_t self=THIS;
  switch(m){
  case 0: W32(A(1),self); W32(self+4,R32(self+4)+1); RET(2,0);          /* QueryInterface */
  case 1: W32(self+4,R32(self+4)+1); RET(0,R32(self+4));
  case 2: { uint32_t r=R32(self+4); if(r>1) r--; W32(self+4,r); RET(0,r); }
  case 3: W32(A(0),1); RET(1,0);                                          /* GetDeviceCount */
  case 4: { uint32_t d=A(1); memset(MEMP(d),0,1068); const char *id="bsi-quest-audio", *nm="BSI-Quest audio (SDL)";
      for(int k=0;id[k];k++) W16(d+2*k,id[k]); for(int k=0;nm[k];k++) W16(d+512+2*k,nm[k]); W32(d+1024,0xF);
      uint32_t f=d+1028; W16(f,0xFFFE); W16(f+2,OUT_CH); W32(f+4,OUT_RATE); W32(f+8,OUT_RATE*OUT_CH*4); W16(f+12,OUT_CH*4); W16(f+14,32); W16(f+16,22);
      W16(f+18,32); W32(f+20,3); W32(f+24,3); W16(f+28,0); W16(f+30,0x10); const uint8_t t[8]={0x80,0,0,0xaa,0,0x38,0x9b,0x71}; memcpy(MEMP(f+32),t,8);
      RET(2,0); }
  case 5: RET(2,0);                                                        /* Initialize */
  case 6: if(n_eng_cb<8) eng_cb[n_eng_cb++]=A(0); RET(1,0);
  case 7: for(int k=0;k<n_eng_cb;k++) if(eng_cb[k]==A(0)){ eng_cb[k]=eng_cb[--n_eng_cb]; break; } RET(1,0);
  case 8: { uint32_t g=new_voice(1,vt_source); if(!g) RET(7,0x80004005u); Voice *v=voice_of(g); v->cb=A(4); parse_fmt(v,A(1)); W32(A(0),g); RET(7,0); }
  case 9: { uint32_t g=new_voice(2,vt_voice); Voice *v=voice_of(g); v->ch=A(1); v->rate=A(2); W32(A(0),g); RET(7,0); }
  case 10: { uint32_t g=new_voice(3,vt_voice); Voice *v=voice_of(g); v->ch=A(1)?A(1):OUT_CH; v->rate=A(2)?A(2):OUT_RATE;
      if(log_on()) fprintf(stderr,"[xaudio] mastering voice %d ch %d Hz\n",v->ch,v->rate); W32(A(0),g); start_thread(); RET(6,0); }
  case 11: start_thread(); RET(0,0);
  case 12: RET(0,0);
  case 13: RET(1,0);
  case 14: memset(MEMP(A(0)),0,64); RET(1,0);
  case 15: RET(2,0);
  }
  RET(0,0);
}
static int voice_method(CPU *c, int m, int src){
  uint32_t self=THIS; Voice *v=voice_of(self);
  switch(m){
  case 0: { uint32_t d=A(0); W32(d,0); W32(d+4,v?v->ch:2); W32(d+8,v?v->rate:OUT_RATE); RET(1,0); }   /* GetVoiceDetails */
  case 1: RET(1,0); case 2: RET(1,0); case 3: RET(2,0); case 4: RET(2,0);
  case 5: if(A(1)) W32(A(1),1); RET(2,0);
  case 6: RET(4,0);
  case 7: memset(MEMP(A(1)),0,A(2)); RET(3,0);
  case 8: RET(2,0);
  case 9: { uint32_t p=A(0); W32(p,0); W32(p+4,0x3f800000u); W32(p+8,0x3f800000u); RET(1,0); }
  case 10: RET(3,0);
  case 11: { uint32_t p=A(1); W32(p,0); W32(p+4,0x3f800000u); W32(p+8,0x3f800000u); RET(2,0); }
  case 12: if(v) v->vol=AF(c,0); RET(2,0);
  case 13: { float f=v?v->vol:1; uint32_t x; memcpy(&x,&f,4); W32(A(0),x); RET(1,0); }
  case 14: RET(3,0);
  case 15: for(uint32_t k=0;k<A(0);k++) W32(A(1)+4*k,0x3f800000u); RET(2,0);
  case 16: RET(5,0);
  case 17: { uint32_t n=A(1)*A(2); for(uint32_t k=0;k<n;k++) W32(A(3)+4*k,0); RET(4,0); }
  case 18: if(v){ pthread_mutex_lock(&mu); v->used=0; pthread_mutex_unlock(&mu); pthread_mutex_lock(&pass_mu); pthread_mutex_unlock(&pass_mu); } RET(0,0);   /* DestroyVoice */
  }
  if(!src) RET(0,0);
  switch(m){
  case 19: if(v){ v->running=1; } RET(2,0);                                /* Start */
  case 20: if(v){ v->running=0; } RET(2,0);                                /* Stop */
  case 21: { uint32_t b=A(0); if(!v) RET(2,0x80004005u); pthread_mutex_lock(&mu);   /* SubmitSourceBuffer */
      if(v->nq>=64){ pthread_mutex_unlock(&mu); RET(2,0x88960001u); }
      Buf *q=&v->q[v->nq++]; memset(q,0,sizeof *q);
      q->flags=R32(b); q->bytes=R32(b+4); q->data=R32(b+8); q->begin=R32(b+12); q->len=R32(b+16); q->lbegin=R32(b+20); q->llen=R32(b+24); q->lcount=R32(b+28); q->ctx=R32(b+32);
      q->pos=q->begin; q->end=q->len?q->begin+q->len:q->bytes/v->align; if(q->lcount==255) q->lcount=0xffffffffu;
      pthread_mutex_unlock(&mu);
      { static unsigned n; if(log_on() && (n++%2000)==0) fprintf(stderr,"[xaudio] SubmitSourceBuffer #%u %u bytes ctx %08x\n",n,q->bytes,q->ctx); }
      RET(2,0); }
  case 22: if(v){ pthread_mutex_lock(&mu); v->flush=1; pthread_mutex_unlock(&mu); } RET(0,0);   /* FlushSourceBuffers: buffers are ended (OnBufferEnd) on the next pass */
  case 23: RET(0,0);
  case 24: RET(1,0);
  case 25: { uint32_t s=A(0); pthread_mutex_lock(&mu); W32(s,v&&v->nq?v->q[0].ctx:0); W32(s+4,v?v->nq:0); W64(s+8,v?v->played:0); pthread_mutex_unlock(&mu); RET(1,0); }   /* GetState */
  case 26: if(v) v->ratio=AF(c,0)>0?AF(c,0):1; RET(2,0);
  case 27: { float f=v?v->ratio:1; uint32_t x; memcpy(&x,&f,4); W32(A(0),x); RET(1,0); }
  case 28: if(v) v->rate=A(0); RET(1,0);
  }
  RET(0,0);
}
int xaudio_dispatch(CPU *c, uint32_t pc){
  if(pc<XA_BASE || pc>=XA_BASE+0x10000u) return 0;
  if(pc==XA_THREAD){ engine_thread(c); return 1; }
  int k=(pc-XA_BASE)/4;
  if(k<K_VOICE) return engine_method(c,k);
  if(k<K_SOURCE) return voice_method(c,k-K_VOICE,0);
  if(k<K_SOURCE+NM_SOURCE) return voice_method(c,k-K_SOURCE,1);
  fprintf(stderr,"[xaudio] bad method %d\n",k); c->esp+=4; return 1;
}
