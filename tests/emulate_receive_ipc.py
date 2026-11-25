"""Execute the built NRO's receive wrapper up to IPC send, without running NS.

Requires Unicorn. Arguments: corrected ELF, native input fixture, optional old ELF.
The old SDK reproduction is optional; no firmware instructions are copied here.
"""
from pathlib import Path
import shutil,struct,subprocess,sys
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *

B=0x7100000000;S=0x80000000;D=0x90000000;TLS=0xa0000000;STOP=0xb0000000
APP=0x01003a400c3da000
nm=shutil.which('aarch64-none-elf-nm') or '/opt/devkitpro/devkitA64/bin/aarch64-none-elf-nm'

def capture(path):
    data=Path(path).read_bytes()
    assert data[:6]==b'\x7fELF\x02\x01' and struct.unpack_from('<H',data,18)[0]==183
    table,=struct.unpack_from('<Q',data,32)
    stride,count=struct.unpack_from('<HH',data,54)
    segments=[struct.unpack_from('<IIQQQQQQ',data,table+i*stride) for i in range(count)]
    segments=[p for p in segments if p[0]==1]
    u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.ctl_set_cpu_model(UC_CPU_ARM64_MAX)
    end=max(p[3]+p[6] for p in segments)
    u.mem_map(B,(end+0xfff)&~0xfff)
    for _,_,offset,address,_,size,_,_ in segments:u.mem_write(B+address,data[offset:offset+size])
    for address,size in ((S,0x200000),(D,0x10000),(TLS,0x1000),(STOP,0x1000)):u.mem_map(address,size)
    symbols={line.split()[-1]:B+int(line.split()[0],16) for line in
        subprocess.check_output([nm,'--defined-only',str(path)],text=True).splitlines() if len(line.split())==3}
    api='__wrap_nsRequestReceiveApplication' if '__wrap_nsRequestReceiveApplication' in symbols else 'nsRequestReceiveApplication'
    x=[UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,
       UC_ARM64_REG_X4,UC_ARM64_REG_X5,UC_ARM64_REG_X6]
    def ret(value=0):
        u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
    def hook(cpu,address,size,user):
        if address==symbols['svcSendSyncRequest']:u.emu_stop();return
        if address==symbols['hosversionGet']:ret(0x170001);return
        if address==symbols['nsGetApplicationManagerInterface']:
            cpu.mem_write(cpu.reg_read(UC_ARM64_REG_X0),struct.pack('<IIIHH',3,1,0,0,0));ret();return
        if address in (symbols.get('labLog'),symbols.get('labLogBytes')):ret();return
        if address==STOP:raise AssertionError('Wrapper returned before IPC send')
    u.hook_add(UC_HOOK_CODE,hook)
    u.reg_write(UC_ARM64_REG_TPIDRRO_EL0,TLS)
    u.reg_write(UC_ARM64_REG_SP,S+0x1ff000);u.reg_write(UC_ARM64_REG_LR,STOP)
    u.mem_write(D+0x1000,struct.pack('<QIBBH',APP|0x800,0x40000,0x81,0,0))
    for register,value in zip(x,(D,0xa9fe3b02,55556,APP,D+0x1000,1,6)):u.reg_write(register,value)
    u.emu_start(symbols[api],STOP+4,count=100000)
    assert u.reg_read(UC_ARM64_REG_PC)==symbols['svcSendSyncRequest']
    ipc=bytes(u.mem_read(TLS,0x100));offset=ipc.index(b'SFCI')
    assert struct.unpack_from('<I',ipc,offset+8)[0]==2010
    # Non-domain CMIF request with one map-alias input buffer: raw words
    # include 16 alignment bytes and a 16-byte CMIF header, then scalar input.
    words=struct.unpack_from('<I',ipc,4)[0]&0x3ff
    length=words*4-32
    return ipc[offset+16:offset+16+length]

fixture=bytes.fromhex(Path(sys.argv[2]).read_text())
assert len(fixture)==16
actual=capture(sys.argv[1]);assert actual==fixture,(actual.hex(),fixture.hex())
print('PASS: built AArch64 receive wrapper emits the exact native 16-byte command-2010 input:',actual.hex())
if len(sys.argv)>3:
    old=capture(sys.argv[3]);assert len(old)==24 and old!=fixture,old.hex()
    title,=struct.unpack_from('<Q',old,8);assert title!=APP
    print(f'PASS: old SDK reproduction emits {len(old)} bytes; NS would read application ID {title:016x} instead of {APP:016x}')
