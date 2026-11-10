"""Make BioShockInfinite_recomp.exe: original exe + small loader section that LoadLibrary's bsirecomp.dll and calls bsi_start()."""
import pefile, struct, sys
src = sys.argv[1] if len(sys.argv) > 1 else '/home/claude/bsi/BioShockInfinite.exe'
dst = sys.argv[2] if len(sys.argv) > 2 else '/home/claude/bsi/recomp/BioShockInfinite_recomp.exe'
pe = pefile.PE(src)
iat = {i.name.decode(): i.address for d in pe.DIRECTORY_ENTRY_IMPORT for i in d.imports if i.name}
LL, GPA, EP = iat['LoadLibraryA'], iat['GetProcAddress'], iat.get('ExitProcess')
oh = pe.OPTIONAL_HEADER; IB = oh.ImageBase; SA = oh.SectionAlignment; FA = oh.FileAlignment
last = pe.sections[-1]
va = (last.VirtualAddress + last.Misc_VirtualSize + SA - 1) // SA * SA
data = bytearray(open(src, 'rb').read())
raw_off = (len(data) + FA - 1) // FA * FA
base = IB + va
code = bytearray(); strs = {}
S_DLL, S_FN = 0x200, 0x220
code += b'\x68' + struct.pack('<I', base + S_DLL)          # push "bsirecomp.dll"
code += b'\xff\x15' + struct.pack('<I', LL)                 # call [LoadLibraryA]
code += b'\x85\xc0'                                         # test eax,eax
code += b'\x74\x18'                                         # jz fail (patched below)
code += b'\x68' + struct.pack('<I', base + S_FN)           # push "bsi_start"
code += b'\x50'                                             # push eax
code += b'\xff\x15' + struct.pack('<I', GPA)                # call [GetProcAddress]
code += b'\x85\xc0'
code += b'\x74\x04'                                         # jz fail
code += b'\xff\xd0'                                         # call eax
code += b'\xeb\xfe' if False else b'\x90\x90'
fail = len(code)
code[code.index(b'\x74\x18') + 1] = fail - (code.index(b'\x74\x18') + 2)
code += b'\x6a\x01'                                         # push 1
code += (b'\xff\x15' + struct.pack('<I', EP)) if EP else b'\xcc'
code += b'\xcc'
# fix second jz
j2 = code.index(b'\x74\x04'); code[j2 + 1] = fail - (j2 + 2)
sec = bytearray(0x1000)
sec[:len(code)] = code
sec[S_DLL:S_DLL + 14] = b'bsirecomp.dll\0'
sec[S_FN:S_FN + 10] = b'bsi_start\0'
# section header
hdr_off = pe.sections[-1].get_file_offset() + 40
assert hdr_off + 40 <= oh.SizeOfHeaders
sh = struct.pack('<8sIIIIIIHHI', b'.bsild\0\0', 0x1000, va, len(sec), raw_off, 0, 0, 0, 0, 0x60000020)
data[hdr_off:hdr_off + 40] = sh
data += b'\0' * (raw_off - len(data)) + sec
nt = pe.DOS_HEADER.e_lfanew
struct.pack_into('<H', data, nt + 6, pe.FILE_HEADER.NumberOfSections + 1)
opt = nt + 24
struct.pack_into('<I', data, opt + 16, va)                  # AddressOfEntryPoint
struct.pack_into('<I', data, opt + 56, va + 0x1000)         # SizeOfImage
struct.pack_into('<I', data, opt + 64, 0)                   # CheckSum
open(dst, 'wb').write(data)
p2 = pefile.PE(dst)
print('ok', dst, 'EP', hex(IB + p2.OPTIONAL_HEADER.AddressOfEntryPoint), [s.Name for s in p2.sections])
