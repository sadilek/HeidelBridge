#!/usr/bin/env python3
"""Reads holding registers from HeidelBridge's Daheimladen Modbus TCP server (function 3).

    tools/modbus_read.py [host] [start] [count]     defaults: heidelbridge 32 22

The default is the block evcc reads without a sponsor token (charger/daheimladen.go,
checkStation()): max/min/cable current and the station ID. Read-only, standard library only.
"""
import socket
import struct
import sys

EXCEPTIONS = {1: "illegal function", 2: "illegal data address", 3: "illegal data value", 4: "server device failure"}


def read_holding_registers(host, start, count, unit=255, port=502):
    request = struct.pack(">HHHBBHH", 1, 0, 6, unit, 3, start, count)
    with socket.create_connection((host, port), timeout=5) as s:
        s.sendall(request)
        header = s.recv(7, socket.MSG_WAITALL)
        _, _, length, _ = struct.unpack(">HHHB", header)
        pdu = s.recv(length - 1, socket.MSG_WAITALL)
    if pdu[0] & 0x80:
        raise RuntimeError(f"modbus: exception '{pdu[1]}' ({EXCEPTIONS.get(pdu[1], '?')}), function '{pdu[0] & 0x7F}'")
    return list(struct.unpack(f">{pdu[1] // 2}H", pdu[2:2 + pdu[1]]))


def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "heidelbridge"
    start = int(sys.argv[2]) if len(sys.argv) > 2 else 32
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 22
    print(f"Reading {count} registers from {start} on {host} ...")
    try:
        words = read_holding_registers(host, start, count)
    except RuntimeError as e:
        print(e)
        return 1
    for i, w in enumerate(words):
        print(f"  reg {start + i:3d}: {w:5d}  0x{w:04X}  {''.join(chr(b) if 32 <= b < 127 else '.' for b in w.to_bytes(2, 'big'))}")
    if start <= 38 and start + count >= 54:
        sid = b"".join(w.to_bytes(2, "big") for w in words[38 - start:54 - start]).rstrip(b"\0").decode(errors="replace")
        print(f"Station ID: '{sid}'" + ("  -> evcc without sponsor token: 'sponsorship required'" if "heidelbridge" in sid.lower() else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
