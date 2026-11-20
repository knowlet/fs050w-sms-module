#!/usr/bin/env python3
"""Check the two compile-time NAS SET request variants without device access."""
from pathlib import Path
import re

source = (Path(__file__).parent / "nas_domain_permanent_set.c").read_text()
match = re.search(r"static const u8 set_request\[\] = \{([^}]*)\};", source, re.S)
assert match, "request array missing"
tokens = [token.strip() for token in match.group(1).replace("\n", "").split(",") if token.strip()]

for target in (2, 1):
    packet = bytes(target if token == "NAS_TARGET_DOMAIN" else int(token, 0) for token in tokens)
    expected = bytes.fromhex("17010001180400") + target.to_bytes(4, "little")
    assert packet == expected, (target, packet.hex(), expected.hex())
    pos = 0
    fields = []
    while pos < len(packet):
        assert pos + 3 <= len(packet)
        field_id = packet[pos]
        length = int.from_bytes(packet[pos + 1 : pos + 3], "little")
        pos += 3
        assert length <= len(packet) - pos
        fields.append((field_id, packet[pos : pos + length]))
        pos += length
    assert fields == [(0x17, b"\x01"), (0x18, target.to_bytes(4, "little"))]
    print(f"target={target} request={packet.hex()} tlvs=0x17,0x18")
