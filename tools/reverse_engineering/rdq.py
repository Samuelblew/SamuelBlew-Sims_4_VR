import sys, struct
raw=open(r"D:\Games\The Sims 4\Game\Bin\TS4_x64.exe","rb").read()
pe_off=struct.unpack_from("<I",raw,0x3c)[0]; nsec=struct.unpack_from("<H",raw,pe_off+6)[0]; osz=struct.unpack_from("<H",raw,pe_off+20)[0]
secs=[struct.unpack_from("<8sIIII",raw,pe_off+24+osz+i*40) for i in range(nsec)]
def off(va):
    rva=va-0x140000000
    for nm,vs,v,rs,rp in secs:
        if v<=rva<v+vs: return rp+rva-v
va=int(sys.argv[1],16); n=int(sys.argv[2])
for i in range(n): print(hex(va+8*i), hex(struct.unpack_from("<Q",raw,off(va+8*i))[0]))
