import struct
from unicorn import *
from unicorn.x86_const import *
GDT=0x7ff00000
def desc(base,limit,access,flags):
    return struct.pack('<Q', (limit&0xffff)|((base&0xffffff)<<16)|(access<<40)|(((limit>>16)&0xf)<<48)|(flags<<52)|(((base>>24)&0xff)<<56))
def setup_fs(mu, fs_base, remap=True):
    if remap: mu.mem_map(GDT,0x1000)
    g=bytearray(0x100)
    code=desc(0,0xfffff,0x9a,0xc); dat=desc(0,0xfffff,0x92,0xc); fsd=desc(fs_base,0xfff,0x92,0x4)
    g[8:16]=code; g[16:24]=dat; g[24:32]=fsd
    mu.mem_write(GDT,bytes(g))
    mu.reg_write(UC_X86_REG_GDTR,(0,GDT,0xff,0x0))
    mu.reg_write(UC_X86_REG_FS,(3<<3))
    for r in (UC_X86_REG_DS, UC_X86_REG_ES, UC_X86_REG_SS, UC_X86_REG_GS): mu.reg_write(r, 2<<3)
    mu.reg_write(UC_X86_REG_CS, 1<<3)
