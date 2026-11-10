#!/usr/bin/env python3
"""Package a diagnostic APK from the signed base APK's binary manifest.

Avoids the pyaxml dependency for local probe builds. It changes only
versionCode in the already-built base manifest, then signs with the existing
personal key. All outputs are new files; existing APKs are never overwritten.
"""
import argparse
import datetime
import hashlib
import io
import os
import struct
import sys
import zipfile

from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import padding

KIT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.normpath(os.path.join(KIT, "..", "..", "..", "apk")))
from fix_axml import fix as fix_binary_manifest


def apk_digest(sections):
    chunks = []
    for section in sections:
        for off in range(0, len(section), 1 << 20):
            chunk = section[off:off + (1 << 20)]
            chunks.append(hashlib.sha256(b"\xa5" + struct.pack("<I", len(chunk)) + chunk).digest())
    return hashlib.sha256(b"\x5a" + struct.pack("<I", len(chunks)) + b"".join(chunks)).digest()


def length_prefixed(data):
    return struct.pack("<I", len(data)) + data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--base-apk", required=True)
    ap.add_argument("--lib-dir", required=True)
    ap.add_argument("--output-dir", required=True)
    ap.add_argument("--version-code", type=int, required=True)
    ap.add_argument("--key", required=True)
    args = ap.parse_args()
    os.makedirs(args.output_dir, exist_ok=True)
    out_apk = os.path.join(args.output_dir, "bsi_b0probe.apk")
    parts = [os.path.join(args.output_dir, f"p{i}") for i in range(3)]
    md5_path = os.path.join(args.output_dir, "MD5.txt")
    if any(os.path.exists(p) for p in [out_apk, *parts, md5_path]):
        sys.exit("refusing to overwrite an existing diagnostic package output")

    with zipfile.ZipFile(args.base_apk, "r") as base:
        manifest = base.read("AndroidManifest.xml")
    # b95: an override binary manifest next to this script (compiled with pyaxml from AndroidManifest_override.xml:
    # the base manifest + hand-tracking support, so the Quest never shows "Switch to Controllers" when the controllers sleep)
    ovr = os.path.join(os.path.dirname(os.path.abspath(__file__)), "AndroidManifest_override.axml")
    if os.path.exists(ovr):
        with open(ovr, "rb") as f:
            manifest = f.read()
        print("manifest: AndroidManifest_override.axml (%d bytes)" % len(manifest))
    manifest = fix_binary_manifest(manifest, {}, {"versionCode": args.version_code})

    buf = io.BytesIO()
    def add(zf, name, data, compress):
        zi = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
        zi.compress_type = zipfile.ZIP_DEFLATED if compress else zipfile.ZIP_STORED
        if not compress:
            off = buf.tell() + 30 + len(name.encode("utf-8"))
            pad = (-off) % 4096
            zi.extra = b"\x00" * (pad if pad == 0 or pad >= 4 else pad + 4096)
        zf.writestr(zi, data)

    with zipfile.ZipFile(buf, "w") as zf:
        add(zf, "AndroidManifest.xml", manifest, True)
        for name in sorted(os.listdir(args.lib_dir)):
            if name.endswith(".so"):
                with open(os.path.join(args.lib_dir, name), "rb") as f:
                    add(zf, "lib/arm64-v8a/" + name, f.read(), True)
    apk = buf.getvalue()

    with open(args.key, "rb") as f:
        pem = f.read()
    key = serialization.load_pem_private_key(pem, None)
    cert = x509.load_pem_x509_certificate(pem[pem.index(b"-----BEGIN CERTIFICATE"):])
    eocd = apk.rindex(b"PK\x05\x06")
    cd_off = struct.unpack_from("<I", apk, eocd + 16)[0]
    sections = [apk[:cd_off], apk[cd_off:eocd], apk[eocd:]]
    digest = apk_digest(sections)
    lp = length_prefixed
    algorithm = 0x0103  # RSASSA-PKCS1-v1_5 with SHA-256
    cert_der = cert.public_bytes(serialization.Encoding.DER)
    signed_data = lp(lp(struct.pack("<I", algorithm) + lp(digest))) + lp(lp(cert_der)) + lp(b"")
    signature = key.sign(signed_data, padding.PKCS1v15(), hashes.SHA256())
    public_key = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
    signer = lp(signed_data) + lp(lp(struct.pack("<I", algorithm) + lp(signature))) + lp(public_key)
    v2 = lp(lp(signer))
    pair = struct.pack("<Q", 4 + len(v2)) + struct.pack("<I", 0x7109871a) + v2
    block_size = len(pair) + 8 + 16
    block = struct.pack("<Q", block_size) + pair + struct.pack("<Q", block_size) + b"APK Sig Block 42"
    new_eocd = bytearray(apk[eocd:])
    struct.pack_into("<I", new_eocd, 16, cd_off + len(block))
    signed_apk = apk[:cd_off] + block + apk[cd_off:eocd] + bytes(new_eocd)
    with open(out_apk, "wb") as f:
        f.write(signed_apk)

    chunk = 17_000_000
    hashes_out = [hashlib.md5(signed_apk).hexdigest()]
    for i in range(3):
        data = signed_apk[i * chunk:(i + 1) * chunk] if i < 2 else signed_apk[2 * chunk:]
        with open(parts[i], "wb") as f:
            f.write(data)
        hashes_out.append(hashlib.md5(data).hexdigest())
    with open(md5_path, "w", encoding="utf-8") as f:
        f.write(f"bsi_b0probe.apk {hashes_out[0]} ({len(signed_apk)} B), versionCode {args.version_code}\n")
        for i, digest_hex in enumerate(hashes_out[1:]):
            f.write(f"p{i} {digest_hex}\n")
    print(f"wrote {out_apk} ({len(signed_apk)} bytes); versionCode {args.version_code}")


if __name__ == "__main__":
    main()
