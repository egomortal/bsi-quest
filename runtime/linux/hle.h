#pragma once
#include "cpu.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
extern FILE *LOGF_EXT;
uint32_t gmalloc(uint32_t n); void gfree(uint32_t p); uint32_t gsize(uint32_t p); uint32_t gpages(uint32_t n);
void guest_call(CPU*c, uint32_t fn);
#define ARG(k) R32(c->esp+4+4*(k))
#define RET_STD(nargs, v) do{ c->eax=(uint32_t)(v); c->esp+=4+4*(nargs); return; }while(0)
#define RET_C(v) do{ c->eax=(uint32_t)(v); c->esp+=4; return; }while(0)
#define HLE(dll,name) void hle_##dll##_##name(CPU*c)
/* guest strings */
static inline uint32_t gwcslen(uint32_t p){ uint32_t n=0; while(R16(p+2*n)) n++; return n; }
static inline uint32_t gstrlen(uint32_t p){ uint32_t n=0; while(R8(p+n)) n++; return n; }
static inline void g2host_w(uint32_t p, char *out, int max){ int k=0; if(p) for(;k<max-1;k++){ uint16_t ch=R16(p+2*k); if(!ch) break; out[k]=ch<128?(char)ch:'?'; } out[k]=0; }
static inline void g2host_a(uint32_t p, char *out, int max){ int k=0; if(p) for(;k<max-1;k++){ uint8_t ch=R8(p+k); if(!ch) break; out[k]=ch; } out[k]=0; }
static inline uint32_t galloc_wstr(const char *s){ uint32_t n=strlen(s); uint32_t p=gmalloc(2*n+2); for(uint32_t k=0;k<=n;k++) W16(p+2*k,(uint8_t)s[k]); return p; }
static inline uint32_t galloc_str(const char *s){ uint32_t n=strlen(s); uint32_t p=gmalloc(n+1); memcpy(MEMP(p),s,n+1); return p; }
static inline uint16_t wlower(uint16_t c){ return (c>='A'&&c<='Z')?c+32:c; }
extern uint32_t g_last_error;
