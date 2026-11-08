#!/usr/bin/env python3
"""Generate real-world AES-128/AES-256 encrypted fixtures with mutool.

The C reader must interoperate with files produced by established tools, not
only with the package's own generator. This script builds a small plain PDF with
a correct classic xref, then asks MuPDF's ``mutool clean -E`` for AES-128
(V4/AESV2) and AES-256 (V5/R6/AESV3) copies with an empty user password.

``mutool`` is only needed to regenerate the fixtures; the resulting PDFs are
committed next to this script and used by the CTest suite.

Run:  python make_mutool_encrypted.py
"""

from __future__ import annotations

import shutil
import subprocess
from pathlib import Path

TEXT = b"AES256 mutool hello"


def build_plain_pdf() -> bytes:
    content = b"BT /F1 12 Tf 10 40 Td (" + TEXT + b") Tj ET\n"
    objects = [
        b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] "
         b"/Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>"),
        b"<< /Length %d >>\nstream\n" % len(content) + content + b"endstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>",
    ]
    out = bytearray(b"%PDF-1.4\n")
    offsets = []
    for number, body in enumerate(objects, start=1):
        offsets.append(len(out))
        out += b"%d 0 obj\n" % number + body + b"\nendobj\n"
    xref = len(out)
    out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objects) + 1)
    for offset in offsets:
        out += b"%010d 00000 n \n" % offset
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (
        len(objects) + 1,
        xref,
    )
    return bytes(out)


def main() -> None:
    mutool = shutil.which("mutool")
    if not mutool:
        raise SystemExit("mutool not found; install MuPDF to regenerate fixtures")
    directory = Path(__file__).resolve().parent
    plain_path = directory / "_plain-mutool-input.pdf"
    plain_path.write_bytes(build_plain_pdf())
    try:
        for cipher, name in (("aes-128", "encrypted-mutool-aes128.pdf"),
                             ("aes-256", "encrypted-mutool-aes256.pdf")):
            target = directory / name
            subprocess.run(
                [mutool, "clean", "-E", cipher, "-O", "owner", str(plain_path),
                 str(target)],
                check=True,
            )
            print(f"wrote {target} ({target.stat().st_size} bytes)")
        # Same document with a non-empty user password: the full reader must
        # reject it, while the structure-only path still reports geometry.
        target = directory / "encrypted-mutool-aes256-userpw.pdf"
        subprocess.run(
            [mutool, "clean", "-E", "aes-256", "-O", "owner", "-U", "secret",
             str(plain_path), str(target)],
            check=True,
        )
        print(f"wrote {target} ({target.stat().st_size} bytes)")
    finally:
        plain_path.unlink(missing_ok=True)


if __name__ == "__main__":
    main()
