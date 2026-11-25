"""Check the supplied LRP's actual management serializers/parsers with Unicorn.

Requires unicorn, lz4 and a C compiler. This does not emulate networking or NS.
Only generated protocol fixtures are written; firmware instructions stay local.
"""
import sys,struct,ctypes,subprocess,lz4.block
from pathlib import Path

from unicorn import *
from unicorn.arm64_const import *
P=Path(__file__).resolve().parents[1]
B=0x7100000000;S=0x80000000;D=0x90000000;TLS=0xa0000000;STOP=0xb0000000
nso=Path(sys.argv[1]).read_bytes()
import hashlib,tempfile
assert hashlib.sha256(nso).hexdigest()=='527905e4be5659448cd7bbff1dbb83a58973dcfe1db3b2f41e709db6cf12f41f', 'Expected the supplied 23.0.1 LRP NSO'
work=tempfile.TemporaryDirectory(prefix='local-update-native-wire-')
u=Uc(UC_ARCH_ARM64,UC_MODE_ARM)
u.ctl_set_cpu_model(UC_CPU_ARM64_MAX)
u.mem_map(B,0x500000)
for i in range(3):
 off,addr,size=struct.unpack_from('<III',nso,0x10+i*0x10);packed=struct.unpack_from('<I',nso,0x60+i*4)[0]
 data=lz4.block.decompress(nso[off:off+packed],uncompressed_size=size)
 u.mem_write(B+addr,data)
u.mem_map(S,0x200000);u.mem_map(D,0x100000);u.mem_map(TLS,0x1000);u.mem_map(STOP,0x1000)
u.reg_write(UC_ARM64_REG_TPIDRRO_EL0,TLS)
u.mem_write(TLS+0x110,struct.pack('<I',1))
output=bytearray();rx=bytearray();halt_at=None;advertisement=None;canonical_hook=False
socket_options=[]
sleep_ns=[]
from collections import deque
trace=deque(maxlen=300)
X=[UC_ARM64_REG_X0,UC_ARM64_REG_X1,UC_ARM64_REG_X2,UC_ARM64_REG_X3,UC_ARM64_REG_X4,UC_ARM64_REG_X5,UC_ARM64_REG_X6,UC_ARM64_REG_X7]
def ret(value=0):
 u.reg_write(UC_ARM64_REG_X0,value);u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR))
def hook(u,address,size,user):
 global rx,advertisement
 at=address-B
 trace.append(hex(at))
 if address==STOP or address==halt_at:u.emu_stop();return
 op=struct.unpack('<I',u.mem_read(address,4))[0]
 if op in (0xd503237f,0xd50323ff) and at not in (0x2283d0,0x5c80,0x2283e0,0x11f80,0x11400,0xf570,0xf610,0xb43e0,0xac320,0xab320,0x18f60,0x13530,0x166d0,0x13da0,0x340):u.reg_write(UC_ARM64_REG_PC,address+4);return
 if op==0xd65f0fff:u.reg_write(UC_ARM64_REG_PC,u.reg_read(UC_ARM64_REG_LR));return
 a=[u.reg_read(x) for x in X]
 if at in (0x2283d0,0x5c80):u.mem_write(a[0],bytes(u.mem_read(a[1],a[2])));ret(a[0])
 elif at==0x2283e0:u.mem_write(a[0],bytes([a[1]&255])*a[2]);ret(a[0])
 elif at==0x11f80:output.extend(u.mem_read(a[1],a[2]));ret(a[2])
 elif at==0x11400:
  got=bytes(rx[:a[2]])
  if not(a[3]&2):del rx[:len(got)]
  u.mem_write(a[1],got);ret(len(got))
 elif at==0x18f60:
  assert a[:3]==[2,1,6];ret(3)
 elif at==0x13530:
  assert a[0]==3;ret(4)
 elif at==0x166d0:
  socket_options.append((a[0],a[1],a[2],bytes(u.mem_read(a[3],a[4]))));ret(0)
 elif at==0x13da0:ret(0)
 elif at==0x340:sleep_ns.append(a[0]);ret(0)
 elif at==0xab320:ret(0)
 elif at==0xb43e0:
  u.reg_write(UC_ARM64_REG_X15,D+0x81000);ret(D+0x80000)
 elif address==STOP+0x100:
  data,size=struct.unpack('<QQ',u.mem_read(a[1],16));advertisement=bytes(u.mem_read(data,size));ret(0)
 elif at==0xac320 and canonical_hook:
  u.mem_write(a[1],bytes(container));ret(0)
 elif at in (0xf570,0xf610):raise AssertionError(('unexpected lock contention',hex(at)))
u.hook_add(UC_HOOK_CODE,hook)
def run(at,args):
 u.reg_write(UC_ARM64_REG_SP,S+0x1ff000);u.reg_write(UC_ARM64_REG_LR,STOP)
 for reg,value in zip(X,args):u.reg_write(reg,value)
 for index,value in enumerate(args[8:]):u.mem_write(S+0x1ff000+index*8,struct.pack('<Q',value))
 
 try:u.emu_start(B+at,STOP+4,count=100000)
 except UcError:
  pc=u.reg_read(UC_ARM64_REG_PC);print("CPU failure",hex(pc),[x for x in trace if 0xafb00<=int(x,16)<=0xb3000 or 0x5000<=int(x,16)<=0x9000],[hex(u.reg_read(reg)) for reg in X]);raise
 assert u.reg_read(UC_ARM64_REG_PC) in (STOP,halt_at),hex(u.reg_read(UC_ARM64_REG_PC))
 return u.reg_read(UC_ARM64_REG_X0)
# Record the real instructions' setsockopt arguments on both paths. In 0.2.3
# the accepted path's -0x7fffffff immediate was incorrectly transcribed.
assert run(0xadd30,[D+0xf0000])==0
assert struct.unpack('<I',u.mem_read(D+0xf0000,4))[0]==3
accepted_host=D+0x50000
u.mem_write(accepted_host+0xd8,b'\x01')
u.mem_write(accepted_host+0x9530,struct.pack('<I',3))
halt_at=B+0x9ec54
run(0x9eb80,[accepted_host]);halt_at=None
expected_options=[]
for fd in (3,4):
 expected_options.extend([(fd,0xffff,0x80000001,struct.pack('<II',1,1)),(fd,0xffff,4,struct.pack('<I',1))])
assert socket_options==expected_options,socket_options
print('PASS: native listener and accepted socket setup both use option 0x80000001, {1,1}, then reuse-address')
# The native host pauses before +0xa7900 sends final result packets. Record
# the actual delay argument without blocking this test or emulating threads.
u.reg_write(UC_ARM64_REG_X19,D+0x40000)
halt_at=B+0x9ffb8
run(0x9ffa4,[]);halt_at=None
assert sleep_ns==[500000000],sleep_ns
print('PASS: native host completion path waits 500,000,000 ns before sending final results')
app=0x01003a400c3da000
# Execute the original command-2010 encoder up to the native IPC-send boundary.
# This checks the actual scalar offsets independently of our SDK and wrapper.
u.mem_write(TLS+0x1f8,struct.pack('<Q',D+0xe0000))
u.mem_write(D+0xd0004,struct.pack('<I',3))
u.mem_write(D+0xd1000,struct.pack('<QIBBH',app|0x800,0x40000,0x81,0,0))
halt_at=B+0x283d0
run(0x7d2b0,[D+0xd0000,D+0xd2000,D+0xd3000,0xa9fe3b02,55556,D+0xd1000,16,app,6,D+0xd4000])
halt_at=None
ipc=bytes(u.mem_read(TLS,0x100));cmif=ipc.index(b'SFCI')
assert struct.unpack_from('<I',ipc,cmif+8)[0]==2010
native_receive=ipc[cmif+16:cmif+32]
assert native_receive==struct.pack('<BBHIQ',6,0,55556,0xa9fe3b02,app),native_receive.hex()
print('PASS: native command-2010 encoder uses 16-byte storage/port/IP/application input at offsets 0/2/4/8')
summary=bytearray(128);struct.pack_into('<Q',summary,0,app);summary[0x18:0x1e]=b'2.0.0\0'
record=bytes(range(256))
container=summary+struct.pack('<I',1)+record+bytes(0x1088-128-4-256)
nodes=bytearray(0x90*2)
struct.pack_into('<I',nodes,0,0x11223344);nodes[8:17]=b'Receiver\0'
struct.pack_into('<I',nodes,0x90,0x55667788);nodes[0x98:0x9e]=b'Donor\0'
u.mem_write(D,bytes(container));u.mem_write(D+0x3000,bytes(nodes))
run(0xafba0,[3,1,D,2,D+0x3000]);native=bytes(output)
subprocess.run(['cc','-shared','-fPIC','-I'+str(P/'include'),str(P/'source/delivery_wire.c'),str(P/'source/host_profile.c'),'-o',str(Path(work.name)/'wire.dylib')],check=True)
class Peer(ctypes.Structure):
 _fields_=[('protocol',ctypes.c_uint8),('stage',ctypes.c_uint8),('nickname',ctypes.c_char*129),('system',ctypes.c_uint8*256),('summary',ctypes.c_uint8*128),('delivery',ctypes.c_uint8*(256*16)),('count',ctypes.c_uint)]
lib=ctypes.CDLL(str(Path(work.name)/'wire.dylib'));lib.labNetworkBody.restype=ctypes.c_size_t
peer=Peer();peer.nickname=b'Donor';peer.summary[:]=summary;peer.delivery[:256]=record;peer.count=1
buf=ctypes.create_string_buffer(8192)
n=lib.labNetworkBody(buf,8192,ctypes.byref(peer),0x11223344,0x55667788,b'Receiver',b'Donor')
current=bytes([0x10,9,0,0])+struct.pack('>I',n)+buf.raw[:n]
print('Official/current type-9 sizes',len(native),len(current))
print('Differences',[(hex(i),a,b) for i,(a,b) in enumerate(zip(native,current)) if a!=b][:30])
assert native==current
output.clear();run(0xb0830,[3,1,0xa9fe0101,0x11223344,app,0])
native_request=bytes(output)
lib.labSendApplicationBody.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_uint64]
lib.labSendApplicationBody(buf,0xa9fe0101,0x11223344,app)
current_request=bytes([0x10,20,0,0,0,0,0,24])+buf.raw[:24]
print('Type-20 stock/current',native_request.hex(),current_request.hex())
assert native_request==current_request
# Execute the stock sender reply serializer, including its raw metadata-key
# copies. Check the NRO parser against this independent firmware output rather
# than assuming a rejected hardware reply had the shape used by our mocks.
lib.labParseSenderResponse.argtypes=[ctypes.c_void_p,ctypes.c_uint,ctypes.POINTER(ctypes.c_uint),ctypes.c_void_p,ctypes.c_size_t,ctypes.c_uint64]
lib.labParseSenderResponse.restype=ctypes.c_bool
for key_count in (1,2):
 native_keys=b''.join(struct.pack('<QIBBH',app|0x800,0x40000+i,0x81,0,0) for i in range(key_count))
 u.mem_write(D+0x7000,native_keys);output.clear()
 assert run(0xb0980,[3,D+0x7000,key_count])==0
 native_reply=bytes(output)
 assert native_reply==bytes([0x10,21,0,0])+struct.pack('>I',8+len(native_keys))+bytes([2,key_count])+bytes(6)+native_keys
 reply=ctypes.create_string_buffer(native_reply[8:]);decoded=ctypes.create_string_buffer(18*16);count=ctypes.c_uint()
 assert lib.labParseSenderResponse(decoded,18,ctypes.byref(count),reply,len(native_reply)-8,app)
 assert count.value==key_count and decoded.raw[:16*key_count]==native_keys
print('PASS: actual stock type-21 sender replies preserve raw keys and pass the NRO parser for one and two Patch keys')
output.clear();run(0xb1e60,[3,0x55667788]);ack=bytes(output)
rx=bytearray(ack+native);out=D+0x8000
rc=run(0xb0d80,[out,3]);assert rc==0 and bytes(u.mem_read(out,4))==struct.pack('<I',0x55667788)
obj=D+0x9000;context=D+0x10000
u.mem_write(obj,struct.pack('<Q',context));u.mem_write(obj+0xb0,struct.pack('<I',3))
halt_at=B+0x68190
run(0x67bc0,[obj])
assert not rx
print('PASS: exact stock serializer output, native initial acknowledgement and native type-9 parser reached membership commit')
# Execute the actual progress/advertisement generator, mocking only the LDN IPC.
halt_at=None
host=D+0x40000;context=D+0x10000
u.mem_write(host,struct.pack('<Q',context))
u.mem_write(host+0x9524,struct.pack('<I',2))
u.mem_write(host+0x1e0,struct.pack('<I',0x11223344))
u.mem_write(host+0x1448,struct.pack('<I',0x55667788))
u.mem_write(host+0x394,b'\x01');u.mem_write(host+0xdc,b'\x01')
u.mem_write(host+0xe6,b'\x02')
u.mem_write(context+0x2f940,struct.pack('<II',2,0x11223344))
u.mem_write(context+0x2f94c,struct.pack('<I',0x55667788))
u.mem_write(B+0x43ca68,struct.pack('<Q',D+0x80000))
u.mem_write(D+0x80000,struct.pack('<Q',D+0x81000))
u.mem_write(D+0x810b8,struct.pack('<Q',STOP+0x100))
run(0xa3680,[host]);native_progress=advertisement
assert native_progress is not None and len(native_progress)==256
lib.labUpdateHostProgress.argtypes=[ctypes.c_void_p,ctypes.c_uint32,ctypes.c_uint32,ctypes.c_bool,ctypes.c_bool,ctypes.c_bool]
expected=ctypes.create_string_buffer(256)
lib.labUpdateHostProgress(expected,0x11223344,0x55667788,True,True,True)
assert native_progress==expected.raw[:256]
print('PASS: native progress/completion advertisement generator matches the NRO')
# Zero outcome counts for a participant are encoded and exposed as zero,
# not automatically a failed update. Keep the donor's truthful current status.
client=D+0x9000
u.mem_write(client+0xaa,b'\x01');u.mem_write(client+0x1c0,native_progress[0x98:0xd8])
for node,expected_count in ((0x11223344,1),(0x55667788,0)):
 assert run(0x63170,[client,D+0x8000,node])==0
 assert bytes(u.mem_read(D+0x8000,2))==bytes([expected_count,0])
print('PASS: native final-result accessor preserves receiver and donor counts without inventing updates')
# The canonical application refresh fills estimated size +0xe8 and version +0xf0.
canonical_hook=True
struct.pack_into('<Q',summary,0x10,0x0102030405060708)
summary[0x18:0x28]=b'12.3.4'+bytes(10)
container=summary+struct.pack('<I',1)+record+bytes(0x1088-128-4-256)
u.mem_write(host+0xe0,bytes(256));u.mem_write(context+0x255c0,b'\x01')
u.mem_write(context+0x253b0,struct.pack('<I',1))
u.mem_write(host+0x142c,struct.pack('<I',1));u.mem_write(host+0x2694,struct.pack('<I',1))
run(0xa2b60,[host]);native_application=advertisement
expected=ctypes.create_string_buffer(256);expected[2]=2
lib.labUpdateHostApplication(expected,ctypes.c_char_p(bytes(summary)))
assert native_application==expected.raw[:256]
print('PASS: native canonical application advertisement refresh matches the NRO')
# Output protocol fixtures only; never copy firmware instructions into the project.
fixtures=P/'tests/fixtures';fixtures.mkdir(exist_ok=True)
(fixtures/'official-network.hex').write_text(native.hex()+'\n')
(fixtures/'official-completion.hex').write_text(native_progress[0x98:0xd8].hex()+'\n')
(fixtures/'official-receive-ipc.hex').write_text(native_receive.hex()+'\n')
print('Firmware serializers/parsers were emulated with mocked socket and LDN boundaries; wireless/NS installation was not emulated.')
