"""Post-fix pyaxml output: encode layout dimension attributes (e.g. 1600dp) and gravity correctly."""
import struct
def strings(b):
    off=8; t,hs,sz=struct.unpack_from('<HHI',b,off); assert t==1
    cnt,scnt,flags,sstart,_=struct.unpack_from('<IIIII',b,off+8); utf8=flags&0x100
    offs=struct.unpack_from('<%dI'%cnt,b,off+hs); out=[]
    for o in offs:
        p=off+sstart+o
        if utf8:
            n=b[p]; p+=2 if n&0x80 else 1
            n=b[p]; 
            if n&0x80: n=((n&0x7f)<<8)|b[p+1]; p+=2
            else: p+=1
            out.append(b[p:p+n].decode('utf8'))
        else:
            n=struct.unpack_from('<H',b,p)[0]; p+=2
            out.append(b[p:p+2*n].decode('utf-16le'))
    return out, off+sz
def fix(b, dims, ints):
    b=bytearray(b); S,p=strings(b)
    while p<len(b):
        t,hs,sz=struct.unpack_from('<HHI',b,p)
        if t==0x0102:
            ast,asz,acnt=struct.unpack_from('<HHH',b,p+16+8)
            a0=p+16+ast
            for i in range(acnt):
                a=a0+i*asz; ns,name,raw=struct.unpack_from('<iii',b,a)
                nm=S[name] if name>=0 else ''
                if nm in dims:
                    struct.pack_into('<iHBBI',b,a+8,-1,8,0,0x05,(dims[nm]<<8)|1)   # TYPE_DIMENSION, radix 23p0, unit dip
                    print('fixed',nm,dims[nm],'dp')
                if nm in ints:
                    struct.pack_into('<iHBBI',b,a+8,-1,8,0,0x11,ints[nm])          # TYPE_INT_HEX
                    print('fixed',nm,hex(ints[nm]))
        p+=sz
    return bytes(b)
