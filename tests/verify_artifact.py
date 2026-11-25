#!/usr/bin/env python3
"""Check actual executable containers, without claiming hardware compatibility."""
import pathlib
import struct
import subprocess
import sys

root = pathlib.Path(sys.argv[1])
nro = (root / 'switch/local-update-tool/local-update-tool.nro').read_bytes()
elf_path = root / 'local-update-tool.elf'
elf = elf_path.read_bytes()
assert elf[:6] == b'\x7fELF\x02\x01', 'Expected a little-endian ELF64'
assert struct.unpack_from('<H', elf, 18)[0] == 183, 'Expected AArch64 machine type'
assert nro[0x10:0x14] == b'NRO0', 'Missing NRO header'
nro_size = struct.unpack_from('<I', nro, 0x18)[0]
assert 0x80 <= nro_size <= len(nro), 'Invalid NRO body size'
for offset in (0x20, 0x28, 0x30):
    start, size = struct.unpack_from('<II', nro, offset)
    assert start + size <= nro_size, 'NRO segment exceeds body'
assert nro[nro_size:nro_size + 4] == b'ASET', 'Missing NRO assets'
offset, size=struct.unpack_from('<QQ',nro,nro_size+24)
assert size==0x4000 and nro_size+offset+size<=len(nro)
nacp = nro[nro_size + offset : nro_size + offset + size]
title = nacp[:0x200].split(b'\0', 1)[0]
author = nacp[0x200:0x300].split(b'\0', 1)[0]
version = nacp[0x3060:0x3070].split(b'\0', 1)[0]
assert title == b'Local Update Tool', title
assert author == b'alzxk11', author
assert version == b'1.0.0', version
symbols=subprocess.check_output(['aarch64-none-elf-nm','--defined-only',str(elf_path)],text=True)
names={line.split()[-1] for line in symbols.splitlines() if line.split()}
required={'labGameRoomOpen','labCheckDonorVersion','labBuildHostAdvertisement','labPatchKeysFromDelivery','labReceiverStart','__wrap_nsRequestReceiveApplication',
          'nsCommitReceiveApplication','nsVerifyDeliveryProtocolVersion','labResetLaunchVersion','avmPushLaunchVersion'}
assert required<=names,required-names
assert 'nsRequestReceiveApplication' not in names,'Uncorrected SDK receive wrapper linked'
assert not any(n.startswith('nssu') for n in names),'Firmware update API linked'
for forbidden in ('nsGetSystemUpdateInterface','lcsOpen','lcsCreateHost','lcsClose','lcsMakeHostConfig',
                  'captureProfile','ldnmInitialize','labReceiveMetadata','ncmContentStorageRegister',
                  'ncmContentMetaDatabaseSet','ncmContentMetaDatabaseCommit','esImportTicket'):
    assert forbidden not in names,f'Forbidden native coordinator/firmware/direct installer API: {forbidden}'
assert b'\0lcs\0' not in nro,'Native LCS service name embedded'
print(f'PASS: {title.decode()} v{version.decode()} by {author.decode()} ({len(nro):,} bytes). Native game APIs present. LCS backend and firmware APIs absent.')

