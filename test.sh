#!/bin/sh
set -eu
project_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
test_dir=$(mktemp -d /tmp/local-update-tool-test.XXXXXX)
trap 'rm -rf "$test_dir"' EXIT HUP INT TERM
python3 - "$test_dir" <<'PY'
import pathlib, struct, sys, zlib
root = pathlib.Path(sys.argv[1])
table = bytearray(32 * 0x300)
for index, text in [(0, b'Compressed English'), (8, b'Compressed Dutch'), (17, b'Slot seventeen')]:
    table[index * 0x300:index * 0x300 + len(text)] = text
for filename, data in [('valid.nacp', table), ('short.nacp', table[:0x3000]),
                       ('long.nacp', table + bytes(0x300)), ('empty.nacp', bytes(0x6000))]:
    compressor = zlib.compressobj(wbits=-15)
    packed = compressor.compress(data) + compressor.flush()
    assert len(packed) <= 0x2ffe
    nacp = bytearray(0x4000)
    struct.pack_into('<H', nacp, 0, len(packed))
    nacp[2:2 + len(packed)] = packed
    nacp[0x3215] = 1
    (root / filename).write_bytes(nacp)
PY
cc -std=c11 -Wall -Wextra -Werror -I"$project_dir/include" \
    "$project_dir/source/title_name.c" "$project_dir/tests/test_title_name.c" -lz -o "$test_dir/title"
"$test_dir/title" "$test_dir/valid.nacp" "$test_dir/short.nacp" "$test_dir/long.nacp" "$test_dir/empty.nacp"
cc -std=c11 -Wall -Wextra -Werror -I"$project_dir/include" \
    "$project_dir/source/delivery_wire.c" "$project_dir/tests/test_delivery.c" -o "$test_dir/delivery"
"$test_dir/delivery" "$project_dir/tests/fixtures/official-network.hex" \
    "$project_dir/tests/fixtures/newer-donor-delivery.hex" "$project_dir/tests/fixtures/newer-donor-sender.hex"
cc -std=c11 -Wall -Wextra -Werror -I"$project_dir/tests/fakes" -I"$project_dir/include" \
    "$project_dir/source/delivery_wire.c" "$project_dir/source/game_receiver.c" \
    "$project_dir/tests/test_receiver.c" -o "$test_dir/receiver"
"$test_dir/receiver"
cc -std=c11 -Wall -Wextra -Werror -I"$project_dir/tests/fakes" -I"$project_dir/include" \
    "$project_dir/source/donor_version.c" "$project_dir/tests/test_donor_version.c" -o "$test_dir/donor_version"
"$test_dir/donor_version"
cc -std=c11 -Wall -Wextra -Werror -I"$project_dir/tests/fakes" -I"$project_dir/include" \
    "$project_dir/source/ldn_identity.c" "$project_dir/tests/test_ldn_identity.c" -o "$test_dir/identity"
"$test_dir/identity"
cc -std=c11 -Wall -Wextra -Werror -I"$project_dir/include" \
    "$project_dir/source/delivery_wire.c" "$project_dir/source/host_profile.c" \
    "$project_dir/tests/test_host_profile.c" -o "$test_dir/profile"
"$test_dir/profile" "$project_dir/tests/fixtures/official-completion.hex"
cc -std=c11 -Wall -Wextra -Werror -I"$project_dir/tests/fakes" -I"$project_dir/include" \
    "$project_dir/source/delivery_wire.c" "$project_dir/source/host_profile.c" \
    "$project_dir/source/game_receiver.c" "$project_dir/source/donor_version.c" \
    "$project_dir/source/launch_version.c" \
    "$project_dir/tests/test_room.c" -o "$test_dir/room"
"$test_dir/room"
cc -std=c11 -Wall -Wextra -Werror -I"$project_dir/tests/fakes" -I"$project_dir/include" \
    "$project_dir/source/ns_receive.c" "$project_dir/tests/test_ns_receive.c" -o "$test_dir/ns_receive"
"$test_dir/ns_receive" "$project_dir/tests/fixtures/official-receive-ipc.hex"
