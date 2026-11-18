#!/usr/bin/env python3
# ChaseGuard - binary patcher. License: GPL-3.0 (see ../LICENSE.txt)
# Input : orig.asi  (base MWCrashFix build, SHA-256 in BUILD.txt)
# Output: MWCrashFix.asi (patched, still carries the old name; rename.py renames it)
import struct, subprocess
SRC='orig.asi'; DST='MWCrashFix.asi'
d=bytearray(open(SRC,'rb').read())
IB=0x10000000
def off(va):                      # VA -> file offset (.text only)
    r=va-IB
    assert 0x1000<=r<0x12000, hex(va)
    return r-0x1000+0x400
def rd(va,n): return bytes(d[off(va):off(va)+n])
def wr(va,b): d[off(va):off(va)+len(b)]=b

cave=open('caves.bin','rb').read()
CAVE0=0x10011f25
sym={l.split()[2]:int(l.split()[0],16) for l in subprocess.check_output(['nm','caves.elf']).decode().splitlines()}

patched_ranges=[]     # (start_va, end_va_exclusive, tag)
keep_reloc=set()
def patch(va,old,new,tag):
    assert len(old)==len(new),(tag,len(old),len(new))
    assert rd(va,len(old))==old,(tag,rd(va,len(old)).hex(),old.hex())
    wr(va,new); patched_ranges.append((va,va+len(old),tag))
def jmp(site,target): return b'\xe9'+struct.pack('<i',target-(site+5))

# 0. cave (unused zero padding at the end of .text)
assert rd(CAVE0,len(cave))==b'\0'*len(cave)
wr(CAVE0,cave)
# 1. P: early filter (0x10001171: mov eax,[edi+4]; push [ecx+0xc])
patch(0x10001171,bytes.fromhex('8b4704ff710c'),jmp(0x10001171,sym['cP'])+b'\x90','P')
# 2. C: skip decoding when the fault address is not executable
patch(0x100011de,bytes.fromhex('8b076a018b480c'),jmp(0x100011de,sym['cC'])+b'\x90\x90','C')
# 3. page protection mask: PAGE_EXECUTE_READ(0x20) -> any executable type (0xF0)
patch(0x1000120b,bytes.fromhex('f6442424'+'20'),bytes.fromhex('f6442424'+'f0'),'mask1')
patch(0x10001295,bytes.fromhex('f6442424'+'20'),bytes.fromhex('f6442424'+'f0'),'mask2')
# 4. A
patch(0x10001216,bytes.fromhex('8b07b901000000'),jmp(0x10001216,sym['cA'])+b'\x90\x90','A')
# 5. B
patch(0x10001271,bytes.fromhex('6a1c8d442414'),jmp(0x10001271,sym['cB'])+b'\x90','B')
# 6. register dump: stderr -> log file (the abs operand stays at 0x100010e8, so its relocation stays valid)
patch(0x100010e4,bytes.fromhex('6a02ff158820011083c40450'),
      bytes.fromhex('9090'+'ff35c8630810'+'90909090'),'dump->log'); keep_reloc.add(0x100010e8-IB)
# 7. D: failed check -> exit without installing the handler
patch(0x1000150b,bytes.fromhex('6a00ff1540200110'),jmp(0x1000150b,sym['cD'])+b'\x90\x90\x90','D')
# 8. E: log open failed -> NUL
patch(0x10001385,bytes.fromhex('6a1068c8230110'+'68d4230110'+'50'+'ff1540200110'),
      jmp(0x10001385,sym['cE'])+b'\x90'*14,'E')

# ---------- base relocations ----------
pe=struct.unpack_from('<I',d,0x3c)[0]; opt=pe+24
rel_rva,rel_sz=struct.unpack_from('<II',d,opt+96+5*8)
rel_off=0x85800
blocks=[]; o=0
while o<rel_sz:
    pg,bs=struct.unpack_from('<II',d,rel_off+o)
    ents=list(struct.unpack_from('<%dH'%((bs-8)//2),d,rel_off+o+8)); blocks.append([pg,ents]); o+=bs
def intersects(rva):
    for a,b,t in patched_ranges:
        if rva < b-IB and rva+4 > a-IB: return True
    return False
neut=[]
for blk in blocks:
    for i,e in enumerate(blk[1]):
        if e>>12==3:
            rva=blk[0]+(e&0xfff)
            if intersects(rva) and rva not in keep_reloc:
                blk[1][i]=0; neut.append(hex(rva+IB))
print("neutralized relocs:",neut)
# new relocations for the absolute operands inside the cave
absvals=[0x100123b4,sym['nulstr'],0x10012040,0x1001208c,0x100863c8]
new=[]
for i in range(len(cave)-3):
    v=struct.unpack_from('<I',cave,i)[0]
    if v in absvals: new.append(CAVE0+i)
print("new abs fields:",[hex(x) for x in new])
assert len(new)==5 and len({rd(x,4) for x in new})==5
blk11=[b for b in blocks if b[0]==0x11000][0]
for va in new:
    rva=va-IB; assert rva>>12==0x11
    blk11[1].append((3<<12)|(rva&0xfff))
if len(blk11[1])%2: blk11[1].append(0)     # keep block size a multiple of 4
out=b''
for pg,ents in blocks:
    e=list(ents)
    if len(e)%2: e.append(0)
    out+=struct.pack('<II',pg,8+2*len(e))+struct.pack('<%dH'%len(e),*e)
assert len(out)<=0x2400, len(out)
d[rel_off:rel_off+0x2400]=out+b'\0'*(0x2400-len(out))
struct.pack_into('<II',d,opt+96+5*8,rel_rva,len(out))
sec=opt+0xe0
for i in range(5):
    n=bytes(d[sec+40*i:sec+40*i+8]).rstrip(b'\0')
    if n==b'.reloc': struct.pack_into('<I',d,sec+40*i+8,len(out))
    if n==b'.text': struct.pack_into('<I',d,sec+40*i+8,0x11000)   # make the cave part of the section
print("reloc size %#x -> %#x"%(rel_sz,len(out)))
open(DST,'wb').write(d)
print("written",DST,len(d))
