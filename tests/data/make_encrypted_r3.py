#!/usr/bin/env python3
"""Generate the encrypted PDF fixtures used by the reader tests.

* encrypted-r3.pdf / encrypted-r3-objstm.pdf: Standard handler V2/R3, 128-bit
  RC4 (ISO 32000-1 algorithms 2, 3 and 5).
* encrypted-aes.pdf / encrypted-aes-objstm.pdf: Standard handler V4/R4 with
  /AESV2 crypt filters (AES-128-CBC, per-object key salted with "sAlT").

The generator is deliberately independent from the C reader: RC4 is implemented
here from the published algorithm, and AES comes from the `cryptography`
package (only needed to regenerate the AES fixtures). The resulting fixtures
therefore prove the reader rather than echoing its own output.

Run:  python make_encrypted_r3.py      (writes the PDFs next to it)
"""

from __future__ import annotations

import hashlib
import struct
import zlib
from pathlib import Path

PADDING = bytes.fromhex(
    "28bf4e5e4e758a4164004e56fffa01082e2e00b6d0683e802f0ca9fe6453697a"
)

FILE_KEY_BITS = 128
FILE_KEY_BYTES = FILE_KEY_BITS // 8


def rc4(key: bytes, data: bytes) -> bytes:
    state = list(range(256))
    j = 0
    for i in range(256):
        j = (j + state[i] + key[i % len(key)]) & 0xFF
        state[i], state[j] = state[j], state[i]
    out = bytearray()
    i = j = 0
    for value in data:
        i = (i + 1) & 0xFF
        j = (j + state[i]) & 0xFF
        state[i], state[j] = state[j], state[i]
        out.append(value ^ state[(state[i] + state[j]) & 0xFF])
    return bytes(out)


def xor_key(key: bytes, value: int) -> bytes:
    return bytes(byte ^ value for byte in key)


def file_key(owner: bytes, permissions: int, document_id: bytes) -> bytes:
    digest = hashlib.md5(
        PADDING + owner + struct.pack("<i", permissions) + document_id
    ).digest()
    for _ in range(50):
        digest = hashlib.md5(digest[:FILE_KEY_BYTES]).digest()
    return digest[:FILE_KEY_BYTES]


def owner_key(user_password: bytes, owner_password: bytes) -> bytes:
    digest = hashlib.md5((owner_password + PADDING)[:32]).digest()
    for _ in range(50):
        digest = hashlib.md5(digest[:FILE_KEY_BYTES]).digest()
    key = digest[:FILE_KEY_BYTES]
    result = rc4(key, (user_password + PADDING)[:32])
    for index in range(1, 20):
        result = rc4(xor_key(key, index), result)
    return result


def user_key(key: bytes, document_id: bytes) -> bytes:
    value = rc4(key, hashlib.md5(PADDING + document_id).digest())
    for index in range(1, 20):
        value = rc4(xor_key(key, index), value)
    return value + b"\x00" * 16


def object_key(key: bytes, number: int, generation: int) -> bytes:
    digest = hashlib.md5(
        key + number.to_bytes(3, "little") + generation.to_bytes(2, "little")
    ).digest()
    size = min(len(key) + 5, 16)
    return digest[:size]


def aes_object_key(key: bytes, number: int, generation: int = 0) -> bytes:
    digest = hashlib.md5(
        key
        + number.to_bytes(3, "little")
        + generation.to_bytes(2, "little")
        + b"sAlT"
    ).digest()
    return digest[: min(len(key) + 5, 16)]


def aes_encrypt(key: bytes, plaintext: bytes, iv: bytes) -> bytes:
    from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

    padding = 16 - len(plaintext) % 16
    padded = plaintext + bytes([padding]) * padding
    encryptor = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
    return iv + encryptor.update(padded) + encryptor.finalize()


def assemble(objects: list[bytes], document_id: bytes, encrypt_object: int) -> bytes:
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % number + body + b"\nendobj\n"
    xref_offset = len(out)
    out += b"xref\n0 %d\n" % (len(objects) + 1)
    out += b"0000000000 65535 f \n"
    for offset in offsets:
        out += b"%010d 00000 n \n" % offset
    identifier = document_id.hex().encode()
    out += b"trailer\n<< /Size %d /Root 1 0 R /Encrypt %d 0 R " % (
        len(objects) + 1,
        encrypt_object,
    )
    out += b"/ID [<" + identifier + b"><" + identifier + b">] >>\n"
    out += b"startxref\n%d\n%%%%EOF\n" % xref_offset
    return bytes(out)


def build() -> bytes:
    document_id = bytes.fromhex("112233445566778899aabbccddeeff00")
    permissions = -4
    owner = owner_key(b"", b"owner")
    key = file_key(owner, permissions, document_id)
    user = user_key(key, document_id)

    plaintext = b"BT /F1 12 Tf 10 40 Td (R3 encrypted hello) Tj ET\n"
    # The reader trims trailing CR/LF before decrypting; pad until the
    # ciphertext does not end with one so no encrypted byte is discarded.
    while True:
        cipher = rc4(object_key(key, 4, 0), plaintext)
        if cipher[-1] not in (0x0A, 0x0D):
            break
        plaintext += b" "

    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
         b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"),
        b"<< /Length %d >>\nstream\n" % len(cipher) + cipher + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        (b"<< /Filter /Standard /V 2 /R 3 /Length 128 "
         b"/O <" + owner.hex().encode() + b"> "
         b"/U <" + user.hex().encode() + b"> /P -4 >>"),
    ]
    return assemble(objects, document_id, 6)


def build_objstm() -> bytes:
    """Same document, but the page object lives in an encrypted object stream.

    This exercises the reader's ordering rule: the security handler must be
    ready before an /ObjStm payload is decrypted and inflated.
    """
    document_id = bytes.fromhex("112233445566778899aabbccddeeff01")
    permissions = -4
    owner = owner_key(b"", b"owner")
    key = file_key(owner, permissions, document_id)
    user = user_key(key, document_id)

    plaintext = b"BT /F1 12 Tf 10 40 Td (R3 objstm hello) Tj ET\n"
    while True:
        cipher = rc4(object_key(key, 4, 0), plaintext)
        if cipher[-1] not in (0x0A, 0x0D):
            break
        plaintext += b" "

    page = (
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"
    )
    header = b"30 0 "
    padding = b""
    while True:
        stream_plain = header + page + padding
        stream_cipher = rc4(
            object_key(key, 20, 0), zlib.compress(stream_plain, 9)
        )
        # The reader trims trailing CR/LF before decrypting and stops the
        # stream at the first "endstream"; keep both assumptions safe.
        if (
            stream_cipher[-1] not in (0x0A, 0x0D)
            and b"endstream" not in stream_cipher
        ):
            break
        padding += b" "

    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",                      # 1
        b"<< /Type /Pages /Kids [30 0 R] /Count 1 >>",              # 2
        b"<< >>",                                                   # 3
        b"<< /Length %d >>\nstream\n" % len(cipher) + cipher + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",  # 5
        (b"<< /Filter /Standard /V 2 /R 3 /Length 128 "            # 6
         b"/O <" + owner.hex().encode() + b"> "
         b"/U <" + user.hex().encode() + b"> /P -4 >>"),
    ]
    objects.extend([b"<< >>"] * 13)  # objects 7..19
    objects.append(                                                 # 20
        b"<< /Type /ObjStm /N 1 /First %d /Length %d >>\nstream\n"
        % (len(header), len(stream_cipher)) + stream_cipher + b"\nendstream"
    )
    return assemble(objects, document_id, 6)


AES_IV = bytes.fromhex("000102030405060708090a0b0c0d0e0f")


def aes_encrypt_dict(owner: bytes, user: bytes) -> bytes:
    return (
        b"<< /Filter /Standard /V 4 /R 4 /Length 128 "
        b"/CF << /StdCF << /CFM /AESV2 /Length 16 >> >> "
        b"/StmF /StdCF /StrF /StdCF "
        b"/O <" + owner.hex().encode() + b"> "
        b"/U <" + user.hex().encode() + b"> /P -4 >>"
    )


def build_aes() -> bytes:
    document_id = bytes.fromhex("112233445566778899aabbccddeeff02")
    permissions = -4
    owner = owner_key(b"", b"owner")
    key = file_key(owner, permissions, document_id)
    user = user_key(key, document_id)

    plaintext = b"BT /F1 12 Tf 10 40 Td (AES encrypted hello) Tj ET\n"
    while True:
        cipher = aes_encrypt(aes_object_key(key, 4), plaintext, AES_IV)
        if cipher[-1] not in (0x0A, 0x0D):
            break
        plaintext += b" "

    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
         b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"),
        b"<< /Length %d >>\nstream\n" % len(cipher) + cipher + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
        aes_encrypt_dict(owner, user),
    ]
    return assemble(objects, document_id, 6)


def build_aes_objstm() -> bytes:
    document_id = bytes.fromhex("112233445566778899aabbccddeeff03")
    permissions = -4
    owner = owner_key(b"", b"owner")
    key = file_key(owner, permissions, document_id)
    user = user_key(key, document_id)

    plaintext = b"BT /F1 12 Tf 10 40 Td (AES objstm hello) Tj ET\n"
    while True:
        cipher = aes_encrypt(aes_object_key(key, 4), plaintext, AES_IV)
        if cipher[-1] not in (0x0A, 0x0D):
            break
        plaintext += b" "

    page = (
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
        b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"
    )
    header = b"30 0 "
    padding = b""
    while True:
        stream_plain = header + page + padding
        stream_cipher = aes_encrypt(
            aes_object_key(key, 20), zlib.compress(stream_plain, 9), AES_IV
        )
        if stream_cipher[-1] not in (0x0A, 0x0D) and b"endstream" not in stream_cipher:
            break
        padding += b" "

    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",                      # 1
        b"<< /Type /Pages /Kids [30 0 R] /Count 1 >>",              # 2
        b"<< >>",                                                   # 3
        b"<< /Length %d >>\nstream\n" % len(cipher) + cipher + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",  # 5
        aes_encrypt_dict(owner, user),                              # 6
    ]
    objects.extend([b"<< >>"] * 13)  # objects 7..19
    objects.append(                                                 # 20
        b"<< /Type /ObjStm /N 1 /First %d /Length %d >>\nstream\n"
        % (len(header), len(stream_cipher)) + stream_cipher + b"\nendstream"
    )
    return assemble(objects, document_id, 6)


if __name__ == "__main__":
    directory = Path(__file__).resolve().parent
    for name, builder in (
        ("encrypted-r3.pdf", build),
        ("encrypted-r3-objstm.pdf", build_objstm),
        ("encrypted-aes.pdf", build_aes),
        ("encrypted-aes-objstm.pdf", build_aes_objstm),
    ):
        target = directory / name
        target.write_bytes(builder())
        print(f"wrote {target} ({target.stat().st_size} bytes)")
