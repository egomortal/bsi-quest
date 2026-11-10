import sys; sys.path.insert(0,'/home/claude/bsi')
from lib import pe, IB
from capstone import *
img=pe.get_memory_mapped_image(); md=Cs(CS_ARCH_X86,CS_MODE_32)
a=int(sys.argv[1],16); n=int(sys.argv[2],16) if len(sys.argv)>2 else 0x100
for i in md.disasm(img[a-IB:a-IB+n],a): print(hex(i.address),i.mnemonic,i.op_str)
