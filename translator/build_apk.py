#!/usr/bin/env python3
"""Build + sign (APK Signature Scheme v2) a NativeActivity APK without the Android SDK.
usage: build_apk.py <AndroidManifest.xml> <libdir with *.so> <out.apk> [keystore.pem]"""
import sys, os, zipfile, struct, hashlib, io, datetime
import pyaxml
from lxml import etree
from cryptography import x509
from cryptography.x509.oid import NameOID
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import rsa, padding

man, libdir, out = sys.argv[1], sys.argv[2], sys.argv[3]
keyf = sys.argv[4] if len(sys.argv) > 4 else os.path.join(os.path.dirname(os.path.abspath(__file__)), 'bsiquest_key.pem')

# ---- key + self-signed certificate (kept so updates install over older builds) ----
if os.path.exists(keyf):
    pem = open(keyf, 'rb').read(); key = serialization.load_pem_private_key(pem, None)
    cert = x509.load_pem_x509_certificate(pem[pem.index(b'-----BEGIN CERTIFICATE'):])
else:
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, 'BSI-Quest personal build')])
    now = datetime.datetime(2026, 1, 1)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now).not_valid_after(now + datetime.timedelta(days=36500))
            .sign(key, hashes.SHA256()))
    open(keyf, 'wb').write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8, serialization.NoEncryption())
                           + cert.public_bytes(serialization.Encoding.PEM))

# ---- binary manifest ----
ax = pyaxml.AXML(); ax.from_xml(etree.parse(man).getroot()); ax.compute(); manifest = ax.pack()

# ---- unsigned zip (native libs stored uncompressed and 4 KiB aligned, other entries 4-byte aligned) ----
buf = io.BytesIO()
def add(zf, name, data, store):
    zi = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0)); zi.compress_type = zipfile.ZIP_STORED if store else zipfile.ZIP_DEFLATED
    if store:   # pad the local header's extra field so the data starts aligned
        align = 4096 if name.endswith('.so') else 4
        off = buf.tell() + 30 + len(name.encode())
        pad = (-off) % align
        zi.extra = b'\x00' * pad if pad == 0 or pad >= 4 else b'\x00' * (pad + align)
        if pad and pad < 4:
            zi.extra = b'\x00' * (pad + align)
    zf.writestr(zi, data)
with zipfile.ZipFile(buf, 'w') as zf:
    add(zf, 'AndroidManifest.xml', manifest, False)
    for f in sorted(os.listdir(libdir)):
        if f.endswith('.so'):
            add(zf, 'lib/arm64-v8a/' + f, open(os.path.join(libdir, f), 'rb').read(), False)   # deflated: the manifest sets extractNativeLibs=true
apk = buf.getvalue()

# ---- APK Signature Scheme v2 ----
eocd = apk.rindex(b'PK\x05\x06'); cd_off = struct.unpack_from('<I', apk, eocd + 16)[0]
sections = [apk[:cd_off], apk[cd_off:eocd], apk[eocd:]]
def digest(sections):
    chunks = []
    for s in sections:
        for i in range(0, len(s), 1 << 20):
            c = s[i:i + (1 << 20)]; chunks.append(hashlib.sha256(b'\xa5' + struct.pack('<I', len(c)) + c).digest())
    return hashlib.sha256(b'\x5a' + struct.pack('<I', len(chunks)) + b''.join(chunks)).digest()
lp = lambda b: struct.pack('<I', len(b)) + b
ALG = 0x0103   # RSASSA-PKCS1-v1_5 with SHA2-256
dg = digest(sections)
certder = cert.public_bytes(serialization.Encoding.DER)
signed_data = lp(lp(struct.pack('<I', ALG) + lp(dg))) + lp(lp(certder)) + lp(b'')
sig = key.sign(signed_data, padding.PKCS1v15(), hashes.SHA256())
pub = key.public_key().public_bytes(serialization.Encoding.DER, serialization.PublicFormat.SubjectPublicKeyInfo)
signer = lp(signed_data) + lp(lp(struct.pack('<I', ALG) + lp(sig))) + lp(pub)
v2 = lp(lp(signer))
pair = struct.pack('<Q', 4 + len(v2)) + struct.pack('<I', 0x7109871a) + v2
blk_size = len(pair) + 8 + 16
block = struct.pack('<Q', blk_size) + pair + struct.pack('<Q', blk_size) + b'APK Sig Block 42'
neweocd = bytearray(apk[eocd:]); struct.pack_into('<I', neweocd, 16, cd_off + len(block))
open(out, 'wb').write(apk[:cd_off] + block + apk[cd_off:eocd] + bytes(neweocd))
print('wrote', out, len(apk) + len(block), 'bytes')
