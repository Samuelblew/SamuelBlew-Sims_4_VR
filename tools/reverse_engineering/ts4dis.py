import sys, struct, capstone
raw=open(r"D:\Games\The Sims 4\Game\Bin\TS4_x64.exe","rb").read()
pe_off=struct.unpack_from("<I",raw,0x3c)[0]; nsec=struct.unpack_from("<H",raw,pe_off+6)[0]; osz=struct.unpack_from("<H",raw,pe_off+20)[0]
secs=[struct.unpack_from("<8sIIII",raw,pe_off+24+osz+i*40) for i in range(nsec)]
class P:
    def get_data(self,rva,n):
        for nm,vs,va,rs,rp in secs:
            if va<=rva<va+vs: o=rp+rva-va; return raw[o:o+n]
pe=P()
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64)
for a in sys.argv[2:]:
    va = int(a, 16); rva = va - 0x140000000
    data = pe.get_data(rva, int(sys.argv[1]))
    print("=== %x" % va)
    for i in md.disasm(data, va):
        print("%x  %s %s" % (i.address, i.mnemonic, i.op_str))
        if i.mnemonic == "ret": break
