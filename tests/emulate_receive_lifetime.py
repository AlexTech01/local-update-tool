"""Run NS's actual receive-object cleanup and commit guard with mocked NIM.

Requires Unicorn and lz4. Input: the supplied Ounce 23.0.1 NS NSO.
This is not a Horizon/NIM emulator and does not run an actual installation.
"""
from pathlib import Path
import hashlib,struct,sys
import lz4.block
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *

B=0x7100000000;S=0x80000000;D=0x90000000;TLS=0xa0000000;STOP=0xb0000000
APP=0x01003a400c3da000
nso=Path(sys.argv[1]).read_bytes()
assert hashlib.sha256(nso).hexdigest()=='f55581c6652576ef1631cf7b444f3212807801f0bcf7bc7014a722e45ea70277'
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM);u.ctl_set_cpu_model(UC_CPU_ARM64_MAX)
u.mem_map(B,0x800000)
for i in range(3):
    offset,address,size=struct.unpack_from('<III',nso,0x10+i*0x10)
    packed=struct.unpack_from('<I',nso,0x60+i*4)[0]
    u.mem_write(B+address,lz4.block.decompress(nso[offset:offset+packed],uncompressed_size=size))
for address,size in ((S,0x200000),(D,0x10000),(TLS,0x1000),(STOP,0x1000)):u.mem_map(address,size)
u.reg_write(UC_ARM64_REG_TPIDRRO_EL0,TLS);u.mem_write(TLS+0x110,struct.pack('<I',1))
X=[UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,
   UC_ARM64_REG_X4,UC_ARM64_REG_X5,UC_ARM64_REG_X6,UC_ARM64_REG_X7]
task_id=bytes(range(16));present=True;state=3;deleted=commits=0
def ret(value=0):
    u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
def hook(cpu,address,size,user):
    global present,state,deleted,commits
    if address==STOP:cpu.emu_stop();return
    at=address-B;a=[cpu.reg_read(r) for r in X]
    if at==0x101200: # NIM list application receive tasks
        assert a[1:3]==[1,APP]
        if present:cpu.mem_write(a[0],task_id)
        ret(int(present));return
    if at==0x101040: # NIM receive-task info
        assert present and bytes(cpu.mem_read(a[1],16))==task_id
        cpu.mem_write(a[0],bytes([state])+bytes(0x37));ret();return
    if at==0x100e10: # NIM command 54: receive-task cleanup
        assert present and bytes(cpu.mem_read(a[0],16))==task_id
        present=False;deleted+=1;ret();return
    if at==0x1e0e24: # Native commit work beyond the readiness guard
        assert present and state==3;commits+=1;state=4;ret();return
    if at in (0x17903c,0xfc804,0x1e081c,0xdf338):ret();return
    opcode,=struct.unpack('<I',cpu.mem_read(address,4))
    if opcode in (0xd503237f,0xd50323ff):cpu.reg_write(UC_ARM64_REG_PC,address+4);return
    if opcode==0xd65f0fff:cpu.reg_write(UC_ARM64_REG_PC,cpu.reg_read(UC_ARM64_REG_LR));return
u.hook_add(UC_HOOK_CODE,hook)
def run(at,args):
    u.reg_write(UC_ARM64_REG_SP,S+0x1ff000);u.reg_write(UC_ARM64_REG_LR,STOP)
    for register,value in zip(X,args):u.reg_write(register,value)
    u.emu_start(B+at,STOP+4,count=100000)
    assert u.reg_read(UC_ARM64_REG_PC)==STOP,hex(u.reg_read(UC_ARM64_REG_PC))
    return u.reg_read(UC_ARM64_REG_X0)&0xffffffff
def ready():
    u.mem_write(D+0x1000,b'\xff')
    assert run(0x1e0d94,[0,D+0x1000,APP])==0
    return bytes(u.mem_read(D+0x1000,1))==b'\x01'
obj=D+0x2000;context=D+0x4000
# Model a completed/joined worker; the destructor's thread/event flags are zero.
# Its NIM-task cleanup block still executes, as in the normal destruction path.
u.mem_write(obj+0x298,struct.pack('<Q',APP))
assert ready()
run(0x22abd8,[obj]);assert not present and deleted==1
assert not ready()
assert run(0x1e0c6c,[context,APP])==0x6cc10 and commits==0
print('PASS: actual NS destructor calls task cleanup; missing task makes the actual commit guard return 0x6CC10')
present=True;state=3
assert ready()
assert run(0x1e0c6c,[context,APP])==0 and commits==1 and present and state==4
run(0x22abd8,[obj]);assert not present and deleted==2
print('PASS: retaining state-3 task reaches commit work; closing afterward performs cleanup')
print('NIM task operations, running-application callback and commit work were mocked; hardware installation was not emulated.')
