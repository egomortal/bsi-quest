/* guest 32-bit COM -> host DXVK bridge */
#pragma once
#include "hle.h"
#define GP(a) ((a)?(void*)MEMP(a):(void*)0)
void *br_host(uint32_t guest_obj);
void *br_hostc(uint32_t guest_obj);
void br_fail(const char *what, int32_t hr);
uint32_t br_wrap(void *host, const char *iface);
const char *br_iid_name(void *iid);
void *br_handle(uint32_t g);
uint32_t br_handle_to_guest(void *h);
void br_null(CPU*c, const char *what, int argbytes);
void br_forget(void *host);
/* shader dump / override (bridge.c): the ID3D11Device::Create{Vertex,Geometry,Pixel,Hull,Domain,Compute}Shader wrappers call this
   instead of vtable slot `slot` directly (same arguments). Without BSI_SHADER_DUMP_DIR / BSI_SHADER_DUMP / BSI_SHADER_OVERRIDE_DIR
   it is exactly that call; otherwise the DXBC is hashed, dumped once per distinct shader and/or replaced by
   <BSI_SHADER_OVERRIDE_DIR>/<stage>_<hash16>.dxbc (stage = "vs" "gs" "ps" "hs" "ds" "cs"). */
int32_t br_shader_create(void *dev, int slot, const char *stage, const void *code, size_t len, void *linkage, void **out);
