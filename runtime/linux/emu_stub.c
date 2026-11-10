/* Emulator fallback stub for builds without Unicorn: report the untranslated target and stop the thread's call. */
#include "hle.h"
#include <stdio.h>
#include <stdlib.h>
void emu_add_region(uint32_t a, uint32_t n){ }
void emu_init(uint32_t img, uint32_t imgsz, uint32_t heap_lo, uint32_t heap_hi, uint32_t stk_lo, uint32_t stk_sz, uint32_t teb){ }
void emu_call(CPU*c, uint32_t t){ fprintf(stderr,"[emu] no emulator in this build: untranslated code at %08x (return %08x)\n",t,R32(c->esp)); fflush(stderr); abort(); }
void emu_call_mid(CPU*c, uint32_t t){ fprintf(stderr,"[emu] no emulator in this build: untranslated instruction at %08x\n",t); fflush(stderr); abort(); }
