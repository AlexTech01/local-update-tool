"""Reproduce NS's local send-key lookup using the supplied 23.0.1 NS NSO.

Run: python3 tests/emulate_delivery_keys.py /path/to/ns/main
Requires Unicorn/lz4 and a C compiler. NCM lookups, runtime configuration and
SHA-256/HMAC implementation are substituted; native record validators, version
comparison and command-2007 control flow execute. No firmware key or instruction
bytes are copied into the project or printed.
"""
import ctypes,hashlib,hmac,struct,subprocess,sys,tempfile
from pathlib import Path
import lz4.block
from unicorn import Uc,UC_ARCH_ARM64,UC_MODE_ARM,UC_HOOK_CODE
from unicorn.arm64_const import *

P=Path(__file__).resolve().parents[1]
B=0x7100000000;S=0x80000000;D=0x90000000;TLS=0xa0000000;STOP=0xb0000000
record=bytes.fromhex((P/'tests/fixtures/newer-donor-delivery.hex').read_text())
reply=bytes.fromhex((P/'tests/fixtures/newer-donor-sender.hex').read_text())
APP=struct.unpack_from('<Q',record,8)[0]
LOCAL_VERSION=0xa30000;DONOR_VERSION=0xa40000
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
lookups=0
def ret(value=0):
    u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
def hook(cpu,address,size,user):
    global lookups
    if address==STOP:cpu.emu_stop();return
    at=address-B;a=[cpu.reg_read(r) for r in X]
    if at==0x1a0338:ret(D+0x3000);return # Normal runtime flags.
    if at in (0xd47d0,0xd4854):ret(1);return # Protocol configuration bounds.
    if at==0x184858:ret(1);return # NS delivery stopper held.
    if at==0x1008e0:ret(0);return # No conflicting local task.
    if at==0x124da0:
        assert a[1]==a[5]==32 and a[3]==0xe0
        digest=hmac.new(bytes(cpu.mem_read(a[4],a[5])),bytes(cpu.mem_read(a[2],a[3])),hashlib.sha256).digest()
        cpu.mem_write(a[0],digest);ret();return
    if at==0x11b318:
        assert a[3]==APP;lookups+=1
        key=struct.pack('<QIBBH',APP|0x800,LOCAL_VERSION,0x81,0,0)
        cpu.mem_write(a[1],bytes(24));cpu.mem_write(a[2],b'\x01'+bytes(7)+key)
        ret();return # Controlled local NCM lookup boundary.
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
obj=D+0x2000;remote=D+0x4000;result=D+0x5000;keys=D+0x6000
u.mem_write(obj,struct.pack('<Q',D+0x7000));u.mem_write(remote,record)
assert run(0x22d234,[0,remote])==0
assert run(0x22da50,[obj,result,remote,1,remote,1])==0
assert bytes(u.mem_read(result,4))==bytes(4)
print('PASS: native NS validator authenticates the captured donor record; comparing it with itself returns zero')
corrupt=bytearray(record);corrupt[0x10]^=1;u.mem_write(remote,bytes(corrupt))
assert run(0x22da50,[obj,result,remote,1,remote,1])==0x74410
u.mem_write(remote,record)
print('PASS: native NS comparison rejects an altered donor version with HMAC result 0x74410')
assert run(0x22dcb4,[obj,result,keys,18,0,remote,1])==0 and lookups==1
local_key=bytes(u.mem_read(keys,16))
assert struct.unpack_from('<I',local_key,8)[0]==LOCAL_VERSION
assert bytes(u.mem_read(remote,256))==record
print('PASS: actual NS command-2007 handler returns the controlled LOCAL A3 Patch despite authenticated remote A4 metadata')
with tempfile.TemporaryDirectory(prefix='local-update-donor-keys-') as work:
    library=Path(work)/'wire.dylib'
    subprocess.run(['cc','-shared','-fPIC','-I'+str(P/'include'),str(P/'source/delivery_wire.c'),'-o',str(library)],check=True)
    lib=ctypes.CDLL(str(library))
    lib.labPatchKeysFromDelivery.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.POINTER(ctypes.c_uint),ctypes.c_void_p,ctypes.c_uint,ctypes.c_uint64]
    lib.labPatchKeysFromDelivery.restype=ctypes.c_bool
    src=ctypes.create_string_buffer(record);out=ctypes.create_string_buffer(18*16);count=ctypes.c_uint()
    assert lib.labPatchKeysFromDelivery(out,18,ctypes.byref(count),src,1,APP) and count.value==1
    assert out.raw[:16]==reply[8:] and out.raw[:16]!=local_key and src.raw[:256]==record
    assert struct.unpack_from('<I',out.raw,8)[0]==DONOR_VERSION
print('PASS: corrected donor selection exactly matches captured sender A4 key without changing delivery/HMAC/firmware fields')
print('NCM lookup, protocol configuration and HMAC primitive were substituted; wireless transfer and installation were not emulated.')
