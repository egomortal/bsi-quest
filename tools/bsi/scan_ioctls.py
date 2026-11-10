#!/usr/bin/env python3
"""List the KGSL ioctl request codes an aarch64 ELF (e.g. Meta's /vendor/lib64/libgsl.so or hw/vulkan.adreno.so) builds.
Usage: python tools\\bsi\\scan_ioctls.py <file.so> [...]
Scans .text for MOVZ/MOVK (W or X) pairs that assemble a 32-bit constant whose type byte is 0x09 (KGSL_IOC_TYPE) and whose
direction bits are a valid _IOC direction, plus raw 32-bit words in data sections; prints nr, dir, size and the msm_kgsl.h names.
Written 3 Nov 2026 (chat4-drv5) to learn which KGSL ioctls Qualcomm's own stack uses on the Quest kernel."""
import struct, sys, collections
NAMES = {2: ['IOCTL_KGSL_DEVICE_GETPROPERTY[WR]'], 6: ['IOCTL_KGSL_DEVICE_WAITTIMESTAMP[W]'], 7: ['IOCTL_KGSL_DEVICE_WAITTIMESTAMP_CTXTID[W]'], 16: ['IOCTL_KGSL_RINGBUFFER_ISSUEIBCMDS[WR]'], 17: ['IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_OLD[R]', 'IOCTL_KGSL_CMDSTREAM_READTIMESTAMP[WR]'], 18: ['IOCTL_KGSL_CMDSTREAM_FREEMEMONTIMESTAMP[W]', 'IOCTL_KGSL_CMDSTREAM_FREEMEMONTIMESTAMP_OLD[R]'], 19: ['IOCTL_KGSL_DRAWCTXT_CREATE[WR]'], 20: ['IOCTL_KGSL_DRAWCTXT_DESTROY[W]'], 21: ['IOCTL_KGSL_MAP_USER_MEM[WR]'], 22: ['IOCTL_KGSL_CMDSTREAM_READTIMESTAMP_CTXTID[WR]'], 23: ['IOCTL_KGSL_CMDSTREAM_FREEMEMONTIMESTAMP_CTXTID[W]'], 32: ['IOCTL_KGSL_SHAREDMEM_FROM_PMEM[WR]'], 33: ['IOCTL_KGSL_SHAREDMEM_FREE[W]'], 49: ['IOCTL_KGSL_CFF_USER_EVENT[W]', 'IOCTL_KGSL_TIMESTAMP_EVENT_OLD[W]'], 34: ['IOCTL_KGSL_DRAWCTXT_BIND_GMEM_SHADOW[W]'], 35: ['IOCTL_KGSL_SHAREDMEM_FROM_VMALLOC[WR]'], 36: ['IOCTL_KGSL_SHAREDMEM_FLUSH_CACHE[W]'], 37: ['IOCTL_KGSL_DRAWCTXT_SET_BIN_BASE_OFFSET[W]'], 46: ['IOCTL_KGSL_CMDWINDOW_WRITE[W]'], 47: ['IOCTL_KGSL_GPUMEM_ALLOC[WR]'], 48: ['IOCTL_KGSL_CFF_SYNCMEM[W]'], 50: ['IOCTL_KGSL_SETPROPERTY[W]'], 51: ['IOCTL_KGSL_TIMESTAMP_EVENT[WR]'], 52: ['IOCTL_KGSL_GPUMEM_ALLOC_ID[WR]'], 53: ['IOCTL_KGSL_GPUMEM_FREE_ID[WR]'], 54: ['IOCTL_KGSL_GPUMEM_GET_INFO[WR]'], 55: ['IOCTL_KGSL_GPUMEM_SYNC_CACHE[W]'], 56: ['IOCTL_KGSL_PERFCOUNTER_GET[WR]'], 57: ['IOCTL_KGSL_PERFCOUNTER_PUT[W]'], 58: ['IOCTL_KGSL_PERFCOUNTER_QUERY[WR]'], 59: ['IOCTL_KGSL_PERFCOUNTER_READ[WR]'], 60: ['IOCTL_KGSL_GPUMEM_SYNC_CACHE_BULK[WR]'], 61: ['IOCTL_KGSL_SUBMIT_COMMANDS[WR]'], 64: ['IOCTL_KGSL_SYNCSOURCE_CREATE[WR]'], 65: ['IOCTL_KGSL_SYNCSOURCE_DESTROY[WR]'], 66: ['IOCTL_KGSL_SYNCSOURCE_CREATE_FENCE[WR]'], 67: ['IOCTL_KGSL_SYNCSOURCE_SIGNAL_FENCE[WR]'], 68: ['IOCTL_KGSL_CFF_SYNC_GPUOBJ[W]'], 69: ['IOCTL_KGSL_GPUOBJ_ALLOC[WR]'], 70: ['IOCTL_KGSL_GPUOBJ_FREE[W]'], 71: ['IOCTL_KGSL_GPUOBJ_INFO[WR]'], 72: ['IOCTL_KGSL_GPUOBJ_IMPORT[WR]'], 73: ['IOCTL_KGSL_GPUOBJ_SYNC[W]'], 74: ['IOCTL_KGSL_GPU_COMMAND[WR]'], 75: ['IOCTL_KGSL_PREEMPTIONCOUNTER_QUERY[WR]'], 76: ['IOCTL_KGSL_GPUOBJ_SET_INFO[W]'], 80: ['IOCTL_KGSL_SPARSE_PHYS_ALLOC[WR]'], 81: ['IOCTL_KGSL_SPARSE_PHYS_FREE[W]'], 82: ['IOCTL_KGSL_SPARSE_VIRT_ALLOC[WR]'], 83: ['IOCTL_KGSL_SPARSE_VIRT_FREE[W]'], 84: ['IOCTL_KGSL_SPARSE_BIND[W]'], 85: ['IOCTL_KGSL_GPU_SPARSE_COMMAND[WR]'], 86: ['IOCTL_KGSL_GPUMEM_BIND_RANGES[WR]'], 87: ['IOCTL_KGSL_GPU_AUX_COMMAND[WR]'], 88: ['IOCTL_KGSL_TIMELINE_CREATE[WR]'], 89: ['IOCTL_KGSL_TIMELINE_WAIT[W]'], 90: ['IOCTL_KGSL_TIMELINE_QUERY[WR]'], 91: ['IOCTL_KGSL_TIMELINE_SIGNAL[W]'], 92: ['IOCTL_KGSL_TIMELINE_FENCE_GET[WR]'], 93: ['IOCTL_KGSL_TIMELINE_DESTROY[W]'], 94: ['IOCTL_KGSL_GET_FAULT_REPORT[WR]'], 95: ['IOCTL_KGSL_RECURRING_COMMAND[WR]'], 96: ['IOCTL_KGSL_READ_CALIBRATED_TIMESTAMPS[WR]']}
DIRS = {0: '', 1: 'W', 2: 'R', 3: 'WR'}
def sections(b):
    shoff = struct.unpack_from('<Q', b, 0x28)[0]; ent, num, strndx = struct.unpack_from('<HHH', b, 0x3A)
    secs = [struct.unpack_from('<IIQQQQIIQQ', b, shoff + i * ent) for i in range(num)]
    so = secs[strndx][4]
    def nm(o): e = b.index(b'\x00', so + o); return b[so + o:e].decode()
    return [(nm(s[0]), s[1], s[2], s[4], s[5]) for s in secs]
def decode(v):
    typ = (v >> 8) & 0xff; nr = v & 0xff; size = (v >> 16) & 0x3fff; d = (v >> 30) & 3
    if typ != 0x09 or size == 0 or size > 0x400: return None
    return nr, DIRS[d], size
def scan(path):
    b = open(path, 'rb').read(); found = collections.Counter()
    for name, typ, flags, off, size in sections(b):
        if size == 0 or typ == 8: continue
        data = b[off:off + size - size % 4]
        words = struct.unpack('<%dI' % (len(data) // 4), data)
        if name == '.text':
            last = {}
            for w in words:
                if (w & 0x7f800000) == 0x52800000 and ((w >> 21) & 3) == 0:   # MOVZ, hw 0
                    last[w & 31] = (w >> 5) & 0xffff
                elif (w & 0x7f800000) == 0x72800000 and ((w >> 21) & 3) == 1:  # MOVK, hw 1 (lsl 16)
                    rd = w & 31
                    if rd in last:
                        v = last.pop(rd) | (((w >> 5) & 0xffff) << 16); r = decode(v)
                        if r: found[(v,) + r] += 1
                elif (w & 0x7f800000) == 0x52800000 and ((w >> 21) & 3) == 1:  # MOVZ hw1 alone (low half 0) - rare
                    pass
        else:
            for w in words:
                r = decode(w)
                if r and (w >> 30) != 0: found[(w,) + r] += 0
    print('==', path)
    for (v, nr, d, size), cnt in sorted(found.items(), key=lambda x: x[0][1]):
        print('  0x%08x nr 0x%02x dir %-2s size %4d  x%-3d %s' % (v, nr, d, size, cnt, ', '.join(NAMES.get(nr, ['?']))))
for p in sys.argv[1:]:
    scan(p)
