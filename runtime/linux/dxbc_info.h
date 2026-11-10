/* DXBC bytecode key + best-effort container inspection for the shader dump / override hook (bridge.c br_shader_create).
   Header-only, no runtime dependencies, so test_dxbc.c and the PC-side dxbc_info.py implement the very same rules.
   key: FNV-1a 64 over the bytes, then the length folded in (h ^= len; h *= prime) - printed as 16 lowercase hex digits = the
        <hash16> in <stage>_<hash16>.dxbc.
   Container (D3D "DXBC" blob): 'DXBC', 16-byte digest, version 1, total size, chunk count, chunk offsets; each chunk = FourCC,
        size, data. Program chunk = SHEX (SM5) or SHDR (SM4): DWORD0 version token (bits 3:0 minor, 7:4 major, 31:16 program type
        0 ps 1 vs 2 gs 3 hs 4 ds 5 cs), DWORD1 program length in DWORDs, then instructions. Opcode token: bits 10:0 opcode type,
        30:24 instruction length in DWORDs (incl. the token), 31 = extended token follows (counted in the length). CUSTOMDATA (53)
        carries its block length in the next DWORD instead. Opcode numbers from the D3D10/11 tokenized program format enum
        (d3d11TokenizedProgramFormat.hpp: D3D11_SB_OPCODE_DCL_THREAD_GROUP = 155 = 0x9b, followed by 3 DWORDs x,y,z;
        DCL_THREAD_GROUP_SHARED_MEMORY_RAW = 159 (g# operand, byte count), _STRUCTURED = 160 (g# operand, stride, count));
        the same numbers are the ones vkd3d (tpf.c: VKD3D_SM5_OP_DCL_THREAD_GROUP 0x9b, DCL_TGSM_RAW 0x9f, DCL_TGSM_STRUCTURED 0xa0)
        and DXVK (dxbc_enums.h: DclThreadGroup = 155, DclThreadGroupSharedMemoryRaw = 159, ...Structured = 160) decode. Best effort:
        anything unexpected leaves the fields 0 / "?" (the dump prints "-" / "?"); validate on a real blob with fxc /dumpbin, see NOTES.md. */
#pragma once
#include <stdint.h>
#include <string.h>
#include <stddef.h>
typedef struct { int ok, shex; uint32_t type, major, minor, tg[3], tgsm; char chunks[72]; } DxbcInfo;   /* ok: container parsed; shex: program chunk seen */
static inline uint64_t dxbc_key(const void *p, size_t n){ const uint8_t *b=(const uint8_t*)p; uint64_t h=0xcbf29ce484222325ull;
  for(size_t i=0;i<n;i++){ h^=b[i]; h*=0x100000001b3ull; } h^=(uint64_t)n; h*=0x100000001b3ull; return h; }
static inline uint32_t dxbc_r32(const uint8_t *p){ uint32_t v; memcpy(&v,p,4); return v; }
static inline const char *dxbc_stage_name(uint32_t t){ static const char *n[]={"ps","vs","gs","hs","ds","cs"}; return t<6?n[t]:"?"; }
static inline void dxbc_info(const void *blob, size_t n, DxbcInfo *o){
  const uint8_t *p=(const uint8_t*)blob; memset(o,0,sizeof *o); strcpy(o->chunks,"?");
  if(n<32 || memcmp(p,"DXBC",4)) return;
  uint32_t nch=dxbc_r32(p+28); if(nch>64 || 32+4ull*nch>n) return; o->ok=1; int cl=0; o->chunks[0]=0;
  for(uint32_t i=0;i<nch;i++){ uint32_t off=dxbc_r32(p+32+4*i); if(off>n-8) continue; uint32_t sz=dxbc_r32(p+off+4); if(sz>n-off-8) continue;
    if(cl+5<(int)sizeof o->chunks){ if(cl) o->chunks[cl++]=','; memcpy(o->chunks+cl,p+off,4); cl+=4; o->chunks[cl]=0; }
    if(memcmp(p+off,"SHEX",4) && memcmp(p+off,"SHDR",4)) continue;
    const uint8_t *s=p+off+8; uint32_t nd=sz/4; if(nd<2) continue; o->shex=1;
    uint32_t v=dxbc_r32(s); o->type=v>>16; o->major=(v>>4)&15; o->minor=v&15;
    uint32_t len=dxbc_r32(s+4); if(len>nd) len=nd;
    for(uint32_t k=2;k<len;){ uint32_t t=dxbc_r32(s+4*k), op=t&0x7ff, l=(t>>24)&0x7f;
      if(op==53){ if(k+1>=len) break; l=dxbc_r32(s+4*(k+1)); }   /* CUSTOMDATA (immediate constant buffer, comments): length in DWORD 1 */
      if(!l || l>len-k) break;
      if(op==155 && l>=4){ o->tg[0]=dxbc_r32(s+4*(k+1)); o->tg[1]=dxbc_r32(s+4*(k+2)); o->tg[2]=dxbc_r32(s+4*(k+3)); }
      else if(op==159 && l>=4) o->tgsm+=dxbc_r32(s+4*(k+l-1));                             /* dcl_tgsm_raw g#, bytes */
      else if(op==160 && l>=5) o->tgsm+=dxbc_r32(s+4*(k+l-2))*dxbc_r32(s+4*(k+l-1));      /* dcl_tgsm_structured g#, stride, count */
      k+=l; } } }
