#!/usr/bin/env python3
"""Local Tibia 8.60 login/takeover smoke test. No gameplay packets except logout.

Run without arguments for the three seeded accounts, or select one with
--character, --account, --password, and --expect login|reject.
--self-test checks the packet codec and extracts the local PEM public key; no sockets.
"""

import argparse
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import time
import zlib


HOST = "127.0.0.1"
LOGIN_PORT = 7171
GAME_PORT = 7172
KEY_FILE = Path(__file__).resolve().parents[1] / "server" / "key.pem"
VERSION = 860
OS = 2  # CLIENTOS_WINDOWS; avoids OTClient extended-opcode negotiation.
REJECTION = "This character is controlled by the server."
MASK = 0xFFFFFFFF
DELTA = 0x9E3779B9


def u16(value):
    return struct.pack("<H", value)


def u32(value):
    return struct.pack("<I", value)


def text(value):
    data = value.encode("utf-8")
    if len(data) > 127:  # Leave room for all fields in the 128-byte RSA block.
        raise ValueError("credential/character field exceeds smoke-test limit")
    return u16(len(data)) + data


def read_text(data, offset):
    if offset + 2 > len(data):
        raise ValueError("truncated string length")
    size = struct.unpack_from("<H", data, offset)[0]
    offset += 2
    if offset + size > len(data):
        raise ValueError("truncated string")
    return data[offset:offset + size].decode("utf-8"), offset + size


def der_tlv(data, offset, tag):
    if offset >= len(data) or data[offset] != tag:
        raise ValueError("unexpected public-key DER tag")
    offset += 1
    if offset >= len(data):
        raise ValueError("truncated public-key DER length")
    size = data[offset]
    offset += 1
    if size & 0x80:
        count = size & 0x7F
        if not 1 <= count <= 2 or offset + count > len(data):
            raise ValueError("invalid public-key DER length")
        size = int.from_bytes(data[offset:offset + count], "big")
        offset += count
    if offset + size > len(data):
        raise ValueError("truncated public-key DER value")
    return data[offset:offset + size], offset + size


def public_key():
    # Derive SPKI from the same PEM the server loads; never print the private key.
    result = subprocess.run(
        ["openssl", "pkey", "-in", str(KEY_FILE), "-pubout", "-outform", "DER"],
        check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
    )
    spki, end = der_tlv(result.stdout, 0, 0x30)
    if end != len(result.stdout):
        raise ValueError("trailing public-key data")
    _, pos = der_tlv(spki, 0, 0x30)  # AlgorithmIdentifier
    bits, end = der_tlv(spki, pos, 0x03)
    if end != len(spki) or not bits or bits[0] != 0:
        raise ValueError("invalid RSA public-key bit string")
    rsa, end = der_tlv(bits, 1, 0x30)
    if end != len(bits):
        raise ValueError("trailing RSA public-key data")
    modulus, pos = der_tlv(rsa, 0, 0x02)
    exponent, end = der_tlv(rsa, pos, 0x02)
    if end != len(rsa) or len(modulus) not in (128, 129):
        raise ValueError("expected 1024-bit RSA key")
    return int.from_bytes(modulus, "big"), int.from_bytes(exponent, "big")


def rsa_block(payload, key):
    plain = b"\x00" + payload
    if len(plain) > 128:
        raise ValueError("credentials do not fit the RSA block")
    plain += os.urandom(128 - len(plain))
    n, e = key
    value = int.from_bytes(plain, "big")
    if value >= n:
        raise ValueError("RSA plaintext is outside the modulus")
    return pow(value, e, n).to_bytes(128, "big")


def xtea_encrypt(data, key):
    if len(data) % 8:
        raise ValueError("XTEA input is not block aligned")
    out = bytearray()
    for offset in range(0, len(data), 8):
        left, right = struct.unpack_from("<II", data, offset)
        total = 0
        for _ in range(32):
            left = (left + ((((right << 4) ^ (right >> 5)) + right) ^ (total + key[total & 3]))) & MASK
            total = (total + DELTA) & MASK
            right = (right + ((((left << 4) ^ (left >> 5)) + left) ^ (total + key[(total >> 11) & 3]))) & MASK
        out += struct.pack("<II", left, right)
    return bytes(out)


def xtea_decrypt(data, key):
    if len(data) % 8:
        raise ValueError("XTEA input is not block aligned")
    out = bytearray()
    for offset in range(0, len(data), 8):
        left, right = struct.unpack_from("<II", data, offset)
        total = (DELTA * 32) & MASK
        for _ in range(32):
            right = (right - ((((left << 4) ^ (left >> 5)) + left) ^ (total + key[(total >> 11) & 3]))) & MASK
            total = (total - DELTA) & MASK
            left = (left - ((((right << 4) ^ (right >> 5)) + right) ^ (total + key[total & 3]))) & MASK
        out += struct.pack("<II", left, right)
    return bytes(out)


def frame(body, key=None):
    if key is not None:
        plain = u16(len(body)) + body
        body = xtea_encrypt(plain + b"\x33" * (-len(plain) % 8), key)
    packet = u32(zlib.adler32(body)) + body
    return u16(len(packet)) + packet


def recv_exact(conn, length):
    result = bytearray()
    while len(result) < length:
        chunk = conn.recv(length - len(result))
        if not chunk:
            raise ValueError("connection closed before a complete packet")
        result.extend(chunk)
    return bytes(result)


def recv_frame(conn, key=None):
    length = struct.unpack("<H", recv_exact(conn, 2))[0]
    if not 4 < length < 32768:
        raise ValueError("invalid frame size")
    packet = recv_exact(conn, length)
    checksum = struct.unpack_from("<I", packet)[0]
    body = packet[4:]
    if checksum != zlib.adler32(body):
        raise ValueError("invalid Adler-32 checksum")
    if key is not None:
        plain = xtea_decrypt(body, key)
        if len(plain) < 2:
            raise ValueError("short encrypted packet")
        size = struct.unpack_from("<H", plain)[0]
        if size > len(plain) - 2:
            raise ValueError("invalid encrypted inner length")
        return plain[2:2 + size]
    return body


def session_key():
    return struct.unpack("<IIII", os.urandom(16))


def character_list(account, password, rsa):
    key = session_key()
    payload = b"".join(map(u32, key)) + text(account) + text(password)
    # 0x01 protocol ID, OS, version, and three asset signatures (ignored by TFS).
    request = b"\x01" + u16(OS) + u16(VERSION) + b"\x00" * 12 + rsa_block(payload, rsa)
    # Ban::acceptConnection rejects bursts with inter-connection gaps <= 500 ms.
    time.sleep(0.6)
    with socket.create_connection((HOST, LOGIN_PORT), timeout=5) as conn:
        conn.settimeout(5)
        conn.sendall(frame(request))
        reply = recv_frame(conn, key)
    if not reply:
        raise ValueError("empty login response")
    pos = 0
    if reply[pos] == 0x14:  # optional MOTD
        _, pos = read_text(reply, pos + 1)
    if pos >= len(reply):
        raise ValueError("empty login response")
    if reply[pos] == 0x0A:
        message, _ = read_text(reply, pos + 1)
        raise ValueError("account login refused: " + message)
    if reply[pos] != 0x64:
        raise ValueError(f"unexpected character-list opcode 0x{reply[pos]:02x}")
    pos += 1
    count = reply[pos]
    pos += 1
    names = []
    for _ in range(count):
        name, pos = read_text(reply, pos)
        _, pos = read_text(reply, pos)  # world name
        pos += 6  # advertised IP and game port; never connect to the advertised IP
        if pos > len(reply):
            raise ValueError("truncated character list")
        names.append(name)
    if pos + 2 != len(reply):
        raise ValueError("invalid premium-days/character-list tail")
    return names


def logout(conn, key):
    conn.sendall(frame(b"\x14", key))  # normal logout; no other game actions
    # The server may have already queued more map packets. Drain them
    # until its logout handler closes the connection; do not send input.
    while conn.recv(4096):
        pass


def game_login(account, password, character, expected, rsa):
    key = session_key()
    time.sleep(0.6)
    with socket.create_connection((HOST, GAME_PORT), timeout=5) as conn:
        conn.settimeout(5)
        challenge = recv_frame(conn)
        if len(challenge) != 8 or challenge[:3] != b"\x06\x00\x1f":
            raise ValueError("invalid game challenge")
        payload = (b"".join(map(u32, key)) + b"\x00" + text(account) +
                   text(character) + text(password) + challenge[3:])
        # 0x0A pending-game ID, OS, version, 128-byte RSA block.
        conn.sendall(frame(b"\x0a" + u16(OS) + u16(VERSION) + rsa_block(payload, rsa)))
        reply = recv_frame(conn, key)
        if not reply:
            raise ValueError("empty game response")
        if expected == "reject":
            if reply[0] == 0x0A:
                logout(conn, key)  # guard failed: do not leave an orphaned login
                raise ValueError("takeover guard failed: character logged in")
            if reply[0] != 0x14:
                raise ValueError(f"expected takeover rejection, got opcode 0x{reply[0]:02x}")
            message, end = read_text(reply, 1)
            if end != len(reply) or message != REJECTION:
                raise ValueError("expected server-controlled takeover rejection, got: " + message)
        else:
            # 0x0A is the server's own-character login header (before map data).
            if reply[0] != 0x0A:
                if reply[0] == 0x14:
                    message, _ = read_text(reply, 1)
                    raise ValueError("human game login refused: " + message)
                raise ValueError(f"expected human login header, got opcode 0x{reply[0]:02x}")
            logout(conn, key)


def self_test():
    key = (0x01234567, 0x89ABCDEF, 0xFEDCBA98, 0x76543210)
    # Standard XTEA zero-block vector, plus nonzero round-trip and framing.
    assert xtea_encrypt(b"\x00" * 8, (0, 0, 0, 0)).hex() == "d8d4e9ded91e13f7"
    body = b"\x14\x01\x00x"
    encoded = frame(body, key)
    assert struct.unpack_from("<H", encoded)[0] == len(encoded) - 2
    assert zlib.adler32(encoded[6:]) == struct.unpack_from("<I", encoded, 2)[0]
    plain = xtea_decrypt(encoded[6:], key)
    assert plain[:2] == u16(len(body)) and plain[2:2 + len(body)] == body
    assert xtea_decrypt(xtea_encrypt(bytes(range(16)), key), key) == bytes(range(16))

    class ChunkedReader:
        def __init__(self, data):
            self.data = data

        def recv(self, size):
            chunk, self.data = self.data[:min(size, 3)], self.data[min(size, 3):]
            return chunk

    assert recv_frame(ChunkedReader(encoded), key) == body
    challenge = b"\x06\x00\x1f" + u32(123456) + b"\x7f"
    assert recv_frame(ChunkedReader(frame(challenge))) == challenge
    assert read_text(text("Bot One"), 0) == ("Bot One", len(text("Bot One")))
    # Parse the real SPKI, including its nested BIT STRING/SEQUENCE offsets.
    n, e = public_key()
    assert n.bit_length() == 1024 and e == 65537
    print("codec and local public key OK (no network access)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--self-test", action="store_true", help="offline codec and local PEM public-key checks")
    parser.add_argument("--character")
    parser.add_argument("--account")
    parser.add_argument("--password")
    parser.add_argument("--expect", choices=("login", "reject"))
    args = parser.parse_args()
    if args.self_test:
        if any((args.character, args.account, args.password, args.expect)):
            parser.error("--self-test cannot be combined with login options")
        self_test()
        return
    options = (args.character, args.account, args.password, args.expect)
    if any(options) and not all(options):
        parser.error("specify --character, --account, --password, and --expect together")
    cases = [(args.character, args.account, args.password, args.expect)] if all(options) else [
        ("GOD Admin", "admin", "admin", "login"),
        ("Bot One", "bot-one", "bot-one", "reject"),
        ("Bot Two", "bot-two", "bot-two", "reject"),
    ]
    rsa = public_key()
    for character, account, password, expected in cases:
        names = character_list(account, password, rsa)
        if character not in names:
            raise ValueError(f"{character}: missing from authenticated character list; guard not tested")
        game_login(account, password, character, expected, rsa)
        print(f"{character}: {'logged in and out' if expected == 'login' else 'takeover rejected'}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError, AssertionError) as error:
        print(f"login smoke failed: {error}", file=sys.stderr)
        sys.exit(1)
