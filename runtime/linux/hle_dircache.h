#pragma once
/* hle_dircache.h (BSI_DIRCACHE, default OFF) — case-insensitive guest (Windows) -> host path resolution with a two-level cache.
   Port of Fable's path_cache.h (inbox 2026-10-04_fable_patches/filecache), renamed pc_ -> dc_ / BSI_PATHCACHE -> BSI_DIRCACHE so
   it cannot be confused with our hle_pathcache.c (BSI_PATHCACHE, HLE_HOT H3: memoises probe RESULTS per exact guest string at the
   handler level). This one sits one level lower, inside g2h(): it removes the per-component stat() and the full readdir() on
   every miss / case mismatch, and answers exact-name FindFirstFileW from the table. The two stack (H3 calls the original handler,
   which calls g2h). Notes: notes/FABLE_REVIEW.md. Header-only and self-contained (no guest memory, no HLE state) so it can be
   unit-tested on the host (regress/fable_rt/dc_test.c) and used by hle_file.c.

   The reference algorithm (dc_resolve_naive, the old g2h loop): for every path component stat() the exact-case candidate;
   on a miss opendir()+readdir() the whole parent and take the first strcasecmp() match; if nothing matches keep the
   component in the case the guest gave (so callers can create new files). UE3 probes thousands of files that do not
   exist (localisation variants, optional packages, .ini, DLC, texture-cache variants), and every one of those probes was
   a full readdir() of a directory with thousands of entries.

   Level 1 — directory tables: one readdir() per host directory builds a hash table (case-folded name -> real name, is_dir)
             keyed by the host directory path. Component lookups, hits and misses alike, are then O(1) without syscalls.
             A directory that does not exist (ENOENT/ENOTDIR) is cached as an empty "missing" table.
   Level 2 — path entries: (host base, case-folded guest-relative path) -> host path of the components that exist (real
             case) + how many that is, so repeated probes of the same file never walk components again. For a path whose
             tail does not exist the tail is re-appended from the query in the case the guest gave (the g2h quirk callers
             rely on when they create files), whatever case the entry was created with. Paths through a directory that
             holds names differing only in case are never cached (the exact-case entry must win there, as with stat()).
   Invalidation: dc_invalidate(host_path) after anything that changes a directory listing (file created/opened for
             writing, mkdir, unlink, move, copy) drops the parent's table, every table below it and every path entry below
             it. Tables and entries under the TTL prefix (C:\BSIUSER, the only tree written in practice) also expire after
             DC_TTL_NS (2 s) as a belt-and-braces measure. Game data under C:\BSI is treated as read-only unless the HLE
             itself writes there.
   Locking:  one mutex; lookups hold it for a few hundred ns and make no syscalls. The first readdir() of a directory
             runs with the lock released; the finished table is inserted afterwards unless an invalidation that could
             affect it happened meanwhile (generation counter + ring of recently invalidated directories) or another
             thread inserted the same directory first.
   Bounds:   DC_MAX_DIRS tables / DC_MAX_DIR_BYTES (LRU eviction), DC_MAX_PATHS entries (4-way sets, oldest-stamp eviction).
   Switches: BSI_DIRCACHE=1 enables it (unset/0 = the naive walk, i.e. the previous g2h loop); BSI_DIRCACHE_STAT=1 prints a
             "[dircache] ..." stats line every 30 s; BSI_FILELOG=1 also logs every directory scan and invalidation. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#include <dirent.h>
#include <sys/stat.h>

#ifndef DC_MAX_DIRS
#define DC_MAX_DIRS 4096                 /* directory tables kept (LRU) */
#endif
#ifndef DC_MAX_DIR_BYTES
#define DC_MAX_DIR_BYTES (64u<<20)       /* total bytes of directory tables */
#endif
#ifndef DC_MAX_PATHS
#define DC_MAX_PATHS 65536               /* path entries (power of two, multiple of DC_WAYS) */
#endif
#ifndef DC_TTL_NS
#define DC_TTL_NS 2000000000ull          /* tables/entries under the TTL prefix expire after 2 s */
#endif
#ifndef DC_STAT_NS
#define DC_STAT_NS 30000000000ull        /* stats line period with BSI_DIRCACHE_STAT=1 */
#endif
#define DC_MIN_DIRS 16                   /* never evict below this many tables: a walk in progress needs its own ones alive */
#define DC_WAYS 4
#define DC_DIRH 4096                     /* directory hash buckets */
#define DC_INVRING 32                    /* recently invalidated directories remembered for the build race check */
#define DC_HOSTMAX 1024                  /* host path buffers (callers use char h[1024]) */
#define DC_KEYMAX 3072                   /* base + folded relative path */
#define DC_TRIES 8                       /* table builds per lookup before falling back to the naive walk */
#define DC_UNUSED __attribute__((unused))

typedef struct { uint32_t h, off; uint8_t isdir; } DCEnt;      /* isdir: 0 no, 1 yes, 2 unknown (DT_UNKNOWN / symlink) */
typedef struct DCDir {
  struct DCDir *hn, *lp, *ln;                                   /* hash chain; LRU prev (newer) / next (older) */
  char *path; uint32_t plen, ph;                                /* host directory path = key */
  uint64_t built;                                               /* CLOCK_MONOTONIC ns */
  uint32_t n, mask, *slot; DCEnt *ent; char *names; size_t bytes;   /* open addressing: slot -> entry index+1 */
  uint8_t missing, ttl, dups;                                   /* missing: opendir ENOENT/ENOTDIR; ttl: under the TTL prefix; dups: names differing only in case */
} DCDir;
typedef struct { char *s; uint64_t exp; uint32_t h, stamp; uint16_t kl, nfound; uint8_t exists; } DCPath;   /* s = key\0host-prefix\0; nfound = components that exist */
typedef struct { uint64_t lookup, hit, hitneg, miss, scan, inval, inval_dirs, inval_paths, evict_dir, evict_path, fallback, dirlookup; uint32_t ndirs, npaths; size_t dir_bytes; } DCStats;

static struct {
  pthread_mutex_t mu; int inited, enabled, stat_on, flog;
  char ttl_prefix[DC_HOSTMAX]; size_t ttl_len;
  uint64_t gen; struct { uint64_t gen; char dir[DC_HOSTMAX]; } inv[DC_INVRING];
  DCDir *dirh[DC_DIRH], *head, *tail;
  DCPath *paths; uint32_t stamp;
  DCStats st; uint64_t next_stat;
} DC = { .mu = PTHREAD_MUTEX_INITIALIZER };

static inline uint32_t dc_hash(const char *s, size_t n){ uint32_t h=2166136261u; for(size_t i=0;i<n;i++){ uint32_t c=(uint8_t)s[i]; if(c-'A'<26u) c+=32; h=(h^c)*16777619u; } return h; }   /* FNV-1a over ASCII-folded bytes (same folding as strcasecmp in the C locale) */
static inline uint64_t dc_now(void){ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (uint64_t)t.tv_sec*1000000000ull+(uint64_t)t.tv_nsec; }
static inline int dc_under(const char *p, const char *d, size_t dl){ return !strncmp(p,d,dl) && (p[dl]==0||p[dl]=='/'); }   /* p == d or p inside d */
static inline int dc_haswild(const char *pat){ for(;*pat;pat++) if(*pat=='*'||*pat=='?'||*pat=='['||*pat=='\\') return 1; return 0; }
static void dc_app(char *host, size_t hostsz, const char *c, size_t cl){   /* host += "/" + c (bounded; the old code overflowed instead) */
  size_t hl=strlen(host); if(hl+1>=hostsz) return; host[hl++]='/'; if(cl>hostsz-1-hl) cl=hostsz-1-hl; memcpy(host+hl,c,cl); host[hl+cl]=0; }

/* reads the switches once; ttl_prefix (may be 0) = host directory whose tables/entries expire (C:\BSIUSER's host dir) */
static void dc_init(const char *ttl_prefix){
  pthread_mutex_lock(&DC.mu);
  if(!DC.inited){ const char *e=getenv("BSI_DIRCACHE"); DC.enabled=e && e[0] && e[0]!='0';
    e=getenv("BSI_DIRCACHE_STAT"); DC.stat_on=e && e[0] && e[0]!='0'; DC.flog=getenv("BSI_FILELOG")!=0;
    DC.paths=calloc(DC_MAX_PATHS,sizeof(DCPath)); if(!DC.paths) DC.enabled=0; DC.next_stat=dc_now()+DC_STAT_NS; }
  if(ttl_prefix){ snprintf(DC.ttl_prefix,sizeof DC.ttl_prefix,"%s",ttl_prefix); DC.ttl_len=strlen(DC.ttl_prefix); while(DC.ttl_len>1 && DC.ttl_prefix[DC.ttl_len-1]=='/') DC.ttl_prefix[--DC.ttl_len]=0; }
  __atomic_store_n(&DC.inited,1,__ATOMIC_RELEASE);
  pthread_mutex_unlock(&DC.mu);
}
static inline int dc_ready(void){ if(!__atomic_load_n(&DC.inited,__ATOMIC_ACQUIRE)) dc_init(0); return DC.enabled; }   /* hle_init calls dc_init(USERD) before any thread exists; this covers stand-alone use */
static inline int dc_ttl_path(const char *host){ return DC.ttl_len && dc_under(host,DC.ttl_prefix,DC.ttl_len); }

/* the old g2h loop, kept verbatim in behaviour: the reference and the fallback. returns 1 if the last component was found */
static int dc_resolve_naive(const char *base, const char *rest, char *host, size_t hostsz){
  snprintf(host,hostsz,"%s",base); char comp[1024]; const char *p=rest; int ex=1;
  while(*p){
    while(*p=='\\') p++;
    if(!*p) break;
    int k=0; while(*p && *p!='\\'){ if(k<(int)sizeof comp-1) comp[k++]=*p; p++; } comp[k]=0;
    char cand[2048]; snprintf(cand,sizeof cand,"%s/%s",host,comp);
    struct stat st;
    if(stat(cand,&st)==0){ snprintf(host,hostsz,"%s",cand); continue; }
    DIR *d=opendir(host); int found=0;
    if(d){ struct dirent *e; while((e=readdir(d))){ if(!strcasecmp(e->d_name,comp)){ snprintf(cand,sizeof cand,"%s/%s",host,e->d_name); found=1; break; } } closedir(d); }
    if(!found) ex=0;
    snprintf(host,hostsz,"%s",cand);
  }
  return ex;
}

/* ---- directory tables ---- */
static void dc_dir_free(DCDir *t){ free(t->path); free(t->slot); free(t->ent); free(t->names); free(t); }
static void dc_dir_unlink_locked(DCDir *t){
  DCDir **pp=&DC.dirh[t->ph&(DC_DIRH-1)]; while(*pp && *pp!=t) pp=&(*pp)->hn; if(*pp) *pp=t->hn;
  if(t->lp) t->lp->ln=t->ln; else DC.head=t->ln; if(t->ln) t->ln->lp=t->lp; else DC.tail=t->lp;
  DC.st.ndirs--; DC.st.dir_bytes-=t->bytes; }
static void dc_lru_front_locked(DCDir *t){ if(DC.head==t) return; t->lp->ln=t->ln; if(t->ln) t->ln->lp=t->lp; else DC.tail=t->lp;
  t->lp=0; t->ln=DC.head; DC.head->lp=t; DC.head=t; }
static DCDir *dc_dir_find_locked(const char *path, size_t plen, uint32_t ph, uint64_t now){
  for(DCDir *t=DC.dirh[ph&(DC_DIRH-1)]; t; t=t->hn) if(t->ph==ph && t->plen==plen && !memcmp(t->path,path,plen)){
    if(t->ttl && (int64_t)(now-t->built)>(int64_t)DC_TTL_NS){ dc_dir_unlink_locked(t); dc_dir_free(t); return 0; }   /* expired: rebuild */
    dc_lru_front_locked(t); return t; }
  return 0; }
static void dc_dir_insert_locked(DCDir *t){
  t->hn=DC.dirh[t->ph&(DC_DIRH-1)]; DC.dirh[t->ph&(DC_DIRH-1)]=t; t->lp=0; t->ln=DC.head; if(DC.head) DC.head->lp=t; DC.head=t; if(!DC.tail) DC.tail=t;
  DC.st.ndirs++; DC.st.dir_bytes+=t->bytes;
  while(DC.tail!=t && DC.st.ndirs>DC_MIN_DIRS && (DC.st.ndirs>DC_MAX_DIRS || DC.st.dir_bytes>DC_MAX_DIR_BYTES)){ DCDir *v=DC.tail; dc_dir_unlink_locked(v); dc_dir_free(v); DC.st.evict_dir++; } }
/* one readdir() of `path` -> table. Called WITHOUT the lock. ENOENT/ENOTDIR give a "missing" table; other errors
   (EACCES, EMFILE, EIO...) return 0 with *err set and are not cached. */
static DCDir *dc_dir_build(const char *path, uint64_t now, int *err){
  DCDir *t=calloc(1,sizeof *t); if(!t){ *err=ENOMEM; return 0; }
  t->plen=(uint32_t)strlen(path); t->path=malloc(t->plen+1); if(!t->path){ free(t); *err=ENOMEM; return 0; } memcpy(t->path,path,t->plen+1);
  t->ph=dc_hash(path,t->plen); t->built=now; t->ttl=(uint8_t)dc_ttl_path(path); t->bytes=sizeof *t+t->plen+1;
  DIR *d=opendir(path);
  if(!d){ int e=errno; if(e==ENOENT||e==ENOTDIR){ t->missing=1; return t; } dc_dir_free(t); *err=e; return 0; }
  uint32_t cap=64, n=0; size_t ncap=4096, nl=0; DCEnt *ent=malloc(cap*sizeof *ent); char *names=malloc(ncap); struct dirent *de;
  while(ent && names && (de=readdir(d))){
    size_t l=strlen(de->d_name);
    if(n==cap){ DCEnt *ne=realloc(ent,2*cap*sizeof *ent); if(!ne){ free(ent); ent=0; break; } ent=ne; cap*=2; }
    if(nl+l+1>ncap){ size_t nc=ncap; while(nl+l+1>nc) nc*=2; char *nn=realloc(names,nc); if(!nn){ free(names); names=0; break; } names=nn; ncap=nc; }
    ent[n].h=dc_hash(de->d_name,l); ent[n].off=(uint32_t)nl; ent[n].isdir=de->d_type==DT_DIR?1:(de->d_type==DT_UNKNOWN||de->d_type==DT_LNK)?2:0;
    memcpy(names+nl,de->d_name,l+1); nl+=l+1; n++;
  }
  closedir(d);
  uint32_t ns=16; while(ns<2*n) ns*=2; uint32_t *slot=(ent&&names)?calloc(ns,sizeof *slot):0;
  if(!slot){ free(ent); free(names); dc_dir_free(t); *err=ENOMEM; return 0; }
  for(uint32_t i=0;i<n;i++){ uint32_t s=ent[i].h&(ns-1);   /* readdir order is kept along each probe sequence */
    while(slot[s]){ const DCEnt *o=&ent[slot[s]-1]; if(o->h==ent[i].h && !strcasecmp(names+o->off,names+ent[i].off)) t->dups=1; s=(s+1)&(ns-1); } slot[s]=i+1; }
  t->n=n; t->mask=ns-1; t->slot=slot; t->ent=ent; t->names=names; t->bytes+=ns*sizeof *slot+cap*sizeof *ent+ncap;
  return t;
}
/* same answer as the old stat()-then-readdir() pair: the exact-case entry if there is one, else the first entry in
   readdir order that strcasecmp() considers equal */
static int dc_dir_lookup_in(const DCDir *t, const char *c, size_t cl, const char **real, int *isdir){
  if(t->missing || !t->n) return 0;
  uint32_t h=dc_hash(c,cl), s=h&t->mask; const char *first=0; int fd=0;
  for(;;){ uint32_t i=t->slot[s]; if(!i) break; const DCEnt *e=&t->ent[i-1];
    if(e->h==h){ const char *nm=t->names+e->off; if(!strncasecmp(nm,c,cl) && !nm[cl]){ if(!memcmp(nm,c,cl)){ *real=nm; *isdir=e->isdir; return 1; } if(!first){ first=nm; fd=e->isdir; } } }
    s=(s+1)&t->mask; }
  if(first){ *real=first; *isdir=fd; return 1; } return 0;
}
/* did an invalidation since gen0 touch `dir` (or a parent of it)? ring overflow counts as yes */
static int dc_dirty_locked(const char *dir, uint64_t gen0){
  if(DC.gen==gen0) return 0;
  if(DC.gen-gen0>DC_INVRING) return 1;
  for(uint64_t g=gen0+1; g<=DC.gen; g++){ if(DC.inv[g%DC_INVRING].gen!=g) return 1; const char *d=DC.inv[g%DC_INVRING].dir; if(dc_under(dir,d,strlen(d))) return 1; }
  return 0; }
/* lock held on entry and on return. Builds the table of `need` with the lock released, inserts it unless it was
   superseded (another thread built it, or an invalidation touched it meanwhile: the caller re-walks and retries).
   returns 0 when the directory cannot be read (caller falls back to the naive walk). */
static int dc_dir_make_locked(const char *need, uint64_t now){
  uint64_t gen0=DC.gen; pthread_mutex_unlock(&DC.mu);
  int err=0; DCDir *t=dc_dir_build(need,now,&err);
  if(DC.flog){ if(t) fprintf(stderr,"[file] dirscan '%s': %s%u entries\n",need,t->missing?"missing, ":"",t->n); else fprintf(stderr,"[file] dirscan '%s' failed: %s\n",need,strerror(err)); }
  pthread_mutex_lock(&DC.mu); DC.st.scan++;
  if(!t) return 0;
  size_t nl=strlen(need);
  if(dc_dirty_locked(need,gen0) || dc_dir_find_locked(need,nl,dc_hash(need,nl),now)){ dc_dir_free(t); return 1; }
  dc_dir_insert_locked(t); return 1;
}

/* ---- path entries ---- */
static size_t dc_key(const char *base, const char *rest, char *key, size_t ksz, uint32_t *h){   /* base + canonical folded rest; (size_t)-1 if it does not fit */
  size_t n=0; for(;*base;base++){ if(n>=ksz-1) return (size_t)-1; key[n++]=*base; }
  const char *p=rest;
  while(*p){ while(*p=='\\') p++; if(!*p) break; if(n>=ksz-1) return (size_t)-1; key[n++]='\\';
    while(*p && *p!='\\'){ uint32_t c=(uint8_t)*p++; if(c-'A'<26u) c+=32; if(n>=ksz-1) return (size_t)-1; key[n++]=(char)c; } }
  key[n]=0; *h=dc_hash(key,n); return n; }
static DCPath *dc_path_find_locked(const char *key, size_t kl, uint32_t h, uint64_t now){
  DCPath *set=DC.paths+(size_t)(h&(DC_MAX_PATHS/DC_WAYS-1))*DC_WAYS;
  for(int w=0;w<DC_WAYS;w++){ DCPath *e=set+w; if(e->s && e->h==h && e->kl==kl && !memcmp(e->s,key,kl)){
      if(e->exp && (int64_t)(now-e->exp)>0){ free(e->s); e->s=0; DC.st.npaths--; return 0; }   /* expired */
      e->stamp=++DC.stamp; return e; } }
  return 0; }
static void dc_path_insert_locked(const char *key, size_t kl, uint32_t h, const char *host, size_t plen, int nfound, int exists, uint64_t now){
  DCPath *set=DC.paths+(size_t)(h&(DC_MAX_PATHS/DC_WAYS-1))*DC_WAYS, *v=0;
  for(int w=0;w<DC_WAYS && !v;w++) if(set[w].s && set[w].h==h && set[w].kl==kl && !memcmp(set[w].s,key,kl)) v=set+w;   /* same key raced in: replace */
  for(int w=0;w<DC_WAYS && !v;w++) if(!set[w].s) v=set+w;
  if(!v){ v=set; for(int w=1;w<DC_WAYS;w++) if((int32_t)(set[w].stamp-v->stamp)<0) v=set+w; DC.st.evict_path++; }
  char *s=malloc(kl+1+plen+1); if(!s) return;
  memcpy(s,key,kl); s[kl]=0; memcpy(s+kl+1,host,plen); s[kl+1+plen]=0;
  if(v->s) free(v->s); else DC.st.npaths++;
  v->s=s; v->h=h; v->kl=(uint16_t)kl; v->nfound=(uint16_t)nfound; v->exists=(uint8_t)exists; v->stamp=++DC.stamp; v->exp=dc_ttl_path(host)?now+DC_TTL_NS:0;
}
/* hit: the stored prefix (real case) + the query's own tail (given case) for the components that do not exist */
static void dc_path_hit(const DCPath *e, const char *rest, char *host, size_t hostsz){
  snprintf(host,hostsz,"%s",e->s+e->kl+1); if(e->exists) return;
  const char *p=rest; int k=0;
  while(*p){ while(*p=='\\') p++; if(!*p) break; const char *c=p; while(*p && *p!='\\') p++; if(k++>=e->nfound) dc_app(host,hostsz,c,(size_t)(p-c)); }
}

/* ---- the walk ---- */
/* 1: resolved: *ex = every component found, *nfound = components found, *plen = length of host after the last found
   component, *dups = a consulted directory holds case-duplicate names (do not cache); 0: the table of directory `need`
   (DC_HOSTMAX) must be built first */
static int dc_walk_locked(const char *base, const char *rest, char *host, size_t hostsz, int *ex, int *nfound, size_t *plen, int *dups, char *need, uint64_t now){
  snprintf(host,hostsz,"%s",base); int e=1, nf=0, du=0; size_t pl=strlen(host); const char *p=rest;
  while(*p){
    while(*p=='\\') p++;
    if(!*p) break;
    const char *c=p; while(*p && *p!='\\') p++; size_t cl=(size_t)(p-c);
    if(e){   /* once a component is missing the rest is appended as given (stat/opendir of a missing dir fail in the old code too) */
      DCDir *t=dc_dir_find_locked(host,pl,dc_hash(host,pl),now);
      if(!t){ snprintf(need,DC_HOSTMAX,"%s",host); return 0; }
      const char *real; int isdir; du|=t->dups;
      if(dc_dir_lookup_in(t,c,cl,&real,&isdir)){ dc_app(host,hostsz,real,strlen(real)); pl=strlen(host); nf++; continue; }
      e=0; }
    dc_app(host,hostsz,c,cl);
  }
  *ex=e; *nfound=nf; *plen=pl; *dups=du; return 1;
}
static int dc_stats_due_locked(uint64_t now, DCStats *out){ if(!DC.stat_on || (int64_t)(now-DC.next_stat)<0) return 0; DC.next_stat=now+DC_STAT_NS; *out=DC.st; return 1; }
static void dc_stats_fprint(FILE *f, const DCStats *s){
  fprintf(f,"[dircache] lookups=%llu hits=%llu (neg %llu) misses=%llu dirscans=%llu dirlookups=%llu inval=%llu (dropped %llu tables, %llu entries) fallback=%llu evict=%llu/%llu tables=%u (%zu KB) entries=%u\n",
    (unsigned long long)s->lookup,(unsigned long long)s->hit,(unsigned long long)s->hitneg,(unsigned long long)s->miss,(unsigned long long)s->scan,(unsigned long long)s->dirlookup,
    (unsigned long long)s->inval,(unsigned long long)s->inval_dirs,(unsigned long long)s->inval_paths,(unsigned long long)s->fallback,(unsigned long long)s->evict_dir,(unsigned long long)s->evict_path,
    s->ndirs,s->dir_bytes>>10,s->npaths); }

/* ---- API ---- */
/* base = host directory the guest prefix maps to, rest = guest path below it with backslashes ("\XGame\Config\XGame.ini", "" or "\").
   Writes the host path (hostsz bytes; the real on-disk case where the components exist, the given case where they do not).
   returns 1 if the last component was found, 0 if not (a hint only: callers still do their own syscall on `host`). */
DC_UNUSED static int dc_resolve(const char *base, const char *rest, char *host, size_t hostsz){
  char key[DC_KEYMAX]; uint32_t h; size_t kl; int ex=0, fallback=0;
  if(!dc_ready() || (kl=dc_key(base,rest,key,sizeof key,&h))==(size_t)-1) return dc_resolve_naive(base,rest,host,hostsz);
  uint64_t now=dc_now(); char need[DC_HOSTMAX]; DCStats snap; int ps, nf, du; size_t pl;
  pthread_mutex_lock(&DC.mu); DC.st.lookup++;
  DCPath *e=dc_path_find_locked(key,kl,h,now);
  if(e){ dc_path_hit(e,rest,host,hostsz); ex=e->exists; DC.st.hit++; if(!ex) DC.st.hitneg++; }
  else { DC.st.miss++;
    for(int tries=0;;tries++){
      if(dc_walk_locked(base,rest,host,hostsz,&ex,&nf,&pl,&du,need,now)){ if(!du && nf<65535) dc_path_insert_locked(key,kl,h,host,pl,nf,ex,now); break; }
      if(tries>=DC_TRIES || !dc_dir_make_locked(need,now)){ fallback=1; DC.st.fallback++; break; } } }
  ps=dc_stats_due_locked(now,&snap);
  pthread_mutex_unlock(&DC.mu);
  if(ps) dc_stats_fprint(stderr,&snap);
  if(fallback) ex=dc_resolve_naive(base,rest,host,hostsz);
  return ex;
}
/* exact-name lookup in one host directory (FindFirstFileW without wildcards): 1 found (real name copied, *isdir set),
   0 not in the directory, -1 directory missing, -2 cache unavailable (caller falls back to opendir/readdir) */
DC_UNUSED static int dc_dir_lookup(const char *dir, const char *name, char *real, size_t realsz, int *isdir){
  if(!dc_ready()) return -2;
  size_t dl=strlen(dir); if(dl>=DC_HOSTMAX) return -2;
  uint64_t now=dc_now(); int r=-2; const char *rn; int d;
  pthread_mutex_lock(&DC.mu); DC.st.dirlookup++;
  for(int tries=0;tries<=DC_TRIES;tries++){
    DCDir *t=dc_dir_find_locked(dir,dl,dc_hash(dir,dl),now);
    if(t){ if(t->missing) r=-1; else if(dc_dir_lookup_in(t,name,strlen(name),&rn,&d)){ snprintf(real,realsz,"%s",rn); if(isdir) *isdir=d; r=1; } else r=0; break; }
    if(!dc_dir_make_locked(dir,now)) break; }
  if(r==-2) DC.st.fallback++;
  pthread_mutex_unlock(&DC.mu); return r;
}
/* hpath = host path of a file or directory that was just created/written/deleted/renamed: drops the parent directory's
   table, every table below it and every path entry below it */
DC_UNUSED static void dc_invalidate(const char *hpath){
  if(!dc_ready()) return;
  char d[DC_HOSTMAX]; snprintf(d,sizeof d,"%s",hpath); char *sl=strrchr(d,'/'); if(sl) *sl=0; else d[0]=0; size_t dl=strlen(d);
  uint32_t nd=0, np=0;
  pthread_mutex_lock(&DC.mu);
  DC.gen++; DC.st.inval++; DC.inv[DC.gen%DC_INVRING].gen=DC.gen; memcpy(DC.inv[DC.gen%DC_INVRING].dir,d,dl+1);
  for(DCDir *t=DC.head, *nx; t; t=nx){ nx=t->ln; if(dc_under(t->path,d,dl)){ dc_dir_unlink_locked(t); dc_dir_free(t); nd++; } }
  /* an entry is affected when its full host path lies below d: its stored prefix is strictly below d, or equals d and the
     entry is negative (the missing tail is inside d). A positive entry whose prefix equals d is d itself: unaffected. */
  if(DC.st.npaths) for(uint32_t i=0;i<DC_MAX_PATHS;i++){ DCPath *e=DC.paths+i; if(e->s){ const char *hp=e->s+e->kl+1;
    if(!strncmp(hp,d,dl) && (hp[dl]=='/' || (hp[dl]==0 && !e->exists))){ free(e->s); e->s=0; DC.st.npaths--; np++; } } }
  DC.st.inval_dirs+=nd; DC.st.inval_paths+=np;
  pthread_mutex_unlock(&DC.mu);
  if(DC.flog) fprintf(stderr,"[file] dircache inval '%s': %u tables, %u entries dropped\n",d,nd,np);
}
DC_UNUSED static void dc_stats(DCStats *out){ pthread_mutex_lock(&DC.mu); *out=DC.st; pthread_mutex_unlock(&DC.mu); }
DC_UNUSED static void dc_stats_print(FILE *f){ DCStats s; dc_stats(&s); dc_stats_fprint(f,&s); }
/* drop everything (tests; also usable as a big hammer) */
DC_UNUSED static void dc_flush(void){
  pthread_mutex_lock(&DC.mu); DC.gen++; DC.inv[DC.gen%DC_INVRING].gen=DC.gen; DC.inv[DC.gen%DC_INVRING].dir[0]=0;
  for(DCDir *t=DC.head, *nx; t; t=nx){ nx=t->ln; dc_dir_unlink_locked(t); dc_dir_free(t); }
  if(DC.paths) for(uint32_t i=0;i<DC_MAX_PATHS;i++) if(DC.paths[i].s){ free(DC.paths[i].s); DC.paths[i].s=0; }
  DC.st.npaths=0; pthread_mutex_unlock(&DC.mu);
}
