#!/usr/bin/env python3
"""Key tool for the NFC gate.

  python3 make_keys.py init
      New MASTER_KEY and GATE_SECRET as C arrays for pn532_gate_auth.ino,
      plus the master key as hex (keep it secret, you need it for the next command).

  python3 make_keys.py phone MASTER_HEX ID
      Credentials for an ADMIN phone: its ID (1..255, must be in ADMIN_IDS in the sketch)
      and its key (hex, 32 characters) to be entered in the Android app.
      User phones do not need this: they get their credentials over NFC (Join gate).

  python3 make_keys.py mac MASTER_HEX ID CHALLENGE_HEX [CMD [ARG]]
      The 8-byte MAC the gate expects (for debugging).
"""
import hashlib
import hmac
import secrets
import sys


def c_array(b: bytes) -> str:
    rows = []
    for i in range(0, len(b), 16):
        rows.append("  " + ", ".join(f"0x{x:02X}" for x in b[i:i + 16]))
    return "{\n" + ",\n".join(rows) + " }"


def phone_key(master: bytes, phone_id: int) -> bytes:
    """First 16 bytes of HMAC-SHA256(master, ID)."""
    return hmac.new(master, phone_id.to_bytes(2, "big"), hashlib.sha256).digest()[:16]


def main() -> None:
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "init":
        master = secrets.token_bytes(32)
        gate = secrets.token_bytes(16)
        print("static const uint8_t MASTER_KEY[32] =", c_array(master) + ";")
        print("static const uint8_t GATE_SECRET[16] =", c_array(gate) + ";")
        print("\nmaster hex (keep it, needed for 'phone'):", master.hex())
    elif cmd == "phone" and len(sys.argv) == 4:
        master = bytes.fromhex(sys.argv[2])
        pid = int(sys.argv[3], 0)
        assert 1 <= pid <= 255, "admin IDs must be 1..255"
        print("id  =", pid)
        print("key =", phone_key(master, pid).hex())
    elif cmd == "mac" and 5 <= len(sys.argv) <= 7:
        master = bytes.fromhex(sys.argv[2])
        pid = int(sys.argv[3], 0)
        challenge = bytes.fromhex(sys.argv[4])
        c = int(sys.argv[5], 0) if len(sys.argv) > 5 else 0
        arg = int(sys.argv[6], 0) if len(sys.argv) > 6 else 0
        msg = (b"GATE1" + challenge + pid.to_bytes(2, "big") +
               bytes([c]) + arg.to_bytes(2, "big"))
        print(hmac.new(phone_key(master, pid), msg, hashlib.sha256).digest()[:8].hex())
    else:
        print(__doc__)
        sys.exit(1)


if __name__ == "__main__":
    main()
