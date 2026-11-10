import pefile,re
pe=pefile.PE('/home/claude/bsi/BioShockInfinite.exe')
sig={}
for l in open('/home/claude/bsi/import_sigs.tsv'):
    lib,name,n,bytes_,cc,var,rl,rt=l.rstrip('\n').split('\t')
    sig[(lib.upper().replace('.DLL',''),name)]=(int(bytes_),cc,var=='true')
rows=[]
# stdcall byte counts for imports Ghidra has no signature for (Win32 SDK prototypes)
POP={'Direct3DCreate9':4,'D3DPERF_BeginEvent':8,'D3DPERF_EndEvent':0,'D3DXMatrixLookAtRH':16,'D3DXMatrixTranslation':16,
 'D3DXMatrixMultiply':12,'D3DXMatrixScaling':16,'D3DXMatrixInverse':12,'D3DXMatrixOrthoOffCenterRH':28,'D3DXOptimizeFaces':20,
 'D3DXVec3Transform':12,'D3DX11CheckVersion':8,'DirectInput8Create':20,'StackWalk64':36,'SymLoadModule64':28,'SymGetModuleInfo64':16,
 'SymGetSymFromAddr64':20,'MiniDumpWriteDump':28,'SymSetSearchPath':8,'SymGetLineFromAddr64':20,'SymInitialize':12,'SymSetOptions':4,
 'SymGetOptions':0,'SHGetFolderPathW':20,'VerSetConditionMask':16,'RtlCaptureStackBackTrace':16,'RtlCaptureContext':4,
 'CallNtPowerInformation':20,'ReportFault':8,'WSAIoctl':36,'freeaddrinfo':4,'getaddrinfo':16,'D3D11CreateDevice':40,'CreateDXGIFactory':8,
 'NxCreateApexSDK':0,'PxCreatePhysics':0,
 'bind':12,'WSAGetLastError':0,'connect':12,'listen':8,'select':20,'ioctlsocket':12,'sendto':24,'send':16,'recv':16,'recvfrom':24,
 'accept':12,'closesocket':4,'getpeername':12,'getsockname':12,'getsockopt':20,'htonl':4,'htons':4,'inet_addr':4,'inet_ntoa':4,
 'ntohl':4,'ntohs':4,'setsockopt':20,'shutdown':8,'socket':12,'gethostbyname':4,'gethostname':8,'WSASetLastError':4,'WSAStartup':8,
 'WSACleanup':0,'__WSAFDIsSet':8,'SysFreeString':4,'SysAllocString':4,'SysAllocStringLen':8,'SysStringLen':4,'VariantInit':4,
 'VariantClear':4,'TaskDialogIndirect':16,'XInputGetState':8,'XInputSetState':8,'XInputGetCapabilities':12}
ORD={('WSOCK32',1):12,('WSOCK32',2):12,('WSOCK32',3):4,('WSOCK32',4):12,('WSOCK32',5):12,('WSOCK32',6):12,('WSOCK32',7):20,('WSOCK32',8):4,
 ('WSOCK32',9):4,('WSOCK32',10):4,('WSOCK32',11):4,('WSOCK32',12):12,('WSOCK32',13):8,('WSOCK32',14):4,('WSOCK32',15):4,('WSOCK32',16):16,
 ('WSOCK32',17):24,('WSOCK32',18):20,('WSOCK32',19):16,('WSOCK32',20):24,('WSOCK32',21):20,('WSOCK32',22):8,('WSOCK32',23):12,
 ('WSOCK32',52):4,('WSOCK32',57):8,('WSOCK32',111):0,('WSOCK32',112):4,('WSOCK32',115):8,('WSOCK32',116):0,('WSOCK32',151):8,
 ('XINPUT1_3',2):8,('XINPUT1_3',3):8,('XINPUT1_3',4):12,('COMCTL32',345):16,('OLEAUT32',2):4,('OLEAUT32',6):4,('OLEAUT32',7):4,('OLEAUT32',9):4}
for d in pe.DIRECTORY_ENTRY_IMPORT:
    dll=d.dll.decode()
    for i in d.imports:
        nm=i.name.decode() if i.name else 'ord%d'%i.ordinal
        key=(dll.upper().replace('.DLL',''),nm)
        b,cc,var=sig.get(key,(0,'unknown',False))
        std = cc=='__stdcall' and not var and not dll.upper().startswith('MSVC')
        if cc=='unknown':
            m=re.search(r'@(\d+)$',nm); D=dll.upper().replace('.DLL','')
            if m: b,std=int(m.group(1)),True
            elif nm in POP: b,std=POP[nm],True
            elif not i.name and (D,i.ordinal) in ORD: b,std=ORD[(D,i.ordinal)],True
        rows.append((i.address, dll+'!'+nm, 'hle_%s_%s'%(re.sub(r'\W','_',dll.lower().replace('.dll','')), re.sub(r'\W','_',nm)), b if std else 0))
with open('imps.c','w') as f:
    f.write('#include "cpu.h"\n#include <stdio.h>\ntypedef void (*hle_fn)(CPU*c);\ntypedef struct { uint32_t iat; const char *name; hle_fn fn; } Imp;\n')
    f.write('extern FILE *LOGF_RT; void stub_note(const char*n);\n')
    for k,(a,n,s,pop) in enumerate(rows):
        f.write(f'void {s}(CPU*c) __attribute__((weak));\n')
        f.write(f'static void stub_{k}(CPU*c){{ stub_note("{n}"); c->eax=0; c->esp+=4+{pop}; }}\n')
    f.write('Imp g_imps[]={\n')
    for k,(a,n,s,pop) in enumerate(rows): f.write(f'{{0x{a:x}u,"{n}",{s}}},\n')
    f.write(f'}};\nint g_nimps={len(rows)};\n')
    f.write('static hle_fn stubs[]={'+','.join(f'stub_{k}' for k in range(len(rows)))+'};\n')
    f.write('void imps_fill_stubs(void){ for(int k=0;k<g_nimps;k++) if(!g_imps[k].fn) g_imps[k].fn=stubs[k]; }\n')
print(len(rows),'imports')
