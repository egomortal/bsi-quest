/* HLE: user32/gdi32 as a headless stub (window handles are fake; no messages) */
#include "hle.h"
#include <unistd.h>
uint32_t next_hwnd=0x10000;
HLE(user32,LoadIconW){ RET_STD(2,0x7e0001); }
HLE(user32,LoadIconA){ RET_STD(2,0x7e0001); }
HLE(user32,LoadCursorW){ RET_STD(2,0x7e0002); }
HLE(user32,LoadCursorA){ RET_STD(2,0x7e0002); }
HLE(user32,LoadImageW){ RET_STD(6,0x7e0003); }
HLE(user32,LoadImageA){ RET_STD(6,0x7e0003); }
HLE(user32,UnregisterClassW){ RET_STD(2,1); }
HLE(user32,CreateWindowExA){ RET_STD(12,next_hwnd+=16); }
HLE(user32,DestroyWindow){ RET_STD(1,1); }
HLE(user32,ShowWindow){ RET_STD(2,1); }
HLE(user32,UpdateWindow){ RET_STD(1,1); }
HLE(user32,SetWindowPos){ RET_STD(7,1); }
HLE(user32,MoveWindow){ RET_STD(6,1); }
HLE(user32,SetWindowTextW){ RET_STD(2,1); }
HLE(user32,IsWindow){ RET_STD(1,ARG(0)>=0x10000); }
HLE(user32,IsWindowVisible){ RET_STD(1,1); }
HLE(user32,IsIconic){ RET_STD(1,0); }
HLE(user32,IsZoomed){ RET_STD(1,0); }
HLE(user32,SetForegroundWindow){ RET_STD(1,1); }
HLE(user32,SetActiveWindow){ RET_STD(1,0); }
HLE(user32,SetFocus){ RET_STD(1,0); }
HLE(user32,GetAncestor){ RET_STD(2,ARG(0)); }
HLE(user32,GetParent){ RET_STD(1,0); }
HLE(user32,GetDesktopWindow){ RET_STD(0,0x10010); }
static void rect(uint32_t p,int l,int t,int r,int b){ W32(p,l); W32(p+4,t); W32(p+8,r); W32(p+12,b); }
static int gamew(void){ static int v; if(!v){ v = getenv("BSI_GAME_W") ? atoi(getenv("BSI_GAME_W")) : 1920; } return v; }
static int gameh(void){ static int v; if(!v){ v = getenv("BSI_GAME_H") ? atoi(getenv("BSI_GAME_H")) : 1080; } return v; }
HLE(user32,GetClientRect){ rect(ARG(1),0,0,gamew(),gameh()); RET_STD(2,1); }
HLE(user32,GetWindowRect){ rect(ARG(1),0,0,gamew(),gameh()); RET_STD(2,1); }
HLE(user32,AdjustWindowRect){ RET_STD(3,1); }
HLE(user32,AdjustWindowRectEx){ RET_STD(4,1); }
HLE(user32,ClientToScreen){ RET_STD(2,1); }
HLE(user32,ScreenToClient){ RET_STD(2,1); }
HLE(user32,SetCursor){ RET_STD(1,0); }
HLE(user32,ShowCursor){ static int cnt=0; cnt += ARG(0)?1:-1; RET_STD(1,(uint32_t)cnt); }
HLE(user32,ClipCursor){ RET_STD(1,1); }
HLE(user32,PeekMessageA){ RET_STD(5,0); }
HLE(user32,DefWindowProcW){ RET_STD(4,0); }
HLE(user32,DefWindowProcA){ RET_STD(4,0); }
HLE(user32,SendMessageW){ RET_STD(4,0); }
HLE(user32,PostQuitMessage){ RET_STD(1,0); }
HLE(user32,SetTimer){ RET_STD(4,1); }
HLE(user32,KillTimer){ RET_STD(2,1); }
HLE(user32,MessageBoxW){ char t[2048],cap[256]; g2host_w(ARG(1),t,2048); g2host_w(ARG(2),cap,256); fprintf(stderr,"[MessageBox] %s: %s\n",cap,t); RET_STD(4,1); }
HLE(user32,MessageBoxA){ char t[2048]; g2host_a(ARG(1),t,2048); fprintf(stderr,"[MessageBox] %s\n",t); RET_STD(4,1); }
HLE(user32,GetDC){ RET_STD(1,0xdc01); }
HLE(user32,GetWindowDC){ RET_STD(1,0xdc01); }
HLE(user32,ReleaseDC){ RET_STD(2,1); }
HLE(user32,BeginPaint){ RET_STD(2,0xdc01); }
HLE(user32,EndPaint){ RET_STD(2,1); }
HLE(user32,InvalidateRect){ RET_STD(3,1); }
HLE(user32,MonitorFromWindow){ RET_STD(2,0x1000); }
HLE(user32,MonitorFromPoint){ RET_STD(3,0x1000); }
HLE(user32,GetMonitorInfoW){ uint32_t p=ARG(1); rect(p+4,0,0,1920,1080); rect(p+20,0,0,1920,1080); W32(p+36,1); RET_STD(2,1); }
HLE(user32,EnumDisplaySettingsW){ uint32_t idx=ARG(1), p=ARG(2); if(idx!=0xffffffffu && idx>0) RET_STD(3,0); W32(p+104,32); W32(p+108,1920); W32(p+112,1080); W32(p+120,60); RET_STD(3,1); }
HLE(user32,EnumDisplayDevicesW){ if(ARG(1)>0) RET_STD(4,0); uint32_t p=ARG(2); const char*n="\\\\.\\DISPLAY1"; for(int k=0;n[k];k++) W16(p+4+2*k,n[k]); const char*d="Quest 3"; for(int k=0;d[k];k++) W16(p+68+2*k,d[k]); W32(p+324,5); RET_STD(4,1); }
HLE(user32,ChangeDisplaySettingsW){ RET_STD(2,0); }
HLE(user32,MapVirtualKeyW){ RET_STD(2,0); }
HLE(user32,GetKeyNameTextW){ RET_STD(3,0); }
HLE(user32,RegisterRawInputDevices){ RET_STD(3,1); }
HLE(user32,GetSysColor){ RET_STD(1,0); }
HLE(user32,GetSysColorBrush){ RET_STD(1,0x2001); }
HLE(user32,FindWindowW){ RET_STD(2,0); }
HLE(user32,OpenClipboard){ RET_STD(1,0); }
HLE(user32,CloseClipboard){ RET_STD(0,1); }
/* gdi32 */
HLE(gdi32,CreateCompatibleDC){ RET_STD(1,0xdc02); }
HLE(gdi32,DeleteDC){ RET_STD(1,1); }
HLE(gdi32,SelectObject){ RET_STD(2,0x3001); }
HLE(gdi32,DeleteObject){ RET_STD(1,1); }
HLE(gdi32,BitBlt){ RET_STD(9,1); }
HLE(gdi32,StretchBlt){ RET_STD(11,1); }
HLE(gdi32,GetObjectW){ RET_STD(3,0); }
HLE(gdi32,GetStockObject){ RET_STD(1,0x3002); }
HLE(gdi32,CreateSolidBrush){ RET_STD(1,0x3003); }
HLE(gdi32,SetBkMode){ RET_STD(2,1); }
HLE(gdi32,SetTextColor){ RET_STD(2,0); }
HLE(gdi32,GetDeviceCaps){ uint32_t i=ARG(1); RET_STD(2, i==8?1920: i==10?1080: i==12?32: i==116?60: 96); }
HLE(gdi32,CreateFontW){ RET_STD(14,0x3004); }
HLE(gdi32,TextOutW){ RET_STD(5,1); }
HLE(gdi32,SetDeviceGammaRamp){ RET_STD(2,1); }
HLE(gdi32,GetDeviceGammaRamp){ memset(MEMP(ARG(1)),0,1536); RET_STD(2,1); }
