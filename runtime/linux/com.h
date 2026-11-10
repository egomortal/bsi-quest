#pragma once
#include <stdint.h>
typedef struct { uint32_t d1; uint16_t d2, d3; uint8_t d4[8]; } GUIDs;
typedef struct { const char *name; int first, n; GUIDs iid; } ComIface;
typedef struct { const char *name; int iface, idx, argbytes, retkind /*0 HRESULT,1 void,2 other*/, outiface; } ComMethod;
extern const int g_com_nifaces, g_com_nmethods; extern const ComIface g_com_ifaces[]; extern const ComMethod g_com_methods[];
